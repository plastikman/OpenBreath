# Changelog

All notable changes to the **DragonBreath** firmware are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are the
firmware release tags (`vX.Y.Z`). The release workflow pulls the matching section
below into the GitHub Release notes.

## [Unreleased]

### Added
- **Front-panel buttons (`pb_buttons`).** All four buttons (Power, Auto, On, Dry)
  are polled at 10 ms with 20 ms debounce and short/long-press detection. A short
  press toggles that button's labeled mode, arming it from the remembered
  parameters; a 2 s long-press latches a **panic-off**. A long-press on Power
  while a fault is latched attempts a fault clear instead. Every button action is
  attributed to the panel, invalidates any remote control lease, and appears in
  both the dashboard and Klipper. A button held at power-on (or a shorted line)
  is ignored until it releases — Power/Auto/Dry are ESP32-C3 strapping pins, so
  **do not hold a front-panel button while the board boots**. The debounce /
  long-press state machine is split into a dependency-free `pb_buttons_sm` unit
  and host-tested directly.
- **Long-press panic-off.** `pb_heater_request_panic_off()` latches the heater
  off from any task without touching the SSR GPIO, and the policy drives the full
  OFF transition (attributed to the button, lease invalidated) then wakes the
  control task by notification so the SSR drops on the very next scheduling rather
  than at the next periodic tick. It is **not** a safety-rated emergency stop —
  see [`docs/SAFETY.md`](docs/SAFETY.md).
- **Remembered mode parameters (persisted).** The last accepted manual target,
  automatic target and bed threshold, and drying target and duration are now
  stored in NVS and reported as `params` in `GET /api/v2/state`, so the UI
  pre-fills from the device and a mode can be re-armed without re-entering
  values. Closes a gap left by v0.3.0, which documented this persistence but
  never implemented it. Writes are serialized through a single worker task and
  record the clamped value the device actually applied. Parameters remain the
  **only** policy state that survives a reboot — the active mode, target,
  deadline, and lease still do not, so the device always boots OFF.
- **Serial hardware-in-the-loop harness (`pb_hil` / `tools/hil.py`).** A
  line-delimited JSON console for injecting chamber/PTC readings, sensor faults,
  printer environment, and zero-cross events, and for reading back heater demand,
  fan state, LEDs, mode, lease, and fault state. Ships with an isolated ESP32-C3
  dev-board target whose mains GPIO is **compiled out**, so it is structurally
  incapable of energizing Panda hardware, plus scripted scenarios, console
  capture, and JSON pass/fail reports. A non-heating UART profile for the real
  Panda is documented as the pre-release qualification gate. (#14)
- **OEM parity matrix (`docs/OEM_PARITY.md`).** Tracks every user-visible stock
  Panda Breath behavior as implemented, partial, planned, intentionally changed,
  intentionally omitted, or unverified — so deliberate deviations are
  distinguishable from gaps. (#12)

### Changed
- **Front-panel LEDs now show the active mode.** All four outputs are driven from
  the authoritative policy snapshot instead of Power and On duplicating a single
  "heating" signal: Power is solid whenever the device is up and blinks on fault,
  while On, Auto, and Dry each light for their own mode. Auto slow-blinks when
  armed but not engaged (no Moonraker link, or bed below the threshold), so the
  panel distinguishes "waiting" from "heating". Power remains release-only —
  GPIO21 is also the serial console TX.

### Fixed
- **Automatic and drying controls work from the dashboard.** v0.3.0 shipped both
  modes in the state machine and API but the UI could not reliably drive them.
  The cards now submit correctly, their input bounds match the policy's own
  limits, and automatic status refreshes from live state rather than the value
  last typed. (#13)
- **`docs/HARDWARE.md` GPIO map corrected.** It still described buttons on GPIO7
  and GPIO0 — those are the zero-cross detector and the chamber NTC. The table now
  matches the bench-probed map already in `pb_board.h` (buttons on 9/8/10/2, Power
  LED on 21) and documents the strapping-pin caveat.

## [0.3.0] - 2026-07-23

Iteration-2 core: an authoritative device-side control state machine, a
versioned API, configurable safety settings, and real front-panel status LEDs.

### Added
- **Configurable safety settings (persisted).** Runtime-settable max-target
  ceiling (default 70 °C, hard-capped at 70) and comms-watchdog timeout
  (default 5 min, clamped to 10 s–5 min), stored in NVS and exposed via
  `GET`/`POST /settings` and an Advanced/Safety card in the web UI. The fixed
  105 °C PTC / 85 °C chamber cutoffs remain non-configurable. (#10)
- **Real status LEDs.** The three mode LEDs (Auto/On/Dry) plus the **Power LED**
  (GPIO21) are now driven to match the stock panel: Power/On solid while heating,
  blink on a latched fault, off at idle. Because GPIO21 is the console-TX pin, the
  Power LED is enabled only in release builds (`CONFIG_PB_POWER_LED`, set via
  `sdkconfig.release`); dev builds keep the serial console. (#10)
- **Authoritative control state machine (`pb_policy`).** A single device-side
  owner of mode/target/lease state (Off / Power-On / Auto / Dry), with
  lease-based remote ownership, revision-aware commands, and a boot-OFF /
  no-auto-resume safety posture. (#9)
- **API v2 (`/api/v2/*`).** Snapshot-authoritative `state`/`info`/`health`, an
  SSE `events` stream (push instead of polling), and auth-gated `command` /
  `heartbeat` with request-ID idempotency and exact-lease heartbeats. (#11)

### Changed
- **BREAKING (alpha API):** the alpha routes (`/status`, `/target`, `/heartbeat`,
  `/reset`) are removed in favor of API v2. Requires the matching
  **dragonbreath-klipper v2 helper** — flash firmware ≥ v0.3.0, then restart
  klippy. Version mismatches fail safe (the chamber heater simply doesn't engage).

## [0.2.0] - 2026-07-23

### Added
- **In-UI update notification** — on official (tagged) builds, the device `/fw`
  page checks GitHub for a newer release and shows a download link + expected
  SHA-256; you verify and flash it via the existing uploader (fully browser-side).
- **Reproducible release pipeline** — pushing a `v*` tag builds in CI (pinned
  ESP-IDF v5.3.5) and publishes a GitHub Release with a single-file
  `-factory.bin` (first install, flash @ 0x0), the `.bin` application image (OTA),
  a complete `-install-bundle.zip` (flasher + components + `FLASHING.txt`), a
  `manifest.json` (source SHA, ESP-IDF version, submodule, per-artifact SHA-256),
  and `SHA256SUMS.txt`.
- **Post-print fan cooldown** — after a print, the blower keeps running until the
  chamber cools below 40 °C, then stops. Gated on a heat-this-session flag, so it
  never auto-starts on temperature alone (a reboot-while-hot leaves the fan off).

### Changed
- **Renamed OpenBreath → DragonBreath** across the firmware: build/project
  identity and OTA image-identity gate, HTTP auth header `X-DragonBreath-Auth`,
  Wi-Fi AP SSID `DragonBreath_XXXX` (with migration of the legacy default), mDNS
  hostname `dragonbreath.local` (app-layer override; shared OpenVent core
  untouched), web-UI title (🐉) and strings, logs, and docs. "Panda Breath"
  remains only as the underlying-hardware descriptor.

### Migration
- The OTA image-identity gate now requires `project_name == dragonbreath`, so a
  device on pre-rename firmware must be **USB-reflashed once** to cross over; OTA
  works normally afterward.
