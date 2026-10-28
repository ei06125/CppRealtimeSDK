<!-- Maintenance: keep public build/run instructions aligned with enabled CMake components and recorded validation scope. -->

# RTX — ECU execution library

C++20 scheduler and execution management foundation for ADAS nodes. CMake builds
a deterministic scheduler, Linux/POSIX runner, process-shared mailbox, and optional
gRPC management service. The adjacent reference repositories are read-only inputs.

```sh
# Ubuntu dependencies
sudo apt-get install build-essential cmake ninja-build libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/adas_demo configurations/adas.launch
./build/rtx_benchmark
```

Set `-DRTX_GRPC=OFF` for the runtime without gRPC. Set `-DRTX_SANITIZE=ON`
for AddressSanitizer and UndefinedBehaviorSanitizer. `Node` objects constitute an
in-process launch configuration: names, dependencies, priority, period, deadline,
watchdog interval, and callbacks. Dependencies form a DAG and order the initial
invocations; later releases use descending priority. Missed periods are skipped,
not replayed. Deadline accounting uses the oldest pending release, including queue
latency. An exception faults its node. A heartbeat timeout faults that node and
prevents subsequent invocations. Faults remain visible until a new start.

`Scheduler::step` accepts monotonic timestamps and an injected completion clock for
deterministic tests. Its owner serializes start/step and ensures callback and clock
lifetimes; callbacks may use their context and heartbeat only during invocation.
Health inspection, watchdog inspection, and stop may occur concurrently. Stop
prevents future callbacks but cannot cancel one already executing. Callbacks must
terminate. The runner keeps phase-aligned CLOCK_MONOTONIC release times, skips expired ticks,
and uses interruptible idle waits. A stop wakes the idle dispatcher immediately;
it still waits for any active callback to return.
A separate supervisor checks watchdogs at the runner resolution, including while
a callback is blocked. Detection latency also depends on OS scheduling. The
supervisor cannot forcibly terminate a callback; shutdown waits for it to return.
`set_fifo_priority` explicitly reports permission and range failures.

`SharedMemory` is a Linux process-shared, fixed-capacity latest-value mailbox with
robust mutex recovery after owner death. Creation is exclusive; publish its name
to peers only after construction completes. All peers must use the same ABI and
capacity. Unlinking is explicit. Mutex contention can block, so use it outside
hard realtime paths. gRPC also belongs on management threads; applications host
`ControlService` with their own server lifetime and credentials.

Current limits and remaining work are tracked in [docs/roadmap.md](docs/roadmap.md).
This is a development library, without a functional safety certification or
validated worst-case execution time.

## gRPC management example

With `RTX_GRPC=ON`, run these commands in separate terminals:

```sh
./build/rtx_control server 127.0.0.1:50051
./build/rtx_control health 127.0.0.1:50051
./build/rtx_control stop 127.0.0.1:50051
```

The server runs camera and perception nodes until the stop RPC arrives, then
stops execution and shuts down its gRPC server. The client has a three-second
RPC deadline. This example uses an insecure local connection; applications can
host the same service using gRPC TLS credentials. `control_roundtrip` tests an
actual TCP connection on an ephemeral loopback port, observes a running node's
ticks and heartbeat, requests stop, joins execution, and reads the stopped state.

## Per-node execution

`PosixTaskRunner runner(scheduler)` assigns one OS worker per node and requests
`SCHED_FIFO` with each node's priority. All workers must configure successfully
before callbacks begin; permission or priority errors abort startup. Use
`PosixTaskRunner(scheduler, false)` for ordinary OS scheduling during development.
That mode provides independent threads without priority guarantees.

Cooperative dispatch launches callbacks in dependency order, then dispatches
pending releases by descending priority. Equal-priority releases preserve the
original configuration order even when dependency sorting changed launch order.
Per-node OS workers use their operating system's scheduling rules after launch.

Initial callbacks complete in topological launch order. After its first callback,
each node runs independently; releases are checked at the configured resolution.
Callbacks for the same node never overlap. A launch callback fault aborts startup;
a later callback fault disables that node while other workers continue. Stop is
sticky and joins all workers when their active callbacks return. Call `run` once
and exclude other dispatchers using the same Scheduler. Scheduler health and the
gRPC ControlService work with either runner.

Linux scheduler state and runner launch/stop gates use pthread
priority-inheritance mutexes with native monotonic condition waits; unsupported
mutex setup fails explicitly. Rebuild the library and consumers together after
updating this public mutex implementation. The OS provides preemption in FIFO mode. The watchdog supervisor requests
SCHED_FIFO at one level above the highest node before any callback can launch.
Node priorities must leave that level available (1–98 on Linux, supervisor up to
99). Policy failure in any worker or the supervisor aborts launch. Choose a poll
resolution that leaves CPU capacity for workers and management; very frequent
higher-priority supervision can itself starve workers. Stop still waits for
callbacks to return. Strict single-CPU preemption and watchdog detection during a
busy callback pass in the Linux VM; target worst-case latency remains unverified.

## Launch files

`load_launch(stream, callback_registry)` creates validated nodes from a versioned
text file. Applications register callback names explicitly; configuration selects
those callbacks and never loads executable code. Parsing completes before any
callback runs. See `configurations/adas.launch`.

```text
rtx-launch 1
# node NAME CALLBACK PRIORITY PERIOD_NS DEADLINE_NS WATCHDOG_NS [DEPENDENCY ...]
node camera capture 20 10000000 8000000 100000000
node perception infer 10 20000000 15000000 100000000 camera
```

Fields are whitespace separated. Blank lines and comments starting with `#` are
allowed. Durations are signed 64-bit integer nanoseconds; period and deadline must
be positive, watchdog must be nonnegative (zero disables it). Priority must fit an
`int`; FIFO priority limits are additionally enforced by the OS backend. Names and
callback keys are single tokens. Dependencies may refer to nodes defined later.
Duplicate nodes or dependencies, missing callbacks/dependencies, cycles, malformed
numbers, unsupported versions, and empty configurations are rejected. Syntax errors
identify the input line. Graph errors identify the invalid graph or node class.
The same parsed nodes can be passed to either POSIX runner.

## Build targets and installation

The hosted C++20 core (`rtx::core`) contains scheduling policy, health tracking,
and launch loading. It has no Linux headers or source dependencies and requires
standard library threading, allocation, and exceptions. `rtx::posix` contains the
Linux execution and shared-memory backend. `rtx::rtx` combines the enabled runtime
targets; `rtx::grpc` adds the management service.

```sh
cmake -S . -B build-core -DRTX_POSIX=OFF -DRTX_GRPC=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
cmake --install build --prefix /your/install/prefix
```

A consuming CMake project uses `find_package(rtx 0.1 CONFIG REQUIRED COMPONENTS
core)` and links `rtx::core`, `rtx::posix`, or `rtx::grpc` as needed. Set
`CMAKE_PREFIX_PATH` to the install prefix. Available components are `core`,
`posix`, and `grpc`. The gRPC install includes generated protocol headers and the
schema. `BUILD_TESTING=OFF` excludes tests; `RTX_EXAMPLES=OFF` excludes examples
and the benchmark. The installation test moves its install tree before building
and running a separate consumer to verify package relocation and transitive
compiler features and dependencies.

## Clock boundaries

Clock inputs use nonnegative signed 64-bit nanoseconds from a monotonic epoch.
Negative start/dispatch/watchdog times and regressing dispatch times are rejected.
Regressing heartbeat or completion samples fault the affected node. Completion
clock exceptions propagate to the runner, which stops execution and joins workers.
Release advancement avoids multiplying elapsed periods. If the next phase-aligned
release cannot be represented, the node faults with `release time overflow`
before invoking that release's callback. Health counters saturate at their unsigned
64-bit maximum. The POSIX clock conversion and dispatcher release advancement are
also checked. Clock boundary tests run without waiting for real time to pass.

Actual release latency and interval jitter can be measured with `rtx_latency`;
see [measurement methodology and baseline](docs/performance.md).

## Concurrency and quality checks

```sh
cmake -S . -B build-tsan -G Ninja -DRTX_GRPC=OFF -DRTX_TSAN=ON -DRTX_EXAMPLES=OFF
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure -R 'rtx_(runner_ownership|launch_watchdog|stop_latency|runtime_fault|lifecycle|tasks|watchdog)_tests'
```

ThreadSanitizer requires a separate build from ASan/UBSan. The lifecycle test races
multiple stop callers with worker creation and health readers over 150 sessions,
and checks pre-start stop and launch fault suppression. These concurrency tests
passed locally under ThreadSanitizer. This is evidence for exercised paths, not a
proof covering all possible interleavings. The GitHub Actions workflow defines
full, core-only, ASan/UBSan, and TSan jobs; hosted CI execution is still unverified.

Shared-memory recovery is covered by a process-death test that exits a child while
it owns the actual robust mutex and has written an invalid message length. The
next locker discards that payload, restores mutex consistency, and returns an
empty message; later reads and writes remain usable. Bounds tests verify that a
rejected oversize write preserves the previous value. A separate child performs
3000 writes while the parent reads and checks payload consistency. Owner-death
recovery does not preserve the last committed message: consumers must treat the
empty result as missing data and apply their own freshness policy.

Launch dependencies also propagate initial callback failures in the deterministic
scheduler: an unstarted dependent is faulted with its predecessor's name, while
independent nodes continue. Already launched dependents are not stopped by a later
peer failure. The task runner aborts the whole startup on a failed first callback;
a fault after successful launch disables only that node. Deterministic overload
tests verify deadline misses and skipped periods, and a worker test verifies that
a healthy launched dependent continues for several releases after its peer faults.

A Yocto development layer and an emulated foreign-architecture cross-build recipe
are documented in [yocto/README.md](yocto/README.md). BitBake/image integration
remains unverified; selected x86-64 tests and the ADAS demo ran under QEMU on ARM.

The [FreeRTOS kernel simulator target](targets/freertos-sim/README.md) builds the
same core with native mutexes and per-node tasks. Its kernel integration test
verifies priority preemption; MCU deployment and timing remain unverified.

The [application data pipeline](docs/pipeline.md) combines launch loading, node
execution, shared-memory mailboxes, and binary gRPC Read/Write operations.

The [Linux FIFO validation test](docs/realtime-validation.md) verifies scheduling
policy and single-CPU preemption, or explicitly skips when permission is denied.

A node watchdog arms when its first callback is dispatched, not while it is
waiting for launch predecessors. The initial heartbeat baseline is that dispatch
time. Startup waiting has no separate timeout; configure a watchdog on a running
predecessor to detect its stalled callback. Cooperative dispatch refreshes time
between callbacks so queued nodes receive a current initial watchdog baseline.

Use `rtx_resources` and the [resource planning guide](docs/resources.md) to size
threads, stacks, shared-memory mappings, and management buffers on the target.

`BUILD_SHARED_LIBS=ON` builds versioned shared libraries (ABI SONAME major 0),
with development linker symlinks and CMake exports. The shared Release build is
covered by the same runtime and relocated-package consumer tests.

Each runner accepts one `run()` attempt. Concurrent and repeated attempts throw
`std::logic_error` before touching scheduler state. Create a new runner for a new
session, including after a failed startup. The caller still owns serialization
between different runner objects that reference the same Scheduler.

Fault provenance is latched: a timeout reported during a callback is preserved
if that callback later throws or returns a regressing completion timestamp. Valid
completion samples still update deadline-miss counters. Callback exceptions with
empty messages are recorded as `callback exception (empty message)`.

Shared-memory setup failure tests inject ftruncate, mmap and mutex-initialization
errors into the production source. Each case verifies that the descriptor count
returns to its baseline and the failed creator leaves no named object. Duplicate
creation and a capacity-mismatched peer open must preserve the existing owner.
The injection is confined to test-executable linker wrapping.

The [Cortex-M4 FreeRTOS reference firmware](targets/freertos-cortex-m4/README.md)
boots under QEMU using the native M4F interrupt port and verifies preemption,
heartbeats and stop in Debug and Release. Toolchain, ELF and memory-section
evidence is recorded in [the validation report](docs/measurements/cortex-m4-validation.json).
Physical hardware timing remains unverified.

For bounded communication between nodes in the same process or RTOS image,
use [rtx::Mailbox<Capacity>](docs/mailbox.md). It stores the latest byte payload
inline and performs normal reads/writes without heap allocation; its mutex can
block. The native Cortex-M4 reference verifies producer/consumer message integrity.

## Code style

Follow the [Google C++ style guide and formatting instructions](docs/style-guide.md).
Run `python3 tools/format.py` before submitting source changes and
`python3 tools/format.py --check` to verify formatting.

## Cross-platform presets

See [build presets and development environments](docs/build-presets.md) for
macOS, Linux, Windows, FreeRTOS, Docker, and VS Code dev container instructions.
Presets offer Debug, Development (`RelWithDebInfo`), and Release builds.
