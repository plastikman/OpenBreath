#pragma once

// Moonraker WebSocket client. Reads its config from NVS at boot, connects to
// `ws://<host>:<port>/websocket`, subscribes to a fixed set of printer objects,
// and caches the latest values for the vent policy to consult. Reconnects on
// disconnect. Idle (no-op) if no config is saved.

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PB_MK_DISABLED,       // no config saved
    PB_MK_DISCONNECTED,   // config present, not currently connected
    PB_MK_CONNECTING,
    PB_MK_CONNECTED,      // websocket up, subscribe in flight
    PB_MK_SUBSCRIBED,     // receiving status updates
} pb_moonraker_state_t;

// Six-state printer model mirroring what stock reads off Bambu MQTT. Derived
// from a combination of print_stats.state and webhooks.state so a hard
// Klipper error shows as ERROR rather than a stale "printing".
typedef enum {
    PB_PRINTER_UNKNOWN = 0,
    PB_PRINTER_IDLE,       // standby / ready, no active print
    PB_PRINTER_PREPARING,  // print started but early (progress < ~1%)
    PB_PRINTER_PRINTING,   // actively printing
    PB_PRINTER_PAUSED,
    PB_PRINTER_COMPLETE,   // last print finished cleanly
    PB_PRINTER_ERROR,      // print_stats "error"/"cancelled", or Klipper shutdown
} pb_printer_state_t;

const char *pb_printer_state_str(pb_printer_state_t s);

typedef struct {
    char     host[64];    // hostname or IP; empty string = unconfigured
    uint16_t port;        // defaults to 7125 if 0
    char     api_key[65]; // optional; empty if unused
} pb_moonraker_config_t;

typedef struct {
    pb_moonraker_state_t state;
    pb_printer_state_t   printer;      // six-state model
    bool                 printing;     // convenience: printer == PRINTING
    float                bed_temp;     // heater_bed.temperature (°C)
    float                bed_target;   // heater_bed.target (°C)
    float                extruder_temp;// extruder.temperature (°C, informational)
    float                chamber_temp; // heater_generic chamber, if present; NaN if absent
    float                progress;     // 0..1 from virtual_sdcard.progress
    char                 filename[64]; // print_stats.filename
    char                 material[16]; // from save_variables.material or gcode metadata; upper-case
} pb_moonraker_status_t;

esp_err_t pb_moonraker_start(void);

// Overwrite the saved config. If the client is running, it reconnects with
// the new settings.
esp_err_t pb_moonraker_set_config(const pb_moonraker_config_t *cfg);

esp_err_t pb_moonraker_get_config(pb_moonraker_config_t *out);
esp_err_t pb_moonraker_get_status(pb_moonraker_status_t *out);

// Wipe saved Moonraker config. Used for factory reset.
esp_err_t pb_moonraker_clear_config(void);
