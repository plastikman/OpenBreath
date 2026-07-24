# Safety model

The Panda Breath heats a chamber with a **PTC ceramic element** switched by a
**solid-state relay**. Custom firmware controls that heater, so the safety story
matters. Good news: there are **two independent hardware backstops** that do not
depend on any firmware — this firmware is the third, soft layer.

## Layer 1 (hardware) — bonded thermal cutoff
A cylindrical thermal protector (bimetal KSD-type disc or one-shot thermal fuse)
is spliced **in series with the PTC's mains lead** and bonded to the heater
frame (confirmed on the real unit, photo IMG_7631). It sits **upstream of the
SSR**, so it opens on element over-temp **regardless of firmware and regardless
of the SSR failing shorted** — the classic worst-case failure.

## Layer 2 (physics) — PTC self-limiting
The element is a **PTC ceramic**: its resistance rises sharply at the Curie
temperature, collapsing its own power, so it is strongly **self-limiting** rather
than thermally runaway-prone. A firmware bug or a welded SSR should at most drive
the chamber toward the element's self-limited equilibrium, which the Layer-1
cutoff bounds. This is a strong physical safety margin, **not an absolute
guarantee** — no firmware can promise the absence of every fault.

## Layer 3 (this firmware) — soft cutoffs
`pb_heater` enforces, every control tick, before any heat request:
- **PTC element over-temp** → force off at 105 °C (stock parity).
- **Chamber over-temp** → force off at 85 °C.
- **Sensor fault while heating** (open/short) → fail-closed.
- **Comms-loss watchdog** → if no controller link for 5 min while heating, latch
  off. (The device can't tell "idle hold" from "controller crashed", so a live
  link must be refreshed via `pb_heater_notify_link_alive()`.)
- **Set-point clamp** → target capped at `PB_HEATER_MAX_TARGET_C` (70 °C).
- **Boot state** → SSR forced OFF in `pb_heater_init()` before anything else runs;
  a hung control loop trips the task watchdog (`CONFIG_ESP_TASK_WDT_PANIC`).

## Front-panel panic-off (long-press)
Holding **any** front-panel button for 2 s latches the heater off and drops the
mode to OFF. It is called **panic-off**, deliberately, and is **not** a
safety-rated emergency stop — it is software running on the same MCU as the rest
of the control loop, so a firmware fault could defeat it. The Layer-1 bonded
cutoff and Layer-2 PTC physics remain the actual emergency layer.

How it stays inside the safety model:
- The button task never writes the SSR GPIO. `pb_policy_request_panic_off()`
  sets the same mux-guarded latch as an over-temp trip
  (`pb_heater_request_panic_off()`, no GPIO write) and then wakes the control
  task, which drops the SSR from its own context — preserving the
  single-SSR-writer invariant.
- The wake means the drop happens on the very next control-task scheduling, not
  at the next 500 ms periodic tick. Host tests verify that the wake is issued
  before diagnostic logging, and the physical Panda bench verified the complete
  panic-off transition. Electrical long-event-to-SSR-low latency has not been
  instrumented, so no sub-20-ms measurement is claimed; that trace is optional
  characterization, not a safety or release gate.
- The transition is attributed to the button (not the generic safety path), so
  it invalidates any remote control lease immediately; a reconnecting
  dashboard/Klipper cannot silently restore heat.

A **long-press on Power while a fault is latched** attempts a fault clear instead
of a redundant panic. The clear only succeeds if the underlying condition has
recovered — the next control tick re-evaluates safety and re-latches if the trip
still holds, so an accidental press cannot revive an unsafe device.

## What is NOT protected
- **Over-current** is only the 6.3 A mains fuse. There is no PCB over-temp cutoff.
- Residual risk is chamber over-temp *up to the Layer-1 trip point* — identical
  to the stock firmware's failure ceiling.
- **Panic-off is not an emergency stop** (see above): a firmware fault could
  defeat it. Do not rely on it as your only means of making the device safe.

## Rule for contributors
Any code path that can energize `PB_GPIO_RELAY` must be reachable only after the
`pb_heater` safety checks. Never drive the SSR GPIO directly from another module.
Cross-task "off" requests (buttons, HTTP, watchdogs) must go through a
mux-guarded latch that converges on the next `pb_heater_tick()` — never call
`pb_heater_emergency_off()` (which writes the GPIO) from a task other than the
control task.
