// Compile-only FreeRTOS declarations so the backend can be syntax-checked without a kernel.
#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
int xTaskCreate(TaskFunction_t, const char*, configSTACK_DEPTH_TYPE, void*, unsigned, TaskHandle_t*);
