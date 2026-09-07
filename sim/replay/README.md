# EVistDrive recorded-ride replay

`tools/import_ride_log.py` converts CSV captures into a stable schema. The replay is intended
for **real-bike regression**: once a bug is captured, keep the normalized CSV under a regression
case and replay the exact sensor history through current production C modules after every change.

Canonical columns:

- `time_s`
- `cadence_rpm`
- `torque_raw_native` and/or `torque_ckg`
- `wheel_speed_kph`
- `battery_voltage_v`, `battery_current_a`
- `assist_level`, `brake`, `walk`
- `motor_erps`, `iq_actual`
- optional `pas_ab`, `pas_direction` (-1/0/+1)
- optional recorded outputs `recorded_iq_request`, `recorded_iq_ref`

Missing physical channels stay `nan`; the replay harness uses documented safe defaults or
synthesizes PAS edges from cadence. It never silently invents recorded truth.

Example using an existing repository trace:

```bash
python3 tools/import_ride_log.py tests/host/out/RUN_60_ride.csv \
  .build/replay/RUN_60.canonical.csv
```

For a differently named logger:

```bash
python3 tools/import_ride_log.py ride.csv case.csv \
  --map cadence_rpm=Cadence --map torque_ckg=TorqueLoad \
  --map battery_voltage_v=PackV
```

## Coverage classes

The importer can write `--metadata mapping.json` and classifies evidence:
- `CORE`: time + cadence + torque -> assist/control replay;
- `LIMITS`: adds wheel speed + battery V/I -> speed/battery limiter replay;
- `MOTOR`: adds ERPS + measured Iq -> motor tracking context;
- `PAS_RAW`: raw PAS AB and/or decoded direction is present.

A missing class is not a failed log. It is a limit on what conclusions that replay can support.
