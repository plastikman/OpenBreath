#include "pb_evlog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static SemaphoreHandle_t s_lock = NULL;
static pb_evlog_entry_t  s_buf[PB_EVLOG_MAX_ENTRIES];
static size_t            s_head = 0;   // next write slot
static size_t            s_count = 0;  // number of valid entries (<= MAX)

void pb_evlog_init(void)
{
    if (s_lock != NULL) return;
    s_lock = xSemaphoreCreateMutex();
    // If mutex creation fails we just stay in the "not initialised" state and
    // pb_evlog_add becomes a no-op — good enough for a diagnostic aid.
}

void pb_evlog_add(const char *fmt, ...)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;

    pb_evlog_entry_t *e = &s_buf[s_head];
    e->ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);

    s_head = (s_head + 1) % PB_EVLOG_MAX_ENTRIES;
    if (s_count < PB_EVLOG_MAX_ENTRIES) s_count++;

    xSemaphoreGive(s_lock);
}

size_t pb_evlog_snapshot(pb_evlog_entry_t *out, size_t max)
{
    if (out == NULL || max == 0) return 0;
    if (s_lock == NULL) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    size_t want = (max < s_count) ? max : s_count;
    // Copy newest-first: walk backward from s_head.
    size_t idx = (s_head == 0) ? (PB_EVLOG_MAX_ENTRIES - 1) : (s_head - 1);
    for (size_t i = 0; i < want; ++i) {
        out[i] = s_buf[idx];
        idx = (idx == 0) ? (PB_EVLOG_MAX_ENTRIES - 1) : (idx - 1);
    }

    xSemaphoreGive(s_lock);
    return want;
}
