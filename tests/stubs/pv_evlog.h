#pragma once
// Minimal event-log surface used by pb_policy. The host test provides the
// definition of pv_evlog_add so it can count/inspect button events.
void pv_evlog_init(void);
void pv_evlog_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
