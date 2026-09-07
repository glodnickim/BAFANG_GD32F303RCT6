# FW143 — Walk Assist 10–60 rpm: contract and test policy

## Production contract

The user-facing Walk target is **chainring/output rpm**, not wheel speed.

```text
supported target: 10..60 rpm
safe default:      30 rpm
70 / 80 / >80:     invalid target input; never a normal Walk command
```

All production validators use the single source of truth in `inc/config.h`:

- `WALK_ASSIST_RPM_MIN = 10`
- `WALK_ASSIST_RPM_DEFAULT = 30`
- `WALK_ASSIST_RPM_MAX = 60`

`assist_modes.c`, `parser.c` and `walk_assist_motor.c` must not define independent numeric ranges.
Out-of-range/stale input falls back to the safe 30-rpm target.

## What PASS means

Walk is not required to be a laboratory servo-speed controller. On a lightly loaded virtual or
physical drivetrain it may float around the requested speed. The hard verification gate checks
properties that must hold regardless of exact M820 inertia/friction/load values:

- every supported 10/15/20/30/40/50/60-rpm target maps to the correct electrical target;
- the motor starts and produces Hall feedback from all six Hall-sector start anchors;
- normal Walk target never becomes a hidden 70/80-rpm command;
- no runaway: the electrical SIL hard-gates physical chainring speed at 80 rpm;
- commanded Iq stays inside the Walk safety ceiling;
- a detected stall must end in the production STALL state with zero final Iq;
- release/brake/fault/wheel-cut safety zeros remain true zeros (covered by the real-module Walk suites);
- Hall-loss/reacquire and jam/stall behavior remain covered by the real `walk_assist_motor.c` / `walk_speed_controller.c` host suites.

## What is evidence-only, not a hard FAIL

Because the SIL PMSM `R/L/flux/inertia/load` are test parameters rather than measured M820 constants,
these are reported as quality metrics but are not used to retune production Walk:

- mean rpm tracking error;
- peak-to-peak rpm float around a low target;
- exact settling time under a particular virtual load.

A high tracking-error warning is a prompt for a physical ride/log, not permission to tune the real
firmware to the virtual motor.

## Full electrical matrix

`sim/evist_sil.c` with `EVD_SIL_REAL_FOC` runs production Walk through:

```text
walk_assist_motor.c
 -> walk_speed_controller.c
 -> shared assist limits
 -> final Iq mailbox
 -> real FOC.c
 -> real PI/vector limiter
 -> real SVPWM
 -> virtual PMSM
 -> physical Hall edges
 -> production Hall timing feedback
 -> Walk governor
```

Deterministic matrix:

```text
7 targets:      10, 15, 20, 30, 40, 50, 60 rpm
3 loads:        light, medium, heavy
6 Hall starts:  all sector anchors
= 126 closed-loop Walk cases
```

Additional invalid targets include `0, 9, 61, 70, 80, 100 rpm` and must resolve to the safe 30-rpm default.
