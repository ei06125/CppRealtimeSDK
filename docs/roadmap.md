<!-- Maintenance: retain unresolved deployment gates until direct target evidence proves them; passing host tests is not a substitute. -->

# Requirements and verification ledger

The goal is a platform-agnostic realtime scheduler and execution manager supporting
POSIX/Yocto and a realtime OS, priorities and preemption, ticks, heartbeat/watchdog,
launch sequences, shared memory, gRPC, and behavior/performance/quality testing.

Implemented foundation:

- Reusable source-tree CMake module for linking a configured FreeRTOS kernel
  into a board application; the simulator uses the same module.
- Cortex-M4F reference firmware cross-compiles with the GCC_ARM_CM4F port and
  boots without a host OS on QEMU MPS2 AN386; Debug/Release verify launch-file
  parsing, preemption, heartbeat, watchdog latching and runtime fault isolation.
  Supervisor and worker stack high-water samples and ELF section sizes are
  recorded. A post-run resource API retains worker snapshots without querying
  deleted task handles, and distinguishes disabled/uncreated measurements.
  The bare-metal GNU ARM runtime also supplies task-local EHABI exception state;
  a concurrent-catch regression fails with default shared state and passes with
  the per-task integration. This shim is toolchain/architecture-specific. Physical board deployment remains unverified.
- FreeRTOS task/mutex backend, verified with kernel V11.2.0 POSIX simulation,
  including priority preemption of a non-yielding callback, lifecycle failures,
  runtime fault isolation, blocked-callback watchdog detection, and injected
  first/partial task-creation failure cleanup.
- Yocto development layer with optional native gRPC code generation tools;
  x86-64 cross-build and selected QEMU tests on an ARM host.
- Separate hosted core, Linux backend, and optional gRPC CMake targets; static
  and versioned shared-library builds, with relocated installed packages tested
  by compiling and running an external consumer.
- Versioned launch files with explicit callback registry, graph validation, parser
  rejection tests, and an ADAS example loading configuration from disk.
- Deterministic release ordering, including configuration-order priority ties
  after dependency sorting, with a regression covering distinct launch/release
  sequences. Launch DAG validation, tick counters, skipped
  release accounting, deadlines, explicit heartbeat and watchdog state.
- Linux absolute-time POSIX runner; explicit FIFO thread scheduling helper.
- Per-node POSIX workers with strict FIFO startup and independent execution tests.
  FIFO supervision is configured above all nodes before launch; guest tests
  verify watchdog detection during a non-yielding first callback. Linux state
  locks and launch/stop gate mutexes use priority inheritance with native
  monotonic condition waits; state-lock inheritance is verified against an
  ordinary-mutex control
  with three FIFO priorities on one CPU. Injected
  supervisor/worker policy failures must suppress all launch callbacks.
- Independent watchdog supervisor, tested while a callback is blocked. Detection
  faults future invocations; callback cancellation and bounded shutdown are not provided.
- Portable bounded task-local mailbox for Linux and FreeRTOS, with no normal-path
  allocation, concurrent payload-integrity tests, installed-consumer coverage and
  a native Cortex-M4 producer/consumer check.
- Process-shared mailbox, tested with child processes for transfer, owner-death
  recovery, bounds, setup-failure cleanup, existing-object preservation, and payload
  integrity during repeated reads/writes.
- Removed per-dispatch heartbeat-wrapper allocation; a core regression verifies
  20,000 normal callback/heartbeat dispatches without ordinary operator-new calls.
- Behavior tests, synthetic dispatch benchmark, actual release latency/jitter
  harness with sandbox measurements, and sanitizer build option.
- Real TCP gRPC health/stop and binary application-data pipeline tests; runnable
  server/client combines launch files, per-node workers, shared memory and gRPC.
- Eight-node mailbox/health stress test with configurable duration; a 10-second
  host sample passed under ThreadSanitizer.
- Concurrent stop/startup/health stress tests passing under ThreadSanitizer;
  full/core/ASan/TSan GitHub Actions workflow defined.

Required follow-up before claiming completion:

- Strict Linux FIFO preemption now passes as guest root on one emulated ARM64
  CPU, along with all 20 behavior executables in a shared gRPC-enabled build. Repeat on the deployment
  kernel and hardware before selecting timing budgets.

- Extend the FreeRTOS Cortex-M4 reference firmware to physical-board validation,
  constrained C++ runtime/resource sizing and port-specific failure injection. The backend
  now uses native mutexes and tasks; STL allocation and exceptions remain required.
- Build and package the Yocto externalsrc recipe and execute it in a target image.
  BitBake parsing and dry-run graphs pass on Poky walnascar: 842 tasks by
  default and 1097 tasks with meta-oe gRPC dependencies enabled.
  A dedicated single-CPU rtx-validation-image and strict OEQA runtime test
  are provided; its 4514-task dependency dry run passes, but no image was built.
  Optional rtx-ptest metadata and its dependency dry run pass; the runner has
  been exercised with native shared binaries. An actual BitBake build reached
  dependency fetching and stopped on the incomplete offline source cache.
  An online core-only retry also stopped in fetching; the sandbox proxy requires
  approval for upstream source access (observed HTTP 403 from ftp.gnu.org).
  x86-64 cross compilation and selected QEMU user-mode execution are verified
  separately; they do not prove Yocto integration.

- Priority/preemption tests and expanded stress/fault injection coverage.
  Deadline overload and runtime callback fault isolation now have dedicated tests.
  Stop races, launch-failure suppression,
  and pre-start stops now have coverage, including ThreadSanitizer.
- Target latency/jitter measurements, larger stress tests, and measured deployment
  resource budgets. Resource formulas and a host stack/object-size probe are now
  provided; a CI matrix is defined but has not run on hosted infrastructure.
- Extend backend failure injection and long-running target stress tests. POSIX
  clock injection now verifies malformed/overflow timestamps, errno propagation,
  cooperative dispatch/supervisor cleanup, and worker launch-gate cleanup.
  Scheduler release arithmetic also has signed-nanosecond boundary tests.

The dispatch benchmark reports synthetic overhead; the latency harness records
actual host timing. Neither provides a realtime latency or deadline guarantee.
No ECU hardware timing, FreeRTOS board deployment, or Yocto image has been
verified yet. Privileged Linux FIFO execution is verified in a single-CPU VM.
