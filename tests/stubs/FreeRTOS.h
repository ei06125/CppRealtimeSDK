// Declarations-only stand-in so the FreeRTOS backend is compile-checked without
// the kernel.
#pragma once
#include <stdint.h>
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 8
#define portMAX_DELAY 0xffffffffu
#define pdPASS 1
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef uint16_t configSTACK_DEPTH_TYPE;
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
