# EVistDrive — START HERE

This repository snapshot is intended to be self-contained for continued EVistDrive development.

## 1. Authoritative baseline

Current checkpoint: FW143 development state on top of the verified FW142 electrical-SIL baseline.

Read in this order before changing production code:

1. `START_HERE.md`
2. `AGENTS.md`
3. `docs/ARCHITECTURE_CURRENT.md`
4. `README_TESTING.md`
5. `VERIFICATION_STATUS_2026-09-07_PL.md`
6. `docs/FW143_WALK_ASSIST_10_60_TEST_CONTRACT.md`
7. only then inspect historical/reference material in `docs/reference/`.

Do not reconstruct the architecture from old ticket names or old chats. The current ownership map in `AGENTS.md` and `docs/ARCHITECTURE_CURRENT.md` wins when old documents describe superseded implementations.

## 2. What is included

- production `src/` and `inc/`;
- GD32 HAL/CMSIS under `Firmware/`;
- startup and linker script (`gcc_startup/`, `ldscripts/`);
- exact source manifest and build scripts (`scripts/`);
- cross-platform Python build/verification tools (`tools/`);
- real-module host tests and whole-pipeline regression (`tests/`);
- supervisory and real electrical FOC/PMSM/Hall/QZERO/Walk SIL (`sim/`);
- protocol/config schema (`protocol/`);
- current architecture and agent handoff documentation;
- selected G532/Fake Taxi/TSDZ2/M820 reverse-engineering references under `docs/reference/`;
- full Git history for this packaged development line.

## 3. External prerequisites not bundled

The project deliberately does NOT bundle third-party compiler executables.

For PC verification:

- Python 3;
- a host C compiler supported by the test scripts (GCC/Clang as available);
- Git for normal development workflow.

For the final GD32 target build:

- Arm GNU Toolchain `arm-none-eabi` 13.2 Rel1 / GCC 13.2.1.

The exact compiler requirement already existed in the original EVistDrive build system; it is not a new FW142/FW143 dependency.

On Windows, run:

`VERIFY_AND_BUILD_WINDOWS.bat`

For PC-only verification:

`python tools/verify_all.py --quick`

For the full PC stress gate:

`python tools/verify_all.py`

For a target-gated run (must build the actual ARM image):

`python tools/verify_all.py --require-target`

## 4. Current non-negotiable architecture rules

- one final Iq trajectory owner;
- one final Park/theta owner;
- no Hall-gated Gear Preload reintroduction;
- PAS physical bounce filtering must remain ahead of direction safety;
- control assist uses conditioned cadence, not raw cadence;
- torque filters are elapsed-time based;
- ordinary zero torque must not reintroduce foreground PI resets or normal MOE chatter;
- safety/fault hard-off remains independent from normal ride-feel shaping;
- do not tune production firmware merely to make an uncertain plant model look perfect.

## 5. Walk Assist contract

Normal configured target range is **10..60 chainring/output rpm**. Default remains 30 rpm.

70/80/>80 rpm are not normal Walk targets; they are invalid/overspeed test conditions. Minor speed float at very low load is a quality metric, not a failure. Hard failures are: unsafe runaway, invalid lifecycle, failure to release, unsafe Hall-loss behavior, unsafe stall, or violation of safety ceilings.

## 6. Working rule for future agents

Before every production change:

1. reproduce the issue in a host test or SIL whenever reasonably possible;
2. identify the single owning layer;
3. make the smallest change there;
4. run the relevant focused test;
5. run the project verification gate;
6. document the result and any model assumptions;
7. only then produce a target firmware candidate.

Do not send the user back to repeated road flashing merely because isolated unit tests pass.
