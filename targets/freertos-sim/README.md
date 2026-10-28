<!-- Maintenance: document simulator behavior separately from native interrupt ports and physical hardware measurements. -->

# FreeRTOS kernel integration

This target compiles the same scheduler/launch core with native FreeRTOS mutexes
and a per-node FreeRTOS task backend. Kernel source stays external. Tested kernel:
V11.2.0, commit `0adc196d4bd52a2d91102b525b0aafc1e14a2386`.

```sh
git clone --branch V11.2.0 --depth 1 https://github.com/FreeRTOS/FreeRTOS-Kernel.git /tmp/rtx-freertos-kernel
cmake -S targets/freertos-sim -B build-freertos -G Ninja \
  -DFREERTOS_KERNEL_PATH=/tmp/rtx-freertos-kernel
cmake --build build-freertos
ctest --test-dir build-freertos --output-on-failure
```

The integration test starts the real kernel using its GCC POSIX simulator port.
It verifies launch order, priority-based preemption of a non-yielding low-priority
callback, heartbeat calls, and graceful worker stop. Additional lifecycle and fault
tests cover invalid priorities, pre-start stop, first-callback failure suppression,
runtime fault isolation, and watchdog detection while a callback sleeps for 30
ticks. A late heartbeat leaves its timeout latched and healthy tasks continue. It runs without privileged
Linux SCHED_FIFO. This proves simulator kernel behavior, not MCU timing or the
Linux backend's strict FIFO behavior.

`FreeRtosTaskRunner::run` must execute in a supervisor task whose priority exceeds
every configured node priority. Positive node priorities below the supervisor's
priority are required. This reserves supervision priority even when callbacks are
CPU-bound. Each task checks releases once per kernel tick. First callbacks complete
in launch order; a first-callback fault aborts startup. Later callback faults disable
only that node. Watchdogs are inspected by the calling supervisor once per tick.
Stop is sticky and waits for active callbacks to return. Do not call run repeatedly,
from an ISR, or concurrently with another dispatcher using the same Scheduler.

A board integration can include the reusable CMake module after configuring its
FreeRTOS kernel and port. With an existing `freertos_kernel` target:

```cmake
include(/absolute/path/CppRealtimeSDK-Codex/cmake/RTXFreeRTOS.cmake)
rtx_add_freertos_library(rtx_board freertos_kernel)
target_link_libraries(board_application PRIVATE rtx_board)
```

The function creates a static core/launch/backend library and propagates C++20,
`RTX_FREERTOS`, public headers and kernel requirements to its consumers. This is
also the build path exercised by the simulator. The board supplies its compiler,
linker script, configured kernel, port and C++ runtime. The module is consumed
from the source checkout; it is not an installed board SDK. Do not mix this
library's ABI with the hosted core or link both into the same application.
Required kernel settings include preemption, mutexes,
task notifications, dynamic allocation, delay/delete/priority-get APIs, and 64-bit
ticks. Tick frequency must divide one billion exactly. Time conversion rejects
values beyond the signed 64-bit nanosecond range.

This implementation uses STL containers, C++ exceptions, and allocation during
configuration; it is not a freestanding or allocation-free runtime. Task stacks
are specified in StackType_t units (default 4096). Applications must provide
appropriate C++ runtime/allocator support for their port, size stacks and heap,
and validate interrupt, tick, watchdog and priority behavior on their board. The
simulator uses the upstream heap_3 allocator. Kernel code is not copied into this
repository. Shared memory and gRPC remain Linux-side facilities; transport bridging
to the MCU is application-specific and unimplemented.

The CI workflow includes a kernel integration job pinned to the tested commit.
The five simulator tests passed locally in Debug and Release; hosted execution
remains unverified.

The task-creation failure test uses GNU linker wrapping only in its test executable.
It forces xTaskCreate to fail on the first worker and then on the second worker.
Both cases must propagate the allocation failure, leave all nodes stopped, and
invoke no callbacks. A partially created worker exits its launch gate before the
runner returns. This checks kernel task-creation failure handling, not arbitrary
C++ heap exhaustion or board-specific allocator failures.

Runner instances reject a second run attempt with std::logic_error, including
a retry after failed startup. Construct a fresh runner for the next session.

A separate [Cortex-M4 reference target](../freertos-cortex-m4/README.md) now
cross-compiles the same backend for the GCC_ARM_CM4F interrupt port and boots it
on QEMU MPS2 AN386 without a host OS in the guest. This supplements the richer
POSIX-port fault suite; it does not establish physical board timing.

`FreeRtosTaskRunner::task_resources()` is available after the run attempt finishes
cleanup. It reports created tasks and requested stack dimensions, with an optional
minimum-free-stack sample when INCLUDE_uxTaskGetStackHighWaterMark=1. Tests cover
pre-run rejection, snapshots after normal execution, validation/pre-start-stop
handling and partial task-creation failure. This POSIX simulator leaves watermark
collection disabled: its kernel stack storage is not the actual pthread stack.
Use the Cortex-M4 reference's native measurements for the tested MCU call paths.
