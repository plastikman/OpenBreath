#pragma once

// Small in-memory ring buffer of human-readable event lines so the portal can
// show what's been happening without the user needing serial access. Nothing
// clever: fixed slot count, oldest overwritten first, snapshot copied under a
// lock. Add is safe from any task including ISRs-that-can-take-a-mutex (which
// means: not real ISRs — this is a coarse-grained mutex, not a spinlock).

#include <stdint.h>
#include <stddef.h>

#define PB_EVLOG_MAX_ENTRIES  64
#define PB_EVLOG_TEXT_BYTES   96

typedef struct {
    uint32_t ms;                    // millis since boot
    char     text[PB_EVLOG_TEXT_BYTES];
} pb_evlog_entry_t;

// Initialise the log. Safe to call before any producer. If already initialised,
// no-op.
void pb_evlog_init(void);

// Append a printf-formatted line. Truncated to PB_EVLOG_TEXT_BYTES-1. Safe to
// call from any task once pb_evlog_init has been called; also safe to call
// before init (silently drops the line — no crash).
void pb_evlog_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copy up to `max` most-recent entries into `out`, newest first. Returns how
// many were copied.
size_t pb_evlog_snapshot(pb_evlog_entry_t *out, size_t max);
