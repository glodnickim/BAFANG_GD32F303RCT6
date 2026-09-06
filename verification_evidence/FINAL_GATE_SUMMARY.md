# FW142 final PC/SIL gate evidence

Baseline: `EvistDrive06092026v3.zip`

Production/control checkpoint: `2599bc7` (`FW142: add real electrical FOC Hall SIL and portable target build`).

## Full stress gate

- real-module host: **69/69 PASS**
- whole-pipeline deterministic traces: **18/18 PASS**
- missed-tick regression: **PASS**
- supervisory deterministic fuzz: **10,000/10,000 PASS**
- supervisory ASan+UBSan fuzz: **1,000/1,000 PASS**
- real FOC/PMSM/Hall/QZERO deterministic fuzz: **1,000/1,000 PASS**
- real FOC ASan+UBSan fuzz: **100/100 PASS**
- Hall start sweep: **48/48 PASS** (24 electrical start angles x 2 loads)
- worst permission -> first Hall in full FOC sweep: **45 ms**
- maximum initial sector-centre error: **30 degrees electrical**
- real `svpwm()` full-angle sweep at `_U_MAX=1920`: **0 geometry clamp hits**
- BL820 packager self-check + independent bitwise CRC/container regression: **PASS**

## Important STOP/QZERO observation

The electrical plant is deliberately not treated as calibrated M820 truth. With the current test
plant, QZERO enters at about 156 erps and its current guard aborts at about 127 erps. The test
requires the state machine to enter, exit safely and restart without stale Iq; it does **not** tune
production QZERO constants to force a handback in an uncalibrated plant.

## Target-build status

The exact target builder and BL820 packager are ready and checked. This execution environment does
not contain Arm GNU `arm-none-eabi-gcc 13.2.1`, and direct toolchain download was blocked, so an
exact target ELF/BIN was not generated here.

Required command on a machine with Arm GNU 13.2.1:

```text
python tools/verify_all.py --require-target
```

A target BIN must not be called hardware-verified until the controlled M820 gate in
`VERIFICATION_STATUS_2026-09-06_PL.md` is completed.
