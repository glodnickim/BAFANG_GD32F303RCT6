# Bafang M820 stock firmware reverse engineering - knowledge base for EVistDrive

Status: living document, 2026-08-20

## 0. Purpose

This document is a reusable engineering knowledge base extracted from stock Bafang M820 controller firmware. The goal is not blind source-level cloning. The goal is to understand proven mechanisms, identify good design patterns, preserve safety properties, and make them portable to EVistDrive and other motor-controller projects.

Primary stock image used for the newest conclusions:

- file: `CRX30PC3612E102003.5-CR X30P.250.FC 2.1.bin`
- size: 92620 bytes
- SHA256: `c93f892c7dcb506f6674e56ef3f888307ea185494cf3e48caf00ecf0ff56a4e6`
- application flash base: `0x08005000`

Secondary image used for cross-checking:

- `FT_2026_05_22_w1(1).bin`
- contains an update/container header before the application image.

## 1. Confidence legend

- **CONFIRMED** - direct instruction/data-flow evidence in stock firmware, or two independent exact paths agree.
- **STRONG** - multiple consistent clues; safe as a design hypothesis, but one physical mapping or unit is still missing.
- **HYPOTHESIS** - plausible interpretation that must not be hard-coded as fact.
- **OPEN** - not yet resolved.
- **REJECTED** - an earlier interpretation disproved by deeper tracing.

Important correction history:

- **REJECTED:** phase-current zero around 1024 ADC and `x32` internal scaling.
- **CONFIRMED replacement:** phase-current zero is around 2048 ADC12 and stock current normalization is `16 * (zero - sample)`.
- **REJECTED:** stock value `700` as a phase-current maximum. The traced field belongs to a different torque/telemetry/control path.

---

# 2. Architecture summary

Stock separates the fast motor-control path from slower engineering-unit and user-interface paths.

```text
PWM / rotor position
      |
      v
select observable current window
      |
      v
TIM1 CH4 -> injected ADC trigger
      |
      v
PA2 / PA3 / PA5 phase-current samples
      |
      +-> per-channel zero compensation
      +-> 2-of-3 selection
      +-> third-current reconstruction
      +-> current plausibility / protection
      |
      v
Clarke -> Park -> current controllers -> inverse transform / SVPWM
      |
      v
TIM1 CCR1/CCR2/CCR3
```

Slower paths independently handle battery current, battery voltage, temperature, BEMF/phase-voltage diagnostics, CAN telemetry, assist limits and protection state machines.

A recurring stock design principle is:

```text
measure -> validate -> normalize -> use
```

rather than:

```text
read ADC -> immediately feed controller
```

---

# 3. PWM / TIM1 and bridge lifecycle

## 3.1 TIM1 configuration - CONFIRMED

- center-aligned PWM
- ARR approximately `2000`
- PSC `0`
- CH1/CH2/CH3 PWM outputs with complementary outputs
- initial CCR1/CCR2/CCR3 = `1000`
- deadtime field = `25`
- OSSR/OSSI enabled
- MOE is the main bridge-output enable/disable mechanism
- the timer itself remains running during a normal stop

## 3.2 Neutral PWM - CONFIRMED

With ARR=2000:

```c
CCR1 = 1000;
CCR2 = 1000;
CCR3 = 1000;
```

This produces zero differential phase voltage while preserving a known center/common-mode state.

## 3.3 Normal stop sequence - CONFIRMED

Stock does not treat every stop as a fault shutdown.

```text
requested torque/current -> 0
        |
        v
FOC/PWM remains active
        |
        v
current/torque ramp reaches exact zero
        |
        v
MOE = 0
        |
        v
reset FOC / PI / transient state
        |
        v
CCR1=CCR2=CCR3=1000
        |
        v
IDLE, timer still running
```

This is a high-value pattern for eliminating stop clicks and restart kicks.

## 3.4 Start sequence - CONFIRMED

Stock resets/initializes control state before enabling the bridge output:

```text
known neutral CCRs
Iq_ref = 0
Id_ref = 0
controller transient state reset
        |
        v
MOE = 1
        |
        v
torque/current ramp from zero
```

EVistDrive recommendation: keep separate APIs for graceful stop and safety shutdown.

```c
motor_request_stop();
motor_emergency_shutdown(reason);
```

Do not implement graceful stop as an alias of hard bridge-off.

---

# 4. ADC architecture

## 4.1 Analog GPIO coverage - CONFIRMED

Stock configures all external ADC-capable pins in these groups as analog:

- PA0..PA7
- PB0..PB1
- PC0..PC5

ADC1 and ADC2 are used together. Regular conversions feed DMA; fast motor-current channels use injected conversions synchronized to PWM.

## 4.2 Regular sequence - CONFIRMED

ADC1 regular ranks:

| Rank | Channel | Pin | Current interpretation |
|---:|---:|---|---|
| 1 | 1 | PA1 | unresolved/reference candidate |
| 2 | 0 | PA0 | battery/current-related path |
| 3 | 10 | PC0 | phase-voltage/BEMF set |
| 4 | 17 | internal | VREFINT |
| 5 | 12 | PC2 | phase-voltage/BEMF set |
| 6 | 13 | PC3 | battery/DC-link voltage |
| 7 | 11 | PC1 | phase-voltage/BEMF set |
| 8 | 1 | PA1 | duplicate sample |

ADC2 regular ranks:

| Rank | Channel | Pin | Current interpretation |
|---:|---:|---|---|
| 1 | 7 | PA7 | torque/load candidate, special filtering |
| 2 | 14 | PC4 | unresolved auxiliary |
| 3 | 15 | PC5 | unresolved plausibility/auxiliary |
| 4 | 8 | PB0 | unresolved auxiliary |
| 5 | 9 | PB1 | motor NTC |
| 6 | 4 | PA4 | unresolved auxiliary |
| 7 | 6 | PA6 | unresolved auxiliary |
| 8 | 0 | PA0 | battery/current-related path |

## 4.3 Injected sequence - CONFIRMED

ADC1 injected:

- rank 1: PA2 / ADC1_IN2 = phase-current channel A
- rank 2: PA0 / ADC1_IN0 = battery-current / fast auxiliary current path

ADC2 injected:

- rank 1: PA3 / ADC2_IN3 = phase-current channel B
- rank 2: PA5 / ADC2_IN5 = phase-current channel C

Do not assign physical motor letters U/V/W to PA2/PA3/PA5 until the PCB net ordering is verified. The firmware proof establishes three logical current channels, not the silkscreen phase order.

---

# 5. Phase-current measurement

## 5.1 Per-channel startup offset calibration - CONFIRMED

Stock calibrates each phase-current amplifier independently.

- 16 samples per channel
- separate A/B/C offsets
- zero expected near ADC12 midpoint
- accepted zero region approximately:

```text
1920 < zero_adc < 2176
```

Nominal:

```text
zero_adc approximately 2048
```

Design lesson: never assume all three amplifiers have one identical hard-coded offset.

Recommended EVistDrive improvement:

- use 64 or 128 samples if startup time permits
- calculate mean, min, max and variance/peak-to-peak
- reject an offset that has a valid mean but excessive noise
- retain last-known-good calibration only with explicit validity metadata

## 5.2 Stock normalized current representation - CONFIRMED

The authoritative stock relation is:

```c
I_internal = 16 * (ADC_zero - ADC_sample);
```

Therefore:

```text
1 raw ADC12 count = 16 stock internal current units
```

Sign convention is such that increasing ADC relative to zero produces negative current in this logical orientation.

This native scale is used by the fast fixed-point control path. The FOC itself does not need a conversion to physical amperes.

## 5.3 Physical phase-current gain - OPEN, bounded strongly

Exact stock `A / ADC count` is still not proven directly from the stock binary.

Independent M820 EBiCS work uses an empirical phase-current calibration close to:

```text
95 mA / raw ADC count
```

Stock battery-current scaling and EBiCS cross-checks make roughly `95..101 mA/count` a strong candidate region, with `100 mA/count` aesthetically plausible, but **do not hard-code 100 mA/count as a proven Bafang constant yet**.

Given stock x16 normalization:

- if 95 mA/count -> 1 internal unit = 5.9375 mA
- if 100 mA/count -> 1 internal unit = 6.25 mA

## 5.4 PWM-synchronized current sampling - CONFIRMED, extended reverse 2026-08-25

Injected ADC is triggered from TIM1 CH4 / PWM timing. The sample instant changes dynamically with the current SVPWM geometry.

Exact stock 2.1 anchors:

```text
0x08007604  fast current-loop ordering
0x0800A9E4  SVPWM + sample-window selection + CCR1..CCR4 producer
0x0800B17C  phase-current acquisition/reconstruction + INVALID consumer
```

Key state:

```text
0x200004E4  SVPWM/sampling sector 0..6
0x200004F0  CH4 polarity selector
0x200004F1  sample_state: 0 INVALID, 1 PRIMARY, 2 ALTERNATE
0x200004FA..0x20000500  CCR1..CCR4 mirrors
```

**Important correction:** `0x200004E4` is produced from SVPWM vector geometry/signs, not directly from the Hall state. Therefore the recovered 2-of-3 mapping is a **SVPWM-sector mapping**.

Known constants:

```text
PWM ARR                 2000
fallback/current edge   CCR4 = 1999
end-window margin       243 timer counts
minimum usable gap      372 timer counts
sample offset           129 timer counts
```

Exact sector-dependent decision with `CCR1=r0`, `CCR2=r1`, `CCR3=r2`:

| SVPWM sector | Primary condition | Alternate condition | Alternate CCR4 |
|---:|---|---|---|
| 1 | `2000-r0 > 243` | `r0-r1 >= 372` | `r0-129` |
| 2 | `2000-r1 > 243` | `r1-r0 >= 372` | `r1-129` |
| 3 | `2000-r1 > 243` | `r1-r2 >= 372` | `r1-129` |
| 4 | `2000-r2 > 243` | `r2-r1 >= 372` | `r2-129` |
| 5 | `2000-r2 > 243` | `r2-r0 >= 372` | `r2-129` |
| 6 | `2000-r0 > 243` | `r0-r2 >= 372` | `r0-129` |

Semantics:

```text
PRIMARY:
    CCR4 = 1999
    sample_state = 1

ALTERNATE:
    CCR4 = selected_compare - 129
    sample_state = 2

INVALID:
    CCR4 = 1999
    sample_state = 0
```

### Sample-state/trigger coherency - CONFIRMED

The fast current-loop consumes the current sample first and only later computes the next SVPWM/CCR4:

```text
ADC sample generated by previously programmed CCR4
        -> 0x0800B17C consumes previously latched sector/sample_state
        -> Clarke/Park/current control
        -> 0x0800A9E4 computes next sector/sample_state/CCR4
        -> CCR1..CCR4 written for the next sample
```

This is the key portable pattern. A conversion must be interpreted using the state that was prepared together with that conversion's trigger, not a newly computed unrelated sector state.

### CH4 polarity/edge - CORRECTED

Previous documentation stated that stock dynamically switched CH4 polarity/edge. Exact stock 2.1 tracing does not support that claim.

The code contains a generic CC4P update:

```text
selector 0 -> TIM1_CCER bit 0x2000 cleared
selector 1 -> TIM1_CCER bit 0x2000 set
```

but `0x0800A9E4` sets the selector at `0x200004F0` to zero before every normal sector/window decision, and no other real producer of this RAM byte was found in the analyzed image. Therefore normal stock 2.1 current sampling **forces CC4P=0** rather than switching it dynamically by sector.

Do not use the old "dynamic edge switching" statement as evidence for EVistDrive. EVistDrive's TIMER0_CH3 trigger half-cycle/edge still needs target-specific confirmation.

### Invalid-sample handling - CONFIRMED

`0x0800B17C` backs up the last phase-current state before consuming the new conversion. For `sample_state != 0`, it reads the appropriate injected JDRs and performs sector-specific reconstruction. For `sample_state == 0`, it does not feed those fresh JDR values into FOC; it restores the previous current state and reconstructs the third phase from the held pair.

There is no separate stock `sample_age` timeout found on this path in 2.1. Adding bounded age/quality metadata in EVistDrive is an improvement beyond stock.

## 5.5 2-of-3 shunt selection and reconstruction - CONFIRMED

Stock does not blindly trust all three current channels at every duty/sector. It selects the two observable shunts and reconstructs the third from Kirchhoff current balance:

```c
Ia + Ib + Ic = 0;
```

Recovered sector mapping:

| SVPWM sector | Direct channels | Reconstructed |
|---|---|---|
| 1, 6 | B + C | A = -(B+C) |
| 2, 3 | A + C | B = -(A+C) |
| 4, 5 | A + B | C = -(A+B) |

## 5.6 Invalid-sample handling - CONFIRMED

If there is no trustworthy sampling window, stock does not inject a known-bad fresh ADC sample into FOC. In stock 2.1, `sample_state == 0` causes `0x0800B17C` to restore the previous phase-current state and reconstruct the third phase from that held pair.

**Important boundary:** no separate stock `sample_age` counter or timeout was found on this current-sampling path. Consecutive INVALID windows may therefore continue to reuse the previous valid current state. A bounded age is an EVistDrive safety/diagnostic improvement, not a recovered Bafang feature.

Recommended EVistDrive extension:

```c
if (current_sample_fresh && current_sample_valid) {
    update_phase_currents();
    current_sample_age = 0;
} else {
    use_last_valid_currents();
    current_sample_age++;
}
```

Expose counters by SVPWM sector, duty and RPM so diagnostics can quantify how often samples are rejected and how long held samples survive.

---

# 6. Multi-stage phase-current protection

## 6.1 Temperature-compensated threshold generator - CONFIRMED math, STRONG physical interpretation

Function around `0x0800E98C` reads signed temperature at RAM `0x200002CE` in 0.1 C units and generates a piecewise-linear scalar `q`.

At major breakpoints:

| Temp C | q |
|---:|---:|
| -20 | 192 |
| 0 | 217 |
| 20 | 248 |
| 40 | 281 |
| 60 | 320 |
| 80 | 363 |
| 100 | 409 |
| 120 | 455 |
| 140 | 506 |
| 160+ | 563 |

Three current thresholds are then generated:

```c
T1 = min((q * 0x22E8) >> 8, 28500);
T2 = min((q * 0x2BA2) >> 8, 30000);
T3 = min((q * 0x345D) >> 8, 31500);
```

RAM:

```text
T1 -> 0x2000035E
T2 -> 0x20000360
T3 -> 0x20000362
```

Representative values:

| Temp C | T1 int | T2 int | T3 int | T1 ADC delta | T2 ADC delta | T3 ADC delta |
|---:|---:|---:|---:|---:|---:|---:|
| -20 | 6702 | 8377 | 10053 | 418.9 | 523.6 | 628.3 |
| 0 | 7574 | 9468 | 11362 | 473.4 | 591.8 | 710.1 |
| 20 | 8656 | 10820 | 12986 | 541.0 | 676.3 | 811.6 |
| 40 | 9808 | 12260 | 14714 | 613.0 | 766.3 | 919.6 |
| 60 | 11170 | 13962 | 16756 | 698.1 | 872.6 | 1047.3 |
| 80 | 12670 | 15838 | 19007 | 791.9 | 989.9 | 1187.9 |
| 100 | 14276 | 17845 | 21416 | 892.3 | 1115.3 | 1338.5 |
| 120 | 15882 | 19852 | 23825 | 992.6 | 1240.8 | 1489.1 |
| 140 | 17662 | 22078 | 26495 | 1103.9 | 1379.9 | 1655.9 |
| 160 | 19652 | 24565 | 29480 | 1228.3 | 1535.3 | 1842.5 |

Because these thresholds increase strongly with temperature, they are unlikely to be ordinary thermal derating limits. The strongest current interpretation is analog current-sense/protection compensation versus temperature. This physical interpretation remains **STRONG**, not yet final.

## 6.2 ISR overcurrent voting - CONFIRMED

The fast path computes absolute magnitudes for all three phase-current quantities and compares each phase against T1/T2/T3.

- any phase > T1 -> flag `0x20000694 = 1`
- any phase > T2 -> flag `0x20000695 = 1`
- any phase > T3 -> increment consecutive-hit counter `0x20000671`
- no phase > T3 -> reset that counter

When the highest threshold is exceeded for more than 5 consecutive checks:

```text
counter reset
fault latch 0x2000041B = 1
motor state 0x20000453 = 9
TIM1 MOE = OFF
```

Thus the hard trip requires approximately 6 consecutive accepted detections rather than one isolated spike.

This is a highly reusable safety pattern:

```text
soft threshold 1 -> early limiting
soft threshold 2 -> stronger limiting
hard threshold 3 -> consecutive-event vote -> bridge shutdown
```

## 6.3 What T1/T2 do before hard trip - NEW CONFIRMED link

The ramp/current-reference function around `0x0800EB3C` consumes flags T1/T2. Therefore the lower thresholds are not just diagnostic bits; they actively alter how quickly the current/torque command is reduced.

Recovered examples from this path:

- when the higher soft flag (T2) is active and the internal reference is above the low region, stock can subtract a large block (`500` internal units) in one update;
- when T1 is active, another branch accelerates downward adjustment by a smaller step (observed `5` internal units in that branch);
- without those flags, normal adaptive ramp-down uses much gentler 1/2/3/4-step behavior depending on feedback/error state.

Design lesson: stock layers fast protection into the reference/ramp path before escalating to hard MOE shutdown. That produces a softer first response while preserving an independent hard cutoff.

Open task: fully name every state variable in this limiter and reconstruct the entire T1/T2 branch as clean pseudocode.

---

# 7. Battery current PA0

## 7.1 Exact physical gain - CONFIRMED

Stock CAN `0x3201` publishes controller current in `A * 100` units.

The stock PA0 conversion path is equivalent to:

```c
can_current_x100 = delta_adc * 1000 / 255;
```

Therefore:

```text
I_bat[A] = delta_adc * 10 / 255
```

Exact gain:

```text
1 ADC count = 0.039215686 A
            = 39.215686 mA
25.5 ADC counts = 1 A
```

This is one of the strongest physical calibrations recovered without external measurement equipment.

## 7.2 Battery-current temperature drift compensation - CONFIRMED

Stock maintains a tracked compensation temperature and moves the battery-current zero by one ADC count for each approximately 1.3 C temperature step.

Relevant state:

```text
0x200002CE current temperature, 0.1 C
0x200002D4 tracked compensation temperature
0x200002D6 initialization/state flag
```

Logic concept:

```c
if (temp_x10 > tracked_x10 + 13) {
    tracked_x10 += 13;
    battery_zero_adc -= 1;
}
else if (temp_x10 < tracked_x10 - 13) {
    tracked_x10 -= 13;
    battery_zero_adc += 1;
}
```

Equivalent drift slope:

```text
~0.769 ADC count / C
~30.2 mA apparent-current correction / C
```

This is a valuable lesson for accurate low-current telemetry and battery-current limiting.

Do not copy the compensation blindly without preserving its stock enable/validity conditions. A zero-offset adaptation algorithm must never learn real load current as zero.

---

# 8. Battery/DC-link voltage PC3

PC3 / ADC1_IN13 is averaged over 8 samples.

Recovered stock conversion:

```c
internal = 10 * floor(adc12 * 693 / 4096);
```

Interpretation is consistent with approximately 0.1 V quantization and a roughly 69.3 V ADC full-scale battery/DC-link measurement.

For engineering-unit telemetry, preserve integer order if stock-compatible quantization matters. For EVistDrive internal control, use a higher precision calibrated representation and quantize only at protocol/UI boundaries.

---

# 9. Phase voltage / BEMF on PC0, PC1, PC2

## 9.1 Three-channel set - CONFIRMED

The firmware treats PC0/PC1/PC2 as a coordinated three-phase measurement set.

Regular ranks are deliberately interleaved in ADC1 DMA sequence.

## 9.2 Per-channel baseline - CONFIRMED

Stock learns/stores a separate reference for each phase-voltage channel rather than using one common zero.

## 9.3 Filtering - CONFIRMED

Each channel has a 4-sample moving average:

```c
avg = (s0 + s1 + s2 + s3) >> 2;
```

Further state logic also uses later multi-sample confirmation/averaging.

## 9.4 BEMF domain - CONFIRMED

The important stock math is performed in ADC counts relative to the learned baseline, e.g. conceptually:

```c
bemf = (avg > baseline) ? (avg - baseline) : 0;
```

Hall-sector logic selects/compares channels, with some paths dividing a selected delta by two.

The control/diagnostic logic therefore does **not** require physical volts first.

Physical `V/count` for PC0/PC1/PC2 remains **OPEN**. It is useful for model-based diagnostics and engineering telemetry, but not required to reproduce the known stock BEMF logic.

---

# 10. Rotor position interface

## 10.1 Firmware-visible interface - CONFIRMED

Three digital rotor-position inputs are read on PC6/PC7/PC8 as a 3-bit UVW/Hall state.

Valid 6-step sequences recovered:

```text
one direction: 1 -> 5 -> 4 -> 6 -> 2 -> 3 -> 1
reverse:       1 -> 3 -> 2 -> 6 -> 4 -> 5 -> 1
```

There are six accepted transitions per electrical revolution.

## 10.2 Physical source - STRONG

The hardware map indicates an MT6816 encoder configured to UVW output mode. Firmware itself only proves the UVW/Hall-compatible electrical interface, not whether the source is a dedicated magnetic encoder or three discrete Hall elements.

Design lesson: make the control core consume a generic validated six-state rotor interface and isolate physical sensor implementation behind a driver.

---

# 11. PAS / crank quadrature

Two digital channels are used as a quadrature-like crank/PAS input:

- PD2
- PC12

Recovered forward sequence:

```text
00 -> 10 -> 11 -> 01 -> 00
```

Reverse sequence is the opposite ordering and is not counted as forward cadence.

Confirmed resolution:

```text
16 quadrature cycles / crank revolution
x4 transitions / cycle
= 64 accepted transitions / crank revolution
```

There is software sampling/debounce, approximately a few consecutive stable samples before accepting a change.

---

# 12. Motor temperature NTC

PB1 / ADC2_IN9.

Stock reduces ADC12 to ADC10:

```c
raw10 = adc12 >> 2;
```

Conversion uses a 191-entry lookup table spanning approximately:

```text
-40 C ... +150 C
1 C table spacing
```

with linear interpolation to approximately 0.1 C output resolution.

Stock does not use a runtime Beta/Steinhart-Hart calculation.

Portable lesson: LUT + interpolation gives deterministic fixed-point cost and allows the exact real production sensor curve to be encoded rather than assuming an ideal thermistor equation.

---

# 13. Assist / battery-current configuration

A stock configuration structure around RAM `0x200006E2` contains defaults including:

```text
config[0] = 32
config[1] = 12
```

and a nine-level percentage table:

```text
20, 25, 30, 35, 42, 50, 60, 70, 100
```

A parallel table is initialized to 100% values.

The first table is strongly tied to current/power limiting; the protocol's `0x6011` block also exposes nine current-limit percentages and nine speed-limit percentages.

Strong interpretation:

```text
base battery-current limit approximately 12 A
x per-assist current percentage
-> battery-current target/limiter
```

Illustrative default targets if `config[1]` is 12 A:

| Level | Percent | Battery current target |
|---:|---:|---:|
| 1 | 20 | 2.40 A |
| 2 | 25 | 3.00 A |
| 3 | 30 | 3.60 A |
| 4 | 35 | 4.20 A |
| 5 | 42 | 5.04 A |
| 6 | 50 | 6.00 A |
| 7 | 60 | 7.20 A |
| 8 | 70 | 8.40 A |
| 9 | 100 | 12.00 A |

Continue to treat exact field naming as **STRONG** until the entire producer/consumer path is labeled.

---

# 14. Reusable stock patterns worth adopting in EVistDrive

## Priority P0 - motor quality and control correctness

1. **PWM-synchronized injected ADC current acquisition.**
2. **Dynamic sampling-window selection instead of a fixed ADC instant.**
3. **2-of-3 observable-shunt selection plus third-current reconstruction.**
4. **Reject/hold invalid current samples rather than filtering garbage afterward.**
5. **Separate A/B/C startup offset calibration with plausibility checks.**
6. **Graceful stop: ramp to zero before MOE off.**
7. **Neutral CCR state before/after bridge transitions.**
8. **Reset current-controller transient state before normal restart.**

## Priority P1 - safety architecture

1. **Multiple current thresholds with staged reaction.**
2. **Soft limiting before hard shutdown.**
3. **Consecutive-hit voting for hard overcurrent, not one noisy sample.**
4. **Separate normal stop and emergency shutdown paths.**
5. **Rotor-state transition plausibility.**
6. **Calibration validity and last-known-good handling.**
7. **Measurement-validity metadata in the control layer.**

## Priority P2 - accuracy and diagnostics

1. Battery-current exact engineering-unit calibration.
2. Temperature-dependent current-offset drift correction.
3. ADC VREF monitoring/correction if stock path proves it is actively used.
4. Physical phase-current A/count calibration.
5. Physical phase-voltage V/count calibration.
6. Diagnostic counters for invalid samples, overcurrent threshold hits and per-sector observability.

---

# 15. Recommended EVistDrive improvements beyond stock

Stock mechanisms are useful, but EVistDrive can make them easier to verify and diagnose.

## 15.1 Measurement quality object

Instead of passing a bare integer current sample:

```c
typedef struct {
    int16_t ia;
    int16_t ib;
    int16_t ic;
    uint8_t sector;
    uint8_t valid_mask;
    uint8_t sample_quality;
    uint16_t sample_age;
} phase_current_sample_t;
```

Suggested qualities:

```text
GOOD
MARGINAL
HELD_LAST_VALID
INVALID
```

## 15.2 Better offset calibration

Add diagnostics stock does not expose:

```text
mean
min/max
peak-to-peak
variance
number of rejected samples
calibration temperature
calibration timestamp / boot counter
```

## 15.3 Protection diagnostics

Keep counters such as:

```text
T1 hit count
T2 hit count
T3 hit count
hard-trip consecutive maximum
invalid ADC-window count by sector
invalid ADC-window count by duty bin
last trip currents and CCRs
last trip temperature
```

This turns a black-box protection event into actionable engineering data.

## 15.4 Engineering units outside the ISR

Keep fast FOC in native fixed-point/native-current units. Convert to A/V/W in a slower telemetry layer.

```text
fast ISR: ADC -> normalized current -> FOC
slow path: normalized current -> A -> logging/protection display
```

Avoid float conversions in the critical ISR unless the target architecture and benchmark justify them.

---

# 16. Do not copy blindly

The following are not yet proven enough for a hard-coded portable implementation:

- exact physical phase-current gain in A/count for the stock hardware
- exact physical V/count for PC0/PC1/PC2
- exact U/V/W physical ordering of the three phase-current and phase-voltage ADC channels
- full semantics of all auxiliary ADC channels PA1/PA4/PA6/PB0/PC4/PC5
- exact use of VREFINT for runtime compensation
- complete true thermal-derating algorithm distinct from analog current-sense compensation
- complete semantics of every T1/T2 soft-limit branch
- exact pole-pair count from firmware

Rule for porting:

```text
If a physical unit, polarity, phase order or fault meaning is not proven, preserve it as a named raw/native parameter until hardware evidence closes it.
```

---

# 17. Open reverse-engineering targets

Current work queue:

1. Fully reconstruct the T1/T2 soft current-limiter behavior in `0x0800EB3C` and name every participating variable.
2. Trace RAM `0x20000368`, `0x200005B8`, `0x200005BC` and determine whether this is a true thermal derating / overload integrator or another compensation path.
3. Find a stock path that mathematically connects phase-current native units to a known physical ampere field, to close phase A/count without external lab equipment.
4. Determine whether stock estimates battery current from Iq/modulation anywhere, and compare it with measured PA0.
5. Trace VREFINT consumers and determine if ADC gain is supply-compensated at runtime.
6. Resolve physical U/V/W channel order from firmware-to-PWM relationship plus PCB evidence.
7. Recover physical scaling of PC0/PC1/PC2 if any stock service/diagnostic function exposes it.
8. Identify the full thermal derating curve and its interaction with current limits.
9. Label remaining ADC channels and their plausibility/fault checks.

---

# 18. Evidence anchors / important addresses

These addresses refer to the analyzed stock application linked at `0x08005000`.

| Address | Role |
|---|---|
| `0x0800AF5C` | phase-current offset calibration |
| `0x0800B17C` | fast phase-current selection/reconstruction path |
| `0x0800BE70` vicinity | TIM1 update / FOC fast path |
| `0x0800C020` vicinity | abs phase-current checks vs T1/T2/T3 |
| `0x0800CCF8` | TIM PWM output/MOE control helper |
| `0x0800E98C` | temperature-dependent T1/T2/T3 generator |
| `0x0800EB3C` | torque/current command ramp and protection response |
| `0x08010F38` vicinity | NTC conversion path |
| `0x08012D90` | FOC/control reset; restores neutral CCRs |
| `0x080180E4` vicinity | motor lifecycle state machine |
| `0x08018850` vicinity | temperature-driven battery-current zero compensation |

Key RAM:

| RAM | Current interpretation |
|---|---|
| `0x200002CE` | signed temperature, 0.1 C |
| `0x2000035E` | current protection T1 |
| `0x20000360` | current protection T2 |
| `0x20000362` | current protection T3 |
| `0x20000694` | T1 exceeded flag |
| `0x20000695` | T2 exceeded flag |
| `0x20000671` | consecutive T3 hit counter |
| `0x2000041B` | hard current/fault latch path |
| `0x20000453` | motor state; value 9 used on hard overcurrent trip |
| `0x20000660/64/68` | three phase-current-related fast variables used by protection |
| `0x20000370/74/78` | their absolute magnitudes for protection comparisons |

---

# 19. Reverse-engineering methodology used

The following workflow has proven reliable and should be reused for other controller projects:

1. Establish container/header size and linked application address.
2. Recover vector table and interrupt ownership.
3. Map peripheral base-address accesses to ADC/TIM/GPIO/CAN.
4. Recover ADC rank/channel setup before assigning sensor names.
5. Trace DMA unpacking into RAM and build a RAM slot map.
6. Trace each RAM slot forward to consumers, not just backward from an assumed sensor label.
7. For physical scaling, require a closed chain to a known protocol unit or known hardware ratio.
8. For protection thresholds, trace both threshold generation and every consumer/reaction.
9. Distinguish normal state transitions from fault transitions.
10. Maintain rejected hypotheses explicitly so old incorrect conclusions do not re-enter new code.
11. Cross-check with independent firmware/projects only after stock math is understood; never use external code as proof of stock behavior by itself.
12. When a result affects safety, require at least one second evidence path or bench test before changing production firmware.

---

# 20. Porting checklist for another motor/controller

Before reusing an M820 mechanism on another motor model:

- [ ] ADC pin/channel map verified for that hardware
- [ ] current-sense polarity verified
- [ ] per-phase zero region measured/verified
- [ ] injected trigger source and timer edge verified
- [ ] PWM period/ARR translated; do not reuse raw 243/372/129 counts blindly
- [ ] shunt observability sectors verified for that modulation topology
- [ ] deadtime and gate-driver polarity verified
- [ ] neutral PWM definition verified
- [ ] rotor sensor sequence and phase order verified
- [ ] physical current gain calibrated or left in native units
- [ ] current protection thresholds translated into the target current-sense domain
- [ ] hard shutdown path independently tested
- [ ] graceful stop/start lifecycle tested separately from fault stop
- [ ] thermal compensation and thermal derating treated as different mechanisms
- [ ] CAN/UI engineering units kept outside the critical FOC ISR where possible

---

# 21. Current conclusion

The most valuable knowledge recovered from the M820 is not a single magic gain or PI constant. It is the architecture around measurement quality and state transitions:

```text
sample only when observable
calibrate each channel
reject invalid data
reconstruct mathematically when needed
use staged protection
vote before hard trip when safe
ramp to zero for normal stop
hard-disable independently for faults
keep neutral bridge state deterministic
```

These principles are directly reusable in EVistDrive even when the next motor has different shunts, ADC channels, pole count, voltage dividers or current gain.
