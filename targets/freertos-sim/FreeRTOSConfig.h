// Kernel settings for hosted FreeRTOS behavior and failure-injection tests.
// The simulator uses large test stacks and 64-bit ticks; its stack storage is
// not the pthread stack.
#pragma once
#include <assert.h>
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configCPU_CLOCK_HZ 100000000
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 4096
#define configMAX_TASK_NAME_LEN 24
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_64_BITS
#define configIDLE_SHOULD_YIELD 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 0
#define configUSE_TIMERS 0
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 0
#define configASSERT(x) assert(x)
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
