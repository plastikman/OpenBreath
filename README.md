# OpenBreath

Open firmware for the **BIGTREETECH Panda Breath** chamber heater (ESP32-C3),
replacing the stock cloud integration with **Moonraker/Klipper** (and Home
Assistant) control.

Sibling to [OpenVent](https://github.com/justinh-rahb/OpenVent) — part of an
open-firmware **family for the BTT Panda line** that shares a common core.
OpenBreath mirrors OpenVent's ESP-IDF + `components/` layout on purpose, so the
shared core (WiFi, captive portal, Moonraker client) can be lifted in rather than
re-implemented.

> ⚠️ **Early scaffold.** This is a starting skeleton — the device-specific logic
> (board map, temperature conversion) is real and reverse-engineered; the heater
> control is functional-but-unverified; the fan is a timing skeleton; networking
> is not yet wired. **Not yet flashed to hardware. Do not run near a print.**

## Status
| Component | State |
|---|---|
| `pb_board` | ✅ Pinout RE'd; boots on hardware (INFERRED pins to continuity-test) |
| `pb_ntc` | ✅ Stock conversion ported; **hardware-validated** (reads 33.0 °C = printer extruders) |
| `pb_heater` | 🟡 Bang-bang + full safety cutoffs; SSR pin confirmed, needs load validation |
| `pb_fan` | 🟡 TRIAC phase-angle, proven zero-cross model (**never PWM**); stock values being finalized |
| `pb_policy` | 🟡 Thin glue stub |
| Network core: `pv_wifi` / `pv_evlog` / `pv_moonraker` | 🔗 Referenced from OpenVent (submodule; available, not yet wired) |
| Portal / status LED / buttons | ⬜ Breath-local — device-specific, **not** part of the shared core |
| Flashing / partitions | 🟡 Flashes + boots; restore-to-stock documented, partition strategy TBD |

**Shared-core boundary:** board-agnostic infrastructure (WiFi, event log, Moonraker
client) is referenced from the [OpenVent](https://github.com/justinh-rahb/OpenVent)
family via `external/OpenVent` (git submodule) + `EXTRA_COMPONENT_DIRS`. Everything
device-specific — the board map, sensors, heater/fan actuation, and the portal /
LED / button UI — stays in this repo.

## Hardware
ESP32-C3-MINI-1, mains PSU, PTC heater via SSR (GPIO18), ~220 VAC blower via
TRIAC phase-angle (GPIO3 gate + GPIO7 zero-cross), two NTCs on ADC1. Full map:
[`docs/HARDWARE.md`](docs/HARDWARE.md).

## Safety
Two independent **hardware** over-temp backstops (a bonded thermal cutoff in the
PTC mains lead + PTC self-limiting physics) mean firmware cannot cause a fire.
This firmware adds soft cutoffs + a comms-loss watchdog. Read
[`docs/SAFETY.md`](docs/SAFETY.md) before touching heater code.

## Temperature conversion
Fully reverse-engineered from the stock firmware — resistance divider
(`Rntc = Rref·V/(0.1−V)`) + a 114-entry R/T lookup table, not a beta formula.
Details + derivation: [`docs/NTC_CONVERSION.md`](docs/NTC_CONVERSION.md).

## Build
Requires ESP-IDF v5.3+.
```bash
idf.py set-target esp32c3
idf.py build
# Flash via the CH340K USB-C bridge (native USB is unavailable — GPIO18 is the SSR):
idf.py -p /dev/ttyUSB0 flash monitor
```

## Layout
```
components/
  pb_board/    GPIO single-source-of-truth
  pb_ntc/      ADC -> temperature (RE'd stock conversion)
  pb_heater/   SSR control + safety cutoffs + comms watchdog
  pb_fan/      TRIAC phase-angle blower control
  pb_policy/   controller-state -> actuators glue
main/          app_main: safety-first init + control loop
docs/          hardware map, safety model, NTC RE report
```

## Credits
Hardware + firmware reverse-engineering builds on the BTT Panda Breath work in
`klipper-esp32` (Justin Hayes) and the OpenVent architecture. MIT licensed.

## Support
If OpenBreath is useful to you, you can support development here:
[☕ Buy me a coffee](https://buymeacoffee.com/plastikman)
