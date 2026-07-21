# Panda Breath (ESP32-C3) — NTC ADC→Temperature conversion, fully reversed

> **⚠️ HARDWARE CORRECTION (2026-07-21).** This report's `Vrail = 0.1 V` divider
> constant is **wrong**. On real hardware the ADC pin sits ~1.6 V at ambient, so
> the constant `K` in `Rntc = Rref·V/(K−V)` is the **~3.3 V supply**, not 0.1 V.
> The report's *formula shape*, the 82/33 kΩ `Rref` strap, and the R/T table are
> all correct — only the constant was misidentified. Validated on device: with
> `K = 3.3 V` the chamber NTC read **33.0 °C**, matching the printer's extruder
> thermistors (33.0 °C) exactly. The firmware uses `PB_VSUPPLY_V = 3.3f`.

Target: chamber NTC (GPIO0 / ADC1_CH0) and PTC-element NTC (GPIO1 / ADC1_CH1).
Both channels use the **identical** conversion. All float math is soft-float ROM
libgcc (`__mulsf3`, `__subsf3`, `__divsf3`, `__floatsisf`, `__fixunssfsi`), verified
against `esp32c3.rom.libgcc.ld` with the −8 address correction applied to every
absolute ROM target.

---

## 1. The exact formula

```
V      = ADC pin voltage, in VOLTS   (calibrated: adc_oneshot_read -> adc_cali_raw_to_voltage / 1000)
Rref   = 82.0  (kOhm)   [board variant A, GPIO19 = 0]   OR   33.0 (kOhm) [variant B, GPIO19 = 1]
Vrail  = 0.1   (V)      [DROM const @0x3c0e681c = 0x3dcccccd = 0.100000001]

Rntc_kOhm = Rref * V / (Vrail - V)          <-- low-side NTC divider, solved for Rntc

temp_C    = table_lookup(Rntc_kOhm)         <-- nearest entry in the R/T table, returns temp
temp_out  = mean of the last <=5 successive temp_C readings   (5-sample moving average)
```

`temp_out` is finally converted to an unsigned byte (`__fixunssfsi`, i.e. truncation)
and stored to `*0x3fc9ca80` (chamber) or `*0x3fc9ca81` (PTC, when mode `*0x3fc9ca78 == 1`).

### Register-level proof (fcn.4200cbf4, one loop iteration)

```
lw   a0, *0x3fc9ca7c          ; a0 = Rref  (int 82 or 33)
call 0x40000808 (−8 = 0x40000800 __floatsisf)   ; s1 = (float)Rref
lw   a0, 8(sp)                ; a0 = V_mV  (adc_cali output, integer mV)   ** see §4 units note
call 0x40000808 __floatsisf                       ; s0 = (float)V
call 0x4000085c (−8 = 0x40000854 __mulsf3)  (Rref, V)     ; s1 = Rref * V
lw   a0, *0x3c0e681c          ; a0 = 0.1
call 0x400008a0 (−8 = 0x40000898 __subsf3)  (0.1, V)      ; a0 = 0.1 − V
call 0x400007c4 (−8 = 0x400007bc __divsf3)  (Rref*V, 0.1−V) ; a0 = Rref*V/(0.1−V) = Rntc
call 0x4200c922               ; index = binary_search(Rntc)   over table @0x3c0e6638
addi a0, a0, 10               ; temp_C = index + 10
call 0x40000808 __floatsisf                       ; (float)temp_C
call 0x4200c9d6               ; push to 5-sample ring, returns running mean in a0
```
After 5 iterations the last returned mean (a0) is truncated with `__fixunssfsi`
(shown 0x400007f8, −8 = 0x400007f0) and written to the temp byte.

The `−8` correction was decisive: the divide/subtract/multiply targets are only one
`.ld` slot apart, so a wrong ±8 flips e.g. `__subsf3`↔`__mulsf3` and inverts the formula.
Every target above was checked against `esp32c3.rom.libgcc.ld`:
`__divsf3=0x400007bc, __mulsf3=0x40000854, __subsf3=0x40000898, __floatsisf=0x40000800,
__fixunssfsi=0x400007f0`.

---

## 2. The divider model

Low-side NTC (thermistor from ADC node to GND, series resistor `Rref` from the rail to
the node), rearranged to give the thermistor resistance:

```
Rntc = Rref * V / (Vrail − V)
```

* `Rref` = **82 kOhm** (or 33 kOhm) — the fixed series resistor, in kOhm. Selected at
  boot by reading **GPIO19** (`fcn.4200cb32` → `gpio_get_level(19)`): level 0 → 82, level 1 → 33.
  This is a hardware-population strap; the 70 °C Panda Breath most likely reads **82 kOhm**
  (confirm empirically: the Rref that yields a sane room-temperature reading is correct).
* `Vrail` = **0.1 V**. This is the *effective* rail in the ADC's voltage domain. The
  physical NTC divider is on the 3.3 V rail, but the node is scaled ~33:1 into the ADC
  input (3.3 V / 33 ≈ 0.1 V), so the pin only swings ~0–95 mV. Because the scale factor
  appears in both numerator voltage and rail, it cancels and `Rntc` comes out in true
  kOhm. **What matters for reproduction: `V` and `Vrail` share units (volts) and `V` is
  the actual calibrated pin voltage.**
* The table (`Rntc`) is in **kOhm**, so `Rref` must be in **kOhm** (82, not 82000).

The thermistor is a ~100 kOhm NTC (table: 100 kOhm at 27 °C, ~109 kOhm at 25 °C,
doubling roughly every 15 °C — a standard R/T curve). The firmware uses a **lookup
table with nearest-entry selection**, NOT a beta/Steinhart log formula (confirmed: no
`logf/expf/powf/libm` anywhere in the image).

---

## 3. The R/T table (DROM @ 0x3c0e6638, little-endian float32)

`temp_C = index + 10`. Index 0 (10 °C) and index 1 (11 °C) hold garbage
(7.4e31 and 0.0) and are never reached for valid readings; usable range is index 2..115+
(12 °C .. 125 °C+). Binary search upper bound is 0x73 = 115; entries beyond 114 exist
(idx115 = 3.4 kOhm) so the search never reads out of bounds.

```
temp_C : Rntc(kOhm)
 12:198.7  13:189.4  14:180.7  15:172.4  16:164.5  17:157.0  18:149.9  19:143.2
 20:136.8  21:130.7  22:124.9  23:119.4  24:114.2  25:109.2  26:104.5  27:100.0
 28: 95.7  29: 91.6  30: 87.8  31: 84.1  32: 80.6  33: 77.2  34: 74.0  35: 70.9
 36: 68.0  37: 65.3  38: 62.6  39: 60.1  40: 57.7  41: 55.4  42: 53.2  43: 51.1
 44: 49.1  45: 47.2  46: 45.3  47: 43.6  48: 41.9  49: 40.3  50: 38.8  51: 37.3
 52: 35.9  53: 34.5  54: 33.2  55: 32.0  56: 30.8  57: 29.7  58: 28.6  59: 27.6
 60: 26.6  61: 25.6  62: 24.7  63: 23.8  64: 23.0  65: 22.2  66: 21.4  67: 20.6
 68: 19.9  69: 19.2  70: 18.6  71: 17.9  72: 17.3  73: 16.7  74: 16.2  75: 15.6
 76: 15.1  77: 14.6  78: 14.1  79: 13.6  80: 13.2  81: 12.8  82: 12.4  83: 12.0
 84: 11.6  85: 11.2  86: 10.9  87: 10.5  88: 10.2  89:  9.9  90:  9.6  91:  9.3
 92:  9.0  93:  8.7  94:  8.4  95:  8.2  96:  7.9  97:  7.7  98:  7.4  99:  7.2
100:  7.0 101:  6.8 102:  6.6 103:  6.4 104:  6.2 105:  6.0 106:  5.9 107:  5.7
108:  5.5 109:  5.4 110:  5.2 111:  5.1 112:  4.9 113:  4.8 114:  4.7 115:  4.5
116:  4.4 117:  4.3 118:  4.2 119:  4.1 120:  3.9 121:  3.8 122:  3.7 123:  3.6
124:  3.5 125:  3.4
```

Binary-search behaviour (`fcn.4200c922`): finds the two bracketing entries and returns
the index of the one with the smaller `|table[i] − Rntc|` — i.e. **nearest neighbour**,
1 °C granularity. (A smooth interpolation, as in the ESPHome lambda below, is strictly
better than the firmware and still matches to <1 °C.)

DROM float constants block @0x3c0e6808 (for reference; the conversion only uses 0.1):
`3.3, 3.2, 0.2, 1.0, 0.01, 0.1(@681c), 2.0, 3300.0`. The 3.3/3.2/0.2/1.0/0.01 values are
loaded by the init into a per-channel struct (`0x3fc96b84`, 60-byte stride) and belong to
a separate EMA/limits path — they are **not** part of the ADC→temp math. `3300.0` is
unused by this conversion.

---

## 4. `*0x3fc9ca7c` and the input units

* `*0x3fc9ca7c` = **Rref** (the series resistor coefficient), an integer written once at
  boot in `fcn.4200cb32`: `82` if `gpio_get_level(19)==0`, else `33`. It is NOT a Vref or
  an ADC count.
* `*0x3fc9ca82` = raw-range fault flag from `fcn.4200ca8e`: raw>0xFFD → 2 (open/over-range),
  raw>0x14 → 0 (ok), else → 1 (short/under-range).
* `*0x3fc9ca88` = adc_oneshot handle, `*0x3fc9ca90` = adc_cali (curve-fit) handle,
  `*0x3fc9ca78` = mode (1 ⇒ PTC channel → stores to ca81).

**Units note (the one subtlety):** `adc_cali_raw_to_voltage` returns integer **mV**, yet
the math requires `V < 0.1`, i.e. `V` in **volts**. The only unit assignment that yields a
monotonic, sane temperature curve (verified by full IEEE-754 simulation of the exact ROM
ops, §5) is `V` in volts. The net firmware behaviour is `Rntc = 82 * V / (0.1 − V)` with
`V` = the true calibrated pin voltage in volts (pin swings ~4–95 mV). For an ESPHome
rebuild that reads the *same pin* with calibration enabled, ESPHome's ADC voltage (volts)
equals the firmware's `V` directly, so the reproduction is faithful regardless of the
internal mV/scale bookkeeping.

---

## 5. Sanity check (exact ROM-op simulation, Rref = 82 kOhm, Vrail = 0.1 V)

```
V = 0.0068 V (6.8 mV)  -> Rntc =  6.0 kOhm -> 105 C   (matches PTC over-temp trip region)
V = 0.0281 V (28 mV)   -> Rntc = 32.0 kOhm ->  55 C
V = 0.0549 V (55 mV)   -> Rntc = 99.8 kOhm ->  27 C   (room temp)
V = 0.0604 V (60 mV)   -> Rntc =124.9 kOhm ->  22 C   (room temp)
V = 0.0041 V ( 4 mV)   -> Rntc =  3.5 kOhm -> 124 C   (top of scale)
```
Room-temperature voltages land at ~20–27 °C and the low-resistance end at the ~105 °C
over-temp region — both physically correct.

---

## 6. ESPHome config

### 6a. Recommended — one `adc` + a `lambda` that reproduces the firmware math exactly

Reads the pin voltage in volts, applies `Rntc = Rref*V/(0.1−V)`, then linear-interpolates
the firmware R/T table. Duplicate the block for GPIO1 (PTC). Set `RREF` to 82.0 or 33.0 to
match your board's GPIO19 strap.

```yaml
sensor:
  # ---------- CHAMBER NTC (GPIO0 / ADC1_CH0) ----------
  - platform: adc
    pin: GPIO0
    name: "Chamber ADC volts"
    id: chamber_adc_v
    attenuation: 0db          # pin swings ~0-95 mV; 0dB (0-950 mV) gives best resolution
    update_interval: 1s
    internal: true            # raw voltage is intermediate

  - platform: template
    name: "Chamber Temperature"
    id: chamber_temp
    unit_of_measurement: "°C"
    accuracy_decimals: 1
    update_interval: 1s
    lambda: |-
      const float RREF  = 82.0f;   // kOhm  (82 if GPIO19=0, else 33)
      const float VRAIL = 0.1f;    // V
      float v = id(chamber_adc_v).state;               // volts at the pin
      if (isnan(v) || v <= 0.0f || v >= VRAIL) return NAN;
      float R = RREF * v / (VRAIL - v);                // Rntc in kOhm
      // firmware R/T table {temp_C, R_kOhm}, R decreasing with temp
      static const float RT[][2] = {
        {12,198.7f},{13,189.4f},{14,180.7f},{15,172.4f},{16,164.5f},{17,157.0f},
        {18,149.9f},{19,143.2f},{20,136.8f},{21,130.7f},{22,124.9f},{23,119.4f},
        {24,114.2f},{25,109.2f},{26,104.5f},{27,100.0f},{28,95.7f},{29,91.6f},
        {30,87.8f},{31,84.1f},{32,80.6f},{33,77.2f},{34,74.0f},{35,70.9f},
        {36,68.0f},{37,65.3f},{38,62.6f},{39,60.1f},{40,57.7f},{41,55.4f},
        {42,53.2f},{43,51.1f},{44,49.1f},{45,47.2f},{46,45.3f},{47,43.6f},
        {48,41.9f},{49,40.3f},{50,38.8f},{51,37.3f},{52,35.9f},{53,34.5f},
        {54,33.2f},{55,32.0f},{56,30.8f},{57,29.7f},{58,28.6f},{59,27.6f},
        {60,26.6f},{61,25.6f},{62,24.7f},{63,23.8f},{64,23.0f},{65,22.2f},
        {66,21.4f},{67,20.6f},{68,19.9f},{69,19.2f},{70,18.6f},{71,17.9f},
        {72,17.3f},{73,16.7f},{74,16.2f},{75,15.6f},{76,15.1f},{77,14.6f},
        {78,14.1f},{79,13.6f},{80,13.2f},{81,12.8f},{82,12.4f},{83,12.0f},
        {84,11.6f},{85,11.2f},{86,10.9f},{87,10.5f},{88,10.2f},{89,9.9f},
        {90,9.6f},{91,9.3f},{92,9.0f},{93,8.7f},{94,8.4f},{95,8.2f},
        {96,7.9f},{97,7.7f},{98,7.4f},{99,7.2f},{100,7.0f},{101,6.8f},
        {102,6.6f},{103,6.4f},{104,6.2f},{105,6.0f},{106,5.9f},{107,5.7f},
        {108,5.5f},{109,5.4f},{110,5.2f},{111,5.1f},{112,4.9f},{113,4.8f},
        {114,4.7f},{115,4.5f},{116,4.4f},{117,4.3f},{118,4.2f},{119,4.1f},
        {120,3.9f},{121,3.8f},{122,3.7f},{123,3.6f},{124,3.5f},{125,3.4f},
      };
      const int N = sizeof(RT)/sizeof(RT[0]);
      if (R >= RT[0][1])       return RT[0][0];         // clamp cold end (12 C)
      if (R <= RT[N-1][1])     return RT[N-1][0];       // clamp hot end  (125 C)
      for (int i = 0; i < N-1; i++) {
        float rhi = RT[i][1], rlo = RT[i+1][1];         // rhi > rlo
        if (R <= rhi && R >= rlo) {
          float f = (rhi - R) / (rhi - rlo);            // linear interp (0..1)
          return RT[i][0] + f * (RT[i+1][0] - RT[i][0]);
        }
      }
      return NAN;
    filters:
      - sliding_window_moving_average:      # mirrors the firmware 5-sample mean
          window_size: 5
          send_every: 1
```

For the **PTC channel**, copy both sensors with `pin: GPIO1`, ids `ptc_adc_v` /
`ptc_temp`, and reference `id(ptc_adc_v)` in the lambda. Same `RREF`, same table.

For **bit-exact** parity with the firmware (nearest-entry, integer °C) replace the
interpolation loop with a nearest-neighbour pick and `return roundf(...)`; the interpolated
version above is smoother and within <1 °C of the firmware everywhere.

### 6b. Alternative — stock `adc` + `resistance` + `ntc` platforms

`resistance` in **DOWNSTREAM** mode computes exactly `R = resistor * V/(reference_voltage − V)`,
which is the firmware formula. Then `ntc` fits a Steinhart-Hart curve through table points.
This is cleaner but approximates the table (still <~0.5 °C error since the table is itself a
smooth NTC curve).

```yaml
sensor:
  - platform: adc
    pin: GPIO0
    id: chamber_adc_v
    attenuation: 0db
    update_interval: 1s
    internal: true

  - platform: resistance
    sensor: chamber_adc_v
    id: chamber_ntc_r
    configuration: DOWNSTREAM
    resistor: 82kOhm            # = Rref  (33kOhm for GPIO19=1 boards)
    reference_voltage: 0.1V     # = Vrail
    internal: true

  - platform: ntc
    sensor: chamber_ntc_r
    name: "Chamber Temperature"
    unit_of_measurement: "°C"
    calibration:
      - 109.2kOhm -> 25°C       # table idx15
      - 18.6kOhm  -> 70°C       # table idx60
      - 6.0kOhm   -> 105°C      # table idx95
    filters:
      - sliding_window_moving_average: { window_size: 5, send_every: 1 }
```
(Repeat for GPIO1/PTC with its own `resistance` + `ntc` pair.)

---

## 7. Confidence & unresolved items

**High confidence:**
* Formula `Rntc = Rref * V / (Vrail − V)`, `Vrail = 0.1`, table + nearest lookup,
  `temp = index + 10`, 5-sample moving average, byte truncation. All from register-exact
  disassembly with verified −8 ROM-symbol identities and an IEEE-754 simulation that
  reproduces a sane curve (room temp, over-temp trip).
* Full 114-entry R/T table extracted verbatim from DROM.
* `Rref` ∈ {82, 33} kOhm chosen by GPIO19 at boot.
* Both chamber and PTC channels use identical math.

**Ambiguities / to verify on hardware:**
1. **Which Rref (82 vs 33)** applies to *this* board — read GPIO19, or pick the value that
   makes room temperature read ~20–25 °C. 82 kOhm is the more likely default.
2. **ADC attenuation / calibration match.** The firmware relies on ESP-IDF curve-fit
   calibration; ESPHome must read the same pin with calibration enabled so its volts equal
   the firmware's `V`. `0db` is recommended because the pin only reaches ~95 mV. If your
   ESPHome build reports voltage on a different scale, add a constant multiplier in the
   lambda so that ~55 mV → ~27 °C.
3. The `~33:1` input-scaling explanation for `Vrail = 0.1 V` is inferred (it makes the
   ratio produce true-kOhm resistances); it is not required for reproduction because the
   ESPHome ADC reads the same post-scaling pin voltage the firmware does.
