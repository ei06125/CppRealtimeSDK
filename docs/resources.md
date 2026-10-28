<!-- Maintenance: distinguish object sizes, stack reservations and measured high-water observations when revising resource budgets. -->

# Deployment resource planning

Run `rtx_resources` on the deployment toolchain/runtime to report ABI-dependent
Node, Health, Scheduler and mailbox-header sizes, plus a newly created worker's
default stack and guard reservations. Object sizes exclude owned strings,
dependency vectors, callback captures, allocator metadata and OS thread structures.
Stack reservations are virtual address space; committed/resident memory differs.
The probe does not measure callback stack high-water usage.

| Backend | Execution resources for N nodes | Additional resources |
| --- | --- | --- |
| Deterministic core | Calling dispatcher thread | Scheduler mutex and reserved N-element ready list |
| PosixRunner | Calling dispatcher thread and one watchdog thread | Condition variables and mutexes |
| PosixTaskRunner | N worker threads, one watchdog thread, calling coordinator | Shared health mutex, launch/stop condition variable |
| FreeRtosTaskRunner | N node tasks and calling supervisor task | One scheduler mutex, one failure mutex, N worker descriptors; kernel idle task and tick infrastructure |

Linux thread stacks use the C++ runtime's pthread defaults; this version has no
per-node stack or CPU-affinity configuration. Budget `(N + 1) * default_stack`
for worker and watchdog stack reservations in PosixTaskRunner, in addition to the
calling thread. gRPC manages its own threads and buffers and adds to this total.
Read `/proc/PID/status`, `/proc/PID/smaps_rollup`, and task counts with the actual
workload to size resident memory. Default stack reservations are not a safe
substitute for workload-specific measurement.

For FreeRTOS, the runner's stack_depth argument is in StackType_t units, not bytes.
Node stack requests total `N * stack_depth * sizeof(StackType_t)`, plus N kernel
TCBs, port-specific state, the supervisor stack, idle stack, mutex objects, and
allocator overhead. The default stack_depth is 4096; choose an explicit value for
the board. Enable the port's overflow checks and high-water instrumentation and
measure the deepest callback and exception paths. The simulator port's host
pthread stacks add resources that do not represent an MCU port.

A shared-memory channel maps `sizeof(internal_header) + capacity` bytes in each
process. The OS backs the shared object once, rounded to memory pages; per-process
mappings do not duplicate its payload. The header contains an ABI-specific robust
pthread mutex and size field. Each gRPC read temporarily allocates a buffer equal
to channel capacity, plus protobuf/transport buffers; concurrent reads multiply
that cost. The service registration cap is 16 MiB per channel and gRPC transport
limits apply separately.

The scheduler reserves its ready-list storage at construction and reuses it per
dispatch. Each internal node entry retains one size_t configuration index, independent
of its position in the topological launch sequence. Sorting has an explicit
configuration-order tie break and does not need
stable-sort scratch storage. This is not an allocation-free guarantee: callbacks,
error strings, health snapshots, configuration,
and the C++ library may allocate. Health snapshots copy all N entries. Keep
management polling and gRPC outside timing-critical callback work.

Periods, deadlines and watchdogs are nanoseconds in the core. FreeRTOS execution
is quantized to kernel ticks; Linux execution is limited by poll resolution and
OS scheduling. Establish callback WCET, interruption/queue costs and a watchdog
margin on the target. Watchdogs arm at first invocation and remain latched after
a timeout. Callback deadline misses are counted after return; they do not cancel
work. Stop cannot bound completion of an unbounded callback.

Linux FIFO runners reserve max(node priority)+1 for the watchdog supervisor
before launch; nodes cannot use the maximum OS priority. Include supervisor
polling cost in utilization budgets, since frequent high-priority inspection can
starve lower-priority workers. Ordinary coordinator/management threads may still
be starved by unbounded FIFO work. The FreeRTOS backend likewise enforces
supervisor priority above all nodes. Validate heap/thread/task creation failure, sustained
load and shutdown with the selected configuration before setting deployment
budgets. No fixed ECU memory or timing budget has been verified by the host probe.

Heartbeat wrappers borrow a stack-resident callable through `std::ref`; the
`std::function` reference-wrapper constructor does not allocate. Context and
heartbeat remain valid only during callback invocation. The core regression
`rtx_dispatch_allocation_tests` counts ordinary scalar/array operator-new calls
across 20,000 callback/heartbeat dispatches and watchdog inspections with two
preconfigured nodes, an allocation-free clock and allocation-free callbacks. It
observes zero allocations after start on the tested standard library. This is a
normal-path regression check, not a guarantee for arbitrary callbacks, faults,
management APIs, allocators, or other standard-library implementations.

On Linux, RuntimeMutex uses PTHREAD_PRIO_INHERIT for scheduler state; a blocked
FIFO supervisor can boost a lower-priority lock owner above intervening FIFO
threads. Construction reports pthread setup errors instead of falling back to
an ordinary mutex. FreeRTOS retains its native inheriting mutex; other hosted
platforms retain std::mutex. This change requires rebuilding libraries and all
consumers together. Priority inheritance bounds one source of inversion, not
arbitrary lock hold time: health copies and configuration/error work can still
allocate under the state lock. Linux runner launch/stop gate mutexes also use this inheritance protocol and
native CLOCK_MONOTONIC condition variables. Mailbox robust process-shared mutexes
remain separate and do not gain this protocol. Avoid
unbounded work while holding any lock and validate the full locking budget on
the target.

The Cortex-M4 reference's GNU ARM exception runtime adds 12 bytes per task to
FreeRTOS's existing newlib C-runtime TLS block; it uses no separate heap object.
This is part of the kernel TCB allocation, not Node/Health object size. Other
C++ runtimes have different TLS and exception requirements. The reference's
concurrent-catch test validates separation across task switches; it does not
establish arbitrary C++ runtime thread safety or allocation-failure behavior.

Task-local Mailbox<N> adds N inline payload bytes, a RuntimeMutex, a size_t length
and alignment padding per channel. Allocate it before starting callbacks. The
normal-path dispatch allocation regression now includes mailbox producer/consumer
operations; mailbox errors and construction are outside that measured scope.

## FreeRTOS worker-stack snapshots

After FreeRtosTaskRunner::run returns or throws, `task_resources()` copies a
management snapshot for each prepared worker descriptor. Each entry contains
its node name, requested stack words, bytes per word, whether task creation
succeeded, and an optional minimum-free-stack watermark. Calling it before the
run attempt has finished cleanup throws std::logic_error. Validation failure
before worker preparation yields an empty snapshot; pre-start stop and partial
creation failure retain descriptors with `created=false` for uncreated tasks.
The returned vector and copied names allocate outside worker execution.

Enable INCLUDE_uxTaskGetStackHighWaterMark=1 to collect the optional measurement.
Each worker samples its own stack after callback/failure handling and before
publishing completion and deleting itself. The runner retains values after task
deletion, so applications never query stale kernel task handles. With measurement
disabled the optional is absent; zero is not used as an unavailable sentinel.
The POSIX kernel simulator's configured stack storage does not represent its
underlying pthread stack, so native Cortex-M measurements are the relevant
reference for this API's stack-usage observations.

The current native Cortex-M4 scenarios observe 464–1276 bytes used per worker in
Release and 1016–1496 bytes in Debug with 16384 bytes reserved per worker. These
are tested workload observations, including instrumentation and interrupt/context
switch effects present before sampling. They exclude later task-deletion code
and do not prove worst-case stack use under different inputs, libraries, fault
paths or interrupt load. Keep a target-specific margin and repeat on hardware.
