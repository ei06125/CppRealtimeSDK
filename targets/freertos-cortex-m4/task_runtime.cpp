// Bind the GNU ARM exception-global ABI entry points to the currently selected
// task runtime. Use boot fallback state before scheduling; do not share
// caught-exception chains between tasks.
#include "task_runtime.h"

#include <cxxabi.h>
#if !defined(__ARM_EABI__) || !defined(__arm__) || _GLIBCXX_RELEASE < 13 || \
    _GLIBCXX_RELEASE > 14
#error "Verify and adapt the GCC ARM exception-state ABI for this toolchain"
#endif
static_assert(sizeof(RtxExceptionState) == 12 &&
              alignof(RtxExceptionState) == 4);
extern "C" {
RtxTaskRuntime* volatile rtx_current_runtime = nullptr;
}
namespace {
RtxExceptionState boot_exceptions{};
}
namespace __cxxabiv1 {
// Bare-metal libsupc++ otherwise returns one global state for all tasks. These
// definitions satisfy its ABI symbols before the archive's eh_globals object.
extern "C" __cxa_eh_globals* __cxa_get_globals() noexcept {
  auto* current = rtx_current_runtime;
  return reinterpret_cast<__cxa_eh_globals*>(current ? &current->exceptions
                                                     : &boot_exceptions);
}
extern "C" __cxa_eh_globals* __cxa_get_globals_fast() noexcept {
  return __cxa_get_globals();
}
}  // namespace __cxxabiv1
