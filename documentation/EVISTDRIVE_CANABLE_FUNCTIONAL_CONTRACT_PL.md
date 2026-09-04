# eVistDrive — KONTRAKT FUNKCJONALNY CANable ↔ FIRMWARE

Data: 2026-09-02
Baseline: `77e4743`, CANable `evistdrive` (gałąź `evistdrive` forka)
Status: **inwentaryzacja + kontrakt kanoniczny, zero zmian w kodzie**

Powiązane: [stan bieżący dynamiki](EVISTDRIVE_DYNAMICS_CURRENT_STATE_PL.md) ·
[słownik nazw](EVISTDRIVE_TERMINOLOGY_ALIAS_PL.md) ·
[dług](EVISTDRIVE_ARCHITECTURE_DEBT_PL.md)

---

## 0. Prostym językiem: co znaczą klasy

| Klasa | Znaczenie dla użytkownika |
|---|---|
| **ACTIVE** | Ustawiasz, sterownik to czyta, czujesz efekt. |
| **PARTIAL** | Działa, ale nie tak jak mówi opis w aplikacji, albo tylko w części trybów. |
| **DEAD** | Aplikacja pozwala ustawić, sterownik tego nie czyta. Zero efektu. |
| **ALIAS** | Nazwa mówi o czymś innym niż realna funkcja. |
| **DUPLICATED** | Dwa różne pola robią tę samą rzecz fizyczną. |
| **HIDDEN** | Mechanizm działa, ale nie masz na niego żadnego suwaka. |

**Wynik zbiorczy:** wśród pól odczucia jazdy 2 są DEAD i 3 PARTIAL, jeden duży mechanizm jest
HIDDEN (RUN asym, opadanie 350 ms), jedno pole ma skutek uboczny dla bezpieczeństwa (podłoga
omija sufit poziomu).

---

## 1. Karta DYNAMICS (globalny blok tuning, 0x6023 / 0x6024, v8)

Transport: `tuning_config.c`, 32 B, CRC16. Zapis zawsze razem z bankami (SAVE_BANKS).

| POLE UI | ETYKIETA | ZAKRES | WIRE | SYMBOL FW | KONSUMENT RUNTIME | REALNY EFEKT | KLASA |
|---|---|---|---|---|---|---|---|
| `assist_start_steps` | Crank movement to start | 1–20 | off 22 | `tuning_config_start_steps()` | `ride_control.c:506`, `main.c:2733` | ile kroków PAS do startu (w ruchu -1) | **ACTIVE** |
| `startup_boost_cadence_step` | Startup boost fade per cadence step | 1–100 | off 12 | `tuning_config_cadence_step()` | `assist_start.c:41` | tempo zaniku boostu startowego | **ACTIVE** |
| `assist_run_deadband_mv` | Run deadband (keep-alive load) | 0–100 mV | off 14 | `tuning_config_run_deadband_mv()` | **tylko** `main.c:3093` do `rearm_delay_diag` | **ŻADEN** | **DEAD** |
| `assist_hold_ms` | Sustain through dead-spot | 0–3000 ms | off 16 | `tuning_config_assist_hold_ticks()` | `ride_control.c:500` | czas podtrzymania podłogi min-Iq | **PARTIAL** — opis w UI powołuje się na Run deadband, który nie istnieje w sterowaniu |
| `assist_min_iq_pct` | Current floor while latched | 0–25 % | off 18 | `tuning_config_min_iq_pct()` | `ride_control.c:718` | podłoga prądu jako % `ride_core_iq_limit` | **ACTIVE** + ryzyko AD-005 |
| `assist_torque_run_window_deg` | RUN torque smoothing (anti-pulse) | 0–360 | off 20 | `tuning_config_assist_torque_run_window_deg()` | `main.c:2725` | **tylko 0 = wyłącz, reszta = włącz filtr 120/350 ms**; stopnie ignorowane | **PARTIAL + ALIAS** |
| `assist_torque_full_scale_centikg` | Assist torque full scale | 20–120 kg | off 24 | `tuning_config_assist_torque_full_scale_centikg()` | `assist_modes.c:583` | oś 0..160 dla eMTB i Torque | **ACTIVE** |
| `crank_length_mm` | Crank length | 150–190 mm | off 26 | `tuning_config_crank_length_mm()` | `assist_modes.c:565` | człon mocy ridera w trybach Power | **ACTIVE** |
| (brak w UI) | 4 rampy globalne | 20–5000 ms | off 4–10 | `rise_slow_ms` itd. | **brak** | **ŻADEN** — tylko zgodność wire | **DEAD** |

---

## 2. Karta PROFILES — pola per poziom (bank blob v8, rekord 48 B)

| POLE UI | ETYKIETA | WIRE | KONSUMENT RUNTIME | REALNY EFEKT | KLASA |
|---|---|---|---|---|---|
| `mode_type` | wybór trybu | [0] | `assist_modes_calculate()` | wybór algorytmu | **ACTIVE** (0 i 4 = brak wspomagania) |
| `support_ratio/min/max_pct` | Rider power support / Min / Max | [1..6] | `calculate_power()` | % mocy ridera | **ACTIVE** |
| `reference_power_w` | Reference rider power | [7..8] | Power Progressive / Curve | punkt odniesienia progresji | **ACTIVE** |
| `progression_pct` | Progression | [9] | Power Progressive | krzywizna | **ACTIVE** |
| `curve_exponent_x10`, `_high_x10` | Curve shape lower / upper | [1], [9] reużyte | `power_curve.c` | gamma połówek okna | **ACTIVE** (tylko Power Curve) |
| `emtb_parameter` | eMTB sensitivity | [10] | `calculate_emtb()` | mianownik 510 minus 2x | **ACTIVE** (tylko eMTB) |
| `emtb_based_on_power` | Cadence-dependent response | [11] | `emtb_denominator()` | cadence obniża mianownik | **ACTIVE** |
| `emtb_reference_voltage_mv` | Reference voltage | [12..13] | `emtb_target_to_power_mw()` | normalizacja mocy | **ACTIVE** |
| `torque_assist_factor` | Torque gain, 120 = 1.0x | [14] | `calculate_torque_assist()` | wzmocnienie | **ACTIVE** (tylko Torque) |
| `max_motor_power_w` | Limit maximum motor power | [15..16] | `finish_power_request()` | sufit mocy, obie kotwice | **ACTIVE** |
| `max_iq_pct` | Maximum motor torque (Nm) | [17] | `profile_iq_pct_limit()` | sufit prądu poziomu | **ACTIVE**, patrz AD-005 |
| `assist_without_rotation` | Assist without crank rotation | [18] | `prepare_assist_input()` | start bez obrotu korby | **ACTIVE** |
| `minimum_pedal_load_kg` | Minimum pedal load | [19..20] | `ride_control.c:499` | próg startu z postoju | **ACTIVE** |
| `riding_minimum_pedal_load_kg` | Minimum pedal load while riding | [35] | `ride_control.c:520` | próg chwytu w ruchu | **ACTIVE** |
| `startup_boost_enabled` | Startup boost | [21] | `assist_start_apply_boost()` | włącznik | **ACTIVE** |
| `startup_boost_strength_pct` | Startup boost strength | [22..23] | tamże | siła przy 0 rpm | **ACTIVE** |
| `startup_boost_end_rpm` | Startup boost end cadence | [24] | `assist_start.c:116` | **tylko tryb SPEED**; w domyślnym CADENCE bez efektu | **PARTIAL** |
| `smooth_start_enabled`, `_ms` | Smooth start, duration | [25..27] | `assist_start_apply_smooth()` | koperta startu z postoju | **ACTIVE** |
| `release_ms` | Release duration, 0 = auto | [28..29] | `ride_final_iq_slew_compute()` | czas zaniku po ustaniu pedałowania | **ACTIVE** |
| `power_rise_filter_ms` | Power rise filter — INACTIVE | [30..31] | brak | żaden | **DEAD** (oznaczone w UI) |
| `power_fall_filter_ms` | Power fall filter — INACTIVE | [32..33] | brak | żaden | **DEAD** (oznaczone w UI) |
| `iq_rise_slow_ms`, `_fast_ms` | Acceleration low / high | [38..41] | `ride_final_iq_slew_compute()` | rampa narastania, pełna skala na czas | **ACTIVE** |
| `iq_fall_slow_ms`, `_fast_ms` | Deceleration low / high | [42..45] | tamże | rampa opadania | **ACTIVE, ale maskowana** przez RUN asym i podłogę |
| `extended_boost_trigger_load_kg` | Trigger pedal load | [36] | `assist_extended_boost.c` | próg uzbrojenia | **ACTIVE** (bezczynne przy duration 0) |
| `extended_boost_strength_pct` | Boost strength | [37] | tamże | mnożnik prądu | **ACTIVE** (jw.) |
| `extended_boost_duration_ms` | Boost duration, 0 = Off | [46..47] | tamże | włącznik i czas | **ACTIVE**, domyślnie 0 |

Pola per bank (nagłówek bloba): `wa_max_wheel_x10`, `wa_current_pct`, `wa_target_rpm`,
`wa_latch_after_release`, `wa_latch_timeout_s`, `cadence_comp_enabled` — wszystkie **ACTIVE**
(`wa_current_pct` to zgodność wsteczna, WA ma własny kontroler prędkości).

---

## 3. Pola legacy (Para0 / Para1, karta Limits i stare zakładki Bafang)

| POLE | ETYKIETA W UI | KONSUMENT | KLASA |
|---|---|---|---|
| `current_limit` | Maximum battery current | `MP.battery_current_max` do `battery_iq_cap` | **ACTIVE** |
| `overvoltage`, `undervoltage_under_load` | odcięcia napięcia | `assist_limits_apply` | **ACTIVE** |
| `speed_limit`, `circumference`, flaga legal | prędkość | `assist_limits_apply` | **ACTIVE** |
| `limp_mode_soc_limit`, `_stage2` | Limp | `ride_core_iq_limit_scaled` | **ACTIVE** |
| `assist_settings[poziom][0]` | procent prądu na poziom (stare zakładki) | `phase_current_max_scaled` trafia tylko do `.iq_scale`, nadpisane w gałęzi assist | **DEAD w ride core** (ustalenie N6) |
| `start_current` | Old overrun duration (unused) | brak | **DEAD**, oznaczone |
| `current_shedding_time` | Ramp-end control (unused) | brak | **DEAD**, oznaczone |
| `temperature_sensor_type`, `full_capacity_range` | Decay base / Cadence exponent (unused) | brak | **DEAD**, oznaczone |
| — | brak pola w UI | `PH_CURRENT_MAX = 700` compile-time | **HIDDEN** |

---

## 4. Kierunek odwrotny: mechanizmy firmware BEZ parametru w CANable

To jest właściwa odpowiedź na pytanie z rozdziału 13 mandatu: „czy użytkownik ma na to jawny
parametr?".

| MECHANIZM | CZAS / SIŁA | CZY UŻYTKOWNIK MA SUWAK | SKUTEK |
|---|---|---|---|
| **RUN asym fall 350 ms** | tau 350 ms, ~2 s do zera | **NIE** (tylko włącznik pod cudzą nazwą) | dominuje odczuwane Ramp Down |
| **RUN asym rise 120 ms** | tau 120 ms | **NIE** | opóźnia odczuwane Ramp Up |
| FAST filter 35 ms | tau 35 ms | NIE | prawidłowe, sensoryczne |
| Hard-cut ramp 200 ms | stała | NIE, celowo | bezpieczeństwo |
| Coast release poniżej 10 erps | próg | NIE | koniec wybiegu |
| Gear preload 10 counts / 300 ms | stała | NIE | pierwszy moment startu |
| Kotwica startowa `ASSIST_LAUNCH_REFERENCE_U_ABS = 1024` | hipoteza, **niepotwierdzona na rowerze** | NIE | skala wspomagania poniżej 60 rpm |
| Adaptacyjne progi ramp (4 i 20 km/h, 50 i 110 rpm) | stałe `config.h` | NIE (schemat je wymienia jako legacy) | który z 4 czasów rampy obowiązuje |
| Cadence compensation (mapa 70/80/100/110/120 rpm) | stała mapa | tylko on/off per bank | kształt mocy przy wysokiej kadencji |

---

## 5. FAZA 2 — KANONICZNY KONTRAKT FUNKCJI

Format: FUNKCJA KANONICZNA / ZNACZENIE DLA UŻYTKOWNIKA / DZISIEJSZE POLE CANable / DZISIEJSZE
SYMBOLE FW / DZISIEJSZE IMPLEMENTACJE / DOCELOWY WŁAŚCICIEL.

### F1. Assist (ile pomocy dostaję z mojego nacisku)
- CANable: `mode_type`, `support_*`, `emtb_parameter`, `torque_assist_factor`, `reference_power_w`
- FW: `assist_modes.c` (`calculate_power` / `calculate_emtb` / `calculate_torque_assist`)
- Implementacje: jedna, spójna po FW-129. **Docelowy owner: bez zmian.**

### F2. Power (jak wysoko może dojść moc)
- CANable: `max_motor_power_w`, `max_iq_pct`, Max battery current, limity napięcia i temperatury
- FW: `finish_power_request()`, `assist_limits.c`, `battery_iq_cap.c`
- Implementacje: 3 warstwy, wszystkie bezstanowe albo z jawną histerezą. **Docelowy owner: bez zmian.**
- Wyjątek do naprawy: podłoga min-Iq omija `max_iq_pct` (AD-005).

### F3. Ramp Up (jak szybko pomoc może narastać)
- CANable: `iq_rise_slow_ms`, `iq_rise_fast_ms`
- FW dziś: `ride_final_iq_slew_compute()` **+ ukryty RUN asym rise 120 ms**
- Implementacje: **2** (jedna jawna, jedna ukryta). **Docelowy owner: final Iq slew, jeden.**

### F4. Ramp Down (jak szybko pomoc może maleć)
- CANable: `iq_fall_slow_ms`, `iq_fall_fast_ms`, `release_ms`
- FW dziś: `ride_final_iq_slew_compute()` **+ RUN asym fall 350 ms + hold/podłoga 1400 ms**
- Implementacje: **3**. To jest główny problem projektu. **Docelowy owner: final Iq slew, jeden.**

### F5. Torque Measurement Filter (odszumienie czujnika)
- CANable: dziś brak jawnego pola (suwak RUN smoothing pełni tę rolę pod złą nazwą)
- FW: `update_assist_filter()` 35 ms + `update_run_asym_filter()`
- **Docelowy owner: warstwa sensora, tylko odszumienie, bez roli w odczuciu jazdy.**

### F6. Dead-spot bridging (utrzymanie ciągu w martwym punkcie korby)
- CANable: `assist_hold_ms`, `assist_min_iq_pct`, (nieaktywne `assist_run_deadband_mv`)
- FW: podłoga min-Iq + hold; **dublowane** przez RUN asym fall
- **Docelowy owner: DECYZJA WŁAŚCICIELA — jeden z dwóch, nie oba.**

### F7. Smooth Start / Startup Boost / Gear preload (start z postoju)
- CANable: `smooth_start_*`, `startup_boost_*`; preload bez pola
- FW: `assist_start.c`, blok preload w `ride_control.c`
- **Docelowy owner: bez zmian — to jawnie oddzielna dynamika startu (rozdział 27 mandatu).**

### F8. Extended Boost, F9. Speed Limit, F10. Battery Current Limit, F11. Thermal Limit,
### F12. Walk Assist, F13. Safety Release
- Wszystkie mają dziś jednego właściciela, jawny trigger i jawną semantykę. **Bez zmian.**
