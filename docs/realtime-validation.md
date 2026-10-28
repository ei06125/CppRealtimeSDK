<!-- Maintenance: describe what each test actually demonstrates and retain the distinction between host skips and guest FIFO passes. -->

# Linux priority preemption check

`rtx_fifo_tests` verifies actual OS scheduling, distinct from release ordering in
the deterministic scheduler or ordinary worker concurrency.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -R '^rtx_fifo_tests$' -V
```

The test first probes SCHED_FIFO in a short-lived thread. If the OS reports EPERM
or EACCES, the executable returns 77 and CTest marks it skipped. A skipped result
is not preemption evidence. On a development target, grant the test's process
realtime scheduling permission using the target's normal privilege/rlimit policy,
then repeat the command and require an actual pass.

The test selects one CPU from its allowed affinity set and pins the initiating
thread to it; node workers inherit that affinity. Node callbacks check their
observed CPU, SCHED_FIFO policy, and priorities (10 and 20). The lower-priority
node's second callback spins for at most 30 ms without yielding or holding a
mutex. The higher-priority callback must execute while that spin is active. The
spin has a monotonic-time bound so a failed preemption observation terminates
instead of consuming the CPU indefinitely. The runner then joins workers.

This does not establish worst-case response time or robustness to CPU overload.
Strict FIFO runners configure the supervisor at max(node priority)+1 before
releasing launch gates. Nodes must leave room below the OS maximum FIFO priority.
The test also runs a 40 ms non-yielding first callback with a 3 ms watchdog; the
callback observes its fault before returning. This verifies supervisor execution
during CPU-bound launch, rather than a timeout discovered after callback return.
Both checks use a 1 ms polling interval. The old 100 us interval starved the low
worker in the emulator once supervision ran above it, illustrating that the
supervisor's CPU utilization must be included in deployment budgets. The calling
coordinator and external management may still be starved by unbounded FIFO work;
watchdog faulting does not forcibly cancel callbacks. The FreeRTOS backend also
requires its supervisor task above all nodes. Host permission denial still skips
FIFO tests; the single-CPU guest run below supplies the actual pass evidence.

### Concurrent execution and mailbox stress

`rtx_stress_tests` runs eight ordinary POSIX workers (four producer/consumer
pairs), four latest-value shared-memory mailboxes opened through separate
mappings, and two concurrent health readers. It checks sequential callback ticks,
nonoverlapping invocation per node, monotonic health ticks/heartbeats, complete
sequence/complement message payloads, nonregressing received sequences, progress
for every node, and stopped health after joining. Latest-value channels may skip
messages; this test does not require delivery of every release. Watchdog timing
is covered by separate tests; this workload disables watchdogs to avoid imposing
host scheduling bounds.

The default 500 ms run is included in CTest and the ThreadSanitizer CI selection.
For a longer local sample (100 through 60000 ms):

```sh
RTX_STRESS_MS=10000 ctest --test-dir build-tsan -R '^rtx_stress_tests$' --output-on-failure
```

The sandbox passed the full ASan/UBSan and shared Release suites (19 passes,
one privileged FIFO skip each), all eight selected ThreadSanitizer concurrency
tests, and a 10-second ThreadSanitizer stress sample. These checks establish
host behavior and race-detection coverage, not target timing guarantees.

### POSIX clock fault injection

`rtx_posix_clock_fault_tests` compiles the backend sources with GNU linker
wrapping of `clock_gettime`, so the same injection works for static and shared
library configurations without production test hooks. It checks preserved EIO
errors, negative seconds/nanoseconds, malformed nanoseconds, conversion overflow,
and the maximum representable nanosecond timestamp. Runtime cases cover a
cooperative dispatch error, a cooperative watchdog-supervisor error, and 25
worker startup failures with threads waiting at launch gates. Each must return
an error, join its threads, and leave scheduler health stopped. The worker
startup failures must execute no callbacks. The test passes under ASan/UBSan,
ThreadSanitizer, and shared Release and is included in the TSan CI selection.

## Single-CPU Linux guest validation

All 17 non-gRPC behavior executables passed as guest root under QEMU's ARM64
`virt` machine with one Cortex-A72 CPU and Ubuntu generic kernel 7.0.0-38.38.
This includes an actual FIFO preemption pass (no skip), shared-memory owner-death
recovery, concurrent worker stress, watchdogs and fault cleanup. The VM supplies
its own Linux scheduler; it does not need host CAP_SYS_NICE or KVM. The host
CTest FIFO skip is retained. This verifies Linux scheduling behavior under
emulation, not ECU latency, board behavior, PREEMPT_RT, or Yocto image integration.

The [validation report](measurements/linux-vm-validation.json) records binary and
kernel hashes; the [serial log](measurements/linux-vm-serial.log) contains guest
kernel messages and per-test exit codes. Reproduce on an ARM64 Ubuntu host:

```sh
sudo apt-get install qemu-system-arm busybox-static zstd
# Download and extract an ARM64 kernel without installing it on the host:
mkdir -p /tmp/rtx-linux-vm
cd /tmp/rtx-linux-vm
apt-get download linux-image-unsigned-7.0.0-38-generic
dpkg-deb -x linux-image-unsigned-7.0.0-38-generic_7.0.0-38.38_arm64.deb kernel
# Return to the SDK checkout, then:
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release -DRTX_GRPC=OFF
cmake --build build-performance
python3 tools/linux_vm_tests.py \
  --kernel /tmp/rtx-linux-vm/kernel/boot/vmlinuz-7.0.0-38-generic \
  --log /tmp/rtx-linux-vm-serial.log build-performance/rtx_*tests
```

Use an available matching ARM64 kernel package if that version is no longer in
the package index. The kernel must support initramfs, procfs, devtmpfs and tmpfs.
The runner builds a temporary diskless initramfs, copies trusted test executables
and their `ldd` dependencies, mounts /dev/shm, and requires every test to exit zero.
It checks a final completion marker and enforces a 120-second VM timeout. Skips
are failures here so absent FIFO permission cannot produce a green result. It
also handles zstd EFI zboot kernels that QEMU cannot directly load. The VM uses
no disk or network; generated initramfs and extracted kernel files are temporary.
Run only trusted binaries: dependency discovery invokes the host's `ldd`.

The guest validation now also passes all 19 behavior executables in a shared,
gRPC-enabled Release build. The init script enables guest loopback; control and
binary mailbox pipeline tests use real TCP within that guest. Dynamic RTX,
gRPC and protobuf libraries are copied from the trusted build's `ldd` output.
[The shared/gRPC report](measurements/linux-vm-grpc-validation.json) and
[serial log](measurements/linux-vm-grpc-serial.log) record this separate run.
The runner's rejection of a guest exit 77 was also checked explicitly.

To reproduce that configuration, use `-DBUILD_SHARED_LIBS=ON -DRTX_GRPC=ON`
in a separate build directory, then pass its `rtx_*tests` executables to the
same VM command. The `linux-guest` job in the quality workflow automates this
on `ubuntu-24.04-arm`, downloads the available distro virtual kernel, and uploads
its serial log even on failure. The job's hosted execution remains unverified;
local QEMU execution is the evidence recorded here. Setup mount/network failures
stop guest initialization rather than silently reducing test coverage.

The clock/policy fault test additionally wraps pthread_setschedparam to force
supervisor-priority and worker-priority setup failures independently. Both must
propagate EPERM, execute no callbacks and leave all nodes stopped. Successful
policy requests are simulated only within those failure-injection scenarios;
actual policy/preemption behavior is checked separately in the Linux guest.

The updated FIFO-supervisor implementation passed the complete 19-test guest
suite; [the supervisor report](measurements/linux-vm-supervisor-validation.json)
and [serial log](measurements/linux-vm-supervisor-serial.log) capture this run.
The full ASan/UBSan suite also passed (21 passes and the host FIFO permission
skip), as did all ten selected ThreadSanitizer tests.

### Scheduler state-lock priority inheritance

The FIFO guest test additionally configures low/medium/high FIFO threads at
priorities 10/20/30 on the same CPU. All policies are configured before release.
The low thread holds a state mutex, the medium thread spins, and the high thread
wakes after 5 ms and waits for that mutex. A plain std::mutex control must leave
the medium spinner running until its 100 ms bound. With RuntimeMutex's Linux
PTHREAD_PRIO_INHERIT protocol, the low owner must instead run and release the
mutex before the medium spinner's timeout. This tests observed lock-owner
progress rather than inspecting only the mutex attribute. It does not establish
worst-case lock or supervision latency and does not establish bounds for condition-variable internals or mailbox locks.

All 19 shared/gRPC guest tests passed with this protocol and its plain-mutex
control: [report](measurements/linux-vm-pi-validation.json),
[serial log](measurements/linux-vm-pi-serial.log). The ASan/UBSan hosted suite
(21 passes, FIFO permission skip), ten selected TSan tests, and five FreeRTOS
Release tests also passed after the mutex change.

The current shared/gRPC guest suite includes the portable Mailbox regression and
passes all 20 behavior executables: [current report](measurements/linux-vm-current-validation.json).
The portable channel also passes between tasks on the native Cortex-M4 FreeRTOS
port in Debug and Release. The hosted ASan/UBSan suite passes 22 tests with the
host FIFO permission skip; all 11 selected TSan tests pass. Normal mailbox reads
and writes are included in the zero-allocation dispatch regression, and the
relocated installed consumer now compiles and uses the public mailbox header.

### Linux launch/stop gate synchronization

Both POSIX runners now pair RuntimeMutex's PTHREAD_PRIO_INHERIT protocol with a
native pthread condition variable configured for CLOCK_MONOTONIC. This replaces
the ordinary launch/stop gate mutex shared by FIFO workers, supervisor and owner.
Predicate updates remain under that same mutex, and pthread waits atomically
release/reacquire it. Relative waits form checked absolute nanosecond deadlines;
unrepresentable deadlines fail explicitly instead of wrapping. No allocation is
needed for these wait objects or their normal wait/notify operations.

The stop-latency test checks immediate predicates, zero timeout, no early timeout,
retained mutex ownership, rejected deadline overflow, rejected unowned waits, and
stop notification during a five-second idle interval. The existing startup/stop
stress and watchdog tests exercise the same gates. All 20 shared/gRPC guest tests
and 11 selected TSan tests pass. The full hosted ASan/UBSan suite passed (22 passes,
FIFO permission skip), with targeted wait/clock/installed-consumer checks repeated
after adding boundary assertions. Native synchronization improves the explicit
gate mutex protocol; it does not provide a worst-case bound for OS scheduling,
pthread internals, callback execution or other application locks.
