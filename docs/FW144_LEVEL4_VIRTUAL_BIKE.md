# FW144 - Level 4 virtual bicycle / battery / SOC / real-ride replay

## Goal
Turn EVistDrive verification from isolated controller tests into a closed virtual bicycle where the
production firmware sees sensor consequences of its own motor command. A hardware bug should become a
permanent replay case after one real capture.

## Closed loop

```text
virtual rider
 -> crank torque / PAS
 -> production torque + cadence + permission + assist + limits
 -> production final Iq @16 kHz
 -> production FOC / PI / SVPWM
 -> virtual PMSM
 -> virtual drivetrain / bicycle / grade / road load
 -> wheel/crank/Hall feedback
 -> firmware

virtual battery truth (SOC/chemistry/R0/dynamic sag)
 -> Vbus + battery current
 -> production limiters + production soc_core
 -> displayed/estimated SOC
```

## Production vs model ownership
Production C is authoritative for control. Level 4 must not duplicate torque filtering, assist mode math,
final Iq, PI/vector limiting, SVPWM, Hall/theta policy, QZERO, Walk or SOC estimator logic.

Plant parameters are assumptions until measured: PMSM R/L/flux, exact motor torque per Iq count, bike mass,
CdA/Crr, rider response and battery dynamic-sag constants. Test safety/invariants against them; do not tune
production firmware merely to make an assumed plant match a desired graph.

## Current battery/SOC evidence
Physical battery profiles include an LG-M58T-like profile matching the production OCV model and an FEB21700G
11S profile used to expose chemistry mismatch. The production SOC math was extracted unchanged to `soc_core.c`
and has a randomized exact-parity host test.

Current Level-4 evidence:
- matched chemistry: max SOC error about 0.2% in the endurance case;
- FEB profile: OCV/model mismatch can reach about 14% in part of the range;
- fixed routes exercise SOC 90/80/50/30/20/10/5%, sag and battery-current limiting;
- endurance sweep starts at 100/90/80/60/40/20/10/5%.

The FEB mismatch is evidence to drive a future SOC/calibration decision. It is not permission to alter SOC
until the battery configuration contract and/or real ride/charge logs define the desired behavior.

## Start classification in fuzz
Random physics can generate a rider who simply cannot overcome static hill load. Level 4 therefore classifies:
- `ROBUST START`: rider launch torque > 115% of static required torque -> assist/Hall start is mandatory;
- `MARGINAL START`: >102% but <=115% -> movement may be too slow to confirm PAS inside the short fuzz window;
- `PHYSICAL STALL`: <=102% -> no firmware no-start allegation; safety invariants still apply.

This prevents a physically impossible scenario from being mislabeled as a firmware regression.

## Real-ride replay
Use `tools/import_ride_log.py` and `tools/run_replay.py`. Registered cases under `sim/replay/cases/` are executed
by the global verification gate. A captured bug should remain unaccepted until the corrected replay output is
reviewed; never freeze known-bug output only to obtain a green hash.

See `protocol/EVISTDRIVE_LIVE_RIDE_LOG_CONTRACT.md` for capture fields and timing discipline.
