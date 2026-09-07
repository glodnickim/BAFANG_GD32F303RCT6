# EVistDrive — agent entry point

This repository is the current FW144 Level-4 testable baseline. Start here before changing production code.

## 1. Required reading order

1. `AGENTS.md` (this file)
2. `README_TESTING.md`
3. `VERIFICATION_STATUS_2026-09-07_PL.md`
4. `docs/ARCHITECTURE_CURRENT.md`
5. `docs/FW144_LEVEL4_VIRTUAL_BIKE.md`
6. `protocol/EVISTDRIVE_LIVE_RIDE_LOG_CONTRACT.md`
7. `verification_evidence/FW144_FINAL_GATE_SUMMARY.md`

Do not start from old FW/QS ticket notes and do not recreate removed workarounds without new evidence.

## 2. Baseline identity

Current documented history:

- `8e602e7` baseline from `EvistDrive06092026v3`
- `7033c71` FW139 PAS bounce rejection
- `0be2557` FW139 removal of Hall-gated Gear Preload
- `e6150d9` FW140 one conditioned cadence for control
- `18d81e0` FW141 elapsed-time-invariant torque filters
- `2599bc7` FW142 shared production PI/vector limiter + electrical FOC/Hall SIL
- `7ef4779` one-command exact target gate on Windows
- FW143 Walk 10..60 rpm + full Walk FOC matrix
- FW144 production SOC-core parity + Level-4 rider/bike/battery + recorded-ride replay

Use `git log --oneline` to confirm the current checkout before work.

## 3. One-owner architecture

Production ownership rules:

- PAS electrical plausibility: `pas_sampler.c`
- PAS direction safety: `pas_direction.c`
- cadence used by control: `cadence_filter.c`
- rider torque conditioning: `torque_input.c`
- ride permission / supervisory command: `ride_control.c` plus current session/gate modules
- assist demand: `assist_modes.c`
- final Iq trajectory: `fast_iq_slew.c` (16 kHz owner)
- current-loop PI + common voltage-vector saturation: `foc_current_loop.c`
- transforms / current measurement / SVPWM path: `FOC.c`
- final rotor angle policy: `rotor_angle.c` + `rotor_motion.c`
- ordinary zero-torque lifecycle: persistent FOC / ARMED_ZERO semantics
- low-speed stop helper: `quiet_zero.c` (QZERO; still hardware-sensitive)

Never add a second independent writer for final Iq, PI setpoint, or final Park angle.

## 4. Important fixes that must not regress

### FW139 PAS
Very short physically impossible reverse bounces are rejected before direction safety. A real reverse must still inhibit assist immediately.

### FW139 START
The old Hall-gated ~1 A Gear Preload state was removed. Do not restore a state that waits for Hall before releasing normal start torque. Normal start must begin from the final Iq trajectory at zero and rise continuously.

### FW140 cadence
Raw cadence remains useful for telemetry/diagnostics, but control uses the conditioned cadence. Do not feed short-window raw cadence back into assist/dynamics without an explicit experiment and regression.

### FW141 torque timebase
Torque filters use elapsed 4 kHz time. Do not replace elapsed-time catch-up with call-count-based filtering.

### FW142 FOC testability
`foc_current_loop.c` is production code used by both target firmware and electrical SIL. Do not fork/copy this math into a simulator-only implementation.

### FW143 Walk range and verification
Walk target is **10..60 chainring/output rpm** with 30 rpm as the safe default. Do not raise the normal
target above 60 rpm. 70/80/>80 are negative/range tests only. Small low-load speed float is acceptable;
do not tune production Walk to force exact tracking in the virtual PMSM. Hard failures are safety/lifecycle
violations: no start, runaway, current-ceiling violation, unsafe stall, or broken release/brake/fault/wheel cut.
Read `docs/FW143_WALK_ASSIST_10_60_TEST_CONTRACT.md` before changing Walk.


### FW144 Level 4 and SOC/replay
`soc_core.c` is the production SOC math shared with Level 4. Do not fork the SOC algorithm into the simulator.
`sim/l4/` owns only virtual physics/rider/battery assumptions. `sim/replay/` replays recorded sensor history
through production C. A real-bike bug should become a registered replay case before its fix is considered closed.
Read `docs/FW144_LEVEL4_VIRTUAL_BIKE.md` and `protocol/EVISTDRIVE_LIVE_RIDE_LOG_CONTRACT.md`.

## 5. Verification gate

Before any production-code change:

```bash
python tools/verify_all.py --quick
```

After the change:

```bash
python tools/verify_all.py
```

Before producing a flash candidate, require exact Arm GCC 13.2.1 target build:

```bash
python tools/verify_all.py --require-target
```

On Windows use:

```text
VERIFY_AND_BUILD_WINDOWS.bat
```

A change is not accepted because a local unit test passes. The full gate must stay green.

## 6. Test philosophy

Prefer invariants and end-to-end scenarios over isolated expected constants. At minimum consider:

- cold start from every Hall sector;
- loaded start;
- PAS bounce and true reverse;
- steady 20/40/60/80 rpm riding;
- Walk 10/15/20/30/40/50/60 rpm across load/Hall-start matrix;
- stop -> ARMED_ZERO -> restart;
- current/power/speed/thermal limiter transitions;
- missed foreground ticks;
- deterministic fuzz;
- sanitizer runs.

If a new bug is found on hardware, first add a deterministic reproduction to SIL/host tests, then change production code.

## 7. Reference hierarchy

Use references for different purposes, not as code to copy:

- G532: START/STOP lifecycle, bounded Q/Iq behavior, bicycle feel
- Fake Taxi: robust 3-Hall timing/angle behavior
- TSDZ2: rider-facing functionality and parameter semantics
- VESC: FOC/current-control/Hall-confidence cross-check

Do not blindly copy raw constants across different current/angle/time domains.

## 8. Known open / hardware-sensitive areas

Do not claim these are closed solely from SIL:

- exact real M820 PMSM R/L/flux/inertia/backlash parameters;
- final QZERO tuning/handback behavior on the physical motor;
- final ride-feel tuning of Start Take-up / Accel / Decel / Release;
- exact target `.bin` reproducibility until built with Arm GNU 13.2.1 on the supported target toolchain.

The simulator must test invariants and safety here, not force production constants to match an assumed virtual motor.

## 9. Change discipline

For every non-trivial production change:

1. state the symptom/root cause;
2. identify the current owner;
3. add/reuse a reproducing test;
4. make the smallest owner-local change;
5. run quick gate;
6. run full gate;
7. update architecture/evidence only if semantics changed;
8. commit one coherent change.

Do not mix rider-feel tuning, FOC changes, configuration migration, and protocol changes in one patch.
