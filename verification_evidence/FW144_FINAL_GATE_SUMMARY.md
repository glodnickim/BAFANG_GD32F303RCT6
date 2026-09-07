# EVistDrive FW144 — final PC verification summary

**Scope:** FW144 Level-4 virtual rider/bicycle/battery/SOC + real-ride replay on top of the verified FW143 motor-control baseline.

## Production change

FW144 moves the existing SOC/limp/OCV/coulomb/display-anchor mathematics from `main.c` into production module `src/soc_core.c` so the target firmware and Level 4 execute the same implementation. This extraction is not intended to change normal SOC behavior.

Parity evidence:

- OCV and limp parity: PASS;
- 20,000 randomized 1 Hz state transitions against the pre-extraction reference: PASS.

Level-4 rider/bike/battery models are test-plant code only. Their PMSM, rider, drivetrain, road and battery parameters must not be presented as measured M820 constants.

## Full verification results

| Gate | Result |
|---|---:|
| Production C manifest | **60/60 PASS** |
| Full build manifest | **86 entries PASS** |
| Real-module host suites | **70/70 PASS** |
| Whole-pipeline traces | **18/18 PASS** |
| Missed-tick + determinism | **PASS** |
| Supervisory closed-loop fuzz | **10,000/10,000 PASS** |
| Supervisory ASan/UBSan | **1,000/1,000 PASS** |
| Electrical real FOC/PMSM/Hall fuzz | **1,000/1,000 PASS** |
| Electrical ASan/UBSan | **100/100 PASS** |
| Hall cold-start sweep | **48/48 PASS** |
| Walk real-FOC/PMSM/Hall matrix | **126/126 safety/lifecycle PASS** |
| QZERO STOP/restart in current virtual plant | **PASS** |
| Level-4 fixed routes | **9/9 PASS** |
| Level-4 randomized whole-bike physics | **100/100 PASS** |
| Level-4 ASan/UBSan | **25/25 PASS** |
| Recorded-ride replay smoke | **24,000/24,000 rows PASS** |
| Replay determinism | **byte-identical PASS** |
| Replay registration / accepted-baseline workflow self-test | **PASS** |
| BL820 build-tree / packager self-check | **PASS** |

The 100 Level-4 randomized cases contained 34 physically impossible launch cases and 10 marginal launches. These are classified separately from firmware no-start; safety invariants still apply in every case.

The electrical 1,000-case stress run is split into deterministic non-overlapping shards. The seed stream is advanced by the exact seven xorshift draws consumed per randomized real-FOC case, so sharding changes execution time, not the case population.

## SOC evidence

Matched LG-M58T virtual chemistry, which uses the same OCV shape as production, reaches maximum absolute SOC estimator error of approximately **0.19%** in the Level-4 endurance test.

The independent FEB21700G virtual discharge profile exposes an estimator mismatch of up to approximately **14%** in part of the middle SOC range. This is an **OPEN calibration finding**, not evidence that the virtual FEB curve is ground truth and not authorization to retune production SOC without real voltage/current/time data.

The purpose of Level 4 here is to make chemistry mismatch observable and reproducible. Future real battery logs should become replay/calibration evidence.

## Recorded ride workflow

FW144 provides the permanent workflow:

```text
real ride log
 -> import_ride_log.py
 -> canonical sensor trace + coverage metadata
 -> production-C replay
 -> reviewed bug reproduction
 -> register_ride_case.py
 -> accepted regression only after the bug is fixed/reviewed
 -> every future global gate
```

Coverage metadata separates `CORE`, `LIMITS`, `MOTOR`, and `PAS_RAW`. Missing channels reduce evidentiary scope rather than being silently fabricated as full-fidelity measurements.

## Important limitations / remaining hardware gates

- Virtual PMSM/R/L/flux/inertia, bicycle, rider, drivetrain and dynamic battery sag parameters are test assumptions, not measured M820 constants.
- QZERO physical handback remains hardware-sensitive; the current virtual plant proves safe entry/abort/restart behavior, not exact real-bike acoustic feel.
- The FEB21700G SOC discrepancy must be calibrated/decided using real battery V/I/time data before production OCV changes.
- No final ARM `.bin` was generated in this execution environment because exact `arm-none-eabi-gcc 13.2.1` is not installed here. Source/build-tree/BL820 packaging checks pass; a flash candidate still requires the exact target build gate.
- A final firmware candidate still requires one controlled physical M820 validation after exact target compilation.

## Acceptance conclusion

**PC / SIL / digital-twin environment: PASS for continued development.**

**Final physical firmware release: NOT YET CLAIMED.** Exact ARM target build and controlled real-bike validation remain required.
