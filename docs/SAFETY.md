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

## What is NOT protected
- **Over-current** is only the 6.3 A mains fuse. There is no PCB over-temp cutoff.
- Residual risk is chamber over-temp *up to the Layer-1 trip point* — identical
  to the stock firmware's failure ceiling.

## Rule for contributors
Any code path that can energize `PB_GPIO_RELAY` must be reachable only after the
`pb_heater` safety checks. Never drive the SSR GPIO directly from another module.
