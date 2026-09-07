# EVistDrive Level 4 - virtual rider / bicycle / battery digital twin

Level 4 closes the loop above the existing electrical SIL:

`rider -> torque/PAS -> production EVistDrive -> real FOC/PI/SVPWM -> PMSM -> drivetrain/bicycle/road -> sensors -> firmware`

Battery is also closed-loop:

`physical battery OCV/SOC/R0/dynamic sag -> Vbus/current -> firmware limits + production soc_core -> displayed SOC`

## What is production code
The simulator links shipped production C for torque conditioning, rider input, assist modes, limits,
ride/session logic, final 16 kHz Iq trajectory, FOC, PI/vector limit, SVPWM, Hall/theta logic, QZERO,
Walk and `soc_core`. Do not replace those blocks with Python equivalents.

## What is a plant assumption, not measured M820 truth
Current test parameters for PMSM R/L/flux, motor-torque scaling, total bike mass, CdA, rolling resistance,
rider response and battery dynamic sag are virtual-plant parameters. They are suitable for regression and
fault discovery but must not be presented as measured M820 constants. Calibrate them later from logs/bench
measurements without changing the production controller merely to fit the model.

## Current deterministic matrix
Fixed routes cover flat/5/10/15% grades, cadence through 120 rpm, SOC 90/80/50/30/20/10/5%, high-sag
battery and matched/mismatched chemistry. SOC endurance sweeps 100/90/80/60/40/20/10/5%.
Fuzz separates robust starts, marginal starts and physically impossible rider stalls.

Hard FAIL: unsafe direction behavior, non-finite state, illegal Iq, angle error outside 3-Hall bound,
runaway, or failure to produce assist when the virtual rider has a clear (>15%) static launch-torque margin.
A marginal/physically impossible start is classified, not mislabeled as a firmware no-start.
