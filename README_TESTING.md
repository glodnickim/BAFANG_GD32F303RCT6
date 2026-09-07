# EVistDrive v3 — testable motor-control project

This tree is based on `EvistDrive06092026v3.zip` and contains the production GD32/M820 sources,
portable host/regression tests, two closed-loop SIL backends, an electrical FOC/PMSM/Hall SIL,
and a cross-platform M820_BL820 developer build path.

## One-command PC verification

Quick gate:

```bash
python tools/verify_all.py --quick
```

Full PC gate (10k supervisory fuzz + sanitizers + 1k real-FOC/Hall fuzz):

```bash
python tools/verify_all.py
```

The gate covers:

- source-manifest completeness;
- BL820 packager self-check and an independent CRC/container regression;
- real-module host suites;
- deterministic whole-pipeline traces;
- fast closed-loop rider/PAS/torque/assist/final-Iq SIL;
- real `FOC.c` + `PI_control()` + vector limiter + `svpwm()` + PMSM + physical Hall feedback;
- all six Hall sectors / 24 electrical start angles;
- QZERO stop/restart lifecycle;
- deterministic fuzz;
- AddressSanitizer + UndefinedBehaviorSanitizer.

## Exact M820 target build

Required compiler: **Arm GNU Toolchain arm-none-eabi 13.2.1**.

Cross-platform developer build:

```bash
python tools/build_firmware.py --toolchain "<path-to-arm-none-eabi-bin>"
```

Windows example:

```powershell
python tools\build_firmware.py --toolchain "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin"
```

The builder compiles the explicit `scripts/sources-m820.txt` manifest, startup assembly and the
production linker script, checks target memory-map symbols and RWE segments, creates raw BIN/HEX,
wraps the raw application in the BL820 update container, checks the embedded build identity, and
writes a JSON build manifest with SHA-256 hashes.

Run every PC test and require the exact target build in one command:

```bash
python tools/verify_all.py --require-target
```

`--require-target` fails instead of silently skipping when GCC 13.2.1 is unavailable.

## SIL layers

### Fast supervisory SIL

`sim/evist_sil.c` without `EVD_SIL_REAL_FOC` uses the real production supervisory path and a small
current-loop stand-in. It is intentionally cheap enough for 10,000+ deterministic randomized runs.

### Electrical FOC SIL

The same `sim/evist_sil.c` compiled with `EVD_SIL_REAL_FOC` executes the real production:

```text
PAS / torque / cadence
 -> ride_control
 -> final 16 kHz Iq slew
 -> real FOC.c current measurement / Park
 -> real PI Id/Iq + vector saturation
 -> real inverse Park / SVPWM
 -> virtual PMSM
 -> physical current + rotor motion
 -> physical Hall edges
 -> production rotor_angle + rotor_motion confidence
 -> back to FOC and ride_control
```

QZERO is also executed in this full backend. The motor parameters in the virtual PMSM are test
parameters, not claimed M820 measurements; tests therefore assert architecture/invariants and
safe state transitions rather than tuning production constants to the plant.

### Walk Assist full electrical SIL

Walk Assist is also driven through the same real FOC/PMSM/Hall backend. The deterministic matrix
uses 10/15/20/30/40/50/60 chainring rpm, three virtual loads and all six Hall-sector starts
(126 cases). 70/80/>80 rpm are range/limiter tests, not normal Walk targets. Exact low-load rpm
tracking is reported as a quality metric; small speed float is not a FAIL. Safety/lifecycle
invariants (start, no runaway, current ceiling, safe stall, release/brake/fault/wheel-cut behavior)
are the hard gate. See `docs/FW143_WALK_ASSIST_10_60_TEST_CONTRACT.md`.

## Current motor-control fixes in this branch

- FW139: reject physically impossible PAS reverse bounce before direction safety.
- FW139: remove Hall-gated Gear Preload as a second normal-start current owner.
- FW140: use one conditioned cadence for assist/dynamics while retaining raw cadence for telemetry.
- FW141: make FAST/RUN torque filters depend on elapsed 4 kHz time, not foreground call count.
- FW142: extract the existing PI/vector-saturation math into one production module used unchanged
  by both target firmware and electrical SIL; no algorithm change is intended by the extraction.
- FW143: make Walk target range 10..60 chainring rpm from one shared source of truth and add a
  126-case real-FOC/PMSM/Hall Walk matrix; speed float is evidence, not a virtual-motor tuning gate.

See `VERIFICATION_STATUS_2026-09-07_PL.md` for current evidence and remaining hardware gate.
