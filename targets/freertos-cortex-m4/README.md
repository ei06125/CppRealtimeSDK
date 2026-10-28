<!-- Maintenance: update memory observations and ABI constraints together with the matching firmware validation report. -->

# Cortex-M4 FreeRTOS reference firmware

This standalone target cross-compiles the scheduler/launch core and execution
backend for an ARM Cortex-M4F with the pinned FreeRTOS V11.2.0 GCC_ARM_CM4F port.
It boots on QEMU's `mps2-an386` model with no host operating system in the guest.
The M4F port supports the backend's required 64-bit ticks; the pinned M3 port
only supports 16/32-bit ticks and cannot be substituted unchanged.

On an Ubuntu host with the external kernel checkout:

```sh
sudo apt-get install gcc-arm-none-eabi libstdc++-arm-none-eabi-newlib qemu-system-arm
cmake -S targets/freertos-cortex-m4 -B build-cortex-m4 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/arm-cortex-m4.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFREERTOS_KERNEL_PATH=/tmp/rtx-freertos-kernel
cmake --build build-cortex-m4
ctest --test-dir build-cortex-m4 --output-on-failure
arm-none-eabi-size build-cortex-m4/rtx_mps2
```

The firmware runs a priority-6 supervisor, priority-1 low node, and priority-3
high node. Launch follows the dependency order. The second low callback spins
without yielding until the high task preempts it (bounded by 50 ticks). Both
callbacks emit heartbeats. The nodes are loaded from a versioned launch stream
that lists the dependent first, so parser and topological launch are both tested.
The runner must stop and leave both nodes stopped with at least two ticks.

Two further sessions verify watchdog detection during a 20 ms non-yielding
callback (3 ms watchdog), preservation of the timeout after a late heartbeat and
secondary exception, and runtime exception isolation while a healthy node
continues. A fourth session suspends two tasks inside different catch blocks
and verifies that each retains its own std::current_exception across switches. CTest requires the final suite PASS marker, rejects FAIL markers,
and bounds the QEMU process to 20 seconds. Debug and Release pass locally with
GNU Arm Embedded 14.2.1 and QEMU 10.2.1; this is emulation evidence, not physical
board timing or a production ECU deployment.

`startup.S` initializes data/BSS, enables the FPU before C++ initialization, and
routes SVC/PendSV/SysTick directly to FreeRTOS. The linker script models 4 MiB
code and 4 MiB RAM for this QEMU reference. Assertions remain active in Release; stack-overflow checking (mode 2) and the
FreeRTOS allocation-failure hook terminate with explicit FAIL markers.
The firmware uses newlib and C++ exceptions/STL. FreeRTOS supplies per-task
newlib and exception state through the reference runtime integration; board allocator hooks suspend scheduling around the shared
newlib allocator. The bounded heap grows from the end of static data and
reserves 16 KiB for the initial MSP stack; it does not shrink. Node stacks are
4096 StackType_t words each, supervisor 8192 words, and idle 512 words. These
are conservative test reservations. The firmware reports the supervisor's
stack high-water sample after all sessions: 380 words used in Release and 629
words in Debug on the recorded builds (1520 and 2516 bytes). This sample covers
the tested parser/fault paths, not arbitrary workloads, later logging, interrupts,
or worker stack high-water usage. It is not a worst-case stack budget.

Including the stream-based launch parser and runtime fault scenarios increases
the Release ELF sections to 370498 text/read-only bytes, 2016 initialized data
bytes and 5744 BSS bytes. These exclude heap and task stacks. Exact binaries,
toolchain and observations are in
[the report](../../docs/measurements/cortex-m4-validation.json).

Output and process exit use ARM semihosting. It must be enabled only for this
trusted reference firmware and is not a deployable logging/termination facility.
A real board needs its own memory map, clock/interrupt setup, boot code,
allocator and C++ runtime support, fault handling, output transport, stack sizing,
and hardware validation. The sample allocator is task-context-only. Calls from
ISRs and concurrent C++ local-static initialization are outside this reference's
scope. Shared memory and gRPC remain Linux-side facilities. The kernel remains
external and is not copied into this repository.

## Task-local C++ exceptions

The bare-metal GNU libsupc++ archive tested here normally returns one global
exception-state object; newlib reentrancy alone does not make it task-local.
The concurrent-catch regression failed with that default runtime: the low task
observed the high task's exception after resuming. `task_runtime.h/.cpp` extend
the kernel's C-runtime TLS block with 12 bytes of ARM EHABI exception state per
task and provide the ABI's __cxa_get_globals entry points. Kernel context switches
select both newlib and exception state, without allocating a separate TLS object.
Task cleanup requires caught, uncaught and propagation state to be empty before
reclaiming newlib resources. A zero-initialized fallback serves startup before
the scheduler begins. Exceptions from interrupts are unsupported.

This shim is specific to the GCC ARM EHABI layout. Its compile guard admits GNU
libstdc++ major versions 13–14; execution has been validated locally with 14.2.1.
Revalidate the ABI and concurrency test when changing the toolchain; do not copy
this shim unchanged to a different architecture or C++ runtime. Other board
integrations must supply equivalent thread-safe C++ runtime support. This does
not add general C++ thread_local variables or synchronized local-static
initialization. The POSIX kernel simulator uses its hosted C++ runtime and does
not use this board-specific shim.

The preemption session also passes a 16-byte sequence/complement payload from the
low task to the high task through rtx::Mailbox<16>. The high task checks the full
payload before acknowledging preemption, exercising the shared portable channel
with native FreeRTOS mutexes. Its storage is inline; its mutex is created during
supervisor configuration, before worker launch.

## Worker-stack measurements after shutdown

The firmware calls `runner.task_resources()` after each scenario and prints
`STACK scenario/node minimum_free_words=... reserved_words=... word_bytes=...`.
The native worker takes its watermark before self-deletion; the snapshot remains
valid after run returns, including launch-watchdog failure cleanup. Each created
worker must report a nonzero watermark no larger than its requested stack. The
current scenarios observe 464–1276 bytes used per worker in Release and
1016–1496 in Debug. The sample includes the executed callbacks, C++ exception
paths and measurement overhead before task deletion, not arbitrary future load
or deletion internals. Keep the conservative reservations until hardware testing
supports a workload-specific budget. The snapshot API allocates its copied vector
and names in the supervisor after worker cleanup.
