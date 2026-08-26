# FW-112 PATCH B0 - AUDYT + PROJEKT

**Status**: AUDYT KOMPLETNY, BRAK IMPLEMENTACJI
**Verdict**: HOLD

---

## 1. CURRENT AUTHORIZATION GRAPH

```
PAS sensor (quadrature)
  |
  v
pas_direction.c (FORWARD_SAFE / INHIBIT / FORWARD_CONFIRMING)
  -> fwd_run (consecutive forward step count)
  -> direction_inhibit_active (level: true during reverse/invalid hold)
  -> forward_confirmed_this_tick (edge: true on confirm completion)
  |
  v
pas_liveness.c (idle timer -> stopped verdict)
  -> real_stop (PAS timed out)
  |
  v
ride_session.c (COLD -> ACTIVE -> SUSPENDED_BY_DIRECTION -> ...)
  -> latched (true only in ACTIVE state)
  -> cold_arm_this_tick (edge: COLD -> ACTIVE)
  -> fast_rearm_this_tick (edge: SUSPENDED -> ACTIVE)
  |
  v
ride_control.c:
  |
  +-- cold_start_ready = crank_ok AND (torque >= engage_threshold)
  |     crank_ok = !hard_cut AND !assist_off AND crank_forward_steps >= required_steps
  |     engage_threshold = standstill_threshold (or riding_start_load if rolling)
  |
  +-- rearm_permission_active (permission grant, expires at start_steps catchup)
  |     -> fakes pedaling_active and pas_forward for assist_modes_calculate()
  |     -> only on fast_rearm_this_tick edge
  |
  +-- RUN estimator recovery (IDLE/WAIT_FRESH_LOAD/TRACK_FAST)
  |     -> seeds ARUN to current AFILT on rearm
  |     -> 560 tick stable dwell before returning to IDLE
  |
  +-- RIDE LATCH BLOCK:
  |     if !latched: iq_target = 0
  |     if latched:
  |       - assist_hold_ticks = hold_ticks_full (on cold_arm OR positive mode demand)
  |       - if assist_hold_ticks > 0 AND iq_target < min_iq: iq_target = min_iq
  |       - if recovery_wait AND demand==0: hold=0, force_zero=1
  |
  +-- FINAL SAFETY GATE (default-deny, unconditional):
  |     if direction_inhibit_active OR safety_cut_non_direction: iq_target = 0
  |
  +-- HARD CUT BLOCK:
  |     if hard_cut: iq_target=0, ramp=200ms, hold=0, profile_pedaling=false
  |
  +-- THROTTLE MERGE: iq_target = max(pedal_iq, throttle_iq)
  |
  +-- SAME-TICK ZERO (force_zero_reference):
  |     if iq_target==0 AND (fast_rearm_this_tick OR recovery_wait):
  |       force ramp accumulator to 0
  |
  +-- SMOOTH START -> GEAR PRELOAD -> COAST RELEASE -> DYNAMICS RAMP
  |
  v
motor_command (iq_target -> FOC)
```

## 2. MECHANISM AUDIT TABLE

| Mechanism | File:Line | Role in authorization | Verdict |
|---|---|---|---|
| ride_session.latched | ride_session.c:113 | THE permission owner: true only in ACTIVE state | **KEEP** - sole permission owner |
| cold_start_ready | ride_control.c:442 | Input to session: crank_ok AND torque>=threshold | **MERGE INTO GATE** - becomes gate SET condition |
| rearm_permission_active | ride_control.c:75 | Fakes pedaling for modes during fast rearm | **REMOVE FROM AUTHORIZATION** - new gate grants permission directly |
| pedaling_active | rider_input.t:37 | Real PAS liveness signal | **KEEP AS SIGNAL** - used by modes, boost, dynamics |
| pas_forward | rider_input.t:35 | Forward crank direction | **KEEP AS SIGNAL** - used by modes |
| fwd_run | pas_direction.c | Consecutive forward step count | **KEEP AS INPUT** - feeds cold_start_ready and gate |
| WAIT_FRESH_LOAD | torque_input.h:244 | Recovery automaton middle state | **KEEP AS FILTER** - recovery is signal processing, not authorization |
| TRACK_FAST | torque_input.h:245 | Recovery automaton engaged state | **KEEP AS FILTER** - same as above |
| 560 stable dwell | torque_input.h (ROllING_REARM_STABLE_TICKS) | Recovery completion threshold | **KEEP AS FILTER** - belongs to ARUN estimator |
| assist_hold_ticks | ride_control.c:58 | Hold grace counter | **SIMPLIFY** - becomes pure floor bookkeeping, no authorization role |
| min_iq | tuning_config.c:28 | Minimum current floor % | **KEEP AS FILTER** - output shaping only |
| force_zero_reference | ride_control.c:881 | Same-tick ramp zeroing | **KEEP AS FILTER** - output shaping only |
| hard_cut | ride_control.c:297 | Immediate motor kill | **KEEP AS SAFETY** - independent final override |
| real_stop | main.c:346 | Genuine PAS stop verdict | **MERGE INTO GATE** - becomes gate RESET condition |
| direction_inhibit_active | pas_direction.c:133 | Reverse/invalid detection | **KEEP AS SAFETY** - independent final override |
| TORQUE_ASSIST_DEADBAND_NATIVE | torque_input.h:32 | Sensor noise deadband (10 native ~0.53kg) | **KEEP AS FILTER** - separate from start_load |
| engage_threshold_centikg | ride_control.c:409 | Live start threshold (standstill vs rolling) | **MERGE INTO GATE** - becomes gate threshold |
| riding_start_load_centikg | assist_modes.h:77 | Per-level rolling start threshold | **KEEP** - gate reads this directly |
| minimum_pedal_load_centikg | assist_modes.h | Per-level standstill start threshold | **KEEP** - gate reads this directly |

## 3. NEW GATE DESIGN

### Gate State

```c
typedef enum {
    PEDAL_ASSIST_GATE_CLOSED = 0,
    PEDAL_ASSIST_GATE_OPEN   = 1
} pedal_assist_gate_state_t;

static pedal_assist_gate_state_t pedal_assist_gate;
```

### SET Condition (CLOSED -> OPEN)

```
gate OPEN when ALL of:
  1. forward pedaling confirmed (pas_direction forward_confirmed_this_tick
     OR forward_pedaling with active liveness)
  2. AND (
       configured_start_load == 0
       OR
       raw_corrected_load >= configured_start_load
     )
  3. AND no terminal event active
```

In code:
```c
bool gate_should_open =
    forward_pedaling_valid
    && (engage_threshold_centikg == 0 || torque_centikg >= engage_threshold_centikg)
    && !terminal;
```

### KEEP Condition (stays OPEN)

```
gate stays OPEN while:
  forward pedaling continues (pas_direction forward AND liveness not stopped)
```

Once OPEN, torque magnitude is NOT re-tested. Dead spots, low-torque phases,
variable pressure - gate stays OPEN as long as the crank turns forward.

### RESET Condition (OPEN -> CLOSED)

```
gate CLOSES when:
  - real_stop (PAS timed out AND wheel not rolling)
  - OR confirmed reverse / direction inhibit terminal transition
  - OR non-direction safety cut (brake, overtemp, fault)
  - OR assist level 0
```

### Semantics of start_load == 0

```
if start_load == 0:
    forward_confirmed -> gate OPEN (no torque required)
if start_load > 0:
    forward_confirmed AND load >= threshold -> gate OPEN
no crank motion: gate CLOSED
```

### Brake Handling

**Decision: B** - brake hard-cuts output only, gate lifecycle remains active.

After brake release with active forward pedaling: gate stays OPEN.
No re-test of START_LOAD required.

Rationale: the rider is still pedalling forward. Requiring START_LOAD after
a brake release during continuous pedalling is hostile to the rider.

## 4. TORQUE MAGNITUDE SEPARATION

### Current Problem

```
assist_delta = max(delta - TORQUE_ASSIST_DEADBAND_NATIVE, 0)
  -> this signal feeds BOTH:
     (a) the mode calculation (AFILT -> ARUN -> torque_run_filtered)
     (b) the recovery automaton transitions (>= deadband check)
```

The deadband is a SENSOR NOISE filter. It should not also serve as the
start_load threshold. They happen to be different values today
(deadband=10 native ~0.53kg, start_load configurable 0-12kg), but they
are LOGICALLY tangled because assist_delta is the single derived signal.

### New Design

```
RAW SENSOR
  |
  +---> torque_load_centikg (raw corrected, no deadband)
  |       -> used by GATE (start threshold comparison)
  |       -> used by mode calculation (torque_assist_factor, support_ratio)
  |
  +---> assist_delta = max(delta - DEADBAND, 0)
          -> DEADBAND = sensor noise filter ONLY
          -> feeds AFILT -> ARUN -> mode calculation
          -> NOT used for gate decisions
```

The gate compares against `torque_load_centikg` (the raw corrected value
already shown to the rider as their live pedal load). The mode calculation
continues to use `assist_delta` with its deadband for signal quality.

**No change to TORQUE_ASSIST_DEADBAND_NATIVE value or application**.
The separation is in WHICH signal feeds the gate vs. which feeds the modes.

## 5. CALCULATION ALWAYS RUNNING

### Current Problem

Several conditions stop or zero the calculation pipeline:

1. `prepare_assist_input()` returns false when:
   - `!torque_sensor_valid` (ride_control.c:656)
   - `!pas_sensor_valid` (ride_control.c:657)
   - `!pedaling_active && !without_rotation_active` (ride_control.c:658)
   - `cadence==0 && !start_phase && !without_rotation_active` (ride_control.c:659)

2. `assist_modes_calculate()` returns false (supported=false) when level==0

3. `force_zero_reference` forces ramp to 0 during recovery_wait

### New Requirement

The calculation path should run independently:

```
torque -> AFILT -> ARUN -> mode calculation -> calculated_iq
```

even when gate is CLOSED. Gate only zeroes the OUTPUT:

```
if gate_closed:
    final_assist = 0
else:
    final_assist = calculated_iq
```

### What Can Stay Running at Gate CLOSED

- `torque_input_update()` - always runs (ADC ISR)
- AFILT (assist_delta_filtered_native) - always updated
- ARUN (assist_delta_run_native) - always updated
- `assist_modes_calculate()` - should always compute from current inputs
- Startup boost state machine - can pre-warm

### What Must NOT Run at Gate CLOSED

- `assist_hold_ticks` - no grace needed
- `min_iq` floor - no current output
- Extended Boost trigger - no boost without active assist
- Gear preload - no current output

### CPU Cost

`assist_modes_calculate()` at gate CLOSED is one function call with
existing inputs. The mode calculation is ~200 cycles on Cortex-M4.
Negligible at 4 kHz.

## 6. OLD MECHANISMS REMOVED

| Mechanism | Reason for removal |
|---|---|
| rearm_permission_active | Replaced by gate: gate stays OPEN through dead spots |
| ride_session latched (as authorization) | Replaced by gate: session becomes lifecycle only |
| cold_start_ready (as session input) | Merged into gate SET condition |
| WAIT_FRESH_LOAD as authorization | Recovery is filter, not permission |
| TRACK_FAST as authorization | Recovery is filter, not permission |
| 560 dwell as authorization | Recovery completion is filter |
| force_zero_reference (recovery_wait path) | Gate handles the output zeroing |
| Same-tick zero (rearm tick path) | Gate handles the output zeroing |
| rearm_permission faking pedaling_active | Gate grants permission directly |
| rearm_permission faking pas_forward | Gate grants permission directly |

## 7. OLD MECHANISMS RETAINED

| Mechanism | Reason for retention |
|---|---|
| ride_session.c (3 states) | Lifecycle management (COLD/ACTIVE/SUSPENDED), not authorization |
| pas_direction.c | Direction safety - independent, exhaustively tested |
| pas_liveness.c | Real stop detection - independent |
| TORQUE_ASSIST_DEADBAND_NATIVE | Sensor noise filter - separate concern |
| AFILT (35ms EMA) | Signal processing - separate concern |
| ARUN (cadence-windowed RUN) | Signal processing - separate concern |
| WAIT_FRESH_LOAD / TRACK_FAST | Recovery automaton - signal processing |
| 560 stable dwell | Recovery completion - signal processing |
| hard_cut | Safety override - independent final gate |
| direction_inhibit_active | Safety override - independent final gate |
| assist_hold_ticks | Output floor bookkeeping (simplified) |
| min_iq | Output floor (simplified) |
| force_zero_reference | Output shaping (simplified) |
| Extended Boost | Feature - acts on pedal target before final gate |
| Throttle | Feature - separate limiter path |
| Gear preload | Feature - output shaping |
| Coast release | Feature - output shaping |
| Smooth start | Feature - output shaping |
| assist_dynamics (ramp) | Feature - output shaping |

## 8. START_LOAD SEMANTICS

### Current

`engage_threshold_centikg` is computed each tick:
```
engage_threshold = minimum_pedal_load_centikg  (standstill)
if (crank_ok && bike_rolling):
    engage_threshold = riding_start_load_centikg  (lower threshold)
```

This is compared against `torque_centikg` (raw corrected load) in
`cold_start_ready` computation.

### New (unchanged semantics, cleaner ownership)

Gate reads the SAME threshold computation. The only change is that the
gate OWNS the decision, not `cold_start_ready` feeding the session.

```
gate_threshold = minimum_pedal_load_centikg
if bike_rolling && crank_moving_enough:
    gate_threshold = riding_start_load_centikg
```

### Start Load == 0

When `gate_threshold == 0`:
- Gate opens on forward confirmed (no torque required)
- Gate still requires forward pedaling (no opening at standstill)

## 9. GATE vs SESSION INTERACTION

### Option A: Gate replaces session permission

Session becomes pure lifecycle (COLD/ACTIVE/SUSPENDED). Gate decides
when assist flows. Session state drives diagnostics only.

**Problem**: session's COLD -> ACTIVE transition currently requires
`cold_start_ready` which bundles multiple conditions. If gate replaces
this, the session needs a simpler input.

### Option B: Gate sits inside session

Session state machine adds a gate sub-state. ACTIVE has two sub-states:
ASSIST_BLOCKED and ASSIST_ALLOWED.

**Problem**: adds complexity rather than reducing it.

### Recommended: Option A with simplified session input

```
session input:
  cold_start_ready = forward_pedaling_valid  (simplified: no torque check)
  (torque check moves to gate)

gate input:
  threshold = engage_threshold_centikg
  torque = torque_centikg
  forward = forward_pedaling_valid
```

Session decides: "is the rider alive and pedalling forward?"
Gate decides: "is the rider pressing hard enough?"

## 10. S1-S10 SCENARIO ANALYSIS

### S1: COLD START NORMAL
- Forward PAS confirmed + load < threshold -> gate CLOSED, assist=0
- Load exceeds threshold -> gate OPEN, calculated_iq flows
- **PASS** (gate requires both forward AND load)

### S2: DEAD SPOT
- Gate OPEN + torque 100->0->100 + forward continues -> gate stays OPEN
- **PASS** (gate only checks forward, not torque magnitude)

### S3: LONG LOW TORQUE
- Gate OPEN + torque below start_load for many rotations + PAS forward -> gate stays OPEN
- calculated_iq may be 0 but gate is OPEN
- **PASS** (gate keeps open on forward pedaling)

### S4: TORQUE RETURNS
- After S3, torque rises -> no re-test of start_load -> calculated_iq rises immediately
- **PASS** (no re-authorization needed)

### S5: REAL STOP
- Gate OPEN -> no PAS for real_stop condition -> gate CLOSED -> assist=0
- Next start requires forward AND start_load from scratch
- **PASS**

### S6: REVERSE
- Gate OPEN -> confirmed reverse -> gate CLOSED -> assist=0
- Forward re-confirmation required for next start
- **PASS**

### S7: BRAKE
- Gate OPEN + forward + torque -> brake -> final iq=0 immediately
- Brake release with forward still active -> gate stays OPEN
- No re-test of start_load
- **PASS** (option B: brake is output override, not gate reset)

### S8: START_LOAD=0
- Standstill: gate CLOSED (no forward)
- Forward confirmed: gate OPEN (no torque required)
- Stop: gate CLOSED
- **PASS**

### S9: SENSOR NOISE
- No real pressure, sensor noise around zero
- START_LOAD > noise -> gate never opens
- **PASS** (gate requires load >= threshold)

### S10: CALCULATION PRE-WARM
- Gate CLOSED, rider starts pressing, AFILT/ARUN compute in background
- Forward + load met -> gate OPEN -> calculated_iq already has magnitude
- No additional delay from starting AFILT/ARUN after OPEN
- **PASS** (calculation always running)

## 11. MIN_IQ AUDIT

### Definition
- `tuning_config_min_iq_pct` in tuning_config.c:28, default 2%, max 25%
- Global (not per-level)
- Applied in ride_control.c:641

### When Active
- Only when `assist_hold_ticks > 0` AND `iq_target < min_iq`
- assist_hold_ticks is armed on cold_arm_this_tick OR positive mode demand
- assist_hold_ticks counts down each tick
- assist_hold_ticks zeroed on: hard_cut, terminal session, recovery_wait+zero demand

### Relationship with assist_hold_ticks
- min_iq floor is GATED by assist_hold_ticks
- assist_hold_ticks is the "rider recently pressed" grace
- Together they form: "if rider pressed recently, don't let current drop below X"

### After New Gate
- Gate handles "rider exists and is pedalling" question
- assist_hold_ticks becomes pure grace bookkeeping (how long to hold floor after last positive demand)
- min_iq floor stays as-is (output shaping)
- Simplification: remove authorization role from assist_hold_ticks

### Recommendation for PATCH B1 (NOT IMPLEMENTED)
- Make min_iq per-level (currently global)
- Consider different floor for start vs riding
- Keep assist_hold_ticks as grace timer
- Gate provides the "rider exists" signal, hold provides the "grace period"

## 12. MUTATION TESTS

| ID | Mutation | Expected | Rationale |
|---|---|---|---|
| M1 | Gate opens without forward PAS | FAIL | Gate requires forward |
| M2 | START_LOAD > 0 ignored | FAIL | Gate tests load >= threshold |
| M3 | Gate closes on torque dip | FAIL | Gate only checks forward |
| M4 | real_stop doesn't close gate | FAIL | real_stop is RESET condition |
| M5 | Reverse doesn't zero assist | FAIL | reverse -> gate CLOSED -> assist=0 |
| M6 | Brake doesn't zero final Iq | FAIL | brake -> hard_cut -> iq=0 |
| M7 | START_LOAD=0 opens gate at standstill | FAIL | Gate requires forward |
| M8 | Old recovery blocks output when gate OPEN | FAIL | Old mechanisms removed |
| M9 | Stale torque leaks after new start | FAIL | Gate reset clears all stale state |

## 13. SCOPE CONTROL

### IN SCOPE
- pedal_assist_gate state machine
- Simplified authorization flow
- START_LOAD separation from deadband
- Calculation always running
- Old mechanism cleanup

### OUT OF SCOPE (explicitly NOT changed)
- Mode curves / support_ratio / eMTB
- Output ramps / dynamics
- FW-112.4 RISE/FALL parameters
- FOC / current PI
- Legal/temp/voltage limits
- Startup boost (but pre-warming allowed)
- Per-level MIN_IQ (PATCH B1)

## 14. KEY FILES TO MODIFY

| File | Changes |
|---|---|
| ride_control.c | Replace session-latch-hold block with gate logic |
| ride_session.h | Simplify inputs (remove torque check from cold_start_ready) |
| ride_session.c | Simplify cold_start_ready to forward-only |
| NEW: pedal_assist_gate.h | Gate state + API |
| NEW: pedal_assist_gate.c | Gate implementation (~50 lines) |
| fw112_diag.h | Update reason codes |

## 15. RAM/FLASH DELTA (estimated)

- New: pedal_assist_gate.c: ~50 lines, ~20 bytes RAM (state + counters)
- Removed: rearm_permission_active, complex same-tick zero logic
- Net: approximately zero (simplification removes more than addition adds)

## 16. REMAINING CLEANUP CANDIDATES

1. `assist_hold_ticks` could be simplified further (pure timer, no authorization)
2. `recovery_wait` same-tick zero path could be removed entirely
3. `profile_pedaling_active` has multiple override sites (throttle, hold floor)
4. Session cold_start_ready input could be simplified to forward-only
5. ride_control.c is 988 lines - the gate extraction would shorten it by ~100 lines

## 17. RECOMMENDATION FOR PATCH B1 (MIN_IQ FLOOR)

**NOT IMPLEMENTED in this patch.**

Current: global min_iq_pct (default 2%, max 25%)

Proposed: per-level min_iq floor
- Level 0: 0% (no assist)
- Level 1: 2% (gentle floor)
- Level 2-3: 3-5%
- Level 4-5: 5-8%

The gate provides the "rider exists" signal.
The hold grace provides the "how long to keep floor" timer.
The per-level floor provides "how much minimum pull" per level.

---

## 18. VERDICT

**HOLD**

Reason: The audit is complete and the design is sound, but:

1. This is a MAJOR architectural change to the authorization pipeline
2. Every existing host test suite must be reviewed and updated
3. The interaction between gate and session needs careful validation
4. The "brake doesn't close gate" decision (option B) needs real-bike validation
5. The recovery automaton's role change (filter only, not authorization) must be
   proven safe in all rearm scenarios

Next steps before implementation:
1. Get approval on the brake handling decision (A vs B)
2. Get approval on session simplification (Option A)
3. Run all existing host tests to establish baseline
4. Write host tests for S1-S10 BEFORE implementing
5. Implement in small, testable increments

NO COMMIT. NO PUSH. STOP BEFORE FLASH.
