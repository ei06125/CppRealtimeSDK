// Private mapping header preceding shared payload bytes.
// Changing this native layout requires rebuilding and coordinating all participating processes.
#pragma once
#include <cstddef>
#include <pthread.h>
namespace rtx::detail {
// Internal Linux ABI, shared with process-death fault injection tests.
// Not an installed or public interface; peers must use the same library ABI.
struct SharedMemoryHeader { pthread_mutex_t mutex; std::size_t length; };
}
