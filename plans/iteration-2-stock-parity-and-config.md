# OpenBreath — Iteration 2: Stock-Panda parity + configurability

## Context
The Iteration 1 foundation is operational and hardware-validated: heater and fan
control, sensor monitoring, captive provisioning, dashboard/API, web OTA, and
Klipper/Fluidd integration via openbreath-klipper. Iteration 2 adds stock-parity
behavior while also closing remaining safety, state-synchronization, testing,
and release-engineering gaps. The alpha API and UI are not frozen and may
change incompatibly before beta.

Scope came from four exploration passes over the firmware, the RE'd hardware, and the stock behavior. Two realities constrain it:
- **Hardware limits the buttons.** Of the three physical buttons, only **K3 (GPIO2)** is cleanly usable. **K2 is the *same physical pin* as the chamber NTC** (the safety sensor) and **K1 shares the AC zero-cross ISR** — both are unsafe to repurpose. There is **no buzzer** on the board (audible feedback is not a parity item).
- **Safety overrides stock fidelity** where they conflict (notably boot behavior).

Decisions locked with the user: **phased plan**, **K3-only button**, **boot-OFF /
no auto-resume**, **AUTO mode included**, **one authoritative device-side state
machine**, and **breaking alpha API changes are acceptable**.

Not covered / explicitly out of scope: stock OEM WebSocket protocol + web UI, Bambu binding, K1/K2 buttons, the stock `filtertemp`/`heater_temp` auto params (OpenBreath has no filter sensor; the PTC/chamber cutoffs already bound element temp).

## Phase 0 — Product identity + release correctness

Ship these small but user-visible corrections before expanding behavior.

> **✅ DONE — closed by the OpenBreath→DragonBreath rename + the release pipeline; shipped in [v0.2.0](https://github.com/plastikman/DragonBreath/releases/tag/v0.2.0) (2026-07-23).**

**0.1. OpenBreath identity.**
- AP SSID: `OpenBreath_XXXX`, not `OpenPanda_XXXX`.
- mDNS: `OpenBreath.local` or uniquely suffixed `OpenBreath-XXXX.local`, not
  `OpenVent.local`.
- Replace OpenPanda/OpenVent in page titles, logs, setup pages, documentation,
  and release material. Shared components receive an injected product identity;
  a shared-core implementation name must not leak into the product.

> ✅ **Done** (as *DragonBreath* — the project was renamed OpenBreath→DragonBreath):
> AP SSID `DragonBreath_XXXX` (with legacy `OpenPanda_` migration), mDNS
> `dragonbreath.local` (app-layer override of the OpenVent core), web UI (🐉), logs,
> docs, release material. **PRs: firmware rename #3, UI polish #4.** Residual nits
> (non-blocking): mDNS is flat, not per-device-suffixed; the shared OpenVent core still
> prints its own name in *serial* logs (dev-facing only).

**0.2. Reproducible release artifacts.**
- Build releases in CI from the tagged commit and publish a manifest containing
  the source SHA, ESP-IDF version, dependency versions, tested board revision,
  and SHA-256 for every artifact.
- Publish a complete first-install bundle: bootloader, partition table,
  initialized OTA-data image, application image, flasher, and checksum manifest.
  Publish the application image separately for OTA.
- Resolve the current release/CI/local application-image divergence and make the
  documented first-install command work directly with the published bundle.

> ✅ **Done** — tag-triggered CI release (`release.yml` + `tools/package_release.sh`):
> single-file factory image, OTA app image, complete install bundle (nested layout so the
> bundled `flash.py --build-dir .` works **and** keeps the mandatory stock backup),
> `manifest.json` (source SHA, ESP-IDF v5.3.5, OpenVent submodule, per-artifact SHA-256),
> and `SHA256SUMS.txt`; release notes auto-pulled from `CHANGELOG.md`. Divergence resolved
> (documented first-install works off the published bundle). **PR #7; first release v0.2.0.**

## Reusable assets (port — don't reinvent)
All under `external/OpenVent/firmware/components/` (present via `EXTRA_COMPONENT_DIRS`; some not yet built):
- `pv_status_led/` — own task, OFF/SOLID/BLINK, active-high → template for **pb_leds**.
- `pv_button/` — 10 ms poll, 20 ms debounce, short/long-press, single callback → template for **pb_buttons**.
- `pv_policy.c` — NVS centi-degree-in-u32 float storage, `load_persisted()`, validating setters under a lock → template for **settings + mode params**.
- `pv_evlog/` — 64-entry event ring (unused in OpenBreath) → button/mode/fault events.
Port to `pb_board.h` pins; these reference OpenVent GPIOs, so they are code templates, not drop-ins.

## Component/registration pattern (every phase)
New component: `components/pb_X/{pb_X.c, include/pb_X.h, CMakeLists.txt}` with `idf_component_register(SRCS ... INCLUDE_DIRS include REQUIRES ...)`; add the name to `main/CMakeLists.txt` REQUIRES; init in `main/app_main.c`; run from `control_task` (2 Hz) or its own FreeRTOS task.

---

## Phase A — Configurable settings + real status LEDs (smallest, ship first)

✅ **Shipped** — PR `feat/phase-a-settings-leds` (A1–A4). Two deviations from the spec below,
both simplifications: (1) the Advanced/Safety card lives on the **status dashboard**
(`STATUS_BODY`), not `config_page`, and is driven **client-side** from `GET /settings` — so
`pb_portal` needs **no** `pb_heater` include / CMake dep after all. (2) `GET /settings` was
added alongside `POST /settings` (bounds + current values in one read); `max_uri_handlers`
12→14 covers both.

**A1. Runtime settings inside `pb_heater` (sole SSR owner).**
- `components/pb_heater/include/pb_heater.h`: rename `PB_HEATER_MAX_TARGET_C` → `..._C_DEFAULT` (70); **add** `PB_HEATER_ABS_MAX_TARGET_C 70.0f` and `PB_HEATER_MIN_TARGET_C 30.0f`; rename `PB_HEATER_COMMS_TIMEOUT_MS` → `..._MS_DEFAULT` and add `_MS_MIN (10*1000)` / `_MS_MAX (5*60*1000)`. **Leave `PB_HEATER_PTC_CUTOFF_C 105` and `PB_HEATER_CHAMBER_MAX_C 85` untouched.** Declare `pb_heater_set/get_max_target_c`, `pb_heater_set/get_comms_timeout_ms`, `pb_heater_load_config`. Raising the production target ceiling above 70 °C is a separate hardware-validation item: a nominal 80 °C target leaves only 5 °C below the fixed chamber trip, which is not enough margin without measured worst-case lag and overshoot.
- `components/pb_heater/pb_heater.c`: add `s_max_target_c` (float) + `s_comms_timeout_us` (int64_t) guarded by the existing `s_mux`; copy `centi_to_c`/`c_to_centi` from `pv_policy.c`. `pb_heater_init` sets the *defaults* only (NO NVS — nvs isn't up yet). New `pb_heater_load_config()` opens `app_nvs` READONLY, reads+**clamps**+assigns under `s_mux` (NVS read outside the lock). Setters clamp (`≤ ABS_MAX`, timeout `[MIN,MAX]`), persist (keys below), assign under `s_mux`; `set_max_target_c` also pulls a live `s_target_c` down if it now exceeds the cap. Move `set_target`'s upper clamp *inside* `s_mux` to use `s_max_target_c`. Extend the tick snapshot (~`:161-165`) to also copy `s_comms_timeout_us`; compare against it at the watchdog (~`:198`).
- **Init-ordering fix:** `pb_heater_init` runs before `nvs_init()` in `app_main`. Call `pb_heater_load_config()` in `app_main` right after `nvs_init()`/`brand_ap()` (~line 187, before `pb_httpd_start`). Defaults are conservative and the target is 0 during bring-up, so the brief window is race-free.
- NVS keys (`app_nvs`): `heat_max_c` (u32 centi-°C, dflt 7000, clamp 3000–7000), `heat_comms_ms` (u32 ms, dflt 300000, clamp 10000–300000).
- CMake: `pb_heater` REQUIRES += `nvs_flash`.
- **Invariant:** all three writers of `s_max_target_c` clamp `≤ 70`; `set_target` clamps every target `≤ s_max_target_c` → no NVS/API path heats past 70 °C; the remote deadman may be shortened but never disabled or extended beyond five minutes.

**A2. HTTP.** `pb_httpd.c`: add `settings_post` (auth-gated via `auth_reject`, modeled on `target_set`; fields `max`, `comms_ms`, each optional; respond with the *clamped* effective values). Register `POST /settings`; bump `cfg.max_uri_handlers` 12→14. `status_get`: change `"max"` to `pb_heater_get_max_target_c()`, add `"max_abs"` and `"comms_ms"`. Update `pb_httpd.h` doc.

**A3. Portal.** `pb_portal.c`: `#include "pb_heater.h"` (+ `pb_heater` in its CMake REQUIRES). Add an "Advanced / Safety" card in `config_page` rendering current values via getters (input `max=70`, note that 85/105 cutoffs are fixed). Make it its **own form** submitting via `fetch()` to `/settings` (reuse `OB_AUTH_JS`/`hdr()`), **not** through `save_post` (which reboots) — inline "Saved ✓", no reboot. Drive the dashboard set-temp input `max` from `/status` `s.max` in `refresh()` (it's currently a hardcoded `max=70`).

**A4. `pb_leds` (new component).** 3-LED pattern driver (OFF/SOLID/BLINK/BLINK_SLOW/CODE), own task on a 50 ms base tick, active-high GPIO6/5/4, atomic per-LED pattern (+ pulse count for CODE). API `pb_leds_start()`, `pb_leds_set(id, pattern)`, `pb_leds_set_code(id, n)`. REQUIRES `driver pb_board`. **Remove the LED drive from `pb_heater.c` (~lines 168-171)** so pb_heater owns no LED. In `pb_policy_tick()` push K1: `is_faulted()→BLINK` (fault now visible locally — improvement over today's go-dark), `heat_mode()→SOLID`, else `OFF`. Call `pb_leds_start()` in `app_main` before `xTaskCreate(control_task,...)`. `pb_policy` REQUIRES += `pb_leds`.

**Files:** `pb_heater.{c,h}` + CMake, `pb_httpd.{c,h}`, `pb_portal.c` + CMake, `main/app_main.c` + `main/CMakeLists.txt`; add `components/pb_leds/*`.

---

## Phase B — Authoritative state + API v2 + modes

**Delivery split.** Land this phase as a stacked series so safety/control review
is not mixed with HTTP parsing:
1. **Authoritative state-machine foundation:** make `pb_policy` the only
   mode/target writer; add canonical snapshots, revisions, AUTO/DRYING,
   device-issued leases, stale-lease rejection, boot-OFF, and bounded local
   operation. Temporarily adapt the alpha HTTP handlers to call policy so there
   is no second actuator owner between PRs.
2. **API v2 protocol + observers:** delete the alpha `/status`, `/target`,
   `/heartbeat`, and `/reset` contract; add the versioned JSON command/state
   protocol and event stream; migrate the dashboard. `dragonbreath-klipper`
   moves directly from the alpha API to v2 after the firmware contract lands.

The persistent-fault/thermal-purge work in B2 remains independently reviewable
safety work and must not be hidden inside transport parsing.

**B1. `pb_policy` becomes the sole control-state and target writer.** Rewrite
`components/pb_policy/`:
- Remove `pb_policy_input_t` + the dead `pb_policy_apply()`.
- Add `pb_mode_t {OFF, POWER_ON, AUTO, DRYING}` and a source enum that
  distinguishes `BOOT`, `WEB`, `KLIPPER`, `BUTTON`, `SAFETY`, and `WATCHDOG`.
- Keep a portMUX-guarded parameter/state block and an authoritative snapshot
  containing mode, requested/effective target, heater/fan state, sensor
  values/validity, AUTO engagement, drying state/deadline, Moonraker state,
  active lease, latched fault/inhibit reason, last-change source, and a
  monotonically increasing revision. Increment the revision on every
  authoritative state transition.
- API: `set_mode_off`, `set_power_on(t,src)`, `set_auto(t,bed,src)`,
  `start_drying(t,hrs,src)`, `stop_drying`, `set_env(bed_c,mk_connected)`,
  `get_snapshot`, and `get_mode`.
- `pb_policy_tick()`: snapshot mode+params+env → `compute_target_and_fan()` →
  `(target, fan_on, feed_wdt)`; if `feed_wdt` call
  `pb_heater_notify_link_alive()`; `pb_heater_set_target_c(target)` (ignore its
  409 while latched); `pb_heater_tick()`; fan =
  `fan_on || heat_mode() || thermal_purge || is_faulted()`.
- `set_power_on` also does one immediate `set_target_c` and returns its
  `esp_err_t` so command rejection/clamp semantics remain synchronous.
- Buttons, HTTP, Moonraker/Klipper, watchdogs, and safety logic call this layer;
  none may retain a private hardware-control state.

**B2. Thermal purge + persistent fault latch.**
- Add a thermal-purge latch: engage when either chamber or PTC temperature
  reaches 40 °C; disengage only after both fall below a documented hysteresis
  boundary. Purge overrides mode, target, printer state, and user fan requests.
- Any fault forces heater OFF, fan ON continuously, and rejects heat/mode
  commands.
- Persist the fault latch and reason in NVS on latch transitions. Power cycling
  does not clear it. A latched-fault boot initializes heater OFF, fan ON, and
  commands inhibited before normal control is exposed.
- Clear only through an explicit action after all sensor and safety conditions
  recover. If persisted fault state cannot be read reliably, fail safe: heater
  OFF, fan ON, commands inhibited.
- Active modes, targets, deadlines, and leases remain deliberately
  non-persistent; fault persistence is separate from forbidden heat resume.

**B3. AUTO (Moonraker bed threshold).** Params `auto_target` (40–60, dflt 60),
`auto_bed` (40–120, dflt 100), hysteresis latch (engage `bed≥thresh`, disengage
`bed<thresh-3`). `control_task` computes
`mk_connected = s_mk_up && st.state==PV_MK_SUBSCRIBED` (bed_temp is left stale
on WS drop — must gate on SUBSCRIBED) and calls
`pb_policy_set_env(st.bed_temp, mk_connected)` each tick *before*
`pb_policy_tick()` — this keeps `pb_policy` free of any `pv_moonraker` include.
AUTO drives `auto_target` only while engaged+connected and **self-feeds the
watchdog only then**; otherwise target 0.

**B4. DRYING.** Presets PLA=55 / PETG=60 / ABS=60 / custom (40–60, dflt 60);
hours 1–12 (**12 = default and hard cap**, enforced at start). Deadline via
`esp_timer_get_time()`; tick auto-offs at deadline; `stop_drying` → OFF.
Self-feed is safe (bounded ≤12 h). Expose `drying_remaining_s`.

**B5. Boot / resume (safety, endorsed).** `pb_policy_init` sets **mode=OFF,
target 0 unconditionally**; SSR is already forced off by `pb_heater_init`.
Persist *parameters only* to `app_nvs` (`md_auto_tgt`, `md_auto_bed`,
`md_dry_tgt`, `md_dry_hrs`, `md_last` — UI pre-fill), **never** an active mode,
heating flag, deadline, or control lease. `pb_policy_load_params()` follows
`nvs_init()`. Re-arming requires an explicit live command. A persisted fault
restores its inhibited state, never its prior target.

**B6. Device-issued control lease.**
- A successful remote heat command creates a new lease/epoch and returns it with
  the resulting state revision.
- Heartbeats reference that lease. Button, mode, watchdog, safety, fault, and
  explicit-OFF actions invalidate it.
- Stale heartbeats receive a conflict and cannot preserve or restore heat.
- A reconnecting dashboard/helper first consumes a complete device snapshot. It
  may not silently restore its former target; re-arming requires a new command.

**B7. API v2 (breaking alpha change).**
- Introduce an explicit API version and structured JSON state/command documents.
  Exact paths may be finalized during implementation, but provide equivalents
  of `GET state`, `POST command`, `POST heartbeat`, and `GET events`.
- Command responses return the resulting authoritative snapshot/revision, not
  only `{"ok":true}`. Rejections include machine-readable fault, inhibit,
  version, validation, or stale-lease reasons.
- Provide a device-originated SSE or WebSocket event stream; polling remains a
  recovery fallback.
- `"set target" > 0` maps to POWER_ON and zero maps to OFF.
- Update firmware, dashboard, and openbreath-klipper together. Require a
  firmware/helper version handshake and fail clearly on incompatibility. There
  is no obligation to preserve the current query-parameter alpha API.
- User-facing precedence may remain last-writer-wins, but a stale writer must
  not win unknowingly.

**B8. Local POWER_ON cap (safety).** `PB_POLICY_LOCAL_PWRON_MAX_H` (~12 h) so no
local mode is unbounded (AUTO is bounded by live Moonraker state, DRYING by its
timer).

**B9. Observers.** Dashboard and openbreath-klipper consume the canonical
snapshot/event stream, including mode, drying countdown, revision, source,
lease, and inhibit state. A physical or safety action must appear promptly in
both and invalidate any stale local ownership.

**Files:** `pb_policy.{c,h}` + CMake (+`esp_timer`,`nvs_flash`),
`main/app_main.c`, `pb_httpd.{c,h}`, `pb_portal.c`, and openbreath-klipper's
transport/state adapter.

---

## Phase C — K3 physical button

**C1. `pb_buttons` (new component).** Port `pv_button`'s state machine (10 ms poll task, 20 ms debounce, long-press 2 s) behind a per-button table with an `active_low` field (so a wrong polarity guess is a one-line flip). v1 table = K3 only: GPIO2, `GPIO_MODE_INPUT`, internal **pull-up** (never pull-down — GPIO2 is a strap that must be high at reset), poll-only. API `pb_buttons_start(cb)`, `pb_button_cb_t(id, ev)`; SHORT on release, LONG once (suppress trailing short). REQUIRES `driver pb_board` (stays decoupled — no heater/policy link).

**C2. `pb_heater_request_estop(reason)` (new).** Mux-guarded latch (`s_target_c=0; s_latched_off=true; s_fault_reason=reason`) with **no GPIO write** — the next `pb_heater_tick()` drops the SSR in control-task context (≤500 ms), preserving the single-SSR-writer invariant. **Do NOT call `emergency_off()` from the button task** (it writes the SSR GPIO directly).

**C3. Button UX (provisional until the printed icon + OEM behavior are
documented) in `pb_policy_on_button`.** Prefer the familiar OEM meaning unless a
deliberate deviation adds clear safety/usability value. Proposed safety
semantics: short while heating → normal OFF; short while idle → OEM-compatible
action or v1 no-op + evlog; long (2 s) while not faulted →
`pb_heater_request_estop("button")`; long while faulted → attempt clear only if
every recovery condition passes. A failed clear retains the latch and reports
why. Never clear a persistent fault through an accidental short press. Every
button action updates source/revision, invalidates the remote lease, emits an
event, and drives K3 LED (GPIO4) feedback via `pb_leds_set_code` +
`pv_evlog_add`. `app_main` registers a thin
`button_cb → pb_policy_on_button`, started **early** (before control_task) so
panic-off is live ASAP. `pb_policy` REQUIRES += `pb_buttons pv_evlog`.

**Files:** add `components/pb_buttons/*`; `pb_heater.{c,h}` (request_estop), `pb_policy.{c,h}` (on_button), `main/app_main.c` + `main/CMakeLists.txt`.

**Bench-verification (gates trusting the button — do on hardware):**
1. GPIO2 idles **high** unpressed (confirms active-low + pull; also a strap sanity check).
2. Board boots without holding K3; document "don't hold K3 at power-on."
3. K3 polarity end-to-end (idle 1 / pressed 0; else flip `active_low`).
4. LEDs light on **high**; toggling GPIO4/5 does **not** perturb chamber/PTC temps, the ZCD count, or the GPIO2 read; GPIO6 heat LED still works after ownership moved to pb_leds.
5. Press detection: exactly one SHORT on release; one LONG at threshold, no trailing short.
6. Safety regression while heating: button/LED activity causes no spurious sensor-fault trips, ZCD keeps counting; long-press drops the SSR ≤1 tick and latches (`fault:true` in `/status`).
7. Remote-control regression: while heating from dashboard/Klipper, a K3 action invalidates the lease, updates both observers, and stale heartbeats/reconnects do not restore heat.
8. Persistent-fault regression: power-cycle after a latched fault → heater OFF, fan ON, commands rejected until a deliberate valid clear.

---

## Phase D — Responsive dashboard + remaining parity

**D1. Dashboard shell.** Replace the growing utility page with reusable
components that remain usable standalone and fit a narrow Fluidd/Mainsail
iframe without nested scrolling.

**D2. At-a-glance state.** Show chamber/PTC temperatures, requested/effective
target, heater/fan state, mode, AUTO engagement, drying countdown, Moonraker
state, control source/lease, watchdog state, and fault/inhibit reason.

**D3. Controls and feedback.** Provide clear target, mode, drying, OFF, and
fault-reset controls. Show command success, rejection, stale ownership,
connection loss, and external button/safety actions. Keep setup and OTA on
secondary pages.

**D4. Parity matrix.** Track every OEM feature as implemented, planned,
intentionally changed, intentionally omitted, or unverified. The current
exclusions remain explicit. Fan-only filtration is a possible OpenBreath
enhancement, not required parity until OEM behavior confirms it.

---

## Phase E — Static analysis, simulation + hardware-in-loop

**E1. CI/static analysis.** Run warnings-as-errors, formatting, static analysis,
dependency validation, host-side unit tests, frontend validation, and API/schema
tests on every PR.

**E2. Host/simulation coverage.** Exercise sensor open/short/NaN/out-of-range
values, thresholds/hysteresis, corrupt NVS, persistent-fault recovery,
watchdogs, timer rollover, reconnect storms, conflicting controllers, stale
leases, malformed API messages, and OTA validation/rollback decisions.

**E3. ESP32-C3 dev-board target.** Provide a target for a common ESP32-C3 dev
board with production heater output compile-time disabled or redirected. It must
be structurally incapable of energizing Panda mains hardware. Permit injected
chamber/PTC values, sensor faults, zero-cross events, buttons, printer state,
and network state; expose heater demand, fan state, LEDs, mode, lease, and fault
state.

**E4. HIL tooling.** Automate flashing, scenario selection, input injection,
API/event exercise, serial/state capture, assertions, and a pass/fail report.
Use the dev board for safe partial HIL; retain the real Panda as a documented
pre-release qualification gate.

---

## Cross-cutting safety invariants (must hold across all phases)
- Absolute over-temp cutoffs (105 °C PTC, 85 °C chamber) and sensor-fault fail-close stay **compile-time**, untouched by any config / mode / button path.
- **SSR single-writer:** only `control_task`'s `ssr_set()` drives GPIO18. Every cross-task "off" is a mux-guarded latch that converges on the next tick (≤500 ms).
- **No unbounded unattended local heat:** AUTO bounded by live Moonraker data, DRYING by its ≤12 h timer, local POWER_ON by the runtime cap; boot never self-arms.
- Comms deadman is configurable from 10 s to 5 min but never disabled or extended beyond five minutes for remote POWER_ON. It is self-fed only while a bounded local mode's liveness precondition holds — and even then it only defeats the communications timeout, never the over-temp/sensor cutoffs.
- **Residual-heat purge:** the fan remains ON above the 40 °C purge threshold (with hysteresis), even after target/mode OFF.
- **Persistent faults:** fault state survives power loss; active heat state, deadlines, and leases never do. A faulted boot is heater OFF, fan ON, commands inhibited.
- **One authoritative state:** hardware, API, dashboard, Klipper, buttons, watchdog, and safety logic consume the same policy snapshot. No stale client/reconnect path may restore heat.
- Physical OFF, watchdog, and safety actions invalidate remote ownership. Re-arming always requires a fresh command.
- Production target/deadman ceilings may not be loosened without a documented hardware-validation gate.
- Test builds are compile-time prevented from driving the production heater GPIO.

## Docs (each phase)
Update `README.md` status table + `docs/SAFETY.md`: the configurable target cap
(bounded by the validated 70 °C production ceiling; 85/105 fixed), the
boot-OFF/no-resume deviation from stock, persistent-fault and thermal-purge
behavior, the mode set, API/lease semantics, K3 button semantics, and LED
meanings. Clearly distinguish first install, OTA update, and stock restore;
state the tested board revisions and known limitations. Maintain a changelog and
generate release posts from a repeatable template rather than ad hoc prose.

## Shared core (after state/API stabilization)

Define clean seams during these phases, but defer extraction until the
authoritative state model and API stop moving rapidly. Then publish a versioned,
product-neutral ESP-IDF component containing:
- Wi-Fi + captive provisioning and injected mDNS/product identity.
- Moonraker connection, subscription, and reconnect handling.
- Common API state/command/event structures and authentication hooks.
- OTA/rollback, event logging, and a responsive UI shell/component library.

OpenVent and OpenBreath retain branding, GPIO/board maps, sensors/actuators,
safety policy, product modes/thresholds, and button mappings. Both consumers pin
a core revision and build it in CI.

## Verification (end-to-end, per phase)
Build: `cd ~/git/OpenBreath && idf.py build`; flash `idf.py -p /dev/ttyUSB0 flash`. Device on the bench at `10.168.2.53`.
- **0:** all product-visible names are OpenBreath; a tagged CI release produces a complete install bundle + OTA image with a manifest whose hashes match the downloaded files.
- **A:** `max=90` → clamps to 70; `comms_ms=5000` → clamps to 10000; `comms_ms=3600000` → clamps to 300000; lower `max` below an armed target → target pulled down; dashboard input `max` tracks state; force a fault → K1 blinks; over-temp cutoffs unchanged.
- **B:** each mode drives the target; AUTO engages at the bed threshold and fails to 0 when Moonraker drops; drying counts down, auto-offs, and hard-caps at 12 h; reboot → OFF (no resume); 40 °C purge keeps the fan running after target OFF; fault → power-cycle restores inhibited/fan-ON state; stale lease/reconnect cannot restore heat; over-temp still trips; `M141 S45` still heats (= POWER_ON). Verify through the versioned state/command/event API.
- **C:** the bench checklist above.
- **D:** exercise every control from standalone and narrow iframe layouts; button/Klipper/safety changes appear promptly and no stale UI state reasserts a command.
- **E:** static/unit/simulation suites pass; dev-board HIL proves injected failure/ownership scenarios; real-Panda release matrix passes.

Ship each phase as its own branch → CI/build → on-device test → PR/merge. Cut a
release only when that phase is device-validated and its artifact manifest is
traceable to the tagged source.

## Delivery order
1. Phase 0: identity and release correctness. ✅ **Done — v0.2.0** (PRs #3, #4, #7).
2. Phase A: bounded settings and verified LEDs.
3. Phase B: authoritative state/API, modes, leases, thermal purge, and persistent faults.
4. Phase C: K3-only physical control.
5. Phase D: responsive dashboard and remaining intentional parity.
6. Phase E: complete CI, simulation, dev-board HIL, and real-device qualification.
7. Extract/version the shared core after the state/API seams stabilize.
