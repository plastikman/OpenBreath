# OpenBreath — Iteration 2: Stock-Panda parity + configurability

## Context
The OpenBreath core is done (heat control + full safety, captive portal + dashboard, HTTP API, web OTA, Klipper/Fluidd via openbreath-klipper). This iteration moves it toward feature parity with the stock BIGTREETECH Panda Breath and makes safety-adjacent limits user-configurable.

Scope came from four exploration passes over the firmware, the RE'd hardware, and the stock behavior. Two realities constrain it:
- **Hardware limits the buttons.** Of the three physical buttons, only **K3 (GPIO2)** is cleanly usable. **K2 is the *same physical pin* as the chamber NTC** (the safety sensor) and **K1 shares the AC zero-cross ISR** — both are unsafe to repurpose. There is **no buzzer** on the board (audible feedback is not a parity item).
- **Safety overrides stock fidelity** where they conflict (notably boot behavior).

Decisions locked with the user: **phased plan**, **K3-only button**, **boot-OFF / no auto-resume**, **AUTO mode included**.

Not covered / explicitly out of scope: stock OEM WebSocket protocol + web UI, Bambu binding, K1/K2 buttons, the stock `filtertemp`/`heater_temp` auto params (OpenBreath has no filter sensor; the PTC/chamber cutoffs already bound element temp).

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

**A1. Runtime settings inside `pb_heater` (sole SSR owner).**
- `components/pb_heater/include/pb_heater.h`: rename `PB_HEATER_MAX_TARGET_C` → `..._C_DEFAULT` (70); **add** `PB_HEATER_ABS_MAX_TARGET_C 80.0f` (hard ceiling, strictly below the 85 °C chamber trip) and `PB_HEATER_MIN_TARGET_C 30.0f`; rename `PB_HEATER_COMMS_TIMEOUT_MS` → `..._MS_DEFAULT` and add `_MS_MIN (10*1000)` / `_MS_MAX (60*60*1000)`. **Leave `PB_HEATER_PTC_CUTOFF_C 105` and `PB_HEATER_CHAMBER_MAX_C 85` untouched.** Declare `pb_heater_set/get_max_target_c`, `pb_heater_set/get_comms_timeout_ms`, `pb_heater_load_config`.
- `components/pb_heater/pb_heater.c`: add `s_max_target_c` (float) + `s_comms_timeout_us` (int64_t) guarded by the existing `s_mux`; copy `centi_to_c`/`c_to_centi` from `pv_policy.c`. `pb_heater_init` sets the *defaults* only (NO NVS — nvs isn't up yet). New `pb_heater_load_config()` opens `app_nvs` READONLY, reads+**clamps**+assigns under `s_mux` (NVS read outside the lock). Setters clamp (`≤ ABS_MAX`, timeout `[MIN,MAX]`), persist (keys below), assign under `s_mux`; `set_max_target_c` also pulls a live `s_target_c` down if it now exceeds the cap. Move `set_target`'s upper clamp *inside* `s_mux` to use `s_max_target_c`. Extend the tick snapshot (~`:161-165`) to also copy `s_comms_timeout_us`; compare against it at the watchdog (~`:198`).
- **Init-ordering fix:** `pb_heater_init` runs before `nvs_init()` in `app_main`. Call `pb_heater_load_config()` in `app_main` right after `nvs_init()`/`brand_ap()` (~line 187, before `pb_httpd_start`). Defaults are conservative and the target is 0 during bring-up, so the brief window is race-free.
- NVS keys (`app_nvs`): `heat_max_c` (u32 centi-°C, dflt 7000, clamp 3000–8000), `heat_comms_ms` (u32 ms, dflt 300000, clamp 10000–3600000).
- CMake: `pb_heater` REQUIRES += `nvs_flash`.
- **Invariant:** all three writers of `s_max_target_c` clamp `≤ 80`; `set_target` clamps every target `≤ s_max_target_c` → no NVS/API path heats past 80 °C; the deadman is tunable but never 0/∞.

**A2. HTTP.** `pb_httpd.c`: add `settings_post` (auth-gated via `auth_reject`, modeled on `target_set`; fields `max`, `comms_ms`, each optional; respond with the *clamped* effective values). Register `POST /settings`; bump `cfg.max_uri_handlers` 12→14. `status_get`: change `"max"` to `pb_heater_get_max_target_c()`, add `"max_abs"` and `"comms_ms"`. Update `pb_httpd.h` doc.

**A3. Portal.** `pb_portal.c`: `#include "pb_heater.h"` (+ `pb_heater` in its CMake REQUIRES). Add an "Advanced / Safety" card in `config_page` rendering current values via getters (input `max=80`, note that 85/105 cutoffs are fixed). Make it its **own form** submitting via `fetch()` to `/settings` (reuse `OB_AUTH_JS`/`hdr()`), **not** through `save_post` (which reboots) — inline "Saved ✓", no reboot. Drive the dashboard set-temp input `max` from `/status` `s.max` in `refresh()` (it's currently a hardcoded `max=70`).

**A4. `pb_leds` (new component).** 3-LED pattern driver (OFF/SOLID/BLINK/BLINK_SLOW/CODE), own task on a 50 ms base tick, active-high GPIO6/5/4, atomic per-LED pattern (+ pulse count for CODE). API `pb_leds_start()`, `pb_leds_set(id, pattern)`, `pb_leds_set_code(id, n)`. REQUIRES `driver pb_board`. **Remove the LED drive from `pb_heater.c` (~lines 168-171)** so pb_heater owns no LED. In `pb_policy_tick()` push K1: `is_faulted()→BLINK` (fault now visible locally — improvement over today's go-dark), `heat_mode()→SOLID`, else `OFF`. Call `pb_leds_start()` in `app_main` before `xTaskCreate(control_task,...)`. `pb_policy` REQUIRES += `pb_leds`.

**Files:** `pb_heater.{c,h}` + CMake, `pb_httpd.{c,h}`, `pb_portal.c` + CMake, `main/app_main.c` + `main/CMakeLists.txt`; add `components/pb_leds/*`.

---

## Phase B — Mode state machine + drying + auto (behavior)

**B1. `pb_policy` becomes the single target-writer.** Rewrite `components/pb_policy/`:
- Remove `pb_policy_input_t` + the dead `pb_policy_apply()`.
- Add `pb_mode_t {OFF, POWER_ON, AUTO, DRYING}`, `pb_src_t {REMOTE, LOCAL}`, a portMUX-guarded param block, and a snapshot struct. API: `set_mode_off`, `set_power_on(t,src)`, `set_auto(t,bed,src)`, `start_drying(t,hrs,src)`, `stop_drying`, `set_env(bed_c, mk_connected)`, `get_snapshot`, `get_mode`.
- `pb_policy_tick()`: snapshot mode+params+env → `compute_target_and_fan()` → `(target, fan_on, feed_wdt)`; if `feed_wdt` call `pb_heater_notify_link_alive()`; `pb_heater_set_target_c(target)` (ignore its 409 while latched); `pb_heater_tick()`; fan = `fan_on || heat_mode() || is_faulted()` (keep the fault-cooldown override).
- `set_power_on` also does one immediate `set_target_c` and returns its `esp_err_t` so `/target`'s synchronous 409/clamp semantics are preserved.

**B2. AUTO (Moonraker bed threshold).** Params `auto_target` (40–60, dflt 60), `auto_bed` (40–120, dflt 100), hysteresis latch (engage `bed≥thresh`, disengage `bed<thresh-3`). `control_task` computes `mk_connected = s_mk_up && st.state==PV_MK_SUBSCRIBED` (bed_temp is left stale on WS drop — must gate on SUBSCRIBED) and calls `pb_policy_set_env(st.bed_temp, mk_connected)` each tick *before* `pb_policy_tick()` — this keeps `pb_policy` free of any `pv_moonraker` include. AUTO drives `auto_target` only while engaged+connected and **self-feeds the watchdog only then**; otherwise target 0.

**B3. DRYING.** Presets PLA=55 / PETG=60 / ABS=60 / custom (40–60, dflt 60); hours 1–12 (**12 = default and hard cap**, enforced at start). Deadline via `esp_timer_get_time()`; tick auto-offs at deadline; `stop_drying` → OFF. Self-feed is safe (bounded ≤12 h). Expose `drying_remaining_s`.

**B4. Boot / resume (safety, endorsed).** `pb_policy_init` sets **mode=OFF, target 0 unconditionally**; SSR already forced off by `pb_heater_init`. Persist *parameters only* to `app_nvs` (`md_auto_tgt`, `md_auto_bed`, `md_dry_tgt`, `md_dry_hrs`, `md_last` — UI pre-fill), **never** an active/heating flag or the deadline. `pb_policy_load_params()` after `nvs_init()`. Re-arming requires an explicit live command (remote `/target`/`/mode` re-establishes the heartbeat lease; local button). A latched fault still needs `/reset` first.

**B5. HTTP.** `POST /mode` (auth-gated, modeled on `target_set`; `mode` + per-mode params/preset; map `INVALID_STATE`→409). Extend `/status` with `mode`, `drying_remaining_s`, `auto_target`, `auto_bed`, `drying_target`, `drying_hours` (all additive). Route `target_set`: `t>0 → set_power_on(t, REMOTE)`, `t==0 → set_mode_off` — **"set a target" == POWER_ON**; M191 (Klipper macro polling `/status.temp`) unaffected. Precedence = last-writer-wins (document it). `pb_httpd` REQUIRES += `pb_policy`.

**B6. Local POWER_ON cap (safety).** `PB_POLICY_LOCAL_PWRON_MAX_H` (~12 h) so no *local* mode is unbounded (AUTO bounded by Moonraker liveness, DRYING by its timer).

**B7. Observers.** Dashboard gains a mode selector + drying countdown + auto params (posts `/mode`). openbreath-klipper reads the new `/status` fields (mode, `drying_remaining_min`) — additive, no protocol change.

**Files:** `pb_policy.{c,h}` + CMake (+`esp_timer`,`nvs_flash`), `main/app_main.c`, `pb_httpd.{c,h}`, `pb_portal.c`.

---

## Phase C — K3 physical button

**C1. `pb_buttons` (new component).** Port `pv_button`'s state machine (10 ms poll task, 20 ms debounce, long-press 2 s) behind a per-button table with an `active_low` field (so a wrong polarity guess is a one-line flip). v1 table = K3 only: GPIO2, `GPIO_MODE_INPUT`, internal **pull-up** (never pull-down — GPIO2 is a strap that must be high at reset), poll-only. API `pb_buttons_start(cb)`, `pb_button_cb_t(id, ev)`; SHORT on release, LONG once (suppress trailing short). REQUIRES `driver pb_board` (stays decoupled — no heater/policy link).

**C2. `pb_heater_request_estop(reason)` (new).** Mux-guarded latch (`s_target_c=0; s_latched_off=true; s_fault_reason=reason`) with **no GPIO write** — the next `pb_heater_tick()` drops the SSR in control-task context (≤500 ms), preserving the single-SSR-writer invariant. **Do NOT call `emergency_off()` from the button task** (it writes the SSR GPIO directly).

**C3. Button UX (Scheme A) in `pb_policy_on_button`.** short: faulted→`clear_fault`; heating→`set_target_c(0)`; idle→reserved for future mode-cycle (v1 no-op + evlog). long (2 s): `pb_heater_request_estop("button")`. K3 LED (GPIO4) feedback via `pb_leds_set_code` + `pv_evlog_add`. `app_main` registers a thin `button_cb → pb_policy_on_button`, started **early** (before control_task) so panic-off is live ASAP. `pb_policy` REQUIRES += `pb_buttons pv_evlog`.

**Files:** add `components/pb_buttons/*`; `pb_heater.{c,h}` (request_estop), `pb_policy.{c,h}` (on_button), `main/app_main.c` + `main/CMakeLists.txt`.

**Bench-verification (gates trusting the button — do on hardware):**
1. GPIO2 idles **high** unpressed (confirms active-low + pull; also a strap sanity check).
2. Board boots without holding K3; document "don't hold K3 at power-on."
3. K3 polarity end-to-end (idle 1 / pressed 0; else flip `active_low`).
4. LEDs light on **high**; toggling GPIO4/5 does **not** perturb chamber/PTC temps, the ZCD count, or the GPIO2 read; GPIO6 heat LED still works after ownership moved to pb_leds.
5. Press detection: exactly one SHORT on release; one LONG at threshold, no trailing short.
6. Safety regression while heating: button/LED activity causes no spurious sensor-fault trips, ZCD keeps counting; long-press drops the SSR ≤1 tick and latches (`fault:true` in `/status`).

---

## Cross-cutting safety invariants (must hold across all phases)
- Absolute over-temp cutoffs (105 °C PTC, 85 °C chamber) and sensor-fault fail-close stay **compile-time**, untouched by any config / mode / button path.
- **SSR single-writer:** only `control_task`'s `ssr_set()` drives GPIO18. Every cross-task "off" is a mux-guarded latch that converges on the next tick (≤500 ms).
- **No unbounded unattended local heat:** AUTO bounded by live Moonraker data, DRYING by its ≤12 h timer, local POWER_ON by the runtime cap; boot never self-arms.
- Comms deadman is configurable but never disabled (≥10 s floor), and self-fed only while a local mode's liveness precondition holds — and even then it only defeats the 5-min timeout, never the over-temp/sensor cutoffs.

## Docs (each phase)
Update `README.md` status table + `docs/SAFETY.md`: the configurable target cap (bounded by the fixed 80 °C ceiling; 85/105 fixed), the boot-OFF/no-resume deviation from stock, the mode set, the K3 button semantics, and the LED meanings.

## Verification (end-to-end, per phase)
Build: `cd ~/git/OpenBreath && idf.py build`; flash `idf.py -p /dev/ttyUSB0 flash`. Device on the bench at `10.168.2.53`.
- **A:** `curl -X POST -H 'X-OpenBreath-Auth: web' '.../settings?max=90'` → clamps to 80; `comms_ms=5000` → clamps to 10000; lower `max` below an armed target → target pulled down; dashboard input `max` tracks `/status`; force a fault → K1 blinks; over-temp cutoffs unchanged.
- **B:** each mode drives the target; AUTO engages at the bed threshold and fails to 0 when Moonraker drops; drying counts down, auto-offs, and hard-caps at 12 h; reboot → OFF (no resume); over-temp still trips; `M141 S45` still heats (= POWER_ON). Verify via `curl /status` + `/mode` and (headless) render the dashboard.
- **C:** the bench checklist above.
Ship each phase as its own branch → CI/build → on-device test → PR/merge (and cut a release for firmware users when a phase is device-validated).
