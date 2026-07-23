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
