#pragma once
typedef enum {
    PB_NTC_OK = 0,
    PB_NTC_OPEN,
    PB_NTC_SHORT,
    PB_NTC_UNINIT,
} pb_ntc_status_t;
typedef enum {
    PB_NTC_CHAMBER = 0,
    PB_NTC_PTC,
} pb_ntc_channel_t;
pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t channel);
float pb_ntc_smoothed_c(pb_ntc_channel_t channel);
