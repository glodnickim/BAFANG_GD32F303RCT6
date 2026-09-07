# EVistDrive FW142 — current architecture map

## Control flow

```text
RAW SENSORS
  torque ADC / PAS A-B / Hall / wheel / battery / temperature
        |
        v
SENSOR TRUTH
  PAS plausibility -> direction
  conditioned cadence
  torque calibration + filtering
  rotor motion / angle confidence
        |
        v
RIDE PERMISSION / SESSION
        |
        v
ASSIST DEMAND
  Torque / Power / eMTB / Curve / Startup Boost / cadence compensation
        |
        v
LIMITS
  phase current / battery current / power / speed / thermal / safety
        |
        v
FINAL Iq TRAJECTORY @ 16 kHz
  `fast_iq_slew.c`
        |
        v
CURRENT LOOP
  `FOC.c` + `foc_current_loop.c`
  Clarke/Park -> PI Id/Iq -> common vector saturation -> inverse Park -> SVPWM
        |
        v
PWM / INVERTER / MOTOR
```

## START lifecycle

Current intended normal start:

```text
valid forward rider intent
 -> permission
 -> final Iq state starts from zero
 -> continuous bounded rise
 -> motor moves
 -> Hall timing becomes confident
 -> normal ACTIVE
```

There is no normal Hall-gated Gear Preload current cap. Hall is feedback/evidence, not a condition that suddenly unlocks a second torque owner.

## ZERO / RESTART lifecycle

```text
ACTIVE
 -> demand goes to zero
 -> final Iq trajectory reaches zero
 -> PI/ADC/theta remain coherent
 -> ordinary zero torque / ARMED_ZERO semantics
 -> new valid demand
 -> warm restart without unnecessary cold bridge cycle
```

Fault shutdown is separate from ordinary zero torque.

## Hall/angle

3-Hall hardware cannot know exact electrical position at standstill. The safe model is:

- insufficient timing: sector anchor (bounded initial error);
- fresh timing: interpolation only with confidence;
- stale timing: fall back instead of extrapolating forever;
- angle confidence and speed confidence are separate facts;
- one final Park angle owner.

Electrical SIL explicitly sweeps all Hall sectors/start angles.

## Rider-input stability

### PAS
Impossible 1-3 tick reverse bounce is rejected before direction safety. True reverse remains a hard permission loss.

### Cadence
Control uses the conditioned cadence. Raw cadence is retained for telemetry/diagnostics.

### Torque
Filtering is elapsed-time based, so missed/coalesced 4 kHz foreground calls do not stretch a nominal 35/120/250 ms filter by call count.

## Simulation layers

### Fast supervisory SIL
Cheap plant for 10k+ fuzz cases. Exercises real supervisory production path and final Iq logic.

### Electrical FOC SIL
Uses real production:

```text
final Iq slew
 -> FOC current measurement/Park
 -> PI Id/Iq + vector saturation
 -> inverse Park
 -> SVPWM
 -> virtual PMSM
 -> physical currents + rotor
 -> Hall edges
 -> production rotor angle/motion
```

QZERO also executes in this backend.

The PMSM parameters are test parameters, not claimed measured M820 constants. Do not tune production FOC/QZERO to the virtual plant unless hardware evidence confirms the parameter domain.

## User-facing functionality rule

Keep rich TSDZ-style configurability, but do not let each parameter create a new hidden controller/state machine.

Examples:

- Assist -> demand magnitude
- Power -> ceiling
- Acceleration -> positive final-Iq dynamics
- Deceleration/Release -> negative final-Iq dynamics
- Startup Boost -> demand shaping
- Start Smoothness -> start segment of one final-Iq trajectory
- torque smoothing -> sensor/rider-effort estimator only

Every user setting should have a single documented owner.

## Current known architectural debt

These are not authorization to refactor blindly:

1. `ride_session` + `pedal_assist_gate` still contain overlapping permission concepts; current tests do not show active-ride chatter, so preserve behavior until a focused migration has explicit semantics/tests.
2. legacy/inactive configuration fields should eventually be migrated/hidden, not silently repurposed.
3. `min-Iq hold`, QZERO and old low-speed/coast policies require hardware-backed A/B before removal or retuning.
4. `main.c` remains a large hardware/glue node; extract only when ownership is clear and parity is tested.

## Definition of a flash candidate

A flash candidate requires all of:

```text
host modules PASS
+ whole-pipeline traces PASS
+ supervisory SIL PASS
+ electrical FOC/Hall SIL PASS
+ deterministic fuzz PASS
+ sanitizers PASS
+ exact Arm GCC 13.2.1 target build PASS
+ BL820 packaging/CRC/identity PASS
```
