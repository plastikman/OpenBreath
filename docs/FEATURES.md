# DragonBreath — feature set

Current as of **v0.3.0**. Open ESP-IDF firmware for the BIGTREETECH Panda Breath
(ESP32-C3) chamber heater, replacing the stock cloud integration with
Moonraker/Klipper + local web control.

See [`OEM_PARITY.md`](OEM_PARITY.md) for the explicit implemented/planned/
intentionally-changed feature matrix.

## Control modes

The device runs an authoritative on-device state machine (`pb_policy`) — the
single owner of mode/target/lease state. It boots **OFF** and never auto-resumes
heat after a reboot.

| Mode | What it does | How it's triggered |
|---|---|---|
| **Off** | Heater off. | Boot default; `off` command (always accepted). |
| **Manual / Power-On** | Holds the chamber at a set target. Remote sessions take a device-issued lease and must heartbeat to stay alive. | `power_on` command (web "Manual heat", or Klipper `M141`/`SET_HEATER_TEMPERATURE`). |
| **Automatic (follow bed)** | Watches the printer's bed temperature (via Moonraker) and heats the chamber to the target whenever the bed is at/above a threshold; disengages below threshold − 3 °C. Autonomous (no host heartbeat), requires the Moonraker link. | `auto` command / web "Follow printer bed". |
| **Filament drying** | Holds the chamber at a target for a bounded duration (1–12 h), then auto-off. | `drying_start` / web "Filament drying". |

Commands are **revision-aware** (a stale writer can't clobber newer state) and
**idempotent** (request-ID replay cache); `off`/`drying_stop` are always accepted
and never cached.

## Safety

Defense-in-depth — see [`SAFETY.md`](SAFETY.md) for the full model.

- **Hardware backstops (independent of firmware):** a bonded thermal cutoff in
  the PTC mains lead + PTC self-limiting physics bound the worst case even with a
  welded SSR or a firmware bug.
- **Firmware soft cutoffs:** 105 °C PTC over-temp and 85 °C chamber over-temp
  (both **fixed, non-configurable**); sensor-fault fail-closed (a bad thermistor
  latches the heater off).
- **Comms-loss watchdog:** if the controlling client goes silent while heating,
  the heater latches off. Runtime-configurable within **10 s – 5 min** (never
  disabled or extended past 5 min).
- **Boot-OFF / no auto-resume**, single-writer SSR, latched faults require an
  explicit clear, and a permanent inhibit if the control-loop watchdog can't be
  armed (reboot-only).

## Configurable settings (persisted to NVS)

- **Max-target ceiling** — default 70 °C, hard-capped at 70 °C. No API/UI path
  can command heat above it.
- **Comms-watchdog timeout** — default 5 min, clamped to 10 s – 5 min.

Exposed via `GET`/`POST /settings` and the web UI's Advanced/Safety card. The
fixed over-temp cutoffs are not settable.

## Status LEDs

Four front-panel LEDs, matching the stock panel (direct active-high GPIO):

- **Power** (GPIO21) and **On** (GPIO5) — solid while heating, blink on a latched
  fault, off at idle. Because GPIO21 is also the console-TX pin, the Power LED is
  driven only in **release builds** (`CONFIG_PB_POWER_LED`); development builds
  keep the serial console and leave it alone.
- **Auto** (GPIO6) / **Dry** (GPIO4) — mode indicators.

## Fan

AC blower switched by a TRIAC held **on/off** (never phase-angle PWM'd), synced
to the mains zero-cross detector. Airflow follows the heater; **post-print
cooldown** keeps the blower running after a heating session until the chamber
falls below 40 °C (gated on a heat-this-session flag, so it never auto-starts on
temperature alone).

## Control API (port 80)

Versioned JSON API — full contract in [`api-v2.md`](api-v2.md):

- `GET /api/v2/info` · `state` · `health` — identity, authoritative snapshot,
  diagnostics (no side effects).
- `GET /api/v2/events` — Server-Sent Events push (state transitions + telemetry);
  replaces polling.
- `POST /api/v2/command` · `heartbeat` — auth-gated, revision-aware control +
  exact-lease heartbeats.
- `GET`/`POST /settings` — runtime safety settings.
- `POST /update` — authenticated app-image OTA (rejects foreign images; refused
  while heating).

**Security:** every mutating route requires an `X-DragonBreath-Auth` header
(CSRF hardening for a trusted LAN; optional NVS control token for real per-client
auth). CORS is never enabled; reads never energize the heater or feed the
watchdog.

## Web UI (served by the device)

- **Live dashboard** — SSE-driven status (chamber/element temps, mode, target,
  heating, fan, controller/link), Manual/Automatic/Filament-drying/Advanced
  cards.
- **Captive provisioning** — Wi-Fi + Moonraker setup portal (AP mode).
- **OTA page** — upload + flash a DragonBreath image; on official builds it also
  checks GitHub for a newer release and shows a verified download link.
- mDNS: reachable at **`dragonbreath.local`**.

## Klipper / Moonraker integration

[`dragonbreath-klipper`](https://github.com/plastikman/dragonbreath-klipper) is
the host-side helper (Klipper `extras`): exposes the chamber as a heater for
`M141` / `M191` and Fluidd, speaks API v2 (SSE + exact-lease heartbeats,
reactor-safe). Deploy lockstep with firmware ≥ v0.3.0.

## Platform / release

- ESP32-C3-MINI-1; shares the [OpenVent](https://github.com/justinh-rahb/OpenVent)
  core (Wi-Fi, captive portal, Moonraker client) via a submodule.
- **OTA with rollback** (bad image reverts on next boot).
- **Reproducible CI releases** — tag `v*` → factory image, OTA image, install
  bundle, `manifest.json` (source SHA, ESP-IDF version, per-artifact SHA-256),
  and `SHA256SUMS.txt`.
- Hardware reverse-engineered + validated on a **V1.0.1** board (verify the
  pinout on other revisions before flashing).
