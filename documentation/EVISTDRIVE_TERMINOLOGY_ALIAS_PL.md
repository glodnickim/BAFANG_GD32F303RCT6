# eVistDrive — SŁOWNIK NAZW KANONICZNYCH I ALIASÓW

Data: 2026-09-02 · Baseline `77e4743`

Zasada z rozdziału 21 mandatu: dopóki migracja nie jest wykonana, dokumentacja używa **dwóch
warstw nazw** — nazwy kanonicznej (co to robi) i dzisiejszego symbolu w kodzie. Agent nie musi
znać historii, żeby zrozumieć funkcję.

---

## 1. Dynamika i tor prądu

| NAZWA KANONICZNA | DZISIEJSZY SYMBOL / PLIK | NAZWA W CANable | UWAGA |
|---|---|---|---|
| Final Iq Slew | `fast_iq_slew_tick()` / `fast_iq_slew.c` | — | jedyny finalny właściciel, 16 kHz |
| Final Iq Slew — producent komendy | `ride_final_iq_slew_compute()` / `ride_control.c` | — | liczy tryb i krok w domenie 4 kHz |
| Ramp Up | `iq_rise_slow_ms`, `iq_rise_fast_ms` | Acceleration low / high | **dziś dzielone** z RUN asym rise |
| Ramp Down | `iq_fall_slow_ms`, `iq_fall_fast_ms` | Deceleration low / high | **dziś zdominowane** przez RUN asym fall |
| Release (po ustaniu pedałowania) | `release_ms`, `FIS_MODE_RELEASE` | Release duration | czas stały z żywej wartości |
| Safety Release | `FIS_MODE_SAFETY`, `RIDE_HARD_CUT_RAMP_MS` | — | 200 ms, firmware-owned |
| Coast Release | `RIDE_COAST_RELEASE_ERPS` | — | FORCE_ZERO poniżej 10 erps |
| RUN Torque Smoothing (asymetryczne) | `update_run_asym_filter()`, `run_asym_q` | (brak własnej nazwy) | **prawdziwy właściciel odczuwanego opadania** |
| RUN Window (martwe) | `run_buffer`, `run_sum`, `run_window_steps` | RUN torque smoothing (anti-pulse) | nazwa w UI opisuje ten martwy mechanizm |
| Fast Torque Filter | `update_assist_filter()`, `assist_filter_q` | — | 35 ms, symetryczny |
| Min-Iq Hold / Floor | `assist_hold_ticks`, `tuning_config_min_iq_pct()` | Sustain through dead-spot + Current floor while latched | dwa pola, jedna funkcja |
| Rolling Rearm Recovery | `torque_recovery_state_t` | — | automat 3-stanowy, po suspensji kierunku |
| Battery Iq Cap | `battery_iq_cap_update()` | Maximum battery current | limiter upstream |
| Legacy 4 kHz Dynamics Owner | `assist_dynamics_apply()` | — | **martwy**, 0 wywołań |

## 2. Pozwolenie i cykl życia jazdy

| NAZWA KANONICZNA | DZISIEJSZY SYMBOL | UWAGA |
|---|---|---|
| Ride Session (pozwolenie) | `ride_session.c`, `session_out.latched` | COLD / ACTIVE / SUSPENDED_BY_DIRECTION |
| Pedal Assist Gate (bramka nacisku) | `pedal_assist_gate.c`, lokalne `latched` | **kolizja nazw** — patrz AD-011 |
| Direction Safety | `pas_direction.c`, `direction_inhibit_active` | reaguje na pierwszy krok wstecz |
| Cold Start Gate | `cold_start_ready` | kroki korby + próg kg |
| Hard Cut | `hard_cut` w `ride_control.c` | hamulec, temperatura, usterka, kalibracja, kierunek |

## 3. Start

| NAZWA KANONICZNA | DZISIEJSZY SYMBOL | NAZWA W CANable |
|---|---|---|
| Startup Boost (mnożnik obciążenia) | `assist_start_apply_boost()` | Startup boost strength / mode |
| Startup Boost Fade | `tuning_config_cadence_step()` | Startup boost fade per cadence step |
| Smooth Start (koperta) | `assist_start_apply_smooth()` | Smooth start / duration |
| Gear Preload | `PRELOAD_IQ_CAP` w `ride_control.c` | (brak) |
| Start Phase | `START_PHASE_ENABLE`, `start_phase` | (brak) |
| Launch Anchor | `ASSIST_LAUNCH_REFERENCE_U_ABS` | (brak) — patrz AD-013 |

## 4. Wspomaganie (rider intent)

| NAZWA KANONICZNA | DZISIEJSZY SYMBOL | NAZWA W CANable |
|---|---|---|
| Assist Mode | `assist_mode_type_t` | wybór trybu |
| Support Ratio | `support_ratio_pct` / `support_min/max` | Rider power support |
| Assist Torque Axis | `tuning_config_assist_torque_full_scale_centikg()` | Assist torque full scale |
| Rider Power Model | `calculate_human_power_mw()` + `crank_length_mm` | Crank length |
| Cadence Compensation | `cadence_comp.c` | (on/off per bank) |
| Power to Iq Conversion | `power_to_phase_iq()` + `launch_blend_permille()` | (brak) |

---

## 5. Nazwy, których NIE wolno użyć ponownie

| Nazwa | Dlaczego |
|---|---|
| `TQ_GATE_RELEASE` | usunięta w FW-094; schemat nadal ją wymienia (AD-012) |
| `mode_type = 0` i `= 4` | wartości wire zarezerwowane, zapisane banki nadal je niosą |
| `power_rise_filter_ms`, `power_fall_filter_ms` | bajty zajęte na stałe, funkcja wycofana w FW-129B |
| `rearm_state`, `Backwards_counter`, `fwd_run` jako źródło decyzji | zastąpione automatami FW-109 |
| `assist_dynamics` jako „finalny owner" | nieprawda od QS-3D |
