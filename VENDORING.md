# Vendored components

The board-agnostic core components below are **vendored** into this repository —
they live in `components/` and are built like any first-party component. They were
previously consumed from the [OpenVent](https://github.com/justinh-rahb/OpenVent)
family via a git submodule (`external/OpenVent`, `EXTRA_COMPONENT_DIRS`); that
submodule has been removed and the code pulled local so DragonBreath has no
external submodule dependency.

## What was vendored

| Local component | Upstream (OpenVent) | Purpose |
|---|---|---|
| `components/pb_evlog`     | `pv_evlog`     | in-memory event ring |
| `components/pb_wifi`      | `pv_wifi`      | Wi-Fi STA/AP provisioning, mDNS, captive portal support |
| `components/pb_moonraker` | `pv_moonraker` | Moonraker WebSocket client (printer/bed state) |

Only these three were ever built by DragonBreath. The rest of the OpenVent
submodule (`pv_button`, `pv_policy`, `pv_portal`, `pv_status_led`, `pv_board`,
`pv_motor`) was unused — DragonBreath has its own `pb_*` equivalents — and was not
vendored.

## Provenance & license

- **Source:** https://github.com/justinh-rahb/OpenVent
- **Commit:** `ec4691f8d7fe95be8e3c6af4cac35d4992b08c79` (`v0.3.0-4-gec4691f`)
- **License:** MIT (upstream `LICENSE`); the SPDX headers in each vendored file are
  retained. This attribution stands even though the symbols were renamed.

## Changes made while vendoring

- The `pv_` prefix was renamed to `pb_` throughout (directories, component names,
  files, all functions, and `PV_*` macros → `PB_*`) to match the rest of the
  DragonBreath codebase. `pb_moonraker` requires `pb_evlog`.
- App-layer overrides remain in `main/app_main.c`: the mDNS/netif hostname and the
  Wi-Fi AP SSID prefix are set there (not in the vendored components), so the
  product identity is DragonBreath rather than the OpenVent defaults.

## Relationship to OpenVent

This is a clean fork of the shared core, not a live dependency. Because the symbols
were renamed to `pb_*`, OpenVent cannot drop-in reference these copies; keeping the
two in sync (or extracting a shared library) would be a deliberate future effort.
