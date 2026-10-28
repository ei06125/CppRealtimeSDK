# rtsdk — realtime scheduler & execution manager for ECU/ADAS software

Platform-agnostic C++17 library that sits between ADAS applications and the OS
(POSIX/Yocto Linux, FreeRTOS, or a simulator).

## Architecture

```
 gRPC control plane (src/grpc, optional, non-RT)      proto/execution_control.proto
            │  status / restart / stop / event stream
 ┌──────────▼───────────────────────────────────────────────────────────────┐
 │ ExecutionManager   LaunchConfig (nodes, order, deps, delays, fault policy)│
 │   fault policy: ignore | restart(≤N) | stop | safe-state hook            │
 ├───────────────┬──────────────────────┬───────────────────────────────────┤
 │ Watchdog      │ HeartbeatMonitor     │ Executors                         │
 │ faults + HW   │ lock-free liveness   │  TickScheduler  (cooperative FP)  │
 │ kick gating   │ table                │  ThreadExecutor (OS preemption)   │
 ├───────────────┴──────────────────────┴───────────────────────────────────┤
 │ NodeRunner / Node  (priority 0-255, period, deadline, stats)             │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ OSAL: IClock, IThread, IPlatform   → PosixPlatform | FreeRtosPlatform |  │
 │                                      SimPlatform (ManualClock)           │
 └──────────────────────────────────────────────────────────────────────────┘
 comms: SpscRing (variable-length msgs) and Mailbox<T> (latest value, seqlock)
        over HeapRegion (threads) or PosixShm (processes)
```

* **Priorities / preemption.** `ThreadExecutor` gives each node a thread at its
  priority (SCHED_FIFO on POSIX, task priority on FreeRTOS), so the kernel
  preempts. `TickScheduler` is single-threaded fixed-priority dispatch with
  run-to-completion jobs (a higher-priority job released during a running job
  waits until it ends; the wait is recorded as `max_latency_us`).
* **Ticks.** `TickScheduler::tick()` releases due jobs, dispatches, runs the
  watchdog. Drive it from a timer ISR/task or let `start()` do it.
* **Heartbeats / watchdog.** Every completed job beats. A node silent for
  `hb_timeout` (default 3 periods) raises a fault; deadline misses and step
  failures raise one after `fault_threshold` consecutive occurrences. The
  hardware watchdog is kicked only while no heartbeat is expired. A node stuck
  inside `on_step` cannot be restarted and escalates to safe state.
* **Launch configuration.** See `include/rtsdk/launch.hpp` for the text format;
  dependencies are topologically ordered (cycles/unknown nodes rejected), a
  failed `on_init` skips its dependents.
* **gRPC.** `proto/execution_control.proto` + `src/grpc/control_service.*`
  expose `GetStatus`, `ListNodes`, `RestartNode`, `StopNode`, `StreamEvents`.

## Build & test

```
# CMake (verified: cmake 4.2 + Ninja, gRPC 1.51 / protobuf from Ubuntu packages)
sudo apt install cmake ninja-build libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
cmake -S . -B out -G Ninja -DRTSDK_ENABLE_GRPC=ON && cmake --build out && ctest --test-dir out

# Dependency-free quick loop (g++ only): tests, ASan/UBSan, TSan, perf budget, FreeRTOS compile check
make quality

# Bazel (BUILD.bazel / MODULE.bazel / src/grpc/BUILD.bazel) - NOT yet executed, see below
bazel test //... && bazel build //src/grpc:rtsdk_grpc
```

Verification status (authoring sandbox, no GitHub/BCR access):
* Run and passing: CMake build, 31 core tests, the end-to-end gRPC tests
  (`tests_grpc/test_grpc.cpp`: loopback server + client stub for GetStatus,
  ListNodes, RestartNode/StopNode, StreamEvents), ASan/UBSan, TSan, perf budget.
* Bazel: `//:rtsdk_tests`, `//:rtsdk_perf`, `//:rtsdk_freertos_syntax` and
  `//:example_adas` build and pass with the distro Bazel 4.2.3 (with `rules_cc`,
  `rules_java` and coverage repos overridden to local stubs, and the `load()`
  line stripped). NOT run: bzlmod (`MODULE.bazel`) resolution and
  `//src/grpc:rtsdk_grpc`, which need Bazel >= 7 plus the Bazel Central Registry.
* FreeRTOS backend: compiled against declaration stubs (`tests/stubs`) only,
  never run on a real kernel. Run a FreeRTOS port build in CI before relying on it.

Test layers: behaviour (`tests/test_scheduler|manager|launch|comms|posix.cpp`,
deterministic via `SimPlatform`), performance (`bench/bench.cpp --check`),
quality (`-Wall -Wextra -Wpedantic -Werror`, ASan/UBSan, TSan).

## Known limits

* Real-time latency numbers only mean something with RT privileges
  (`CAP_SYS_NICE`/root, `PREEMPT_RT` kernel); otherwise threads fall back to
  SCHED_OTHER and `realtime_priorities_active()` is false.
* Node mutexes are plain `std::mutex` (no priority inheritance); the lock is
  only contended by the supervisor, not between nodes.
* Mailbox/SpscRing are single-writer / single-reader by contract.
* The gRPC server uses insecure credentials; terminate mTLS in front for vehicles.
