# Changelog

All notable changes to eVistDrive firmware, summarized per release. This is a concise,
English summary for the public repository — day-to-day development detail is kept in
local (untracked) notes.

## [Unreleased]

### FW-125 — phase-current same-path calibration (root-cause fix for CASE C)

- Purpose: fix a confirmed domain mismatch in the FW-118/119 phase-current zero calibration.
  Calibration measured the ADC0 REGULAR scan (`adc_value[4]/[7]/[8]`); the FOC ISR reads phase
  current from `adc_inserted_data_read()` on each phase's OWN ADC instance (A=ADC2, B=ADC1,
  C=ADC0) — a different silicon ADC than ADC0's regular scan for phases A and B, each with its
  own independent zero-current offset. FW-122 caught the real-bike signature: at a fixed small
  `FinalIq` (e.g. 14), `IqActual` swung from +8 to -15 depending on the Hall sector alone, and a
  constant post-Park error vector fit that data with R² > 0.9 — the signature of an offset bug,
  not a controller or torque-filter bug.
- Fix: calibration now accumulates `i16_ph1/2/3_current` — the exact `adc_inserted_data_read()`
  values the FOC ISR consumes — captured inside `ADC0_1_IRQHandler` itself, in a new
  `phase_cal_acc` accumulator active only during the startup calibration window (bridge OFF).
  `current_cal_submit()`'s candidate offset is now that measured mean directly; the old second
  subtraction of the hardware IOFFx constant is removed, since `adc_inserted_data_read()` already
  returns `raw - IOFFx` in hardware. The calibration domain is therefore a small SIGNED residual
  around 0, not a ~2048 unsigned ADC12 code — `current_cal_attempt_t.sum/min/max` and
  `current_cal_t.residual_mean`/`lkg_residual_mean` (renamed from `zero_adc`/`lkg_zero_adc`) are
  now signed.
- FW-119's retry → last-known-good → legacy/strict policy is unchanged in shape; only the
  numbers it operates on changed domain. `CURRENT_CAL_SAMPLES` raised 64 → 128 (still ~8 ms at
  the 16 kHz PWM/ISR rate the accumulator now samples at, vs. the old 4 kHz TIMER1-paced regular
  scan). New provisional validation constants `CURRENT_CAL_RESIDUAL_MIN/MAX` (±300 LSB) replace
  the old regular-ADC-domain `CURRENT_ZERO_MIN/MAX_ADC`.
- DIAG-only zero-current self-test: a second accumulator, `phase_cal_verify_acc`, captures the
  signal AFTER the software-offset correction (not the same pre-correction data the calibration
  itself measures) and exposes `fw125_zero_current_selftest_valid/mean[]/p2p[]` — independent
  proof the correction lands near zero, not an assumption.
- Host tests: `fw125_phase_current_calibration_host.c` (T5-T13 — exact mean/P2P over varying
  samples, atomic publish, STRICT/LEGACY_FALLBACK on the new domain, signed correction exact at
  0/+5/-7, a three-independent-phase-delta oracle) and `fw125_wiring_guard_host.c` (source-text
  guard on main.c: T1-T4 same-path source mapping per phase, a regression guard that fails if
  `adc_value[4]/[7]/[8]` ever reappears in calibration, T13 capture-order, T14/T15 DIAG
  isolation). `fw119_current_cal_host.c`/`fw119_current_cal_wiring_host.c` updated for the new
  signed/no-hardware-offset domain; the FW-119 policy tests (T1-T8) are otherwise unchanged.
  Full `tests/host/run-host-tests.ps1`: PASS.
- Scope: does NOT touch PI gains, ride_control, the min-Iq floor, start/stop lifecycle (D2,
  neutral dwell, MOE sequencing), rotor direction, the torque path, or the dynamic PWM-
  synchronized ADC sampling window (a second suspect the audit found but this card deliberately
  does not fix - see the FW-125 doc's "falsification" section for the follow-up condition).
- Awaiting real-bike test: DIAG build, startup calibration readout, standstill starts at low
  torque, then 20-30 assist→coast→reapply cycles watching for `CAL≈14` behavior; FW-122 capture
  if CASE C still occurs.

### FW-121.0B — arm → abort chronology instrumentation

- Purpose: resolve the contradiction FW-121.0A found. The arm gate can only pass with POEN clear,
  yet the ride log showed the sweep aborting on POEN with zero samples counted — while the only
  runtime POEN enable is preceded by a blocking 25 ms delay that should have produced ~400 counted
  interrupts first. Nothing recorded today says which premise is false. This card adds the
  recording; it changes nothing about when or whether the sweep arms or aborts.
- `arm_isr_count` is incremented at the top of the active ISR path, **before** the MOE guard, so
  the first interrupt after arming is unambiguously number 1. The per-point `isr_count` keeps its
  old position and meaning exactly — what the sweep measures is untouched.
- Three frozen snapshots, each written once and immutable afterwards: **ARM** (on the IDLE→RUNNING
  transition itself), **ABORT** (before the state changes, so it records the cause not the
  aftermath) and **POEN ENABLE** (the first `timer_primary_output_config(TIMER0, ENABLE)` seen
  while the sweep is RUNNING). Each carries control tick, TIMER0 CNT, POEN/DIR/PWM_ON/CUTOFF/
  standstill/running/iq flags, `bridge_lifecycle` and |i_q_setpoint|.
- The decisive discriminator is a single bit: `SNAP_POEN_WHILE_RUNNING`. Set + abort means a real
  bridge start interrupted the sweep. Clear + abort means POEN was already high when the arm gate
  read it as low — and the contradiction is on the arm side.
- The module stays hardware-free: main.c reads the registers and hands the snapshot in as data,
  which is what keeps the whole chronology executable in host tests.
- CAN: schema 5 → 6. `0x10240` drops four build constants (production CCR3, point count, ticks per
  point, CCR3 step — all fixed at compile time, and the step is the difference between two
  consecutive point frames' Data1) and carries the chronology instead. After an abort
  `0x10244/45/46` carry the ARM / ABORT / POEN snapshots, tagged `0xA1/0xA2/0xA3` in Data0 — values
  a point frame (`0x80|index`) can never produce, so no state lookup is needed to decode them.
- Host tests T1–T10 added: snapshot written once, immutable under later riding, first ISR is 1,
  abort at ISR 1 reports 1, abort at ISR 400 reports 400, an enable outside RUNNING never sets the
  discriminator bit, a clean sweep behaves exactly as before, and the full frame contract.
  The main.c wiring guard gained W8 (hook existence, placement relative to the enable, single
  site, entry-captured registers) — mutation-tested: moving the hook before the enable or deleting
  it fails the guard.
- Builds: **DIAG=0 still bit-identical** — 99 852 B / 12 032 B, SHA-256 `9AACC1B9…503CD47`, the same
  hash as every build since FW-120.1. DIAG=1 141 776 B / 45 584 B (+48 B RAM, measured 180 B module
  state, budget line 244 B, diagnostic total 34 232 / 34 816 B).
- No change to the ARM condition, the abort guard, CCR3, the sweep, the ADC trigger, FOC/PI,
  reconstruction, or FW-117/118/119/120.1.
- Status: `DIAGNOSTIC_READY / HW_MEASUREMENT_PENDING`. Artifact `0.0404_M820_BL820.bin`.

### FW-121.0 final diagnostic transport fix — 0.0403
- Second confirmed defect from the ride-log decode: the seven sweep frames used EFIDs
  `0x1022F..0x10235`, which FW-112 A/B (`0x1022F..0x10233`) and FW-117 (`0x10234..0x10238`)
  already own. The log did contain those ids - carrying somebody else's records. Moved to
  `0x10240` (status) and `0x10241..0x10246` (points 0..5).
- The ids and both byte layouts now live in `inc/adc_trigger_diag.h`, and the frames are built by
  the module itself. main.c contains no FW-121.0 CAN id literals at all any more.
- New `inc/diag_efid_map.h`: every diagnostic id block expressed in terms of its owning module's
  own defines, with pairwise-disjointness `_Static_assert`s. A collision is now a build failure.
  Verified by mutation: pointing FW-121.0 back at `0x1022F` fails the build on both the FW-112 A/B
  and FW-117 asserts.
- `DIAG_AGGREGATE_SNAPSHOT_MAX` stays at 21 - exactly 14 fixed + 7 FW-121.0, no more. main.c also
  asserts that its own fixed-frame count equals the module's declared first index (14), so the
  absolute index of every frame is a compile-time fact.
- Host tests: the FW-121.0 suite gained S9, which checks the whole transport contract against the
  real module - idx 14..20 -> `0x10240..0x10246`, `Data0 = 0x80|index`, `Data1 = CCR3-3500`
  (F0 C8 A0 78 50 28), plus that one past the last sub-frame returns false and that an unreached
  point still names its CCR3 without the complete bit. The isolation guard gained W7 (no magic
  EFIDs left in main.c, the map header is compiled in).
- Builds: DIAG=0 unchanged and still byte-identical (99 852 B / 12 032 B, SHA-256
  9AACC1B9...503CD47). DIAG=1 140 764 B / 45 536 B. Numbered artifact `0.0403_M820_BL820.bin`.
- No change to the ADC trigger, the CCR3 sweep, FOC, reconstruction, FW-121 or FW-122.

### FW-121.0 fix — the sweep frames never reached the bus
- Symptom: a DIAG=1 ride log contained every other diagnostic frame but none of 0x1022F /
  0x10230..0x10235.
- Root cause: `DIAG_AGGREGATE_SNAPSHOT_MAX` in diag_session.c was 14 and the aggregate block now
  builds 21 frames. The snapshot loop stops at the cap, so the seven FW-121.0 frames were built
  by main.c and then dropped before ever being frozen into a session summary - silently: no
  error, no counter, nothing in the log. The header in diag_session.c had warned in as many
  words ("Do not add a 15th aggregate frame without re-measuring the budget"); the card added
  seven and checked only its own module's RAM line item.
- Fix: cap raised 14 -> 21 (cost 7 x 12 B x 4 summaries = 336 B, re-measured: diag_session.c
  1660 B, line item 1724 B, diagnostic budget total 34 196 / 34 816 B).
- So it cannot happen again: the cap moved to inc/diag_session.h and main.c now asserts its own
  aggregate frame count against it at compile time. Adding a frame without raising the cap is now
  a build failure instead of a silent loss. Verified by mutation - the assert fires.
- Builds: DIAG=0 still byte-identical (99 852 B / 12 032 B, SHA-256 9AACC1B9...503CD47);
  DIAG=1 140 652 B flash / 45 536 B RAM (+336 B, exactly the cap's cost).
- No change to motor control, to the sweep algorithm or to ADC timing.

### ADC Trigger Timing Diagnostic — FW-121.0
- Measurement card, not a fix. FW-121 could not be designed because neither the code nor the
  vendor headers say WHICH edge of TIMER0_CH3 actually starts the injected conversion: the CC3
  event (both matches under CAM=11, so the up-count match wins and the second is swallowed by the
  conversion in progress) or the rising edge of OC3REF (the down-count match only). The two differ
  by 2 x (_T - CCR3) counts and demand OPPOSITE corrections to CCR3, so guessing had a 50% chance
  of pushing the worst-case sample out of the low-side conduction window instead of into it.
- A new diagnostic-only module (`src/adc_trigger_diag.c`) sweeps CH3 through
  3740/3700/3660/3620/3580/3540, holding each point 400 control ticks (100 ms), and records per
  point: injected-ISR count, TIMER0 CNT at ISR entry (min/max), counter direction, and whether
  ADC0 and ADC2 had finished their own injected conversions when ADC1's interrupt fired.
- Two independent readings come out of one sweep. The SIGN of d(CNT)/d(CCR3) decides the trigger
  edge without needing to know the interrupt latency (which then falls out of the intercept), and
  the ISR-per-tick count decides whether a second conversion starts in the same PWM period once
  the two CC3 matches are further apart than the conversion is long — 4 per tick means one, 8
  means two.
- Three interlocks, because this moves the instant the FOC's own current samples are taken: it
  arms only behind the same confirmed-standstill gate that guards Hall autodetect AND a dark
  bridge; every injected ISR re-reads TIMER0's own output-enable bit and restores the production
  CCR3 in that same call if the bridge came up, bounding exposure to one PWM period with no
  current flowing; and CCR3 is clamped inside the module to [3540, 3740]. It arms once per power
  cycle and never re-arms.
- Diagnostics: schema version 4 -> 5, new frames 0x1022F (sweep status) and 0x10230..0x10235 (one
  per sweep point). No existing frame layout changed.
- Host tests: two new suites (sweep behaviour against the real module, driven at the real
  16 kHz / 4 kHz ratio; plus an isolation guard that walks main.c's preprocessor nesting and
  proves every call into the module sits inside `#if CAN_DIAGNOSTICS_ENABLE`).
- Builds: **DIAG=0 is byte-identical to the FW-120.1 build** — same 99 852 B flash, same 12 032 B
  RAM, same SHA-256 9AACC1B9...503CD47. DIAG=1 140 648 B flash / 45 200 B RAM (+1964 B / +128 B).
- Correction to the FW-121 audit note: the GD32F303 ADC clock maximum is 40 MHz, so the existing
  APB2/6 = 20 MHz is comfortably in specification. The prescaler is not touched and there is no
  accuracy concern from it.
- Status: `DIAGNOSTIC_READY / HW_MEASUREMENT_PENDING`. FW-121 proper stays blocked until the
  sweep has been run on the bench.

### Reconstruction State Timing — FW-120.1
- The 2-of-3 phase-current reconstruction (rebuild the phase whose shunt has no usable low-side
  window, from the other two through `Ia + Ib + Ic = 0`) already existed and predates this card.
  What was wrong was WHICH PWM period it was deciding for: the pair was chosen by
  `dyn_adc_state()` in the PREVIOUS ISR, from the `switchtime[]` of the period BEFORE the one
  that was actually sampled. Every crossing of the duty ranking therefore fed Clarke one
  corrupted current — the sample from the phase that could not be measured.
- The verdict is now taken at the top of the ISR, from the `switchtime[]` still in the array at
  that moment — those are the CCRs that shaped the period the samples came from, because this
  ISR's own `FOC_calculation()` has not run yet. No algorithm, no reconstruction math, no ADC
  trigger and no PI behaviour changed; only the point at which the existing decision is made.
- The two jobs the old function conflated are now separate and separately named:
  `dyn_adc_state_select()` / `dyn_adc_state_reconstruct()` in a new hardware-free module
  (`src/dyn_adc_state.c`) answer "which pair describes the sample in hand", while
  `dyn_adc_trigger_update()` in main.c keeps its old, legitimately forward-looking job of arming
  CH3 for the next acquisition. Its behaviour is unchanged, ties included.
- Host tests: two new suites. The module one drives the real module through a model of the whole
  pipeline (switchtime -> CCR -> counter-top sample -> ISR) across a 360° duty sweep in both
  rotation directions and the three named ranking crossings C->A, A->B, B->C, with a negative
  control that replays the pre-card ordering and fails on exactly one period per crossing. The
  wiring guard proves src/main.c really uses that order and that nothing re-decides after FOC.
- Builds: DIAG=0 99 852 B flash / 12 032 B RAM, DIAG=1 138 684 B flash / 45 072 B RAM, both clean
  apart from the same pre-existing warnings. No new RAM: the module is stateless.
- Status: `VERIFIED_SOFTWARE / HW_PENDING` — not yet ridden. Sampling-window validity (is the
  chosen pair itself sampled inside a valid window?) is deliberately untouched and remains FW-121.

### Current Calibration Safety — FW-119
- The FW-118 zero calibration now has a defined policy for failing: retry (up to
  `CURRENT_CAL_MAX_ATTEMPTS`, default 3), then a last-known-good set, then either the legacy
  hardware-offset path or an inhibited start. Before this, one bad attempt silently dropped the
  firmware onto the legacy path and set a status byte nothing acted on.
- The policy lives in a new hardware-free module (`src/current_cal.c`): main.c owns the ADC
  sampling and hands each attempt in as a plain accumulation, which is what makes the whole
  policy executable in the host tests.
- A failing attempt can never overwrite offsets the ISR is using, nor the last-known-good set.
  LKG is RAM/session-level only — no flash persistence was added, and its scope is documented
  explicitly: within one power cycle it is the last attempt that passed validation.
- Start policy is configurable: `LEGACY_FALLBACK` (default) keeps the pre-FW-118 behaviour and
  flags it as degraded; `STRICT` inhibits FOC start. The default must stay `LEGACY_FALLBACK`
  until the FW-118 MIN/MAX/P2P limits are measured — they are still PROVISIONAL, and a hard
  inhibit on top of a guessed threshold can immobilise a working bike.
- Statuses: UNCALIBRATED / OK / OUT_OF_RANGE / TOO_NOISY / SAMPLE_TIMEOUT / USING_LKG /
  LEGACY_FALLBACK / HARD_FAILED. Values 0..3 keep their exact FW-118 numeric meaning. Diagnostics
  (attempt count, final status, active source RUNTIME/LKG/LEGACY, valid, failure reason) are one
  struct; the CAN protocol was deliberately NOT extended.
- No second start machine: the policy gates the SAME bridge-start condition the FW-117 neutral
  dwell lifecycle hangs off, and calibration completes before the main loop, so offsets can never
  move under a live FOC. With the shipped default the gate always allows FOC, so bike behaviour
  is unchanged from FW-118.
- Fixed in passing: the mean was computed as `sum >> 6` against a configurable
  `CURRENT_CAL_SAMPLES`, so changing that constant would have produced a wrong offset. It now
  divides by the samples actually taken — identical result at the default 64.
- Host tests: 40/40 suites pass (two new FW-119 suites). Builds: DIAG=0 99 848 B flash /
  12 032 B RAM, DIAG=1 138 680 B flash / 45 072 B RAM, both clean apart from pre-existing
  warnings. RAM cost: +40 B.
- Status: `VERIFIED_SOFTWARE / HW_PENDING` — every FW-118 bench measurement remains open, and
  `STRICT` must not be enabled before it is done.


### Independent Phase Current Zero Calibration — FW-118
- Runtime calibration of per-phase ADC current offsets (PA2=A, PA3=B, PA5=C) replaces the
  hardcoded hardware offsets (2020/2028/2012) that were never tuned to the actual EVistDrive
  hardware. The Clarke/Park transforms now see ~0 at zero current, eliminating the residual bias
  (up to +36 LSB) that existed before.
- 64-sample calibration runs at startup (after `adc_config()`, before FOC), reading raw ADC values
  from the regular ADC scan (`adc_value[4,7,8]`) while the bridge is OFF. A timeout of 100,000
  iterations prevents deadlock if TIMER1/DMA is not running.
- Software offset = calibrated mean − hardware offset. In the ISR, the subtraction is guarded by
  `current_calibration_valid`: when calibration fails (timeout, out-of-range, or too noisy), the
  firmware falls back to legacy hardware-offset-only behavior — no change from pre-FW-118.
- P2P (peak-to-peak) noise is recorded for all three phases. Range validation uses a provisional
  ±200 LSB window around the nominal 2048 ADC12 midpoint. Both limits are marked
  PROVISIONAL / NEEDS HARDWARE VALIDATION and must be tuned after bench measurement.
- PA0 battery current remains on its own independent calibration path (`bat_current_offset`) —
  not affected by FW-118.
- Host tests: 38/38 suites pass. Builds: DIAG=0 and DIAG=1 compile clean (only pre-existing
  warnings). Artifacts: `0.0401_M820_BL820.bin` (DIAG=0, 98 724 B).
- Status: `VERIFIED_SOFTWARE / HW_PENDING` — needs bench measurement of actual zero ADC values
  for phases A/B/C, actual P2P, and final tuning of MIN/MAX/P2P limits.

### STEP 2A: neutral dwell before FOC start
- After MOE ON (bridge output enable), the firmware now holds all three phase CCRs at the neutral
  value (`_T/2 = 1875`) for a configurable number of real PWM/ISR cycles before releasing the FOC
  algorithm to write active CCR values. This eliminates the audible click/jerk at motor start
  caused by FOC immediately writing non-neutral voltages the instant the bridge goes live.
- Lifecycle states (`BRIDGE_LIFECYCLE_IDLE → NEUTRAL_COMMIT → MOE_ON → NEUTRAL_DWELL →
  FOC_RELEASE → RUN`) are managed in main.c with the ISR gating FOC via the `neutral_dwell_active`
  flag. During the dwell, the ISR writes `_T/2` to all three channels and decrements the counter;
  on reaching zero it signals the main loop, which transitions through FOC_RELEASE to RUN.
- Configurable constants in `inc/config.h`: `START_NEUTRAL_DWELL_CYCLES` (default 4 = 250 µs @ 16 kHz
  PWM) and `START_DWELL_TIMEOUT_CYCLES` (default 100 = main-loop failsafe). The dwell counter runs
  at ISR frequency (16 kHz); the timeout runs at main-loop frequency (~4 kHz).
- Lifecycle state is reset to IDLE on every bridge-off path: soft cutoff, hard cutoff,
  `power_off_controller()`, and the dwell-timeout failsafe. This prevents a stale lifecycle from
  causing an unguarded restart.
- Host test suite extended with a source-text wiring guard (T1-T6) that verifies the structural
  invariants: neutral CCR preload before MOE ON, ISR dwell guard, exact counter decrement, lifecycle
  progression, and reset on all shutdown paths. All 38 host suites pass.
- Artifacts: `0.0393_M820_BL820.bin` (DIAG=0) and `0.0394_M820_BL820.bin` (DIAG=1). Bench
  measurement required to validate the 4-cycle dwell value; no `VERIFIED_BENCH` status without
  physical testing.

### HMI multiframe ACK gate — FW-114
- Factory M510 controllers pace a READ reply to the display (target=3) as LONG_START, wait for the
  display's NORMAL_ACK (`8312XXXX`, ~30 ms), and only then send the DATA/END frames. EVistDrive
  previously sent START+DATA+END back-to-back and ignored the ACK, which left the Controller Info
  tab dependent on the display's tolerance.
- Replies to target=3 now gate the DATA phase on the HMI ACK, with a 100 ms (400 tick @ 4 kHz)
  fail-safe timeout, matching the factory behaviour. The ACK is accepted only while a target=3
  transfer is active, the command matches, and the current fragment is the START; a stale ACK from
  a finished transfer can never release a later transfer, and ERROR_ACK never releases the gate.
- CANable/BESST reads (target=5) are untouched — they never wait for an HMI ACK. The 0x6012
  trailing marker (`821B6012 01 00 02 06`) stays a phase of the same automaton and is produced
  only after the END frame is confirmed, so START → DATA → END → trailer order is preserved.

### Controller session counter — FW-114
- The `0x82F83000` status broadcast previously carried a frozen `00 00 00 0B`. It now carries the
  factory payload: byte0 = session counter incremented once per 10 s from boot
  (`floor(uptime/10 s) mod 256`), bytes 1–3 zero. Counter semantics and payload are verified;
  the stock transmit cadence of this frame is still pending hardware verification.

### FW-114 audit build — 0.0373
- Production code unchanged after the audit. Host test suite extended with G6 (stale ACK must not
  release a new same-command transfer), G7 (ACK timeout survives the uint32 tick wrap) and G8
  (exact START → DATA → END → trailer order for 0x6012 target=3). All 26 host suites pass.
- Artifact `0.0373_M820_BL820.bin` (DIAG, text 132940 / data 268 / bss 29188) is READY FOR FLASH;
  only the pre-existing warnings remain (`-Wpointer-sign` in `CAN_Display.c`, unused `fw_ver`).
- Open (hardware, not a bug): stock transmit cadence of `0x82F83000`.

### Calmer high assist levels: per-level dynamics defaults
- All five levels previously shared almost the same assist dynamics. A higher assist ratio
  multiplies the same rider torque change into a much larger motor-torque request, which made
  L4/L5 noticeably more nervous than the lower levels for the same riding input.
- The compiled-in factory defaults now deliberately slow the rise/fall dynamics as assist
  rises, so the reaction stays calm at high power and high cadence. Only the *default values* of
  the existing per-level parameters changed — the `assist_dynamics` algorithm is untouched.

  | Level | Assist | Power rise | Power fall | Iq rise slow | Iq rise fast | Iq fall slow | Iq fall fast | Release |
  |---|---|---|---|---|---|---|---|---|
  | L1 | 100% | 150 ms | 375 ms | 600 ms | 300 ms | 1000 ms | 180 ms | 650 ms |
  | L2 | 200% | 160 ms | 400 ms | 600 ms | 330 ms | 1000 ms | 210 ms | 650 ms |
  | L3 | 320% | 190 ms | 450 ms | 650 ms | 380 ms | 1050 ms | 250 ms | 650 ms |
  | L4 | 420% | 220 ms | 500 ms | 700 ms | 450 ms | 1100 ms | 300 ms | 650 ms |
  | L5 | 520% | 250 ms | 550 ms | 750 ms | 500 ms | 1200 ms | 350 ms | 650 ms |

- `release_ms` stays 650 ms on every level. The change is factory/default only: stored user
  banks are never overwritten — the bank blob stays v8 with the same record layout, no EEPROM or
  CAN change — and the new values apply to factory reset, creating a new default bank, or
  restoring defaults. The Canable UI placeholder/defaults were synced to the same values.
- Not touched: the `assist_dynamics` algorithm, torque filtering, cadence compensation, safety/
  hard cut, release behaviour.

### Adaptive Iq ramp cadence range for the M820
- The SLOW→FAST interpolation of the adaptive Iq ramp used 20–70 rpm. On the M820 that range
  sat too low: from 70 rpm the whole normal riding cadence already ran fully on the FAST
  characteristic.
- The compile-time thresholds are now 50–110 rpm, so FAST builds up across the cadence band the
  M820 actually uses: 50 rpm = 0% FAST (full SLOW), ~17% at 60, ~33% at 70, ~50% at 80, ~67% at
  90, ~83% at 100, 100% at 110 rpm. Only the input bounds changed — the interpolation math, the
  per-level Iq rise/fall values and every filter are untouched.
- The protocol schema metadata (`ramp_cadence_low_rpm` / `ramp_cadence_high_rpm`) was synced to
  the new defaults (50/110); allowed ranges, types and layout are unchanged.
- The 50/110 bounds are tuned for the M820. TODO: this cadence ramp range belongs in the
  motor-specific profile — future EVistDrive motors may have a different usable cadence band.
- Not touched: Iq rise/fall values, power filters, torque filter, `release_ms`, S-curve,
  PAS/pre-stop, safety/hard cut, CAN and EEPROM.

### Build after these changes
- Firmware builds successfully as `0.0332_M820_BL820.bin`. The existing warnings
  (`-Wpointer-sign` in `CAN_Display.c`, unused variable `fw_ver` in `main.c`) predate these
  changes.

### Extended Boost may no longer outlive real pedalling — FW-095
- Extended Boost used to be motor overrun after the cranks stop: a firm push armed it, the
  EDGE of pedalling stopping started it, and it then held motor current for up to a second
  with the cranks stationary — while raising the profile's "pedalling" flag so the release
  fade would not run. On a bike with no dependable brake-sensor input there was no independent
  way to stop that. It was off by default and had never been confirmed on the bike, so it has
  been changed rather than tuned.
- It now does the opposite. A hard push, held for 30 ms while the rider is genuinely still
  pedalling forward and the ride latch is armed, starts the boost immediately; it runs for the
  configured time and ends on whichever comes first — the timer, or real forward pedalling
  stopping. The pedalling-stopped cancel is unconditional and acts in the same control tick.
  Nothing in the firmware may claim pedalling that is not happening any more: the flag the
  module used to raise for this is gone from the code.
- One push gives one boost. Leaning on the pedal cannot chain one boost into the next — the
  load has to fall back below the trigger before another may start.
- Duration stays 0–1000 ms in real milliseconds, and stays off by default. The range was
  deliberately not widened in the same step as the semantics change, so the first bike test
  cannot be ambiguous about which one caused what.
- Consequence to be aware of: while a boost runs the request is no longer forced to the
  no-pedalling speed classification. That override existed because the cranks were stopped,
  which is no longer true of any tick a boost can run in, so in legal mode the boost now
  follows the normal pedalling speed limit instead of the 5–7 km/h taper.
- Separately, the hard cut is now structural rather than incidental. Brake, backward
  pedalling, critical overtemperature, a torque-sensor fault and a running load calibration
  force the demand to zero and fade it over a fixed, firmware-owned 200 ms bound that an
  assertion keeps short — never over the rider-configurable per-level release time, which
  continues to serve the normal end of assist. Only overcurrent still kills the bridge outright.
- The persistent record size is now pinned by an assertion, so the layout that keeps every
  stored setting alive cannot be changed by accident.

### One ride engine: the pre-ride-core assist path is gone — FW-094
- Engine selection had been removed in FW-030, but the old assist monolith itself was still
  running: Walk Assist and phase 2 of position calibration called it on every control tick. It
  computed its entire pre-ride-core assist body — cadence map, pressure floor, throttle
  override, smooth-start envelope, overrun — and then discarded the result, because both of
  those paths overwrote it. The old arithmetic executed continuously and could not reach the
  motor. It has been deleted.
- The two paths that did reach the motor now have their own named functions in a new motor
  layer (`motor_service.h`): Walk Assist, and position calibration. Both do exactly what the
  reachable branches did, in the same order. The one intentional difference is that the Walk
  result is clamped rather than implicitly truncated, which cannot change a value in the
  reachable range.
- Also removed: the ride-engine type and its getter, the overrun state
  (`Overrun_strength`/`_counter`/`_flag`) and its per-level duration/strength cache, the legacy
  limiter wrapper, `map_rezi()`, `interpolate_assistfactor()`, and the build switches
  `ASSIST_TORQUE_MODE`, `ASSIST_CURVE_EXPO_L*`, `SMOOTH_START_ENABLE`, `START_RAMP_TICKS`,
  `EXTENDED_BOOST_ENABLE`, `RIDE_ENGINE_DEFAULT`, `TQ_GATE_RELEASE`, `START_MIN_STEPS`. No
  compatibility layer and no fallback were left in their place.
- **Stored settings are unaffected.** The persistent parameter layout is byte-for-byte
  unchanged, so no EEPROM migration is needed and nothing on the bike reverts to defaults. The
  parameters that the removed code used to read are now orphaned — still written by the app and
  echoed back by the firmware, but read by nobody. The Canable help text for each of them now
  says so plainly instead of claiming they are "compiled in but not reached".
- The engine byte in the 0x6028 and 0x6029 blocks stays on the wire as a documented constant:
  the shipped app parses those blocks positionally, so dropping it would shift every field
  after it. Assist mode 0 is likewise reserved rather than reused.
- Side effect worth having: the shared limiter no longer depends on this controller's globals,
  so it is genuinely motor-agnostic.
- Not addressed here: FW-084 Extended Boost still holds torque after the cranks stop on a bike
  without a brake sensor. It is native to the ride core, off by default, and unconfirmed on the
  bike — see `documentation/FW-094_LEGACY_REMOVAL_AUDIT.md`.

### Extended Boost: a deliberate drive hold for steps and rocks — FW-084
- New per-level setting group. The rider arms it with a firm push on the pedal; once the
  cranks are recognized as stopped the motor keeps pulling for a configured time at a current
  derived from the peak load of that push, and the existing single release ramp takes over
  afterwards. It exists for lifting over steps, rocks and short breaks in pedalling on a
  technical climb.
- Three settings per level, per bank: trigger pedal load (1.0–60.0 kg in 0.5 kg steps,
  default 20.0), boost strength (0–255 %, default 100) and boost duration (0–1000 ms, default
  **0 = off**). Every new and every migrated profile has it switched off, and with duration 0
  the current trajectory is identical to firmware without this card.
- The level's own ceiling still applies. Because the boost REPLACES the mode's result, both
  Maximum motor current and Maximum motor power are re-applied to it afterwards; without that
  a level limited to 20 % could have been handed the full global limit by one hard push.
- While the boost runs the request is classified as non-pedal — the cranks are stopped — so in
  legal mode it tapers from 5 km/h and gives nothing from 7 km/h. That is a deliberate policy
  written out in the code, not a side effect, and the Canable card says so.
- Writing a profile bank cancels any arming made under the previous settings.
- Deliberately narrow triggering: the load must be *held* for about 30 ms, so an ADC glitch,
  a chain slap or a pothole cannot arm it; a rate of rise alone never arms it; the arming
  goes stale after 1.5 s; and the latest confirmed push replaces an earlier one even when it
  is weaker. Boost starts only on the edge of pedalling stopping, and nothing — no PAS, Hall
  or speed pulse, no limit trimming the current — extends the timer once it runs.
- Brake, backward cranks, a sensor fault, assist level 0, a level or bank change, Walk Assist,
  position calibration, losing motion and resuming pedalling all cancel it in the same control
  tick. Throttle can neither arm it nor feed it: the module acts on the pedal-only target,
  before the throttle floor, and every shared limit still runs after it.
- Bank blob v8: the record grows 46 → 48 B and the blob to 255 B, which is exactly the ceiling
  of the transport (the length travels in a single byte). Older blobs are still accepted and
  migrate with the function off. The next per-level field will need an existing byte reused or
  a new transport version.
- Ride diagnostics 0x6029 extended to v5 with the boost state, latest peak load, computed
  current, remaining time and the reason the last one was cancelled.

### Releasing assist now really lets the motor coast — FW-093
- Zero torque was not a coast. Once the assist target reached zero the half bridges stayed
  enabled and the FOC went on regulating the *measured* current to zero, which on a turning
  rotor behaves as electrical damping — and pulls the gearbox into its last position at the
  very end. The bridge was released only after about three seconds without rotor movement,
  and the Hall interrupt resets that timer on every half rotation, so for as long as the
  motor turned the bridge never let go. Releasing Walk Assist and then turning the motor
  backwards by hand was where it was easiest to feel.
- The power stage now has an explicit DRIVE / COAST state shared by every torque source —
  Torque, Walk Assist, Power and Power Curve, throttle and Extended Boost — because they all
  end in the same current target. When that target reaches zero the controller waits only for
  the real current to decay (about 6 ms, with a 50 ms ceiling) and then switches the bridge
  off for a true high-impedance coast. No module switches the MOSFETs by itself.
- "No torque requested", "the bridge is released" and "the rotor has stopped" are three
  separate states in the code now. The rotor-stopped timer keeps its own job — cutting a
  bridge that is driving into a motor that will not turn — but no longer decides when zero
  torque may become a coast.
- Re-engaging while the motor is still spinning is handled explicitly. Switching the bridge
  on with zero applied voltage would put the full back-EMF across a shorted winding for
  several milliseconds of hard regenerative braking, so the current regulator is pre-loaded
  with the back-EMF measured as the coast began, scaled to the speed assist resumes at, and
  the outputs are enabled only once the first real switching pattern has been computed.
  Falling back to the previous zero start is still what happens from a standstill.
- A second cause of the same symptom was removed with it: the current regulator's integral
  term was being wiped on every control tick while the target was zero. The regulator runs
  four times faster than that, and with the shipped gains the integral could only ever reach
  about 3 % of the proportional term before being cleared — so at zero target the current
  loop was effectively proportional-only. Holding zero current on a turning rotor requires
  the loop to produce the back-EMF at zero error, which a proportional-only loop cannot do,
  so a real braking current kept flowing. The integral is now left to work while the bridge
  still drives; "a zero request makes no torque" is guaranteed by releasing the bridge
  instead, which is stronger, and both regulators are cleared as part of that release.
- Whether the bridge may drive is decided on the whole current command, not on the torque
  axis alone, so a future d-axis use cannot request current from a released bridge.
- The current release ramp, the bumpless bridge-on, Hall angle tracking and every safety
  shutdown are unchanged. Overcurrent, self power-off and position calibration keep their own
  immediate, unconditional cuts: this covers ordinary release only.
- Diagnostics frame 0x00010207 reports each power-stage transition — sent on the change, not
  in the control loop.

### Smooth Start no longer mistakes coasting for a standstill — FW-092
- The standstill test looked only at cadence and motor speed. On a mid-drive the freewheel
  lets the motor stand still while the bike rolls, so ordinary coasting satisfied it and every
  mid-ride re-engage armed the launch envelope: with a 300 ms setting the first current
  arrived about 22 ms late at the default latch floor, and later still at smaller targets.
- The test now also requires the wheel to be stopped. An armed-but-unspent envelope is also
  cancelled once the bike rolls without any demand: arming used to be cleared only when an
  envelope completed, so standing still and then rolling away without pedalling — a push
  off, a downhill start — left the arming lying in wait for the first pedal stroke at speed.
  An envelope already running under real demand still finishes normally.
- Launching from a real standstill is eased in exactly as before; only coasting stops
  counting as a launch. Smooth Start is off by default, so this affected configured bikes
  rather than factory settings.

### Re-engaging assist while riding no longer waits on a cadence filter — FW-091
- The limiter decided whether a request was "the rider pedalling" from filtered cadence, and
  anything at or below 15 rpm was treated as non-pedal and clamped to 5–7 km/h — a hard zero
  at riding speed. That filter is zeroed on every pedal stop and rebuilds exponentially, so
  after a lull assist was blocked for 45° of crank at 60 rpm, 180° at 20 rpm, and **never**
  below 16 rpm, where the filter converges below the threshold. It explains both the delay
  and why it was never the same twice.
- The limiter now classifies the *source* of a request. Confirmed pedalling — the ride latch,
  which already requires forward crank direction, the configured PAS step count and pedal load
  over the configured kg threshold — gets the normal speed limit. Throttle and without-rotation
  launches keep the low non-pedal limit.
- Pedal and throttle currents are limited separately and combined only afterwards. Sharing one
  limiter would have let a latched rider hand the throttle the full pedal speed limit; separate
  calls make that impossible regardless of latch state. Walk Assist is unchanged.
- The latch current floor now rounds up, so a small percentage of a small limit cannot vanish
  in integer division. Minimum Iq = 0% still means no floor.
- The FW-090 fast-attack experiment ships disabled: it addressed a secondary effect, and the
  behaviour should be judged without it first.

### RUN torque estimator follows a genuine rise immediately — FW-090
- Averaging pedal effort over half a crank turn (FW-085) removed the per-leg pulsing, but it
  also delayed a genuine *increase* in effort by the same half turn. Re-catching assist after
  the power faded mid-ride became a lottery: with recent samples still in the window a touch
  was enough, but after coasting the window held near-zero samples and the rider had to push
  through roughly 180° before the motor responded. It was more noticeable at low assist
  levels, because the shortfall is multiplied by the support ratio.
- A sustained rise now re-seeds the window, the same way arming the ride latch already did.
  Both trigger conditions are set so ordinary pedalling can never satisfy them — a leg push
  peaks at about 1.57× its own mean, well under the 2× margin, and no single peak lasts the
  required eight crank steps. Falling effort is untouched, so dips between leg pushes are
  still ridden out.

### The configured start load is now the only pressure condition — FW-089
- Declaring a start also required a raw-ADC pressure threshold sitting 29 counts above the
  sensor zero — roughly 1.19 kg on the default characteristic and 1.53 kg after a user
  calibration, and it moved whenever the sensor was recalibrated. The rider's own
  configured start load is 0.70 kg standing and 0.30 kg rolling, so a push anywhere in
  between cleared the threshold the rider had set and was still refused by a constant they
  could neither see nor change. Assist then waited for the first cadence measurement, one
  crank step later than it should have engaged.
- The start phase now depends on forward crank movement alone. Pressure is not lost as a
  condition — it lives where it belongs, in the configurable kg threshold that still has to
  be met before the ride latch arms and before any current flows. Crank rocking remains
  blocked by the consecutive-forward-step counter, which any reverse step resets.

### Standing starts no longer read as "no effort" on Progressive and Curve — FW-088
- Rider power is pedal load times crank speed, so pulling away from a stop is close to zero
  watts however hard the pedal is pushed. The support curve took that power as its input,
  mapped it to the bottom of the curve, and returned the minimum support ratio — the least
  help exactly where a standing start needs the most. Power Linear was unaffected (its
  ratio is a constant), which is why only some levels felt weak pulling away.
- During the launch phase the curve input alone is now evaluated at a nominal cadence, so a
  given pedal load earns the same support ratio starting as it does riding. Harder pushes
  still earn more than light ones, and the support window still caps the result. Reported
  rider power, motor power and every limit continue to use the real (near-zero) figure, so
  nothing on the display or in the power ceiling is inflated.

### The launch state is an explicit flag, not a fake cadence — FW-087
- Once pedalling clearly started but no cadence had been measured yet, the firmware wrote a
  placeholder 1 rpm into the cadence and set a companion flag. No assist calculation ever
  read that 1 — every consumer substitutes zero while the flag is up — so it existed only
  to get past two gates while presenting itself as a measurement. It gave the cadence two
  meanings, put a fake value on the display and in CAN telemetry, and made the whole launch
  protection collapse whenever anything cleared the flag.
- The launch state is now a plain boolean, and the cadence only ever holds real
  measurements or a clean zero. The two gates it used to slip through — the assist gate and
  forward-pedalling detection — ask the flag directly, so a bad measurement can no longer
  defeat them. Assist behaviour is unchanged; during launch the display now shows 0 rpm
  rather than a fabricated 1.

### Assist no longer stalls part-way into a start — FW-086
- The interval counter behind the cadence measurement was reset only when a cadence pulse
  fired; the pedal-stop branch never touched it, so it kept counting through the whole
  standstill and saturated. The first pulse after pulling away then computed 0 rpm from
  that stale interval and cleared the launch flag with it — which both cut assist (a zero
  cadence is rejected downstream) and prematurely enabled the power-derived current
  ceiling that is deliberately bypassed at launch. Assist only recovered on the second
  pulse, i.e. after roughly twice the crank movement the start setting asks for.
- The first forward step after a stop now starts a fresh interval, and is treated as that
  interval's origin rather than its first count, so the next pulse spans a full interval
  and publishes a true cadence.

### RUN torque smoothing now measured in crank angle — FW-085
- The RUN effort estimator averaged pedal load over a window set in milliseconds, while
  its purpose — as its own source comment stated — was to average over a fraction of a
  crank turn. Those are only the same thing at one cadence: 300 ms covered 45% of a
  revolution at 90 rpm but 25% at 50 rpm, so assist visibly pulsed once per leg on steep
  climbs, and no single millisecond value could be right across the cadence range.
- The estimator is now a moving average over a window of **crank degrees**, advanced by the
  quadrature decoder rather than by the control loop. Ripple rejection is therefore the same
  at every cadence, and response gets quicker as cadence rises instead of staying fixed.
  A window equal to a whole number of leg periods cancels the per-leg ripple outright.
- Tuning blob v7. Layout is identical to v6; only the unit of the field at offset 20
  changes (milliseconds → crank degrees), so v6 blobs are still accepted. The stored
  millisecond value is deliberately **not** converted — what it was worth depended on the
  cadence it was tuned at — so anything other than "off" migrates to the 180° default.
- Canable: the Dynamics field is now 0–360° in 15° steps (default 180°, one leg), and the
  writer negotiates down to v6/v5 for older controllers.

### Walk Assist controller — FW-077 through FW-082
- Start condition thresholds are now expressed directly in kg (per-level standstill and
  rolling minimums), with the old mV-based pressure-rise detector removed.
- Hall sensor autocalibration for Walk Assist.
- The Walk Assist speed controller now regulates to the configured target chainring
  RPM instead of drifting to whatever the drivetrain settles at.
- Walk Assist recovers from coasting to zero without needing the button released and
  re-pressed.
- Added a Hall keepalive current floor and capped the initial start current, fixing a
  slip/reacquire/stall cycle seen on light drivetrains.
- Faster RUN response and a wider normal operating current range, based on real-world
  testing.

### Pedal-assist start sensitivity
- The ride-latch start gate compared a deadbanded and 35 ms filtered torque signal
  against the rider's configured kg threshold, which silently raised every threshold
  by roughly 0.4 kg. It now compares the already-computed raw kg reading directly.
- The required crank-rotation step count is eased by one step specifically while the
  bike is already rolling (never below zero) — resuming pedalling after any pause was
  otherwise treated exactly like a fresh standstill start.

### Housekeeping
- Renamed `protocol/ebics_config_schema.yaml` to `evistdrive_config_schema.yaml`,
  completing the eVistDrive rebrand (HMI and README already used the new name).
- Added `LICENSE` (GPL-3.0) and `QUICKSTART.md`.

All host-side tests pass. Bank blob and CAN protocol are unchanged for the Walk Assist
work; the start-sensitivity fix touches only runtime logic in `ride_control.c`, no wire
format changes.
