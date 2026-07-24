// SPDX-License-Identifier: MIT
// pb_policy — the sole owner of DragonBreath control state.
//
// Network transports, the dashboard, Moonraker integration, and physical
// buttons submit commands here.  They never write pb_heater or pb_fan directly.
// The policy owns mode transitions, targets, controller leases, deadlines, and
// the canonical snapshot consumed by API v2.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pb_ntc.h"

#define PB_POLICY_REVISION_ANY UINT32_MAX
#define PB_POLICY_LEASE_ID_LEN 32
#define PB_POLICY_OWNER_LEN    31

// Remote POWER_ON leases follow pb_heater's runtime-configurable comms deadman,
// so the advertised lease runway and the hardware watchdog cannot disagree.
#define PB_POLICY_LOCAL_POWER_MAX_MS  (12U * 60U * 60U * 1000U)

typedef enum {
    PB_MODE_OFF = 0,
    PB_MODE_POWER_ON,
    PB_MODE_AUTO,
    PB_MODE_DRYING,
} pb_mode_t;

typedef enum {
    PB_SOURCE_BOOT = 0,
    PB_SOURCE_WEB,
    PB_SOURCE_KLIPPER,
    PB_SOURCE_BUTTON,
    PB_SOURCE_SAFETY,
    PB_SOURCE_WATCHDOG,
} pb_source_t;

typedef enum {
    PB_POLICY_OK = 0,
    PB_POLICY_INVALID,
    PB_POLICY_REVISION_CONFLICT,
    PB_POLICY_FAULT_LATCHED,
    PB_POLICY_INHIBITED,
    PB_POLICY_STALE_LEASE,
} pb_policy_result_t;

typedef struct {
    char id[PB_POLICY_LEASE_ID_LEN + 1];
} pb_policy_lease_t;

// Remembered mode parameters: the values a mode is re-armed with when the
// caller does not supply its own (front-panel buttons) and the values the UI
// pre-fills from.  These are the ONLY policy state that survives a reboot --
// never a mode, target, deadline, heating flag, or lease.  See
// pb_policy_load_params().
typedef struct {
    float manual_target_c;        // last accepted POWER_ON target
    float auto_target_c;          // last accepted AUTO target
    float auto_bed_threshold_c;   // last accepted AUTO bed threshold
    float dry_target_c;           // last accepted drying target
    uint8_t dry_hours;            // last accepted drying duration
} pb_policy_params_t;

typedef struct {
    uint32_t state_revision;
    pb_mode_t mode;
    pb_source_t source;

    float requested_target_c;
    float effective_target_c;
    bool heater_demand;
    bool heater_output;

    uint8_t requested_fan_percent;
    uint8_t effective_fan_percent;
    bool thermal_purge;

    float chamber_c;
    float ptc_c;
    pb_ntc_status_t chamber_status;
    pb_ntc_status_t ptc_status;

    bool moonraker_connected;
    float bed_c;
    bool auto_engaged;
    float auto_bed_threshold_c;

    bool drying;
    uint32_t drying_remaining_s;

    bool lease_active;
    char lease_id[PB_POLICY_LEASE_ID_LEN + 1];
    char lease_owner[PB_POLICY_OWNER_LEN + 1];
    uint32_t lease_expires_ms;

    bool fault_latched;
    bool inhibited;
    char fault_reason[64];
} pb_policy_snapshot_t;

esp_err_t pb_policy_init(void);

// Load the remembered mode parameters from NVS (namespace app_nvs) and start the
// persistence worker.  MUST be called AFTER nvs_init() -- pb_policy_init() only
// installs conservative defaults, since NVS is not up that early.  Values are
// clamped on read, and loading NEVER changes the mode or arms a target: the
// device still boots OFF.
void pb_policy_load_params(void);

void pb_policy_get_params(pb_policy_params_t *out);

// Write any pending parameter change to NVS.  Returns true if a commit was
// attempted.  The persistence worker calls this in a loop; it is exposed so the
// host test can drive persistence synchronously without a scheduler.
bool pb_policy_persist_pending(void);

// Remote WEB/KLIPPER POWER_ON commands receive a device-issued lease.  A lease
// is RAM-only, changes on every accepted heat command, and is invalidated by
// OFF, another mode command, safety/watchdog action, or reboot.
pb_policy_result_t pb_policy_set_power_on(
    float target_c,
    pb_source_t source,
    const char *owner,
    uint32_t expected_revision,
    pb_policy_lease_t *lease_out);

pb_policy_result_t pb_policy_set_auto(
    float target_c,
    float bed_threshold_c,
    pb_source_t source,
    uint32_t expected_revision);

pb_policy_result_t pb_policy_start_drying(
    float target_c,
    uint8_t hours,
    pb_source_t source,
    uint32_t expected_revision);

// OFF is deliberately unconditional: stale state must never prevent a caller
// from making the device safer.
void pb_policy_set_mode_off(pb_source_t source);
void pb_policy_stop_drying(pb_source_t source);

// Update printer environment used by AUTO.  This is observer input, not a
// control command, and therefore never creates or refreshes a control lease.
void pb_policy_set_env(float bed_c, bool moonraker_connected);

// Refresh exactly the active lease.  A stale/superseded lease cannot keep heat
// alive.
pb_policy_result_t pb_policy_heartbeat(const pb_policy_lease_t *lease);

// Clearing a fault changes authoritative state and therefore requires the
// caller's observed revision. Unlike OFF, stale state must not clear a newer
// fault or safety transition.
pb_policy_result_t pb_policy_clear_fault(
    pb_source_t source,
    uint32_t expected_revision);

// Periodic control tick (call at ~1-2 Hz).  Computes the effective target,
// applies the heater/fan outputs, enforces deadlines, and synchronizes safety
// trips back into the authoritative state.
void pb_policy_tick(void);

void pb_policy_get_snapshot(pb_policy_snapshot_t *out);
pb_mode_t pb_policy_get_mode(void);
const char *pb_policy_mode_str(pb_mode_t mode);
const char *pb_policy_source_str(pb_source_t source);
const char *pb_policy_result_str(pb_policy_result_t result);
