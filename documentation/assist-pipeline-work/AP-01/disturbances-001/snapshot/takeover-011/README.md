# EVistDrive Controller Lab — source-linked WWW analyzer

Controller Lab is a forced-input host harness for **the production EVistDrive control path**.
It is intentionally not a Python/JavaScript rewrite of assist or Iq math.

## What it does

Synthetic crank/rider input:

- mean crank torque (Nm), two-leg ripple and left/right asymmetry;
- cadence plus optional slow cadence variation;
- forced wheel speed;
- forced pack voltage and SOC;
- assist level 0..5;
- Power Linear / Progressive / eMTB / Torque / Power Curve mode.

The native harness then executes the current checkout's production C path:

`PAS + torque_input -> cadence_filter -> rider_input -> assist_modes -> ride_control -> iq_chain -> fast_iq_slew`

The web UI plots generator torque/cadence and the production observations including FAST/RUN torque,
raw/conditioned cadence, mode Iq request, Iq requested, Iq allowed/pre-ramp and final 16 kHz Iq reference.

## Source synchronization

`server.py` fingerprints the production C list used by `tools/run_level4.py`, all headers in `inc/`, and
the Controller Lab harness. Every `/api/run` calls `ensure_built()`; if the fingerprint changed, the native
executable is rebuilt before the scenario runs. The UI also polls `/api/status` and can auto-rerun after a
source change.

This means a change to the controller source becomes visible in the analyzer after rebuild/run without
copying that algorithm into the web app.

## Run

From repository root:

```bash
python sim/controller_lab/server.py --open
```

Then open `http://127.0.0.1:8765/` if the browser did not open automatically.

Windows shortcut from repository root:

```text
RUN_CONTROLLER_LAB_WINDOWS.bat
```

Host prerequisites are the same as Level 4: Python 3 and a supported host C compiler (`gcc` by default,
or set the `CC` environment variable).

## Important boundary

Controller Lab is for **controller causality and ride-feel tuning**. It deliberately does not invent missing
electrical measurements: battery current, voltage-vector utilization, measured Id/Iq and PMSM response are
not modeled here. Use `sim/l4/` for closed-loop virtual-bike/electrical behavior and `sim/replay/` for real
recorded sensor history.

## Transient events on the CLI (AP-01 transients-001)

The native harness takes `key=value` arguments. Besides the steady-state keys above it accepts
two optional **transient** groups. Both change the SYNTHETIC rider input only — no filter output,
Iq value, session state or ramp is scripted here.

| Key | Meaning |
|---|---|
| `torque_step_at_s=` | time at which the MEAN crank torque steps |
| `torque_step_nm=<Nm>` | new mean crank torque from that time on |
| `ride_restart_s=` | rider starts pedalling again after `ride_stop_s` |
| `ride_stop2_s=` | optional end of that second pedalling interval |

- The step keeps the configured ripple shape and asymmetry; only the mean the shape is applied to
  changes. Both step keys must be given **together**.
- Pedalling intervals are half-open: `[ride_start_s, ride_stop_s)` and
  `[ride_restart_s, ride_stop2_s)`. Without `ride_stop2_s` the rider pedals to the end of the run.
- Nothing is reset in the gap between the intervals: crank angle, torque/cadence filters, PAS,
  session and Iq ramp keep whatever state the production path left them in. Wheel speed (`speed=`)
  stays independent of the pedalling intervals — that is what makes a rolling restart expressible.
- The 4 kHz control clock and the four 16 kHz inner steps are unchanged; no jitter or tick skipping
  is introduced by these keys.

**Errors, not silent substitutions.** Unlike the older keys, these four are parsed strictly and a
contradictory scenario exits with code 2 instead of being clamped into a different, working run:
a lone half of the step pair, a non-numeric / NaN / infinite value, `ride_restart_s` without a
first stop or not after it, `ride_stop2_s` without/not after `ride_restart_s`, and any event time
beyond `duration`.

## Disturbance events on the CLI (AP-01 disturbances-001)

Additional `key=value` arguments extend the generator with three disturbance categories.
All are strictly parsed; contradictory combinations exit with code 2.

### Reverse / bounce
| Key | Meaning |
|---|---|
| `reverse_at_s=` | start crank reverse (negative crank angle rate) |
| `forward_at_s=` | optional return to forward; requires `reverse_at_s` and must be later |
| `reverse_bounce_at_s=` | short 3‑tick reverse excursion (electrical bounce), cannot be used with `reverse_at_s` |

- Crank angle (`crank_rev`) becomes negative during reverse. PAS quadrature follows the signed transition index.
- `reverse_at_s` without `forward_at_s` keeps reverse until the end of the run.
- `reverse_bounce_at_s` forces a 3‑tick reverse line state then returns forward; it does not accumulate angle.

### Sensor invalid (three independent flags)
| Key | Meaning |
|---|---|
| `torque_invalid_from_s=` / `torque_invalid_to_s=` | `torque_sensor_valid = false` in the synthetic observation struct |
| `pas_invalid_from_s=` / `pas_invalid_to_s=` | `pas_sensor_valid = false` in the synthetic observation struct |
| `pas_invalid_seq_from_s=` / `pas_invalid_seq_to_s=` | injects an illegal diagonal PAS quadrature sequence for the window (held ≥ 4 ticks per transition) |

- Each window must satisfy `0 ≤ from < to ≤ duration`. The PAS invalid sequence window must be long enough for at least two accepted transitions (≥ 8 ticks at 4 kHz).
- The flags only toggle the `*_sensor_valid` fields; the production torque/PAS processing still runs on the synthetic values.

### Timebase disturbance (foreground delay / missed ticks)
| Key | Meaning |
|---|---|
| `miss_tick_every_n=<int>` | skip the foreground processing on every N‑th 4 kHz tick (N ≥ 2, 0 = disabled) |
| `fg_delay_at_s=` / `fg_delay_ticks=<int>` | stall the foreground for `fg_delay_ticks` periods starting at `fg_delay_at_s` |

- The 16 kHz `fast_iq_slew_tick` and the ISR‑level PAS sampler continue uninterrupted.
- `elapsed_ticks` passed to `torque_input_update_elapsed` and `ride_control_update` accumulates during the stall and is applied on the first resumed tick (catch‑up semantics).
- Rows are still emitted for every physical tick; skipped foreground rows carry `fg_processed=0` and `elapsed_ticks=0`.

**CSV.** All pre-existing columns and their meanings are unchanged. New observation columns are appended:
`torque_cmd_mean_nm`, `ride_interval`,
`crank_direction` (-1/0/1), `pas_ab` (line state), `pas_normal_ab`, `pas_transition_index` (signed),
`pas_last_step` (0=none,1=fwd,2=rev,3=invalid),
`pas_direction_state` (0=FORWARD_SAFE,1=DIRECTION_INHIBIT,2=FORWARD_CONFIRMING),
`pas_inhibit_reason` (0=none,1=reverse,2=invalid),
`pas_fwd_run`, `pas_rev_run`, `pas_backpedal_confirmed`,
`pas_sampler_forward`, `pas_sampler_reverse`, `pas_sampler_invalid`, `pas_sampler_glitch`, `pas_sampler_overflow`,
`torque_sensor_valid` (commanded), `pas_sensor_valid` (commanded),
`rider_torque_sensor_valid`, `rider_pas_sensor_valid` (stored in rider_input),
`elapsed_ticks`, `fg_processed`, `fg_delay_active`,
`reverse_bounce_active`, `pas_invalid_seq_active`,
`fast_iq_slew_ticks` (cumulative inner steps).
The pre-existing `pedalling` column remains the generator's diagnostic of leg motion.
