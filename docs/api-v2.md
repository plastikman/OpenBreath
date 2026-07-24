# DragonBreath API v2

API v2 is the sole device control contract. It replaces the alpha `/status`,
`/target`, `/heartbeat`, and `/reset` routes; clients must not probe for or fall
back to those routes.

All bodies are JSON. Read-only routes are open on the trusted LAN. Mutating
routes require `X-DragonBreath-Auth`; the device does not enable CORS.

## Read-only routes

- `GET /api/v2/info` — API version, stable device ID, per-boot ID, firmware and
  capabilities.
- `GET /api/v2/state` — full authoritative snapshot. Reading never refreshes a
  lease or otherwise changes device state.
- `GET /api/v2/events` — Server-Sent Events. A `state` event is sent on connect
  and when `state_revision` changes. A full `telemetry` snapshot is sent every
  two seconds between transitions. At most two streams are held concurrently.
- `GET /api/v2/health` — uptime, heap, Wi-Fi RSSI/channel and SSE client count.
- `GET /api/v2/logs` — the in-memory event ring (newest first) as
  `{"api_version":2,"count":N,"entries":[{"ms":<millis since boot>,"text":"…"}]}`.
  Open (read-only, no side effects).

The complete snapshot, not locally remembered intent, is the source of truth:

```json
{
  "api_version": 2,
  "device_id": "dragonbreath-a1b2c3",
  "boot_id": "4f7f00004f7f00004f7f00004f7f0000",
  "firmware": "0.3.0",
  "state_revision": 12,
  "mode": "power_on",
  "source": "klipper",
  "target": {
    "requested_c": 45.0,
    "effective_c": 45.0,
    "maximum_c": 70.0
  },
  "config": {
    "max": 70.0,
    "max_abs": 70.0,
    "comms_ms": 300000
  },
  "heater": {"demand": true, "output": true},
  "fan": {
    "requested_percent": 100,
    "effective_percent": 100,
    "reason": "heater"
  },
  "sensors": {
    "chamber": {"temperature_c": 36.2, "status": "ok"},
    "ptc": {"temperature_c": 73.1, "status": "ok"}
  },
  "environment": {
    "moonraker_connected": true,
    "bed_temperature_c": 100.0,
    "auto_engaged": false,
    "auto_bed_threshold_c": 0.0
  },
  "drying": {"active": false, "remaining_seconds": 0},
  "params": {
    "manual_target_c": 50.0,
    "auto_target_c": 60.0,
    "auto_bed_threshold_c": 100.0,
    "dry_target_c": 60.0,
    "dry_hours": 12
  },
  "control": {
    "lease": {
      "active": true,
      "owner": "u1-klippy",
      "expires_in_ms": 299500
    }
  },
  "safety": {
    "fault_latched": false,
    "inhibited": false,
    "reason": null
  }
}
```

Temperatures are JSON `null` when their sensor status is not `ok`. Public
state and SSE snapshots intentionally omit the raw lease ID; only the
authenticated response that creates a POWER_ON lease returns it.
`state_revision` changes only for control, mode, lease, fault, and safety
transitions; ordinary temperature samples and successful heartbeats do not
increment it.

`params` reports the *remembered* mode parameters — the values most recently
accepted for each mode, used to pre-fill the UI and to re-arm a mode when the
caller supplies no values of its own (the front-panel buttons). These are the
only policy values that survive a reboot; the active mode, target, deadline, and
lease deliberately do not. Updating them is a side effect of an accepted
`power_on` / `auto` / `drying_start` command, and the value recorded is always
the clamped one the device actually applied — there is no separate write path.

## Commands

`POST /api/v2/command` takes a request ID, the revision observed by the caller,
actor identity, and one command:

```json
{
  "api_version": 2,
  "request_id": "client-generated-unique-id",
  "expected_revision": 12,
  "actor": {"kind": "klipper", "id": "u1-klippy"},
  "command": {"name": "power_on", "target_c": 45}
}
```

Supported commands:

- `{"name":"off"}` — unconditional. A missing or stale revision never blocks a
  safer OFF.
- `{"name":"power_on","target_c":45}` — creates a new device-issued remote
  lease and invalidates any previous lease.
- `{"name":"auto","target_c":45,"bed_threshold_c":60}` — device policy follows
  the observed Moonraker bed state.
- `{"name":"drying_start","target_c":50,"hours":4}` — bounded to 1–12 hours.
- `{"name":"drying_stop"}` — unconditional safer stop.
- `{"name":"clear_fault"}` — clears a recoverable fault only at the expected
  revision. Permanent inhibits require correcting the condition and rebooting.

`actor.kind` is `web` or `klipper`; `actor.id` is at most 31 characters.
Successful responses contain `ok`, the `request_id`, and the complete new
`state`. A successful `power_on` response also contains a top-level `lease_id`.
Clients must take a POWER_ON lease only from that authenticated response, never
from public state/events and never invent or reuse one.

The device keeps a small replay cache keyed by `(actor.id, request_id)`.
Repeating the same request while its resulting revision is still current
returns the original response. Reusing it after state changed returns
`revision_conflict`. Safer `off` and `drying_stop` commands are never cached and
always execute.

## Lease heartbeat

```http
POST /api/v2/heartbeat
X-DragonBreath-Auth: ...
Content-Type: application/json

{"api_version":2,"lease_id":"exact 32-character device-issued ID"}
```

Only the currently active lease is accepted. A stale, superseded, expired, or
invented ID returns HTTP 409 `stale_lease`. Heartbeats extend the lease deadline
but do not change `state_revision`. Passive dashboards and reconnected clients
must not heartbeat a lease they did not receive from their own accepted
POWER_ON response.

## Errors

Errors are machine-readable and normally include the current authoritative
state:

```json
{
  "ok": false,
  "error": "revision_conflict",
  "message": "command rejected by authoritative policy",
  "request_id": "...",
  "state": {}
}
```

Defined error codes include `invalid_command`, `unsupported_api_version`,
`auth_failed`, `revision_conflict`, `stale_lease`, `fault_latched`,
`inhibited`, and `busy`.

## Other endpoints (not versioned)

These predate/sit beside the versioned control surface and are stable:

- `GET /settings` — the runtime-configurable safety settings plus their bounds:
  `{"max","max_min","max_abs","comms_ms","comms_ms_min","comms_ms_max","leds_enabled"}`.
  Open (read-only, no side effects). `leds_enabled` is the status-LED master
  enable (bool, default `true`).
- `POST /settings?max=<°C>&comms_ms=<ms>&leds_enabled=<0|1>` — update any subset.
  Auth-gated (`X-DragonBreath-Auth`). Safety values are clamped to the safe
  envelope server-side (the max-target ceiling can never exceed the absolute cap,
  the comms watchdog stays within its min/max) and the clamped effective values
  are echoed back. `leds_enabled=0` turns the four front-panel LEDs off (monitoring
  and safety are unaffected). The fixed 105 °C PTC / 85 °C chamber cutoffs are
  **not** settable.
- `GET /api/v2/calibration` — the per-channel temperature calibration offsets plus
  their bounds and the live readings:
  `{"api_version":2,"chamber_offset_c","ptc_offset_c","min":-5,"max":5,"chamber_c","ptc_c","chamber_raw_c","ptc_raw_c"}`.
  Open (read-only). `*_c` are the calibrated readings; `*_raw_c` are the
  pre-offset readings.
- `POST /api/v2/calibration` — auth-gated. Body
  `{"chamber_offset_c":<°C>,"ptc_offset_c":<°C>}` (either optional; at least one
  required). Each offset is **hard-clamped to ±5 °C** and persisted; the clamped
  effective offsets are echoed back. The offset is applied to the reading used by
  the display, control, AUTO **and** the over-temp cutoffs alike — so the fixed
  cutoffs shift by at most 5 °C and calibration can never disable a fault
  (see `docs/SAFETY.md`).
- `POST /api/v2/restart` — authenticated soft reboot. **Refused while the heater
  is armed/on** (HTTP 409 `heater_active`). Returns `{"ok":true}` then reboots
  once the response has flushed.
- `POST /api/v2/factory-reset` — authenticated. **Refused while heating.** Requires
  explicit confirmation `confirm=factory-reset` (query or urlencoded body) or it
  returns HTTP 400 `confirmation_required`. On confirm it erases the whole
  `app_nvs` namespace (Wi-Fi credentials, Moonraker host, control token, safety
  limits), returns `{"ok":true}`, and reboots — the device comes back up in AP
  provisioning mode.
- `POST /api/v2/token` — authenticated. Body `{"token":"<=64 chars>"}` sets the
  control token; `{"token":""}` / `{"token":null}` / `{}` clears it. The secret is
  never echoed; the response is `{"ok":true,"token_set":<bool>}`. Once a token is
  set, every mutating request must carry it in `X-DragonBreath-Auth`.
- `POST /update` — authenticated DragonBreath application-image OTA. Streams the
  `.bin` into the inactive slot, verifies it (image checksum + `dragonbreath`
  project identity — foreign images are rejected), reports the SHA-256 it
  computed, sets it as the boot slot, and reboots. **Refused while the heater is
  armed/on.** A bad image that crashes before the app marks itself healthy rolls
  back on the next boot.

The device also serves the captive-portal / dashboard HTML on `GET /` (and
`/setup`, `/fw`, `/scan.json`, `POST /save`, `/rescan`) — the human web UI, not
part of the machine control contract.
