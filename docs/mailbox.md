<!-- Maintenance: keep address-space, ownership, blocking and allocation guarantees synchronized with the public mailbox implementation. -->

# Communication within one process or RTOS image

`rtx::Mailbox<Capacity>` stores a bounded latest value as bytes. It is available
from `rtx::core` on hosted platforms and from the FreeRTOS library, using the
backend's RuntimeMutex. It can connect multiple producers and consumers in one
address space. Payload storage is inline in the object; normal reads and writes
perform no heap allocation. Mutex construction can allocate on FreeRTOS, and
lock acquisition can block. Error construction may allocate.

```cpp
#include <rtx/mailbox.hpp>
#include <array>

rtx::Mailbox<64> channel;
std::array<std::byte,3> input{std::byte{0},std::byte{0xff},std::byte{7}};
channel.write(input);
std::array<std::byte,64> output{};
const auto bytes = channel.read(output); // 3; reading does not consume the value
```

Capacity must be positive at compile time. Initially the channel is empty.
Each write atomically replaces the previous value; `write({})` clears it. Reads
copy the complete latest value and return its length. A read buffer smaller than
the current value or a write larger than capacity throws std::length_error before
copying any bytes. A rejected call leaves the channel untouched. Readers can miss
intermediate writes; this is not a queue or an every-message delivery contract.
Concurrent writers are ordered by lock acquisition, not application sequence IDs.

The channel owns no threads and cannot be copied. Keep it alive until all users
have stopped; callers must keep their input/output buffers valid and stable for
the call. It is task-context-only and must not be called from an ISR. Linux and
FreeRTOS use inheriting state mutexes, but neither lock wait time nor copy time
has a deployment-independent realtime bound. Budget capacity and lock hold time
for the target workload. Total object storage includes the payload, RuntimeMutex,
a size field and alignment padding.

Use `rtx::SharedMemory` for Linux cross-process channels. A Mailbox object is not
an inter-process ABI and must not be placed in an mmap region as a substitute.
The gRPC mailbox service currently exposes registered SharedMemory channels;
bridging a task-local channel to an external transport remains application code.

Tests cover empty/clear values, binary bytes, bounds rejection and two concurrent
readers during 10000 writes. The dispatch allocation regression includes 10000
writes and reads with no ordinary operator-new calls after configuration. The
installed CMake consumer compiles and uses the public header. The Cortex-M4
reference passes a sequence/complement payload from the low task to the high
preempting task and verifies complete message integrity.
