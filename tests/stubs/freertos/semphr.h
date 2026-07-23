#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t wait)
{
    (void)sem;
    (void)wait;
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return 1;
}
