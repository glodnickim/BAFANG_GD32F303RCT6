# G532 QUIET START / QUIET STOP -> EVistDrive
## Agent implementation guide based on reverse engineering of G532 FC 5.0

> Scope: reproduce the electrical/control mechanisms responsible for the very quiet and smooth transition `0 torque -> first motor torque` and `last Iq -> 0 torque -> optional bridge disarm` in EVistDrive.
>
> This document intentionally does **not** cover the torque-sensor / rider-demand pipeline. It starts at the motor-control request / `Iq_target` boundary.

---

# 0. Executive implementation decision

Do **not** implement this as only:

```text
MOE ON
 -> hold neutral CCR for many cycles
 -> release FOC
```

That is not the main mechanism found in G532.

The G532 pattern is:

```text
                    COLD / DISARMED
                          |
                          v
                PREPARE COHERENT FOC
                - current theta_e preserved
                - PI/tracking state reset
                - D/Q slew state reset to zero
                - phase command A=B=C=50%
                - output is coherent before torque
                          |
                    ~one fast-cycle
                 staging is sufficient
                          |
                          v
                   FIRST ACTIVE FOC
                - Q slew starts from 0
                - first Iq step is tiny
                - output-valid becomes true
                - central bridge gate may arm
                          |
                          v
                       ACTIVE
                          |
                   Iq target -> 0
                          |
                          v
                     ARMED_ZERO
                - Q slew reaches zero
                - current PI remains alive
                - theta/ADC/SVPWM remain alive
                - ordinary zero torque != MOE OFF
                          |
                    optional lifecycle
                          |
                          v
                       NEUTRAL
                - A=B=C=50%
                - coherent zero line-line voltage
                          |
                  full shutdown only if needed
                          |
                          v
                      MOE OFF
```

For EVistDrive, the recommended architectural change is therefore:

```text
FOC_DISABLED
 -> FOC_PREPARE
 -> FOC_ACTIVE
 -> FOC_ARMED_ZERO
 -> (optional) FOC_PREPARE_DISARM
 -> FOC_DISABLED
```

Safety/fault shutdown remains a separate immediate hard-off path.

---

# 1. Evidence base and confidence rules

Use these labels throughout implementation reviews:

- **CONFIRMED** — directly supported by G532 binary dataflow / constants / callsites.
- **STRONG INFERENCE** — architecture is strongly implied but one outer condition is not completely named.
- **OPEN** — do not hard-code a G532 value into EVistDrive without an EVist-specific measurement or additional dump.

Do not convert `OPEN` items into production constants just because they look plausible.

---

# 2. Hardware premise for this comparison

For the compared old and new motor assemblies, the mechanical motor/geartrain is the same. The relevant changes are:

```text
controller PCB
+
firmware
```

Therefore the quieter mechanical behavior of the G532 version must be caused by how electrical torque is created and removed, plus any PCB-level current-sense / gate-drive improvements.

Do not attribute the improvement to a different gearbox or clutch in the implementation rationale.

---

# 3. Key G532 functions / structures

## 3.1. Motor-control dispatcher

```text
0x080144D4
```

It selects the neutral/reset path and active FOC variants.

The dispatcher is called from the fast motor-control ISR path around:

```text
0x08010C7E -> BL 0x080144D4
```

The same fast path subsequently calls the central bridge/MOE gate:

```text
0x08010C86 -> BL 0x08014E00
```

This ordering matters:

```text
update motor-control state / FOC output
        |
        v
central bridge gate
```

## 3.2. Normal FOC core

```text
base = 0x20002D98
update = 0x0800839C
reset  = 0x0800831C
```

## 3.3. Alternate FOC core

```text
base around 0x20002FD4
update path around 0x08007ABC
```

The same source D/Q slew configuration is propagated into both active FOC variants.

## 3.4. Generic signed slew limiter

```text
reset  = 0x080105F8
update = 0x08010606
```

Normal-core Q slew structure:

```text
core + 0xB4 = target
core + 0xB6 = rise_step
core + 0xB8 = fall_step
core + 0xC0 = internal current/state region
core + 0xC8 = shaped Q output region
```

The essential behavior is:

```c
if (state < target) {
    delta = target - state;
    if (delta > rise_step)
        state += rise_step;
    else
        state = target;
} else {
    delta = state - target;
    if (delta > fall_step)
        state -= fall_step;
    else
        state = target;
}

output = state;
```

No overshoot is generated on the final step.

---

# 4. Neutral state — exact behavior

## 4.1. Neutral dispatcher state

The `TBB` dispatcher at `0x080144D4` was manually decoded. Selector:

```text
c8 = 0
```

enters the neutral/reset branch at approximately:

```text
0x080144F2
```

This corrects an earlier interpretation that assigned neutral to another selector.

## 4.2. FOC state reset

The neutral branch calls reset routines for the active FOC structures, including normal-core reset:

```text
0x0800831C
```

The reset clears dynamic state including:

- D-reference slew state,
- Q-reference slew state,
- current-vector limiter dynamic state,
- current PI dynamic/integrator state,
- tracking / anti-windup residual state,
- related dynamic filters/state variables.

Implementation meaning:

```text
new start must not inherit:
- previous Iq_ref
- previous Id_ref
- previous PI integral
- previous saturation tracking residual
- previous slew output
```

## 4.3. Current electrical angle is preserved

The neutral branch does **not** seed FOC with `theta=0`.

It reads the current electrical angle from the shared position path and copies it into both FOC variants.

This is a central anti-click mechanism.

Bad startup model:

```text
real rotor angle != FOC startup angle
 -> first voltage vector points incorrectly
 -> transient current / torque impulse
 -> gearbox lash is hit
```

G532 model:

```text
current theta_e known before first torque-producing vector
 -> first vector is coherent with rotor position
```

For EVistDrive: **angle validity must be a PREPARE precondition**.

## 4.4. Exact neutral phase command

G532 writes:

```text
A = 0x2000
B = 0x2000
C = 0x2000
```

The phase command domain is Q14-like:

```text
0x4000 = full scale
0x2000 = 50%
```

The PWM conversion is consistent with:

```c
CCR = phase_command * ARR >> 14;
```

Therefore neutral becomes:

```text
CCR_A = ARR/2
CCR_B = ARR/2
CCR_C = ARR/2
```

and:

```text
Vab = 0
Vbc = 0
Vca = 0
```

This is a coherent zero line-to-line voltage state.

Do not interpret this as `PWM OFF`. It is a valid neutral common-mode PWM state.

---

# 5. Neutral staging time — important correction

G532 does **not** show evidence of a long multi-millisecond neutral dwell as the primary smooth-start mechanism.

The fast FOC timebase is:

```text
0x3E80 = 16000 Hz
```

so:

```text
T_FOC = 62.5 us
```

The dispatcher is executed in the same fast motor-control path.

Therefore the logical sequence may be:

```text
cycle N:
  neutral/reset
  reset PI/slew
  seed current theta
  A=B=C=50%

cycle N+1:
  first active FOC calculation
```

with only:

```text
~62.5 us
```

between those logical passes.

### EVistDrive implication

Existing EVistDrive/FW-117 style `neutral dwell` can remain as a safety feature if already useful, but do **not** assume that increasing dwell is how G532 gets its smoothness.

The primary G532 mechanisms are:

1. coherent neutral before active output,
2. correct current angle,
3. clean PI/tracking state,
4. Q slew starting from zero,
5. no unnecessary bridge disable at normal zero torque.

If EVistDrive retains a dwell, its final duration must be justified by EVist hardware/ADC/gate timing, not copied as a supposed G532 behavior.

---

# 6. Factory configuration table — recovered defaults

## 6.1. Descriptor format

The G532 binary contains a factory/default parameter table near the end of the image.

Each descriptor is 13 bytes:

```text
bytes 0..2   parameter ID
bytes 3..6   runtime RAM address, little-endian
bytes 7..8   min
bytes 9..10  max
bytes 11..12 factory default
```

This format was cross-checked against multiple numeric and boolean parameters.

## 6.2. D/Q reference-slew defaults

Raw config base:

```text
0x20004180
```

Recovered parameters:

| Axis/function | Raw offset | Runtime address | Descriptor ID | Default |
|---|---:|---:|---|---:|
| D rise | `+0x38D` | `0x2000450D` | `89 CA 01` | **10** |
| D fall | `+0x38F` | `0x2000450F` | `89 CB 01` | **10** |
| Q rise | `+0x391` | `0x20004511` | `89 CC 01` | **10** |
| Q fall | `+0x393` | `0x20004513` | `89 CD 01` | **10** |

For Q specifically:

```text
0x08013E42  load raw +0x391 -> Q rise
0x08013E46  load raw +0x393 -> Q fall
...
0x080142A8  write normal_core +0xB6
0x080142B2  write normal_core +0xB8
```

### Important correction

Q rise and Q fall are **separate configuration fields**.

They happen to both default to `10` in this build.

This matters for EVistDrive because a future tune may keep a quiet/slow rise while allowing a different normal fall rate, without changing the underlying architecture.

For first G532-like A/B testing, however, use symmetric rise/fall time behavior.

---

# 7. Rejected earlier hypothesis: +0x1C6/+0x1C8/+0x1CA

Fields:

```text
raw +0x1C6
raw +0x1C8
raw +0x1CA
```

have factory descriptors with:

```text
default = 2400
max     = 3200
```

They were previously considered possible Q-slew profiles.

Dataflow now proves they do **not** feed:

```text
normal_core +0xB6 / +0xB8
```

Do not use them for quiet-start implementation.

---

# 8. Current-vector maximum used to quantify the ramp

## 8.1. Source parameter

Raw config:

```text
base + 0x279
address = 0x200043F9
```

Factory descriptor:

```text
ID      = 89 40 01
min     = 0
max     = 65535
default = 55
```

## 8.2. Conversion to FOC current-vector domain

The FOC setup converts this value to the current-vector limiter radius by the equivalent of:

```c
Imax_internal = floor(raw_limit * 16384 / 100);
```

For the factory default:

```text
raw_limit = 55
Imax_internal = 9011
```

Therefore the default geometric current-reference radius is:

```text
~55% of Q14 full scale
```

### Do not mislabel this

This is enough to quantify the **normalized ramp time**.

It is not enough, from the BIN alone, to assert:

```text
55 == 55 A
```

The physical A/count mapping of the G532 current-sense chain remains a separate calibration question.

---

# 9. Exact normalized START ramp from factory defaults

Assume:

```text
Id_target = 0
Iq_target >= current-vector limit
Q slew state starts at 0
Imax_internal = 9011
Q rise_step = 10
FOC_HZ = 16000
```

Number of updates:

```text
ceil(9011 / 10) = 902 cycles
```

Time:

```text
902 / 16000 = 0.056375 s
              = 56.375 ms
```

So the internal G532 Q reference has a default envelope of approximately:

```text
0 -> maximum allowed Iq in 56.4 ms
```

when the target is already at the maximum allowed vector radius.

This is a very useful implementation target because it is independent of the unknown physical A/count scale.

---

# 10. First torque-producing cycle — why there is almost no click

Immediately after Q-slew reset:

```text
state = 0
```

If target is large, the first active cycle produces at most:

```text
Iq_ref = 10 internal units
```

Relative to factory current-vector limit:

```text
10 / 9011 ~= 0.0011098
          ~= 0.111% Imax
```

Relative to Q14 full-scale:

```text
10 / 16384 ~= 0.0610%
```

This is critical.

Even if the upstream demand jumps immediately to a large value, the **first FOC torque command cannot jump to it**.

The gearbox receives an extremely small initial torque increment.

With the correct rotor angle already seeded, there is no large wrong-angle pulse either.

These two effects together strongly explain the quiet mechanical pickup of identical geartrain lash.

---

# 11. Factory ramp timeline at maximum target

For:

```text
Imax=9011
step=10/cycle
FOC=16 kHz
```

| Time after first active FOC cycle | Shaped Iq internal | % of default Imax |
|---:|---:|---:|
| 62.5 us | 10 | 0.11% |
| 1 ms | 160 | 1.78% |
| 5 ms | 800 | 8.88% |
| 10 ms | 1600 | 17.76% |
| 20 ms | 3200 | 35.51% |
| 30 ms | 4800 | 53.27% |
| 40 ms | 6400 | 71.02% |
| 50 ms | 8000 | 88.78% |
| ~56.4 ms | 9011 | 100% |

The curve is linear in internal current-reference space.

It is **not** a long neutral delay followed by a sudden torque release.

---

# 12. STOP ramp — last Iq to zero

Factory Q fall:

```text
fall_step = 10/cycle
```

Therefore, from the default maximum current-vector radius:

```text
9011 -> 0
```

also takes approximately:

```text
56.4 ms
```

Examples:

```text
100% Imax -> 0 : ~56.4 ms
 50% Imax -> 0 : ~28.2 ms
 20% Imax -> 0 : ~11.3 ms
```

This provides a useful compromise:

- much slower than a one-ISR hard drop,
- still short enough that normal torque release does not require a long overrun.

### Important separation

This normal fall ramp is **not** for safety faults or brake hard cut paths.

Safety-critical paths must be able to bypass it and hard-disable the bridge immediately.

---

# 13. `Iq=0` is not `MOE OFF`

A full static audit of the G532 bridge-MOE primitive found:

```text
1 normal central gate
+
3 direct protection/hard-off callsites
```

The central normal gate does not simply test:

```text
Iq != 0
```

or:

```text
Iq_ref != 0
```

Instead bridge permission is separated from torque reference.

Active FOC publishes an `output-valid` condition.

Neutral/reset does not automatically clear that latch.

Therefore ordinary:

```text
positive Iq -> zero Iq -> positive Iq
```

can occur without:

```text
MOE OFF -> MOE ON
```

This is a major architectural difference from a controller that treats every torque-zero event as bridge shutdown.

---

# 14. Recommended EVistDrive state machine

Implement explicit ownership. Suggested state enum:

```c
typedef enum {
    FOC_DISABLED = 0,
    FOC_PREPARE,
    FOC_ACTIVE,
    FOC_ARMED_ZERO,
    FOC_PREPARE_DISARM,
    FOC_FAULT
} foc_lifecycle_t;
```

Do not infer lifecycle directly from `Iq_ref == 0`.

---

# 15. `FOC_DISABLED`

Properties:

```text
MOE = OFF
no normal torque output
safety state known
```

Transition to PREPARE only when all existing EVist safety prerequisites allow motor arming.

Do not weaken any existing interlock to reproduce G532 behavior.

---

# 16. `FOC_PREPARE`

Required ordering:

```text
1. validate current ADC path / offsets
2. validate rotor electrical angle
3. capture current theta_e as FOC seed
4. reset D reference slew state to 0
5. reset Q reference slew state to 0
6. reset D/Q PI integrator state
7. reset anti-windup/tracking residual state
8. reset any voltage/current-limiter dynamic state that can carry stale output
9. prepare coherent neutral PWM:
       CCR_A=ARR/2
       CCR_B=ARR/2
       CCR_C=ARR/2
10. guarantee shadow/preload registers contain coherent values
11. only after that permit transition to ACTIVE
```

### Key rule

Do **not** reset rotor angle to zero.

Do **not** let the first active FOC cycle use stale PI or stale Iq state.

---

# 17. Interaction with existing EVistDrive FW-117 neutral dwell

EVistDrive already has/has tested a neutral-dwell concept around bridge startup.

Do not simply add another independent delay on top of it.

Refactor ownership so that neutral preparation is part of lifecycle.

Preferred conceptual order:

```text
PREPARE while output is not torque-producing
 -> coherent neutral loaded
 -> state clean
 -> angle valid
 -> ACTIVE
 -> Iq slew begins from zero
```

If the existing implementation currently does:

```text
MOE ON
 -> N cycles neutral
 -> release FOC
```

preserve it initially only as a compatibility/safety guard, then A/B test whether it can be shortened once the G532-style coherent PREPARE + Q slew is proven.

Do not remove a safety delay in the same patch that introduces the new lifecycle.

---

# 18. `FOC_ACTIVE`

Normal target path should be:

```text
raw Id/Iq target
      |
      v
current-vector limiter
      |
      v
D/Q slew
      |
      v
D/Q PI
      |
      v
voltage-vector limiter
      |
      v
tracking anti-windup
      |
      v
inverse Park / SVPWM
```

For quiet START, Q slew state **must be zero at entry**.

First active current reference must be limited by the slew, not bypass it.

---

# 19. How to translate the G532 ramp to EVistDrive units

Do **not** copy:

```text
step = 10
```

unless EVistDrive uses exactly the same internal current normalization, which is not assumed.

Copy the **time envelope** instead.

Reference:

```text
G532 default 0 -> allowed Imax ~= 56.4 ms
G532 default allowed Imax -> 0 ~= 56.4 ms
```

If EVistDrive uses integer internal Iq units:

```c
const float t_start = 0.056375f;
const float t_stop  = 0.056375f;

rise_step = ceilf(iq_limit_internal / (foc_hz * t_start));
fall_step = ceilf(iq_limit_internal / (foc_hz * t_stop));
```

If EVistDrive uses physical amperes / float:

```c
diq_dt_up   = iq_limit_A / 0.056375f;
diq_dt_down = iq_limit_A / 0.056375f;

Iq_ref += clamp(Iq_target - Iq_ref,
                -diq_dt_down * dt,
                +diq_dt_up   * dt);
```

Use the **actual EVist FOC update rate**, not 16 kHz unless that is measured/confirmed in the current EVist build.

---

# 20. First implementation should be symmetric

Although the limiter supports separate rise/fall and G532 stores them separately, factory defaults are:

```text
rise = 10
fall = 10
```

So the first G532-like EVist A/B build should use approximately equal normalized rise/fall times.

Only after reproducing baseline smoothness should you tune asymmetry.

Potential later tuning:

```text
normal rider release: perhaps faster than rise
brake/safety: immediate bypass
fault: immediate hard-off
```

Do not mix those tuning experiments into the first parity patch.

---

# 21. `FOC_ARMED_ZERO`

Entry condition for normal rider-demand zero:

```text
Iq_target = 0
```

Behavior:

```text
Q slew -> 0
PI continues regulating current toward zero
theta estimator stays alive
current ADC stays alive
FOC stays alive
SVPWM/sample timing stays alive
bridge ownership remains valid unless another interlock requests disarm
```

Do not implement:

```c
if (iq_ref == 0)
    moe_off();
```

This is specifically contrary to the G532 architecture found in the binary.

---

# 22. Why ARMED_ZERO improves mechanical silence

If every rider torque pause causes:

```text
FOC off
MOE off
position/control state loses continuity
then restart
```

then every re-engagement becomes another startup transient.

With ARMED_ZERO:

```text
moment -> 0
but control remains coherent
then next target -> Q slew rises from 0
```

This avoids repeated bridge re-arming and reduces the energy that can suddenly take up gear lash.

---

# 23. Neutral after ACTIVE

When the higher-level motor-control lifecycle actually leaves an active mode, G532 returns through internal state 0 and on the following dispatcher pass executes the neutral/reset path.

The important behavior is:

```text
active output
 -> control request deactivates
 -> internal state 0
 -> neutral/reset
 -> A=B=C=50%
```

This is different from:

```text
active PWM
 -> immediate MOE OFF at arbitrary final duty/current
```

For EVistDrive, if normal full disarm is needed, first converge to zero-current / coherent-neutral state unless safety requires hard-off.

---

# 24. `FOC_PREPARE_DISARM`

The exact G532 long-idle timeout / outer power-off policy is still not fully recovered.

Therefore do **not** invent a value and label it “G532 timeout”.

Recommended EVist architecture:

```text
ARMED_ZERO
 -> optional configured idle policy
 -> PREPARE_DISARM
```

PREPARE_DISARM should require, for normal non-fault shutdown:

```text
Iq_target == 0
Iq_ref has reached zero
|Iq_measured| qualified near zero
|Id_measured| qualified near zero
neutral CCR prepared coherently
PWM shadow update completed
then MOE OFF
```

The exact qualification thresholds and timeout must be derived from EVist current noise and hardware.

---

# 25. Fault / brake / protection path

Never force all shutdowns through a 56 ms ramp.

At minimum distinguish:

```text
NORMAL TORQUE RELEASE
 -> Q slew
 -> ARMED_ZERO

NORMAL LONG-IDLE DISARM
 -> zero-current qualification
 -> neutral
 -> MOE OFF

SAFETY / HARD FAULT
 -> existing immediate safe shutdown
```

If brake semantics in the existing firmware require immediate torque removal, preserve them until separately validated.

No G532 parity goal justifies weakening a safety response.

---

# 26. Do not change these in the same first patch

To isolate causality, first quiet-start/stop patch should not simultaneously retune:

- torque sensor processing,
- assist maps,
- current PI `Kp/Ki`,
- Hall/angle estimator gains,
- current ADC calibration,
- dynamic sample-point algorithm,
- battery-current limiter,
- field weakening,
- thermal derating.

Those are independent improvements and should have separate A/B cards.

---

# 27. Required EVistDrive audit before code changes

Agent must first identify and document current EVist paths:

```text
all MOE ON callsites
all MOE OFF callsites
all CCR writes
FOC ISR owner
Iq_ref source before PI
Iq_ref filtering/ramping
Id_ref source
PI reset paths
PI integrator variables
anti-windup/tracking variables
rotor angle source
angle validity condition
current offset validity condition
bridge fault/interlock conditions
```

Output a call-flow table before modifying production code.

Goal: prevent a new lifecycle state from fighting an old hidden `MOE_OFF()` path.

---

# 28. Suggested EVistDrive lifecycle API

Example interface; adapt names to existing project conventions:

```c
void foc_lifecycle_init(void);

void foc_request_active(bool enable);

void foc_lifecycle_fast_tick(void);

bool foc_angle_valid(void);
bool foc_current_offsets_valid(void);

void foc_prepare_state(void);
void foc_enter_active(void);
void foc_enter_armed_zero(void);
void foc_prepare_disarm(void);

void foc_fault_hard_off(void);
```

The fast tick should be the sole normal owner of lifecycle transitions.

Avoid multiple modules directly toggling MOE during ordinary operation.

---

# 29. Suggested state transition logic

Illustrative pseudocode:

```c
switch (foc_state) {

case FOC_DISABLED:
    if (run_request && safety_ok()) {
        foc_state = FOC_PREPARE;
    }
    break;

case FOC_PREPARE:
    if (!safety_ok()) {
        foc_state = FOC_DISABLED;
        break;
    }

    if (!foc_current_offsets_valid())
        break;

    if (!foc_angle_valid())
        break;

    foc_seed_theta(current_theta_e);
    foc_reset_pi_and_tracking();
    foc_reset_dq_slew_to_zero();
    foc_set_neutral_pwm();
    foc_mark_coherent_prepared();

    foc_state = FOC_ACTIVE;
    break;

case FOC_ACTIVE:
    if (!safety_ok()) {
        foc_fault_hard_off();
        foc_state = FOC_FAULT;
        break;
    }

    if (!run_request) {
        // Do not hard-off here.
        set_iq_target(0);
    }

    foc_run_current_loop();

    if (iq_target_is_zero() && iq_ref_is_zero()) {
        foc_state = FOC_ARMED_ZERO;
    }
    break;

case FOC_ARMED_ZERO:
    if (!safety_ok()) {
        foc_fault_hard_off();
        foc_state = FOC_FAULT;
        break;
    }

    set_iq_target(0);
    foc_run_current_loop();

    if (run_request) {
        foc_state = FOC_ACTIVE;
        break;
    }

    if (normal_disarm_policy_requests_off()) {
        foc_state = FOC_PREPARE_DISARM;
    }
    break;

case FOC_PREPARE_DISARM:
    set_iq_target(0);
    foc_run_current_loop();

    if (!zero_current_qualified())
        break;

    foc_set_neutral_pwm();

    if (!neutral_pwm_committed())
        break;

    moe_off_normal();
    foc_state = FOC_DISABLED;
    break;

case FOC_FAULT:
    // Existing fault recovery policy only.
    break;
}
```

This is an EVist implementation pattern, not literal recovered G532 source code.

---

# 30. Q-slew implementation requirement

The slew must sit **before current PI**, after current-vector validity/limit.

Preferred ordering:

```text
raw Id/Iq target
 -> circular current-vector limit
 -> D/Q rate limit
 -> current PI
```

Do not put the quiet-start ramp only on PWM duty after the PI.

Reason:

- PI then receives a physically plausible slowly changing current target,
- integrator behavior remains coherent,
- saturation/anti-windup sees the real control problem,
- first commanded torque is bounded in current space.

---

# 31. PI/tracking reset requirement

Before a cold/disarmed start, clear at least:

```text
Id PI integral
Iq PI integral
D tracking/back-calculation memory
Q tracking/back-calculation memory
D reference slew state
Q reference slew state
any voltage-limiter residual stored across cycles
```

If EVistDrive has a bumpless-transfer/preload scheme, audit it carefully.

The goal is not “always zero every state” but specifically:

```text
no stale torque-producing controller memory from the previous run
```

For first parity implementation, zero-state reset is closest to the G532 path recovered here.

---

# 32. Rotor-angle seed requirement

At PREPARE:

```c
theta_seed = theta_e_current;
```

not:

```c
theta_seed = 0;
```

and not:

```c
theta_seed = stale_theta_from_previous_disable;
```

Validation should include:

- legal angle range,
- position source valid state,
- no stale timeout,
- correct direction/sign convention.

If the EVist position estimator requires some motion to become valid, do not fake validity; handle that as a separate startup mode.

---

# 33. PWM coherence requirement

Before bridge arm or normal disarm, all three final timer channels must be coherent.

Neutral intent:

```text
CCR_A = CCR_B = CCR_C = timer_half_period
```

Need to account for:

- center-aligned PWM semantics,
- timer preload/shadow registers,
- update-event timing,
- complementary-output dead-time behavior.

Do not assume that writing CCR registers means the hardware pins have already loaded the new duty.

A normal disarm must wait until the neutral shadow values are actually committed if this is required by the timer architecture.

---

# 34. MOE ownership

Recommended rule:

```text
only one normal lifecycle owner may request MOE ON/OFF
```

Other modules may provide interlock/fault flags but should not randomly toggle the bridge during normal zero torque.

Hard-fault code remains allowed to force immediate hardware off.

Audit outcome should classify every existing callsite as:

```text
NORMAL OWNER
FAULT HARD-OFF
LEGACY / MUST REMOVE OR ROUTE THROUGH OWNER
```

---

# 35. Instrumentation — mandatory before A/B road testing

Use a RAM ring buffer around transitions. Avoid streaming every FOC sample live over CAN if that perturbs timing.

Minimum per-sample fields:

```text
timestamp / FOC cycle counter
lifecycle_state
run_request
MOE
output_valid / bridge_request
fault/interlock mask

theta_e
theta_seed
angle_valid

Iq_target_raw
Iq_target_after_current_vector_limit
Iq_ref_after_slew
Iq_measured
Id_target_raw
Id_ref_after_slew
Id_measured

Q_slew_state
Q_slew_rise_step
Q_slew_fall_step

PI_q_integral
PI_d_integral
antiwindup_q
antiwindup_d
Vq_requested
Vq_applied
Vd_requested
Vd_applied

CCR_A
CCR_B
CCR_C
MOE command
sample_point
SVPWM sector
```

Also log current offset-valid / current-sample-valid state if available.

---

# 36. Transition-triggered capture windows

Capture events:

```text
A. first run_request rising edge
B. first MOE rising edge
C. first Iq_ref != 0
D. Iq target falling to zero
E. Iq_ref reaches zero
F. active -> ARMED_ZERO
G. normal MOE falling edge
H. fault MOE falling edge
```

Recommended retained window:

```text
pre-trigger: enough to see prior stable state
post-trigger: at least 100 ms for G532-like ~56 ms envelope
```

For full-rate FOC data, store in RAM and export after the event.

---

# 37. START acceptance trace

Expected G532-like pattern:

```text
angle valid before torque
PI dynamic state clean
Q slew state = 0 before first active update
neutral/coherent CCR prepared
first shaped Iq very small
Iq rises monotonically at bounded rate
no stale Vq jump
no MOE chatter
```

A failure example:

```text
MOE rises
then CCR jumps to old values
then PI integral is nonzero
then Iq_ref jumps directly to target
```

That is not parity.

---

# 38. STOP acceptance trace

Expected normal torque-zero pattern:

```text
Iq_target -> 0
Iq_ref decreases at bounded rate
Iq_measured follows toward zero
FOC remains active
MOE does not fall only because Iq_ref reached zero
ARMED_ZERO is stable
```

Expected normal full-disarm pattern, if policy asks for it:

```text
Iq_ref already zero
measured current qualified near zero
neutral CCR committed
then MOE OFF
```

Fault pattern is separate and may hard-off immediately.

---

# 39. A/B implementation cards

## Card QS-1 — diagnostics only

No production control changes.

Add transition ring buffer and state visibility.

Goal:

- establish exact existing EVist start/stop sequence,
- measure current first-step spike,
- measure MOE behavior around zero torque.

## Card QS-2 — coherent PREPARE

Add:

- current theta seed,
- PI/tracking reset,
- D/Q slew reset,
- coherent neutral PWM.

Keep existing FW-117 dwell unchanged initially.

Goal: isolate state-cleanup benefit.

## Card QS-3 — G532-like current-reference ramp

Add/replace Q slew so full allowed Iq envelope is approximately:

```text
~56.4 ms up
~56.4 ms down
```

in EVist units.

Do not retune PI.

## Card QS-4 — ARMED_ZERO

Normal rider zero torque no longer automatically causes normal MOE OFF.

Keep FOC and position/current acquisition alive.

Goal: eliminate repeated hard re-arm during ordinary stop/start cycling.

## Card QS-5 — normal delayed disarm

Only after QS-4 is stable.

Add normal PREPARE_DISARM with zero-current qualification and coherent neutral commit.

Do not invent a G532 timeout; use an EVist-specific configurable policy.

## Card QS-6 — FW-117 dwell reduction A/B

Only after QS-2..QS-5 prove stable.

Compare existing dwell versus shorter dwell / one-cycle staging.

Goal: determine whether old dwell is still necessary on EVist PCB.

---

# 40. Recommended feature flags during development

```c
#define FOC_LIFECYCLE_V2_ENABLE          0
#define FOC_PREPARE_THETA_SEED_ENABLE    0
#define FOC_DQ_SLEW_V2_ENABLE            0
#define FOC_ARMED_ZERO_ENABLE            0
#define FOC_NORMAL_DISARM_V2_ENABLE      0
```

Enable one card at a time.

Keep rollback trivial.

---

# 41. Configuration parameters to expose

Do not expose raw G532 internal `10` as a user-facing value.

Prefer physical/time-domain EVist parameters:

```text
FOC_START_IQ_RAMP_MS
FOC_STOP_IQ_RAMP_MS
FOC_ARMED_ZERO_ENABLE
FOC_NORMAL_DISARM_IDLE_MS   // EVist policy; not yet a recovered G532 constant
FOC_ZERO_CURRENT_QUAL_CYCLES
FOC_ZERO_IQ_THRESHOLD
FOC_ZERO_ID_THRESHOLD
```

Initial parity targets:

```text
START_IQ_RAMP ~= 56.4 ms to configured allowed Iq limit
STOP_IQ_RAMP  ~= 56.4 ms from configured allowed Iq limit
```

Do not assign a final long-idle timeout from this reverse.

---

# 42. Runtime values to read on an actual G532 if exact unit parity is required

The BIN gives factory defaults. NVM can override them on a real controller.

Read during runtime if debugger access exists:

```text
0x2000450D  D rise config
0x2000450F  D fall config
0x20004511  Q rise config
0x20004513  Q fall config
0x200043F9  raw current-vector limit
```

After core configuration, verify:

```text
0x20002D98 + 0xB6  Q rise actually used
0x20002D98 + 0xB8  Q fall actually used
0x20002D98 + 0x88  current-vector limit actually used
```

Expected factory values from this BIN:

```text
Q rise = 10
Q fall = 10
Imax internal ~= 9011
```

If runtime differs, document NVM override rather than modifying the reverse result.

---

# 43. Board-level differences that may remain after software parity

G532 also has a more advanced measured-current architecture, including three physical phase-current channels and modulation-aware reconstruction behavior.

Therefore software lifecycle parity may not reproduce every last bit of low-current acoustic behavior if EVist hardware has:

- higher current-sense noise,
- slower current amplifier settling,
- different gate-driver dead-time,
- different ADC timing,
- different PWM/sample topology.

This is not a reason to skip the lifecycle changes. It is a reason to evaluate residual noise only **after** software parity.

---

# 44. Why no PI retune should be first

A startup click can come from:

```text
wrong first angle
stale integral
stale slew state
large first Iq step
MOE re-arm transient
```

Retuning `Kp/Ki` to hide those architecture problems is the wrong layer.

First reproduce:

```text
clean state + correct angle + bounded Iq + persistent zero-torque control
```

Then assess whether PI tuning still differs.

---

# 45. Quantitative comparison metric

For each A/B firmware, derive:

```text
peak |Iq_measured - Iq_ref| during first 10 ms
peak |Vq_requested - Vq_applied|
first-cycle Iq_ref increment
maximum CCR discontinuity at arm
number of MOE transitions per stop/start event
time 0 -> requested Iq fraction
time requested Iq -> 0
```

Optional physical measurements:

- microphone close to gearbox,
- frame accelerometer,
- current probe / phase current oscilloscope,
- DC-link current transient.

The primary success metric is lower torque/current discontinuity, not only subjective sound.

---

# 46. Short repeated stop/start test

This test is especially valuable for ARMED_ZERO.

Sequence:

```text
run
 -> target zero for 100 ms
 -> run
 -> target zero for 300 ms
 -> run
 -> target zero for 1 s
 -> run
```

Compare:

```text
number of MOE OFF/ON cycles
first Iq step
current spike
sound
```

A G532-like lifecycle should avoid unnecessary hard bridge re-arm during ordinary short zero-torque intervals.

---

# 47. Cold-start test

From fully disabled bridge:

1. keep rotor stationary,
2. establish valid rotor angle,
3. issue normal run request,
4. record full-rate buffer,
5. repeat at different static rotor positions,
6. repeat with small reverse preload of drivetrain lash if mechanically safe.

Verify first torque vector quality is independent of rotor electrical angle.

A bad seed often shows position-dependent click severity.

---

# 48. Normal stop test

From several Iq levels:

```text
20% of allowed Iq
50%
80%
near allowed max
```

Set normal target to zero.

Verify fall time scales approximately with starting Iq when using a constant per-cycle slew step.

G532-like behavior:

```text
time_to_zero ~= initial_Iq / slope
```

not a fixed arbitrary delay.

---

# 49. Safety regression tests

Mandatory after changing MOE ownership:

- overcurrent fault,
- invalid current ADC / offset state,
- invalid rotor position,
- watchdog/fatal control state,
- brake hard cut if applicable,
- undervoltage/overvoltage hard protection path,
- forced lifecycle inconsistency.

Verify hard fault can always bypass ARMED_ZERO and normal ramp policy.

---

# 50. Common implementation mistakes

## Mistake 1

```text
Increase neutral dwell to 20 ms and call it G532 behavior.
```

Wrong. G532 can stage neutral->active in one 16 kHz cycle.

## Mistake 2

```text
Copy Q step = 10 into EVist internal units.
```

Wrong unless the normalization is identical.

Use ~56.4 ms normalized envelope first.

## Mistake 3

```text
Set theta=0 during PREPARE.
```

Wrong. Preserve/seed current valid rotor angle.

## Mistake 4

```text
Ramp PWM duty instead of Iq target.
```

Wrong layer. G532 shapes D/Q reference before current PI.

## Mistake 5

```text
MOE OFF whenever Iq reaches zero.
```

Contrary to recovered G532 architecture.

## Mistake 6

```text
Route faults through a 56 ms soft stop.
```

Unsafe architecture. Hard fault stays immediate.

## Mistake 7

```text
Change PI gains, sampling, torque map and lifecycle in one build.
```

Destroys A/B attribution.

---

# 51. What is CONFIRMED

- neutral/reset selector is `c8=0`,
- neutral/reset is part of the real lifecycle,
- neutral writes `A=B=C=0x2000`,
- `0x2000` is 50% Q14 phase command,
- neutral seeds current rotor electrical angle instead of forcing zero,
- neutral/reset clears D/Q slew dynamic state,
- PI/tracking state is reset before fresh active output,
- D/Q references have per-cycle slew limiters before PI,
- D rise factory default = 10,
- D fall factory default = 10,
- Q rise factory default = 10,
- Q fall factory default = 10,
- Q rise and fall are separate config fields,
- FOC fast timebase = 16000 Hz,
- default current-vector radius generated from raw default 55 is 9011 internal Q14-like units,
- ideal Q ramp 0->9011 at step10/16k = ~56.4 ms,
- first Q step is only ~0.111% of default Imax,
- active FOC sets output-valid,
- normal central MOE gating is separated from Iq value,
- ordinary `Iq=0` is not directly equivalent to `MOE OFF`,
- dedicated direct protection hard-off paths exist,
- active deactivation returns through internal state 0 / neutral-reset,
- mechanics of compared assemblies are the same; board+firmware are the meaningful differences.

---

# 52. What is still OPEN

- exact physical amperes represented by one G532 internal current-reference count,
- actual NVM override values in the specific physical G532 controller unless RAM/NVM is read,
- full outer system power-off / power-rail shutdown sequence (normal motor-control zero-torque timeout is no longer OPEN; no such timeout was found),
- exact EVist zero-current qualification thresholds needed for safe normal disarm,
- how much residual acoustic difference remains solely from PCB current-sense/gate-drive quality after software parity.

These do **not** block implementation of coherent PREPARE, normalized Q-slew, or ARMED_ZERO.

---

# 53. Agent deliverables required for EVistDrive implementation

Before production code modification, agent must produce:

1. current EVist START call flow,
2. current EVist STOP call flow,
3. all MOE callsites classification,
4. all FOC reset/preload state variables,
5. exact current `Iq_ref` path into PI,
6. actual EVist fast-loop frequency,
7. current-unit domain / max-Iq internal value,
8. minimal patch plan with feature flags,
9. rollback plan,
10. diagnostic ring-buffer design.

After implementation, agent must provide:

1. build result,
2. diff summary by file/function,
3. A/B logs,
4. first-Iq-step measurement,
5. Q ramp measured time,
6. MOE transition count during normal stop/start,
7. fault regression result,
8. updated documentation/evidence ledger.

---

# 54. Recommended implementation order

```text
PHASE 0  AUDIT ONLY
   |
PHASE 1  DIAGNOSTICS / RING BUFFER
   |
PHASE 2  COHERENT PREPARE
          theta seed + reset PI/tracking + reset D/Q slew + neutral
   |
PHASE 3  G532-LIKE Q SLEW TIME ENVELOPE (~56.4 ms to allowed max)
   |
PHASE 4  ARMED_ZERO / NO NORMAL MOE TOGGLE AT ZERO TORQUE
   |
PHASE 5  NORMAL PREPARE_DISARM WITH ZERO-CURRENT QUALIFICATION
   |
PHASE 6  OPTIONAL REDUCTION OF LEGACY FW-117 DWELL AFTER A/B PROOF
```

Do not skip directly to Phase 4 before auditing all legacy MOE paths.

---

# 55. Final engineering model

The quiet G532 behavior is best understood as reduction of torque discontinuity at every ownership boundary.

```text
START
-----
valid theta
 +
clean controller memory
 +
coherent neutral PWM
 +
very small first Iq step
 +
bounded dIq/dt
 =
no electrical hammer into drivetrain lash

STOP
----
bounded negative dIq/dt
 +
PI actively drives current toward zero
 +
FOC remains coherent at zero torque
 +
no unnecessary MOE cycle
 +
neutral before eventual normal disarm
 =
no abrupt release / re-engagement of drivetrain lash
```

That is the architecture EVistDrive should reproduce.

---

# 56. One-line instruction for an implementation agent

> Implement G532-style quiet START/STOP in EVistDrive by replacing “bridge toggle defines torque lifecycle” with an explicit `PREPARE -> ACTIVE -> ARMED_ZERO -> optional PREPARE_DISARM` state machine, seeding current `theta_e`, clearing stale PI/tracking and D/Q slew state before the first active vector, preparing coherent `CCR_A=CCR_B=CCR_C=T/2`, applying an Iq-reference rate limit equivalent to approximately 56.4 ms from zero to the configured allowed current and approximately 56.4 ms back to zero for initial parity, retaining FOC/MOE through ordinary zero-torque intervals, and preserving immediate independent hard-fault shutdown; instrument every transition before tuning anything else.

# 57. RUNTIME BRIDGE-DISARM POLICY — G532 DOES NOT USE NORMAL ZERO-TORQUE IDLE TIMEOUT

## 57.1 Source-of-truth result

Static reverse of the complete G532 application shows that ordinary zero torque is not converted into a short timed bridge disarm.

The central bridge gate at approximately `0x08014E00` enables MOE when:

```text
motor_control.output_valid == 1
AND interlock_2C1 == 0
AND interlock_2DA == 0
AND interlock_2DB == 0
```

It does **not** test:

```text
Iq
Iq_ref
torque demand
motor speed
zero-torque duration
idle timer
```

`motor_control.output_valid` is read from `0x20004130+0x37`, copied into the global bridge-gate state at `0x200009DC+0x35`, and is set by active FOC execution. Neutral/reset does not clear it.

## 57.2 Interlocks are protection paths, not idle policy

- `global+0x2C1`: OR/aggregate of multiple local fault/status flags; written around `0x080093D6`.
- `global+0x2DB`: protection latch; setting it around `0x08009CCE` immediately executes MOE OFF; separate recovery logic clears it around `0x08009C5C`.
- `global+0x2DA`: protection latch; setting it around `0x0800E3E6` immediately executes MOE OFF; recovery qualification can clear it around `0x0800E452`.

Therefore these fields must not be modeled as normal zero-torque or idle timers.

## 57.3 Normal powered idle model

For G532 parity, use:

```text
ACTIVE
  -> Iq target = 0
  -> Q slew drives Iq_ref to 0
  -> PI actively drives measured Iq toward 0
  -> FOC/ADC/theta remain alive
  -> MOE remains ON
  -> optional motor-control neutral/reset
  -> phase command A=B=C=50%
  -> output_valid remains latched
  -> ARMED_ZERO
```

There is no evidence in the motor-control layer for:

```text
zero torque for N milliseconds/seconds
  -> MOE OFF
```

Full system power-down is a separate outer policy and remains distinct from normal ride stop/start.

## 57.4 EVistDrive implementation rule

**Do not add a short automatic `MOE_OFF()` timeout merely because Iq has reached zero.**

Default parity behavior should be:

```c
if (normal_zero_torque) {
    iq_target = 0;
    keep_foc_running = true;
    keep_angle_tracking = true;
    keep_adc_sync = true;
    keep_moe = true;
    lifecycle = FOC_ARMED_ZERO;
}
```

Only these paths should normally disarm:

```text
A) FAULT / SAFETY
   -> immediate MOE OFF

B) EXPLICIT SYSTEM POWER-DOWN
   -> Iq target 0
   -> qualify current near zero
   -> coherent neutral PWM
   -> MOE OFF
   -> power-domain shutdown
```

If EVistDrive wants a multi-minute energy-saving sleep timeout, implement it as an explicit EVist power-management feature, not as part of the G532 quiet-stop parity patch.

## 57.5 Update to previous OPEN item

Previous wording "exact outer application policy / timeout for long-idle bridge disarm" is refined:

- **normal motor-control zero-torque timeout:** no evidence of one; treat `ARMED_ZERO` as persistent while the system remains operational and fault-free;
- **full application/system power-off:** still OPEN at the rail/power-management level.

---

# 58. FACTORY START/STOP NUMBERS — AGENT QUICK REFERENCE

Use this table as the initial parity target, not raw-count copy into another controller domain.

| Item | G532 factory default / behavior | EVistDrive rule |
|---|---:|---|
| FOC fast loop | `16000 Hz` | use actual EVist measured loop rate |
| D rise step | `10 internal counts/cycle` | convert by normalized ramp time |
| D fall step | `10 internal counts/cycle` | convert by normalized ramp time |
| Q rise step | `10 internal counts/cycle` | convert by normalized ramp time |
| Q fall step | `10 internal counts/cycle` | convert by normalized ramp time |
| Raw current-vector limit default | `55` | do not interpret as amperes |
| Internal vector radius from default 55 | `9011` | use EVist own allowed-Iq domain |
| 0 -> default max Q time | `~56.375 ms` | initial parity target `~56.4 ms` |
| default max Q -> 0 time | `~56.375 ms` | initial parity target `~56.4 ms` |
| first active Q step | `10 / 9011 = ~0.111%` of default allowed max | preserve similarly tiny first torque step |
| neutral command | `A=B=C=0x2000` Q14 = 50% | `CCR_A=CCR_B=CCR_C=T/2` |
| neutral->active logical staging | next 16 kHz pass possible = `62.5 us` | no need to invent multi-ms dwell |
| rotor angle at prepare | preserve/current `theta_e` | seed current valid angle before active vector |
| PI/tracking at prepare | reset | clear stale integral/tracking state |
| zero torque bridge policy | MOE/FOC can remain ON | `ARMED_ZERO` |
| hard fault | immediate bridge off | retain independent hard-off |

### EVist normalized slew formula

```c
const float g532_parity_ramp_s = 0.056375f;

rise_step_internal = ceilf(
    EVIST_ALLOWED_IQ_INTERNAL /
    (EVIST_FOC_HZ * g532_parity_ramp_s)
);

fall_step_internal = ceilf(
    EVIST_ALLOWED_IQ_INTERNAL /
    (EVIST_FOC_HZ * g532_parity_ramp_s)
);
```

Do not hard-code G532 raw `10` unless EVistDrive proves it uses the identical current domain and loop frequency.

---

# 59. FINAL PARITY TARGET FOR AGENT

The first production experiment should reproduce this behavior and nothing broader:

```text
COLD START
  PREPARE:
    current-sense valid
    theta valid
    seed theta
    reset Id/Iq slew state
    reset D/Q PI integral + tracking residual
    write coherent neutral CCR=T/2,T/2,T/2
  NEXT FAST PASS:
    run active FOC
    Q starts at 0
    first Q increment ~= 0.11% of allowed maximum
    ramp envelope ~= 56.4 ms to allowed maximum
    output_valid -> 1
    central bridge gate owns MOE

NORMAL STOP
    target Q -> 0
    Q fall envelope ~= same 56.4 ms from full allowed max
    measured current is actively regulated toward zero
    do NOT toggle MOE merely because Q reached zero
    enter/retain ARMED_ZERO

RESTART FROM ARMED_ZERO
    no bridge re-arm transient
    no angle reacquisition transient
    Q rises from zero through slew

FAULT
    bypass normal ramp if safety requires
    immediate independent hard-off

FULL POWER-DOWN
    separate outer lifecycle
    zero/qualify current -> coherent neutral -> MOE OFF -> system shutdown
```

This is the closest currently supported software explanation for why identical mechanics are substantially quieter with the G532 controller/firmware.
