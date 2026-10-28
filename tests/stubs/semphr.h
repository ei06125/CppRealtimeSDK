#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void vSemaphoreDelete(SemaphoreHandle_t);
