# Changelog

All notable changes to the **DragonBreath** firmware are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are the
firmware release tags (`vX.Y.Z`). The release workflow pulls the matching section
below into the GitHub Release notes.

## [Unreleased]

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
