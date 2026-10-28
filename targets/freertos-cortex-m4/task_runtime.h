// Define the per-task newlib and GNU ARM exception state carried by FreeRTOS
// context switches. The exception-state layout is ABI-specific and must be
// revalidated with a different toolchain.
#pragma once
#include <reent.h>
// GCC ARM EHABI state: caught exception, uncaught count, propagation chain.
// This reference runtime is specific to the documented GNU ARM toolchain ABI.
typedef struct {
  void* caught;
  unsigned uncaught;
  void* propagating;
} RtxExceptionState;
typedef struct {
  struct _reent libc;
  RtxExceptionState exceptions;
} RtxTaskRuntime;
#ifdef __cplusplus
extern "C" {
#endif
extern RtxTaskRuntime* volatile rtx_current_runtime;
#ifdef __cplusplus
}
#endif
#define configTLS_BLOCK_TYPE RtxTaskRuntime
#define configINIT_TLS_BLOCK(block, stack) \
  do {                                     \
    _REENT_INIT_PTR(&(block).libc);        \
    (block).exceptions.caught = 0;         \
    (block).exceptions.uncaught = 0;       \
    (block).exceptions.propagating = 0;    \
  } while (0)
#define configSET_TLS_BLOCK(block)  \
  do {                              \
    _impure_ptr = &(block).libc;    \
    rtx_current_runtime = &(block); \
  } while (0)
#define configDEINIT_TLS_BLOCK(block)                  \
  do {                                                 \
    configASSERT((block).exceptions.caught == 0 &&     \
                 (block).exceptions.uncaught == 0 &&   \
                 (block).exceptions.propagating == 0); \
    _reclaim_reent(&(block).libc);                     \
  } while (0)
