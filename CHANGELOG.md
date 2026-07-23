# Changelog

All notable changes to the **DragonBreath** firmware are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are the
firmware release tags (`vX.Y.Z`). The release workflow pulls the matching section
below into the GitHub Release notes.

## [Unreleased]

### Added
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
