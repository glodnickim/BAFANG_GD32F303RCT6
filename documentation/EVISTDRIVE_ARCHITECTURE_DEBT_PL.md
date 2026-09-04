# eVistDrive — REJESTR DŁUGU ARCHITEKTONICZNEGO

Data: 2026-09-02 · Baseline `77e4743`
Zasada: **pozycji z tej listy nie naprawiamy poza kartą, której dotyczą.**
Numery kroków odsyłają do [planu migracji](EVISTDRIVE_DYNAMICS_MIGRATION_PLAN_PL.md).

---

### AD-001 — Uśrednianie RUN po kącie korby jest martwe
**OBSZAR:** sensor / RUN · **SYMBOL:** `run_buffer`, `run_sum`, `run_filled`, `run_head`
**PROBLEM:** `torque_input_run_filter_step()` liczy średnią co krok korby, ale
`torque_input_update()` w tym samym ticku nadpisuje `run_value_native` wynikiem filtra
asymetrycznego (IDLE) albo sygnałem FAST. Wynik średniej nie jest publikowany w żadnym stanie.
**DLACZEGO WAŻNE:** 192 B RAM, ~70 linii kodu i cały kontrakt FW-085 utrzymywane bez efektu;
kolejny agent czyta FW-085 i buduje na nim błędny model.
**EFEKT DLA UŻYTKOWNIKA:** suwak w stopniach nie działa (patrz AD-002).
**KIEDY NAPRAWIĆ:** STEP 5 · **PILNOŚĆ:** MEDIUM · **BLOKUJE BIEŻĄCĄ PRACĘ:** NIE

### AD-002 — Suwak „RUN torque smoothing" to w rzeczywistości włącznik
**OBSZAR:** kontrakt CANable · **SYMBOL:** `assist_torque_run_window_deg`
**PROBLEM:** wartość w stopniach wpływa tylko na `run_window_steps`, a ten decyduje wyłącznie
o tym, czy publikować filtr asymetryczny, czy surowy FAST. 90, 180 i 270 stopni dają identyczne
zachowanie.
**EFEKT DLA UŻYTKOWNIKA:** rider stroi parametr, który nic nie zmienia — i nie ma dostępu do
parametru, który zmienia najwięcej (fall 350 ms).
**KIEDY:** STEP 1 (opis), STEP 3/5 (funkcja) · **PILNOŚĆ:** HIGH · **BLOKUJE:** NIE

### AD-003 — „Run deadband" nie ma konsumenta sterującego
**OBSZAR:** kontrakt CANable · **SYMBOL:** `assist_run_deadband_mv`
**PROBLEM:** jedyne użycie to `main.c:3093` — pole diagnostyczne `rearm_delay_diag`.
Sterowanie nigdy go nie czyta, mimo obszernego opisu w aplikacji.
**EFEKT:** rider stroi coś, co jest wyłącznie etykietą w logu.
**KIEDY:** STEP 1 · **PILNOŚĆ:** HIGH · **BLOKUJE:** NIE

### AD-004 — Opis „Sustain through dead-spot" odsyła do nieistniejącego mechanizmu
**OBSZAR:** kontrakt CANable · **SYMBOL:** `assist_hold_ms`
**PROBLEM:** help mówi „stays latched at very light pedal load (below Run deadband)". Faktycznie
hold jest uzbrajany **wyłącznie** dodatnim żądaniem trybu (`ride_control.c:701`) i odlicza przez
kolejne ticki zerowego żądania. Deadband nie bierze w tym udziału.
**KIEDY:** STEP 1 · **PILNOŚĆ:** MEDIUM · **BLOKUJE:** NIE

### AD-005 — Podłoga min-Iq omija sufit prądu poziomu
**OBSZAR:** limity / bezpieczeństwo · **SYMBOL:** `ride_control.c:718`
**PROBLEM:** podłoga liczona jest jako `min_iq_pct % z ride_core_iq_limit` (limit globalny), a
nie z sufitu poziomu. Sufit `max_iq_pct` jest nakładany wcześniej, wewnątrz
`assist_modes_calculate()`, więc podłoga może go przebić.
**SCENARIUSZ:** poziom ECO ustawiony na `max_iq_pct = 5 %` (35 counts przy limicie 700) i
`assist_min_iq_pct = 10 %` (70 counts) — przy zaniku nacisku poziom ECO dostaje **dwa razy
więcej prądu, niż wynosi jego własny sufit**. Przy domyślnych (2 % i 100 %) problem się nie
ujawnia, dlatego przeszedł niezauważony.
**KIEDY:** STEP 6 · **PILNOŚĆ:** HIGH · **BLOKUJE:** NIE (domyślne wartości są bezpieczne)

### AD-006 — „Startup boost end cadence" nie działa w domyślnym trybie
**OBSZAR:** start · **SYMBOL:** `startup_boost_end_rpm`
**PROBLEM:** pole czytane tylko w gałęzi `ASSIST_STARTUP_BOOST_SPEED`. W domyślnym trybie
CADENCE zanik boostu ustala globalny `startup_boost_cadence_step`, a krzywa i tak obejmuje
0–119 rpm. Wykres w aplikacji używa `end_rpm` wyłącznie jako zakresu osi X, co utrwala złudzenie.
**KIEDY:** STEP 1 (opis), STEP 7 (funkcja) · **PILNOŚĆ:** MEDIUM · **BLOKUJE:** NIE

### AD-007 — `assist_dynamics_apply()` bez wywołań produkcyjnych
**OBSZAR:** dynamika · **SYMBOL:** `src/assist_dynamics.c`, `inc/assist_dynamics.h`
**PROBLEM:** 0 wywołań w kodzie produkcyjnym po QS-3D. Plik nadal się kompiluje, nadal jest
resetowany z `ride_control_init()` i nadal jest jedynym miejscem, które używa `IQ_SLEW_*`.
**DLACZEGO WAŻNE:** dwa nagłówki (`battery_iq_cap.h`, `ride_control.h`) opisują kolejność
w torze przez odwołanie do tej funkcji, więc dokumentacja architektury wskazuje na martwy moduł.
**KIEDY:** STEP 8 — **nie wcześniej**, testy QS-3 używają go jako punktu odniesienia.
**PILNOŚĆ:** LOW · **BLOKUJE:** NIE

### AD-008 — Martwe stałe ramp w `config.h`
**SYMBOL:** `IQ_RAMP_UP_SLOW_TICKS`, `IQ_RAMP_UP_FAST_TICKS`, `IQ_RAMP_DOWN_SLOW_TICKS`,
`IQ_RAMP_DOWN_FAST_TICKS`, `IQ_SLEW_UP`, `IQ_SLEW_DOWN`, `IQ_SLEW_*_SLOW/FAST`
**PROBLEM:** 0 użyć produkcyjnych. Fallbacki finalnego slewu są zapisane osobno, jako lokalny
`enum` w `ride_final_iq_slew_compute()` (600/300/1000/140 ms) — czyli te same liczby żyją
w dwóch miejscach, a czytane jest to mniej widoczne.
**KIEDY:** STEP 8 · **PILNOŚĆ:** LOW · **BLOKUJE:** NIE

### AD-009 — Cztery globalne rampy w tuning blob są martwe, ale wciąż zapisywane
**SYMBOL:** `rise_slow_ms`, `rise_fast_ms`, `fall_slow_ms`, `fall_fast_ms` (`tuning_config.c`)
**PROBLEM:** trzymane wyłącznie dla zgodności formatu (FW-069). Aplikacja ich nie pokazuje, ale
`TUNING_DEFAULTS` w `dynamics.js` nadal je wysyła. Schemat protokołu opisuje je jako
`legacy_rw` z wiązaniem do `IQ_RAMP_*_TICKS`, które same są martwe (AD-008).
**KIEDY:** STEP 8 (opis), format wire zostaje · **PILNOŚĆ:** LOW · **BLOKUJE:** NIE

### AD-010 — Procent prądu na poziom z Para1 nie działa w ride core
**SYMBOL:** `MP.assist_settings[lvl][0]`, `phase_current_max_scaled`
**PROBLEM:** wartość trafia do `ride_control_input_t.iq_scale`, a gałąź assist nadpisuje
`dynamics_iq_scale` przez `ride_core_iq_limit` (`ride_control.c:618`). W torze Walk Assist tryb
to BYPASS, więc skala też nie ma znaczenia. Efektywnie: pole legacy bez wpływu.
**EFEKT:** rider zmieniający poziomy w starych zakładkach Bafang nie widzi skutku; sufit
poziomu ustawia się dziś przez `max_iq_pct` w Profiles.
**KIEDY:** przy wycofywaniu starych zakładek · **PILNOŚĆ:** LOW · **BLOKUJE:** NIE

### AD-011 — Dwa różne znaczenia słowa „latched"
**SYMBOL:** `latched` w `ride_control.c:644` kontra `session_out.latched`
**PROBLEM:** lokalna zmienna `latched` to stan **bramki pedałowej** (`pedal_assist_gate`),
a `session_out.latched` to stan **sesji jazdy** (`ride_session`). Oba są czytane w tym samym
bloku, kilka linii od siebie, i oznaczają co innego.
**DLACZEGO WAŻNE:** to jest dokładnie ta klasa niejasności, którą FW-109 usuwał na poziomie
kierunku jazdy — tu wróciła na poziomie nazwy.
**KIEDY:** STEP 9 · **PILNOŚĆ:** LOW · **BLOKUJE:** NIE

### AD-012 — Schemat protokołu odsyła do usuniętej stałej
**SYMBOL:** `torque_gate_release_kg` → `current_binding: TQ_GATE_RELEASE`
**PROBLEM:** `TQ_GATE_RELEASE` usunięto w FW-094; `config.h:484` zapisuje to wprost. Schemat
nadal deklaruje wiązanie, więc generator lub audyt zgodności potraktuje je jako istniejące.
**KIEDY:** STEP 2 · **PILNOŚĆ:** MEDIUM · **BLOKUJE:** NIE

### AD-013 — Kotwica startowa `ASSIST_LAUNCH_REFERENCE_U_ABS` nadal niepotwierdzona
**SYMBOL:** `assist_modes.c:64`
**PROBLEM:** 1024 (50 % wypełnienia przy 60 rpm) jest hipotezą zapisaną wprost jako
niezweryfikowana. Skaluje całe wspomaganie poniżej 60 rpm.
**DLACZEGO WAŻNE:** każde strojenie startu i podjazdów opiera się dziś na tej liczbie.
0x6029 raportuje `u_abs` i cadence razem, więc jedna jazda ją rozstrzyga.
**KIEDY:** przed strojeniem startu (STEP 7) · **PILNOŚĆ:** MEDIUM · **BLOKUJE:** NIE

### AD-014 — Extended Boost nadal niepotwierdzony jazdą
**SYMBOL:** `assist_extended_boost.c`, `extended_boost_duration_ms`
**PROBLEM:** kod w gałęzi głównej, domyślnie wyłączony, nigdy nie potwierdzony na rowerze
(stan zgodny z notatką `project-fw084-extended-boost`). Bank v8 ma 255 B — koniec miejsca.
**KIEDY:** osobna karta, poza tą roadmapą · **PILNOŚĆ:** LOW · **BLOKUJE:** NIE

---

## Podsumowanie priorytetów

| Pilność | Pozycje | Wspólny mianownik |
|---|---|---|
| **HIGH** | AD-002, AD-003, AD-005 | użytkownik stroi coś innego, niż myśli — jedna pozycja z ryzykiem limitu |
| **MEDIUM** | AD-001, AD-004, AD-006, AD-012, AD-013 | rozjazd dokumentacji, schematu i kodu |
| **LOW** | AD-007..AD-011, AD-014 | martwy kod i nazewnictwo, do sprzątnięcia przy okazji |

Żadna pozycja nie blokuje bieżącej pracy. **Trzy pozycje HIGH da się zamknąć bez dotykania
firmware** — dwie opisem w CANable (STEP 1), trzecia wymaga karty (STEP 6).
