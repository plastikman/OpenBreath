# OpenBreath

Open firmware for the **BIGTREETECH Panda Breath** chamber heater (ESP32-C3),
replacing the stock cloud integration with **Moonraker/Klipper** (and Home
Assistant) control.

Sibling to [OpenVent](https://github.com/justinh-rahb/OpenVent) — part of an
open-firmware **family for the BTT Panda line** that shares a common core.
OpenBreath mirrors OpenVent's ESP-IDF + `components/` layout on purpose, so the
shared core (WiFi, captive portal, Moonraker client) is lifted in rather than
re-implemented.

> ⚠️ **Hardware tested on a single board revision (V1.0.1) only.** That is the
> only unit available for testing; the pin map, sensor conversion, and
> heater/fan actuation are validated against it and may differ on other Panda
> Breath board revisions. **Verify the pinout against your own board before
> flashing.** Heater control is functional and hardware-validated on V1.0.1, but
> this is community firmware with no warranty — read [`docs/SAFETY.md`](docs/SAFETY.md)
> and supervise early runs.

## Status
| Component | State |
|---|---|
| `pb_board` | ✅ Pinout RE'd (V1.0.1); boots on hardware |
| `pb_ntc` | ✅ Stock conversion ported; **hardware-validated** (reads chamber temp matching the printer) |
| `pb_heater` | ✅ Bang-bang + full safety cutoffs; SSR confirmed, heat cycle validated on hardware |
| `pb_fan` | ✅ TRIAC **on/off held-gate** (stock model — the gate is never PWM'd/phase-chopped) |
| `pb_policy` | ✅ Fan-follows-heater glue |
| Network core: `pv_wifi` / `pv_evlog` / `pv_moonraker` | ✅ Referenced from OpenVent (submodule); WiFi + Moonraker validated on hardware |
| Portal / status dashboard / heat LED | ✅ Breath-local; captive-portal provisioning + live dashboard validated |
| HTTP control API (`pb_httpd`) | ✅ `/status` `/target` `/heartbeat` `/reset`; CSRF-gated mutations |
| Klipper-side helper (M141 / Fluidd) | 🟡 Talks to the HTTP API; next build |
| Flasher (`tools/flash.py`) | ✅ Backs up full stock flash first, then flashes; `--restore` returns to stock |
| Web OTA update | ✅ Dual-OTA + rollback; upload from the UI, verified on hardware (OpenBreath-only, refused while heating) |

**Shared-core boundary:** board-agnostic infrastructure (WiFi, event log, Moonraker
client) is referenced from the [OpenVent](https://github.com/justinh-rahb/OpenVent)
family via `external/OpenVent` (git submodule) + `EXTRA_COMPONENT_DIRS`. Everything
device-specific — the board map, sensors, heater/fan actuation, and the portal /
LED / button UI — stays in this repo.

## Hardware
ESP32-C3-MINI-1, mains PSU, PTC heater via SSR (GPIO18), ~220 VAC blower switched
by a **TRIAC held on/off** (GPIO3 gate + GPIO7 zero-cross — **never** phase-angle
PWM'd), two NTCs on ADC1. Full map: [`docs/HARDWARE.md`](docs/HARDWARE.md).
Reverse-engineered from a **V1.0.1** board.

## Safety
Two independent **hardware** over-temp backstops (a bonded thermal cutoff in the
PTC mains lead + PTC self-limiting physics) bound the worst-case failure to
roughly the stock firmware's ceiling — they are not defeated by a firmware bug or
a welded SSR. This firmware adds soft cutoffs + a comms-loss watchdog on top. No
firmware can *guarantee* the absence of a fault; read [`docs/SAFETY.md`](docs/SAFETY.md)
before touching heater code and supervise the device.

## Control API & access
`pb_httpd` exposes a small HTTP API on port 80:

| Method | Path | Purpose |
|---|---|---|
| GET  | `/status` | read-only JSON (temps, target, heating, fault) — **no** side effects |
| POST | `/target?t=<C>` | set chamber setpoint (0 = off); also counts as liveness |
| POST | `/heartbeat` | controller liveness only (pet the comms watchdog) |
| POST | `/reset` | clear a latched safety fault |

Every **mutating** endpoint (and the portal's STA-mode `/save`) requires a custom
`X-OpenBreath-Auth` header. A cross-origin HTML form can't set a custom header and
CORS is never enabled, so an ordinary drive-by web page can't drive the heater or
rewrite the WiFi config. This is **CSRF hardening for a trusted LAN, not transport
security** — the API is unencrypted HTTP. For untrusted networks, set a control
token in NVS (`app_nvs` / `ctl_token`) and the header must match it exactly; the
same-origin dashboard embeds the configured token automatically.

## Temperature conversion
Fully reverse-engineered from the stock firmware — a low-side resistance divider
(`Rntc = Rref·V / (Vsupply − V)`, `Vsupply = 3.3 V`, `Rref = 82 kΩ`) feeding a
114-entry R/T lookup table, not a beta formula. Details + derivation:
[`docs/NTC_CONVERSION.md`](docs/NTC_CONVERSION.md).

## Build
Requires ESP-IDF v5.3+.
```bash
git clone --recurse-submodules https://github.com/plastikman/OpenBreath
idf.py set-target esp32c3
idf.py build
```

## Install & update

> 🛑 **BACK UP YOUR DEVICE FIRST. THIS IS IRREVERSIBLE WITHOUT A BACKUP.**
> Installing OpenBreath **overwrites the entire flash** and **erases the stock
> firmware**. BIGTREETECH does **not** publish stock images, so **the full backup
> you take is the ONLY way back to stock.** If you skip the backup (or lose the
> file), there is **no going back** — you will be permanently on custom firmware.
> `tools/flash.py` takes and verifies this backup automatically before writing;
> **do not use `--no-backup`** unless you already have a known-good backup stored
> somewhere safe. Copy the backup off your machine (cloud/USB) before flashing.

**First install (stock → OpenBreath):** use the flasher, which backs up the
*entire* stock flash to a timestamped image **before** writing anything, so you
can return to stock. Flashing is over the on-board CH340K USB-C bridge
(native USB is unavailable — GPIO18 is the SSR):
```bash
python3 tools/flash.py                 # backup stock, then flash OpenBreath
python3 tools/flash.py --restore backups/stock-YYYYmmdd-HHMMSS.bin   # back to stock
```
First boot with no stored WiFi starts an `OpenPanda_XXXX` AP + captive portal for
provisioning.

**Updating OpenBreath:** once running, open the **Firmware update** link on the
status page (the `/fw` page) and upload `build/openbreath.bin`. The image lands in
the inactive OTA slot, is verified, and the device reboots into it; a bad image
rolls back on the next boot. Web OTA is **OpenBreath-only** — it does **not**
restore stock firmware (use `tools/flash.py --restore` for that) and is refused
while the heater is on.

## Layout
```
components/
  pb_board/    GPIO single-source-of-truth
  pb_ntc/      ADC -> temperature (RE'd stock conversion)
  pb_heater/   SSR control + safety cutoffs + comms watchdog
  pb_fan/      TRIAC on/off held-gate blower control (never PWM)
  pb_policy/   controller-state -> actuators glue
  pb_httpd/    HTTP control API (CSRF-gated mutations)
  pb_portal/   captive-portal provisioning + live status dashboard
main/          app_main: safety-first init + control loop
docs/          hardware map, safety model, NTC RE report
```

## Credits
Hardware + firmware reverse-engineering builds on the BTT Panda Breath work in
this project's `klipper-esp32` history and the OpenVent architecture
([justinh-rahb](https://github.com/justinh-rahb)). MIT licensed.

## Support
If OpenBreath is useful to you, you can support development here:

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://buymeacoffee.com/plastikman)
