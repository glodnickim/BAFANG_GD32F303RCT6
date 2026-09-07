# EVistDrive FW145 — final verification summary

Date: 2026-09-07

## Scope

FW145 closes the real-bike -> Level-4 bridge. It adds a diagnostic-only, observation-only live CAN stream
for internal EVistDrive state and tooling that converts the existing CANable `All Traffic` raw text log directly
into canonical Level-4 replay input. It does **not** add a second owner of torque, permission, Iq, FOC, QZERO,
rotor angle, Walk or SOC.

## Firmware telemetry architecture

- production module: `src/ride_telemetry.c` / `inc/ride_telemetry.h`;
- protected extended-ID range: `0x10400..0x10407`;
- `0x10300..0x10307` remains STOP_TRACE; compile-time ID disjointness checks PASS;
- seven coherent data frames share the same low-16 control tick; META carries schema + full 32-bit tick;
- snapshot rate ~47.6 Hz; snapshot construction is rate-limited, not performed at 4 kHz;
- no CAN TX from the 16 kHz FOC ISR;
- QZERO is observed through an ISR-owned one-byte mirror; foreground does not read the QZERO state object;
- critical HMI queue, multiframe traffic and detailed diagnostic dumps have priority;
- best-effort/non-blocking behavior; accepted-but-failed mailbox is accounted and advanced, not retry-flooded;
- normal build remains silent; diagnostic target enables the stream.

Observed channels include rider load, FAST/RUN torque, raw/control cadence, permission/session/debug state,
Iq request/allowed/ref/actual, Id, ERPS, battery V/I/SOC, wheel speed, voltage utilization/limiter flags, QZERO,
theta/Hall/trust, bridge/PWM state and sparse PAS/direction state. Sparse PAS A/B is explicitly **not** claimed
as a raw quadrature-event recorder.

## CANable / Level-4 tooling

- `tools/decode_canable_ride_log.py`: existing CANable text `.log` -> rich decoded CSV + canonical replay CSV + metadata;
- missing telemetry fragments remain explicit; time gaps are not compressed;
- only CORE+STATE snapshots are promoted to replay rows; unavailable optional channels remain missing/NaN;
- `tools/register_canable_ride_case.py`: retains the original raw log bytes, source SHA-256, decoded observations,
  canonical input, manifest and (only after review) accepted output as a permanent regression;
- `tests/test_canable_ride_decode.py`: synthetic wire schema -> CANable raw text -> decoder -> canonical CSV ->
  native production-code replay PASS;
- real pre-FW145 CANable log with 4,436 frames parsed correctly and produced **0 false FW145 telemetry frames**.

## Final PC verification after FW145 integration

- source manifest: **61/61 production C files**, **87 total build entries** — PASS;
- real-module host suites: **71/71 PASS**;
- whole-pipeline deterministic traces: **18/18 PASS**;
- supervisory closed-loop fuzz: **10,000/10,000 PASS**;
- supervisory ASan/UBSan: **1,000/1,000 PASS**;
- electrical real-FOC/PMSM/Hall fuzz: **1,000/1,000 PASS**;
- electrical ASan/UBSan: **100/100 PASS**;
- Hall-start matrix: **48/48 PASS**;
- Walk real-FOC/PMSM/Hall safety/lifecycle matrix: **126/126 PASS**;
- Level-4 fixed routes: **9/9 PASS**;
- Level-4 randomized whole-bike physics: **100/100 PASS**;
- Level-4 ASan/UBSan: **25/25 PASS**;
- recorded-ride replay determinism and registration workflow: PASS;
- FW145 raw-CANable decode -> canonical -> native replay: PASS;
- build-tree/startup/linker/CMSIS/HAL checks: PASS;
- independent BL820 packager/container CRC regression: PASS;
- `git diff --check`: PASS before commit.

## Important caught regression during development

A broad temporary text rename (`current_cal` -> `torque_cal`) accidentally touched the production phase-current
calibration object. Existing FW128C/FW126.7 tests immediately failed and blocked the change. The accidental changes
were fully reverted before this checkpoint; the final diff contains no rename or semantic change of production
`current_cal`. This is retained as evidence that the existing gate detected an unintended motor-control edit.

## Target-build status

The exact target diagnostic `.bin` was **not built in this execution environment**, because Arm GNU
`arm-none-eabi-gcc` 13.2.1 is not installed here. Do not describe FW145 as flash-verified from this environment.
On the supported Windows machine run:

```text
VERIFY_AND_BUILD_DIAGNOSTIC_WINDOWS.bat
```

This invokes the full verification gate and requires the diagnostic M820 target build with the supported compiler.
The normal `VERIFY_AND_BUILD_WINDOWS.bat` continues to build the telemetry-silent normal variant.

## Hardware capture workflow

1. Build and flash the diagnostic target.
2. CANable: `All Traffic`, raw logging with timestamps. No FW145-aware UI decoder is required for capture.
3. Decode:
   `python tools/decode_canable_ride_log.py ride.log --output-prefix ride`
4. Replay:
   `python tools/run_replay.py ride.canonical.csv`
5. After the desired/fixed behavior has been reviewed, register the raw capture as a permanent case:
   `python tools/register_canable_ride_case.py ride.log BUG_NAME --accept-current`

Never use `--accept-current` on a capture that still represents the known bad behavior.
