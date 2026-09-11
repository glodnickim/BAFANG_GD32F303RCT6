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

Additional `key=value` arguments extend the generator with four disturbance categories.
All are strictly parsed; contradictory combinations exit with code 2. With none of them given the
run is byte-identical to the run before this task added them.

### Clock map: what runs, what is deferred, and what `elapsed` the module receives

Read this before interpreting any timebase trial. It is the map the card asks for, and it is
written from the target source, not from the harness.

**On the target** (`src/main.c`, `inc/config.h`):

| Clock | Owner on the target | Can the main loop stop it? |
|---|---|---|
| 16 kHz Iq slew | `fast_iq_slew_tick()` called from the FOC ADC ISR (`ADC0_1_IRQHandler`); it is the sole writer of `MS.i_q_setpoint` (`inc/iq_chain.h`, `src/ride_control.c:114`) | **No** - interrupt context |
| 4 kHz control clock | `control_time_ticks`, incremented **only** in `TIMER1_IRQHandler` (`src/main.c:273`, `CONTROL_TIMEBASE_HZ = 4000`) | **No** - interrupt context |
| 4 kHz PAS sampling | the quadrature decoder fed from the same 4 kHz interrupt | **No** |
| Foreground control pass | `reg_ADC_processing()` from `main()`'s `while(1)` | **Yes** - this is the only thing that can fall behind |

The target's foreground does **not** count its own invocations. It reads the free-running ISR
clock and derives (`src/main.c:2626`):

```c
uint32_t control_delta = control_now - control_prev_processed_tick;   /* >= 1 */
...
uint32_t missed = control_delta - 1U;                                 /* main.c:2646 */
```

and hands exactly that number to both consumers, unchanged:

```c
torque_input_update_elapsed(torque_raw_mv, MS.torque_on_crank, torque_fault==0, control_delta);
ride_control_update(&(ride_control_input_t){ ... .elapsed_ticks = control_delta });
```

So a late foreground call does not lose time on the target - it delays when the number is *read*,
never what it *is* (FW-103/FW-104, `src/main.c:250-272`).

**In this harness** the same contract is reproduced:

| | Runs | Deferred | `elapsed` handed to the module |
|---|---|---|---|
| `miss_tick_every_n=N` (inside its window) | 4 kHz PAS ISR sampler, 16 kHz `fast_iq_slew_tick`, the generator's own clock | the whole foreground group for that tick | `1` normally; `2` on the tick after a single skip |
| `fg_delay_at_s=` + `fg_delay_ticks=N` | same as above | the foreground group for `N` consecutive ticks | `N + 1` on the first resumed tick, i.e. `missed = N` |
| `pas_edge_drop_*` / `pas_edge_jitter_*` | everything, foreground included | nothing - this class does **not** touch the scheduler | unchanged (`1`) |

`fg_last_resume_elapsed` in the CSV is that number, latched, so it survives row decimation.

**Declared deviations from the target scheduler** - these are model limits, not findings:

1. On the target the three clocks are genuinely independent interrupt sources. Here they are one
   loop that runs, in fixed order per control tick: PAS sampler -> (foreground, unless deferred)
   -> 4x `fast_iq_slew_tick`. The harness therefore reproduces the *ordering and the elapsed
   arithmetic*, not interrupt latency, preemption, or jitter between the clocks.
2. The harness's "16 kHz" is 4 inner steps per 4 kHz control tick, not a separate timer. It shows
   that the Iq owner keeps stepping while the foreground is stalled; it says nothing about real
   FOC ISR timing.
3. On the target the foreground is gated by a flag the ISR sets; a stall coalesces ticks. Here the
   stall is scripted with an exact tick count instead of arising from load. The lengths used
   (1 / 4 / 20 ticks) are **API exercise points, not measured or safe hardware limits** - nothing
   here establishes what stall the real controller survives.
4. `motor_erps` is derived in the harness as `cadence_gen * 4/3` and is **unsigned**: the crank
   sign is deliberately NOT carried into it. This is an artificial coupling of cadence and
   electrical rotor speed that does not exist on the bike, and it means the reverse trials do not
   exercise any motor-side reverse handling. Stated so no reverse conclusion is read as covering
   the motor path.

### Reverse / bounce
| Key | Meaning |
|---|---|
| `reverse_at_s=` | start crank reverse (negative crank angle rate) |
| `forward_at_s=` | optional return to forward; requires `reverse_at_s` and must be later |
| `reverse_bounce_at_s=` | short 3-tick reverse excursion (electrical bounce), cannot be used with `reverse_at_s` |

- Crank angle (`crank_rev`) becomes negative during reverse. The quadrature transition index is a
  **signed** `int64_t` and `fwd_ab_at()` normalises a negative modulus, so reversing past index 0
  cannot underflow - the historic unsigned-index assumption is gone.
- `reverse_at_s` without `forward_at_s` keeps reverse until the end of the run.
- `reverse_bounce_at_s` forces a 3-tick reverse line state then returns forward; it does not
  accumulate angle. Clean reverse and bounce are separate groups and are never merged.

### Sensor invalid (three independent flags)
| Key | Meaning |
|---|---|
| `torque_invalid_from_s=` / `torque_invalid_to_s=` | `torque_sensor_valid = false` |
| `pas_invalid_from_s=` / `pas_invalid_to_s=` | `pas_sensor_valid = false` |
| `pas_invalid_seq_from_s=` / `pas_invalid_seq_to_s=` | injects an illegal diagonal PAS quadrature sequence for the window (held >= 4 ticks per transition) |

- Each window must satisfy `0 <= from < to <= duration`. The PAS invalid-sequence window must be
  long enough for at least two accepted transitions (>= 8 ticks at 4 kHz).
- The first two toggle the `*_sensor_valid` fields at the input boundary: the same boolean reaches
  `torque_input_update_elapsed()` and `rider_input`, so the CSV's commanded and stored columns can
  be compared and must agree. The third changes the electrical line only.
- **Limit:** these are the flags the pipeline carries, not a sensor model. A real dead or drifting
  sensor (stuck ADC value, out-of-range mV, broken PAS supply) is NOT reproduced by this input -
  the harness has no path to the ADC or to `Error 25` debouncing. Calling these "sensor tests" any
  further than "the valid flag is false" would overstate them.

### PAS edge disturbance (electrical: missed edge / late edge)
| Key | Meaning |
|---|---|
| `pas_edge_drop_from_s=` / `pas_edge_drop_to_s=` / `pas_edge_drop_every_n=<int>` | every N-th quadrature transition is never presented to the ISR (N >= 2) |
| `pas_edge_jitter_from_s=` / `pas_edge_jitter_to_s=` / `pas_edge_jitter_ticks=<int>` | every transition is presented late, alternating `ticks` and `ceil(ticks/2)` ticks of deferral (1..400) |

- This is a **different class** from `miss_tick`/`fg_delay`: the foreground runs on every tick and
  it is the PAS line that misbehaves. `miss_tick` skipping foreground calls is a *scheduling*
  disturbance and is not a substitute for a missed PAS edge.
- A dropped edge makes the next presented value two steps away from the previous one - an illegal
  quadrature step, which is the point. Jitter preserves the edge COUNT and only moves it in time.
- Outside the window the line follows the crank with no shaping at all, so the control phase and
  the recovery after the window are the unmodified generator.
- Cannot be combined with `pas_invalid_seq_*` or `reverse_bounce_at_s`; those two own the line
  outright and mixing them would make the classes unreadable.

### Timebase disturbance (foreground delay / missed ticks)
| Key | Meaning |
|---|---|
| `miss_tick_every_n=<int>` | skip the foreground on every N-th 4 kHz tick (N >= 2, 0 = disabled) |
| `miss_tick_from_s=` / `miss_tick_to_s=` | bound the skipping to a window, so a scenario has a real control phase and a real recovery phase (both keys together; requires `miss_tick_every_n`) |
| `fg_delay_at_s=` / `fg_delay_ticks=<int>` | stall the foreground for `fg_delay_ticks` periods starting at `fg_delay_at_s` |

- The 16 kHz `fast_iq_slew_tick` and the ISR-level PAS sampler continue uninterrupted; the CSV
  proves it (`fast_iq_slew_ticks` advances by 4 on every emitted control tick, including the
  stalled ones).
- Rows are still emitted for every physical tick; skipped foreground rows carry `fg_processed=0`
  and `elapsed_ticks=0`. The catch-up value appears on the first resumed tick.
- With no `miss_tick_from_s`/`miss_tick_to_s`, `miss_tick_every_n` runs from the first tick to the
  end of the run - the historic behaviour.

### Observation resolution
| Key | Meaning |
|---|---|
| `sample_ticks=<int>` | emit one CSV row every N 4 kHz ticks (1..40000); overrides `sample_ms` |

`sample_ms` is a millisecond decimation, so at 4 kHz the finest it can express is `sample_ms=1`
= one row per **4** ticks. That cannot show a single skipped tick and aliases
`miss_tick_every_n=2`. `sample_ticks=1` emits every control tick. Unset, behaviour is unchanged;
`sample_ticks=20` is exactly equivalent to `sample_ms=5`.

**CSV.** All pre-existing columns and their meanings are unchanged, and the accepted 30 columns
keep their positions. New observation columns are appended after them:
`crank_direction` (-1/0/1), `pas_ab` (line state), `pas_normal_ab`, `pas_transition_index` (signed),
`pas_last_step` (0=none,1=fwd,2=rev,3=invalid),
`pas_direction_state` (0=FORWARD_SAFE,1=DIRECTION_INHIBIT,2=FORWARD_CONFIRMING),
`pas_inhibit_reason` (0=none,1=reverse,2=invalid),
`pas_fwd_run`, `pas_rev_run`, `pas_backpedal_confirmed`,
`pas_sampler_forward`, `pas_sampler_reverse`, `pas_sampler_invalid`, `pas_sampler_glitch`,
`pas_sampler_overflow`,
`torque_sensor_valid` (commanded), `pas_sensor_valid` (commanded),
`rider_torque_sensor_valid`, `rider_pas_sensor_valid` (stored in rider_input),
`elapsed_ticks`, `fg_processed`, `fg_delay_active`,
`reverse_bounce_active`, `pas_invalid_seq_active`,
`fast_iq_slew_ticks` (cumulative inner steps),
`fg_skips_total`, `fg_resume_count`, `fg_last_resume_tick`, `fg_last_resume_elapsed`,
`pas_edge_drop_active`, `pas_edge_jitter_active`, `pas_edges_dropped`, `pas_edges_deferred`.

The four `fg_*` counters and the two `pas_edges_*` counters are cumulative/latched deliberately:
they survive row decimation, so a measurement does not depend on catching the exact row an event
landed on.

`pedalling` (column 28) remains the generator's boolean diagnostic of leg motion and
`ride_interval` (column 30) remains the interval id 0/1/2 - two different columns with two
different meanings. Column 30 was emitted as `crank_direction` in the attempt reviewed as
REVIEW-EVD-AP-01-010; it is restored here and every row is now validated against the header by
name (`documentation/assist-pipeline-work/AP-01/disturbances-001/csv_contract.py`).
