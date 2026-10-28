<!-- Maintenance: state workload, sample counts and environment when adding measurements; do not infer worst-case timing from host percentiles. -->

# Timing measurements

Build optimized code before collecting a baseline:

```sh
cmake -S . -B build-performance -G Ninja -DCMAKE_BUILD_TYPE=Release -DRTX_GRPC=OFF
cmake --build build-performance
./build-performance/rtx_latency cooperative 1000
./build-performance/rtx_latency workers 1000
./build-performance/rtx_latency fifo 1000
```

`fifo` requests SCHED_FIFO priority 10 and fails explicitly if unavailable. The
other modes use ordinary OS scheduling. Each run measures one probe node with a
1 ms period, a 1 ms deadline, a disabled watchdog, and a 100 us polling resolution.
The callback reads CLOCK_MONOTONIC at entry, stores the sample, and emits a heartbeat.
The first invocation includes startup overhead; there is no warmup exclusion.

JSON output reports nanoseconds. Release lateness is actual callback entry minus
its scheduled release; that release is the oldest pending release when periods
were skipped. Absolute interval jitter is the absolute difference between two
successive callback entry times and the nominal 1 ms period. It includes missing
periods, unlike a phase-only jitter statistic. Sorted percentiles select index
`floor((sample_count - 1) * percentile / 100)` without interpolation. Deadline
misses also include callback completion costs; skipped releases are reported
separately. A monotonic clock resolution value describes the clock interface,
not achieved timer accuracy.

Samples are bounded to 2–100000 (default 200). Measurement storage reserves
16 bytes per sample on the tested 64-bit ABI; processing adds approximately
16 bytes per sample for latency/jitter arrays. Threads and scheduler state add
platform-dependent overhead. The timeout is ten nominal periods per requested
sample plus one second. The sample callback must return for runner shutdown.

[Sandbox baseline](measurements/sandbox-latency.json) records 1000 samples per
ordinary scheduling mode, compiler, build type, architecture, and kernel. These
are observations of this environment with one unloaded probe node. Repeat on the
ECU with representative node workloads, CPU affinity, memory pressure, thermal
conditions, and deployment scheduling policy before choosing timing budgets.
No target worst-case latency or realtime guarantee is established by this run.
CTest smoke cases verify successful sample collection and no early releases;
they deliberately impose no host-dependent latency threshold.

[Updated 10,000-sample measurements](measurements/sandbox-latency-current.json)
record the scheduler after removing the per-dispatch heartbeat-wrapper allocation.
The report includes source hashes, compiler/kernel metadata, and the synthetic
benchmark result. Validation builds/tests overlapped initial collection in this
shared sandbox; CPU isolation was not used. The older baseline is retained as a
historical observation. Different loads and sample counts prevent attributing
latency differences to the implementation change. The allocation regression test
provides direct evidence for the removed allocation; timing remains observational.
