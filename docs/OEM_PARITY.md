# OEM feature parity

DragonBreath is not intended to reproduce the stock firmware byte-for-byte. This
matrix records which user-visible Panda Breath behaviors are implemented, still
planned, deliberately changed for safety, or intentionally omitted.

Current as of **v0.3.0**.

| Stock/OEM behavior | DragonBreath status | Notes |
|---|---|---|
| Manual chamber target | **Implemented** | Local Web UI and Klipper `M141`/`M191`; device-side regulation and limits remain authoritative. |
| Follow-printer-bed automatic mode | **Partial** | Policy/API support is present: live Moonraker bed temperature, 3 °C disengage hysteresis, and fail-off on disconnect. The shipped dashboard control is not yet accepted as end-to-end validated. |
| Timed filament drying | **Partial** | Policy/API support is present: a target plus bounded 1–12 hour duration and automatic shutoff. The shipped dashboard control is not yet accepted as end-to-end validated; named material presets remain planned. |
| Chamber and PTC temperature display | **Implemented** | Both sensors and their health are exposed in the dashboard and API v2 state. |
| Sensor-fault and over-temperature shutdown | **Implemented** | Heater fails closed; fixed 85 °C chamber and 105 °C PTC cutoffs are not user-configurable. |
| Fan follows heater | **Implemented** | TRIAC is held on/off and never phase-angle PWM'd. |
| Residual-heat fan purge | **Partial** | Current cooldown is session-gated. A persisted, opt-in temperature-latched policy is planned; the default will remain session-gated so a warm idle chamber does not start the fan unexpectedly. Fault airflow remains unconditional. |
| Front-panel mode LEDs | **Implemented** | All four outputs are driven from the authoritative policy snapshot: Power is solid whenever the device is up and blinks on fault; On, Auto, and Dry each light for their own mode. Auto slow-blinks when armed but not engaged (no Moonraker link, or bed below the threshold). Power is release-only — GPIO21 is also the serial console TX. |
| Front-panel buttons | **Implemented** | All four (Power GPIO9, Auto GPIO8, On GPIO10, Dry GPIO2) are polled with debounce and short/long-press detection. A short press toggles the button's labeled mode (arming from the remembered parameters); a 2 s long-press latches a panic-off, except a long-press on Power while faulted attempts a fault clear. Every button action is attributed to the panel and invalidates any remote lease. A button held at power-on is ignored until released (GPIO9/8/2 are strapping pins). |
| Local status/configuration UI | **Implemented** | Responsive work continues, but manual, automatic, drying, safety settings, setup, and OTA controls are present. |
| Wi-Fi captive setup and mDNS | **Implemented** | Product identity is DragonBreath; reachable as `dragonbreath.local` when mDNS works. |
| Firmware update | **Implemented** | Like stock, DragonBreath accepts a local firmware upload from its Web UI. DragonBreath additionally authenticates the write, verifies project identity, refuses updates while heating, and uses dual-slot rollback. |
| Printer integration | **Implemented for Klipper** | Both stock and DragonBreath can read Klipper bed state through Moonraker. DragonBreath also exposes heater control through its Klipper helper/API v2. Stock additionally supports Bambu MQTT and Home Assistant MQTT; DragonBreath intentionally focuses on Moonraker/Klipper. |
| Home Assistant MQTT discovery | **Intentionally omitted** | DragonBreath uses HTTP/JSON plus Server-Sent Events; no MQTT broker is required. |
| Stock WebSocket API and Web UI | **Intentionally omitted** | API v2 and the DragonBreath dashboard are the supported interfaces. |
| Boot/resume active heating | **Intentionally changed** | DragonBreath always boots OFF and never restores active heat, timers, or leases after reboot. |
| Persistent fault latch | **Planned** | Current fault state is RAM-only. NVS persistence is tracked as Phase B2 safety work. |
| Fan-only filtration mode | **Unverified / optional** | Not required for parity until stock behavior is confirmed; may be added as a DragonBreath enhancement. |

Safety takes precedence over exact OEM behavior. See [SAFETY.md](SAFETY.md) for
the enforced invariants and
[the iteration-2 plan](../plans/iteration-2-stock-parity-and-config.md) for
remaining implementation work.
