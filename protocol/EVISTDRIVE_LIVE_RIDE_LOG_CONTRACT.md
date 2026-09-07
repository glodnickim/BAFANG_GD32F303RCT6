# EVistDrive live-ride capture -> permanent replay regression

## Purpose
A real-bike fault must be capturable once and replayable forever. The logger is observational:
it must not own control state, change timing policy, or introduce blocking CAN traffic in the FOC ISR.

## Canonical replay inputs
Minimum useful capture (assist-path replay):
- monotonic `time_s` or hardware control tick
- `cadence_rpm`
- `torque_raw_native` (preferred) or `torque_ckg`

Full-fidelity capture adds:
- `wheel_speed_kph`
- `battery_voltage_v`, `battery_current_a`
- assist level / mode
- brake and Walk state
- `motor_erps`, Hall state/age where available
- `iq_actual`
- raw PAS A/B or decoded direction/event+gap where available

## Strongly recommended diagnostic outputs
These are not replay inputs, but make origin attribution possible:
- torque FAST / RUN / load
- raw cadence and control cadence
- ride permission/session + direction inhibit reason
- mode demand / `Iq_request`
- post-limiter `Iq_allowed`
- final `Iq_ref`
- `Iq_actual`, `Id_actual`
- `u_abs` / voltage saturation
- battery limiter, speed limiter, thermal limiter bits
- `theta_final`, Hall confidence/age
- QZERO state/reason
- firmware build identity + configuration-bank checksum/version

## Time discipline
Prefer a hardware tick or monotonic microsecond timestamp. Do not reconstruct time later from row count
when the real logger can provide it. Missed rows must create a timestamp gap, not a compressed timeline.

## Workflow
1. Capture the real bike without changing motor-control behavior.
2. Export CSV (or convert the capture to CSV).
3. Normalize:
   `python3 tools/import_ride_log.py raw.csv canonical.csv`
4. Replay current firmware:
   `python3 tools/run_replay.py canonical.csv`
5. Once the desired/fixed behavior is reviewed, register it:
   `python3 tools/register_ride_case.py raw.csv BUG_NAME --accept-current`
6. `tools/verify_all.py` replays every registered case on future changes.

Never accept the output of a known-bug firmware merely to turn a regression green.
