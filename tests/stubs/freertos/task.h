#pragma once
#include <stdint.h>

// The host test drives persistence synchronously via pb_policy_persist_pending(),
// so task creation is a no-op that reports success and notifications are dropped.
// Nothing under test depends on the worker actually running.
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1

static inline int xTaskCreate(TaskFunction_t fn, const char *name,
                              uint32_t stack, void *arg, unsigned prio,
                              TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = (void *)1;
    return pdPASS;
}

static inline void xTaskNotifyGive(TaskHandle_t task) { (void)task; }

static inline uint32_t ulTaskNotifyTake(int clear, uint32_t wait)
{
    (void)clear; (void)wait;
    return 0;
}
