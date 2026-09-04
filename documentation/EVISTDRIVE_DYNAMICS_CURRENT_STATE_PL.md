# eVistDrive — DYNAMIKA: STAN BIEŻĄCY (audyt źródłowy)

Data: 2026-09-02
Baseline: `77e4743` (STOP-CLICK-C1), kandydat HW `0.0475_M820_BL820.bin`
Zakres: tor TORQUE → RIDER INTENT → ASSIST → LIMITS → DYNAMICS → FINAL Iq → FOC
Status: **audyt, zero zmian w kodzie produkcyjnym**

Dokumenty siostrzane: [kontrakt CANable](EVISTDRIVE_CANABLE_FUNCTIONAL_CONTRACT_PL.md) ·
[architektura docelowa](EVISTDRIVE_DYNAMICS_TARGET_ARCHITECTURE_PL.md) ·
[plan migracji](EVISTDRIVE_DYNAMICS_MIGRATION_PLAN_PL.md) ·
[dług architektoniczny](EVISTDRIVE_ARCHITECTURE_DEBT_PL.md) ·
[słownik nazw](EVISTDRIVE_TERMINOLOGY_ALIAS_PL.md)

---

## 0. Najpierw prostym językiem

Sterownik ma **jednego, czystego właściciela finalnego prądu** — to zostało potwierdzone dwa razy
i ten audyt tego nie podważa. Problem leży gdzie indziej: **zanim** zadanie trafi do tego jednego
właściciela, przechodzi przez kilka niezależnych mechanizmów, z których każdy ma własną pamięć i
własne opóźnienie. Efekt: ustawiasz „Deceleration 250 ms", a rower zwalnia przez ~2 sekundy — bo
to nie rampa prądu decyduje, tylko dwa mechanizmy **przed** nią.

Trzy najważniejsze rzeczy, które audyt ustalił i które da się sprawdzić w źródle:

1. **Suwak „RUN torque smoothing" (0–360°) nie robi tego, co obiecuje.** Liczba stopni nie ma
   dziś ŻADNEGO wpływu na jazdę. Działa wyłącznie jako włącznik: `0` = filtr wyłączony,
   cokolwiek innego = włączony filtr czasowy 120 ms w górę / **350 ms w dół**. Uśrednianie po
   kącie korby (FW-085) nadal jest liczone w kodzie, ale jego wynik **nigdy nie jest publikowany**.
2. **To ten filtr 350 ms, a nie rampa, odpowiada za wrażenie „ciągnie po puszczeniu"** — do
   dokładnego zera schodzi ~2 s (pomiar host). Rampa opadania na poziomie SPORT to 250 ms
   (szybka) / 1050 ms (wolna) pełnej skali, więc przy normalnym poziomie prądu trwa
   kilkadziesiąt–kilkaset ms. Rampa nie jest wąskim gardłem — filtr jest.
3. **Po nim jest jeszcze podłoga 2 % przez 1400 ms.** Dopiero gdy oba wygasną, rampa opadania
   w ogóle ma co robić.

Do tego: **„Startup boost end cadence" nie działa w domyślnym trybie**, a **„Run deadband" nie
jest w ogóle czytany przez sterowanie** — trafia tylko do diagnostyki.

---

## 1. Rzeczywisty tor sygnału (zweryfikowany w źródle)

Wszystko poniżej dzieje się w jednym wywołaniu `ride_control_update()` na tick 4 kHz, w tej
kolejności. Numery przy strzałkach odsyłają do sekcji 2.

```text
ADC czujnika nacisku (mV)
  |
  |-(1) auto-zero / offset_correction              torque_input_correct()
  |-(2) delta ponad zero, clamp 0..2600
  |-(3) deadband assist 10 native
  |-(4) FAST filter  tau=35 ms (symetryczny)       update_assist_filter()      [STANOWY]
  |-(5) RUN publikacja - zalezna od automatu recovery:
  |        IDLE       -> filtr asymetryczny 120/350 ms  update_run_asym_filter()  [STANOWY]
  |        okno = 0   -> RUN = FAST (filtr pominiety)
  |        TRACK_FAST -> RUN = FAST
  |        srednia po kacie korby (FW-085)              NIGDY NIE PUBLIKOWANA     [MARTWA]
  |
  |-(6) assist_modes_calculate()   <- RUN w kg
  |        + startup boost mnozy OBCIAZENIE (krzywa po cadence)                [STANOWY: latch]
  |        + tryb: Power Linear/Progressive/Curve, eMTB, Torque
  |        + cadence compensation (per bank)
  |        + sufit max_motor_power_w (obie kotwice)
  |        + P -> Iq:  Iq = P/(duty*V), crossfade z kotwica startowa po u_abs
  |        + sufit max_iq_pct poziomu
  |
  |-(7)  ride_session (COLD/ACTIVE/SUSPENDED) = POZWOLENIE                     [AUTOMAT]
  |-(7)  pedal_assist_gate = zmienna `latched` w ride_control.c                [AUTOMAT]
  |-(8)  hold + podloga min-Iq: 1400 ms x 2 % ride_core_iq_limit               [STANOWY]
  |-(9)  FINALNA BRAMKA BEZPIECZENSTWA (direction inhibit / hard cut -> 0)
  |-(10) Extended Boost (domyslnie OFF) - podmienia cel                        [AUTOMAT]
  |-(11) HARD CUT -> cel 0, release := 200 ms (stala firmware)
  |-(12) assist_limits_apply(PEDAL): napiecie, temperatura 75->90 C, predkosc  [BEZSTANOWY]
  |-(13) throttle: osobne wywolanie limitera jako NON_PEDAL (5->7 km/h), max()
  |-(14) smooth start (koperta 0->100 %, uzbrajana tylko na prawdziwym postoju)[STANOWY]
  |-(15) gear preload: cap 10 counts poki wirnik < 3 erps, max 300 ms          [STANOWY]
  |-(16) coast release: cel==0 && erps<10 -> FORCE_ZERO
  |-(17) battery_iq_cap - cap upstream, min-arbitraz, histereza 90 %           [LATCH]
  +-(18) ride_final_iq_slew_compute() -> mailbox -> fast_iq_slew_tick() @16 kHz
                                             |                    [JEDYNY WLASCICIEL]
                                             +-> MS.i_q_setpoint -> PI_iq -> FOC
```

**Potwierdzenie ownership (bez zmian względem QS-3D):** jedynym normalnym dynamicznym pisarzem
`MS.i_q_setpoint` jest `fast_iq_slew_tick()`. `motor_core` udostępnia już tylko
`motor_core_set_id_target()`. Brak post-slew clampu bateryjnego, brak drugiego pisarza.

---

## 2. Wymagana TABELA MECHANIZMÓW DYNAMIKI (sekcja 32 mandatu)

Skróty stanu: **S** = ma własną pamięć między tickami, **B** = bezstanowy.

| # | MECHANIZM | ŹRÓDŁO | POCHODZENIE | PIERWOTNY CEL | ROLA DZIŚ | STAN | CZAS / TEMPO | EFEKT W NORMALNEJ JEŹDZIE | KONFIGUROWALNY | NAKŁADA SIĘ Z | DECYZJA (propozycja) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | FAST torque filter (`assist_filter_q`) | `torque_input.c:186` | EBICS/eVD (FW-033) | odszumienie sygnału ADC | kondycjonowanie sensora | S | tau = 35 ms, symetryczny | pomijalny, właściwy | NIE (stała `TORQUE_ASSIST_FILTER_MS`) | RUN asym | **KEEP** — jedyny uczciwy filtr sensoryczny |
| 2 | RUN asym filter (`run_asym_q`) | `torque_input.c:214` | eVD (FW-112.4) | skrócić lag wolnego narastania po FW-085 | **de facto główna rampa opadania roweru** | S | rise tau=120 ms, fall tau=350 ms, snap do 0 poniżej 5,47 native | **DOMINUJĄCY**: ~2 s od 300 do zera (pomiar host) | tylko 0 / różne od 0 przez suwak RUN smoothing | Ramp Down, hold/floor | **MOVE** — tłumienie tętna nogi zostaje w warstwie sensora, kształtowanie odczucia idzie do finalnego ownera |
| 3 | RUN moving average po kącie korby | `torque_input.c:395` | TSDZ2 → eVD (FW-085) | kasować tętno nogi niezależnie od cadence | **wynik nigdy nie publikowany** | S | okno 0–360 stopni | **BRAK** | pozornie tak (stopnie) | zastąpiony przez #2 | **REMOVE albo REHOME** (decyzja właściciela) |
| 4 | RUN fast attack (`run_attack_steps`) | `torque_input.c:441` | eVD (FW-090) | szybkie przejęcie po realnym wzroście | wyłączony na stałe | S | `TORQUE_RUN_ATTACK_STEPS = 0` | BRAK | NIE | #2 | **REMOVE** razem z #3 |
| 5 | Rolling-rearm recovery (automat 3-stanowy) | `torque_input.c:337` | eVD (FW-112 v2 + PATCH A) | po cofnięciu pedałów wrócić z pełną wartością | aktywny, wąsko zakresowy | S | próg stabilności 4 x 35 ms | tylko po suspensji kierunku | NIE | #2 (podmienia RUN) | **KEEP** |
| 6 | Startup boost | `assist_start.c:70` | TSDZ2 → eVD | mocniejsze ruszanie | mnoży OBCIĄŻENIE (kg), nie prąd | S (latch trybu SPEED) | krzywa 120 pozycji, zanik `cadence_step` na krok | duży przy 0 rpm | strength, mode, end_rpm (patrz D4) | smooth start, preload | **KEEP** — jeden mechanizm startu |
| 7 | min-Iq hold + podłoga | `ride_control.c:687` | eVD (FW-031/032/091/112) | brak pulsowania w martwym punkcie korby | podtrzymuje prąd po zaniku żądania | S | hold 1400 ms, podłoga 2 % x `ride_core_iq_limit` | **wyraźny ogon po ustaniu żądania** | tak (Sustain, Current floor) | #2 (ta sama funkcja), Ramp Down | **DECISION REQUIRED** — patrz sekcja 4 |
| 8 | Extended Boost | `assist_extended_boost.c` | eVD (FW-084/100) | ciągnąć chwilę po ustaniu pedałowania | domyślnie WYŁĄCZONY (duration 0) | S (automat) | 0–1000 ms | brak (off) | tak, per level | rampa release | **KEEP jako OFF** — niepotwierdzony jazdą |
| 9 | Smooth start | `assist_start.c:162` | eVD (FW-092) | miękkie ruszanie z postoju | koperta 0 → 100 % | S | domyślnie OFF, 300 ms | brak przy domyślnych | tak, per level | startup boost, rampa rise, preload | **KEEP** (start-only) |
| 10 | Gear preload | `ride_control.c:996` | eVD (FW-041) | wybrać luz przekładni bez klapnięcia | cap 10 counts | S | do 300 ms, wirnik < 3 erps | tylko pierwszy moment startu | NIE | smooth start | **KEEP** (start-only) |
| 11 | Battery Iq cap | `battery_iq_cap.c` | eVD (QS-3C) | limit prądu baterii w domenie Iq | limiter upstream | S (latch histerezy) | histereza 90 %, ciągły | tylko przy limicie | tak (Max battery current) | inne limitery | **KEEP** |
| 12 | Limiter napięcie / temperatura / prędkość | `assist_limits.c` | EBiCS | ochrona i legalność | limiter | **B** | liniowy `map()`, bez pamięci | próg 25 km/h: taper 2 km/h | tak | — | **KEEP** — wzorzec dla pozostałych |
| 13 | Final Iq slew QS-3D | `fast_iq_slew.c` + `ride_control.c:1153` | eVD (QS-3/3B/3C/3D, wzorzec G532) | jeden właściciel finalnego prądu | **jedyny finalny właściciel** | S (akumulator Q10) | RISE/FALL = pełna skala / czas; RELEASE = czas stały z żywej wartości; SAFETY 200 ms | zgodny z ustawieniami | tak, 4 rampy + release per level | #2, #7 | **KEEP jako docelowy owner Ramp Up/Down** |
| 14 | Hard-cut ramp | `ride_control.c:228` | eVD (FW-037) | brak klapnięcia przy hamulcu | rampa bezpieczeństwa | uzywa #13 | `RIDE_HARD_CUT_RAMP_MS = 200`, asercja <= 250 | tylko przy cut | NIE (celowo) | — | **KEEP** — wzorcowo odseparowany |
| 15 | Coast release | `ride_control.c:1025` | eVD (FW-048) | uciec przed skokiem kąta przy 5,5 erps | FORCE_ZERO poniżej 10 erps | B | natychmiast | koniec wybiegu | NIE | #13 | **KEEP** |
| 16 | `assist_dynamics_apply()` | `assist_dynamics.c:58` | eVD (dawny finalny owner 4 kHz) | poprzednia rampa finalna | **0 wywołań produkcyjnych** | S (martwy) | — | BRAK | — | #13 | **REMOVE** (po migracji #13) |

---

## 3. Ile faktycznie trwa "puszczenie gazu" — rachunek

Scenariusz, na który skarży się właściciel: jazda trwa, rider **zmniejsza nacisk** (nie przestaje
pedałować). Wtedy:

| Etap | Co się dzieje | Czas |
|---|---|---|
| FAST | sygnał czujnika opada | tau 35 ms — pomijalne |
| **RUN asym** | opada z tau=350 ms do snapa przy 5,47 native | **~1,4–2,0 s do dokładnego zera** |
| assist_modes | żądanie mocy podąża liniowo za RUN | bez własnego opóźnienia (FW-129B usunęło filtry mocy) |
| **hold + podłoga** | po zejściu żądania do 0 podtrzymuje 2 % przez 1400 ms | **+1,4 s** |
| **FALL ramp** | dopiero teraz ma co robić: 250 ms pełnej skali (SPORT, szybka) | ~10–60 ms z poziomu 2 % |

Rider ustawia "Deceleration 250 ms", a fizycznie dostaje **~2,0 s + 1,4 s**, po czym rampa
250 ms wykonuje ostatnie 2 %. **Rampa nigdy nie jest wąskim gardłem w tym scenariuszu.**

Scenariusz drugi (przestaje pedałować): cadence spada do 0, `prepare_assist_input()` zwraca
`false`, żądanie = 0 **w tym samym ticku**, tryb RELEASE, czas = `release_ms` (650 ms) liczony
z żywej wartości akumulatora. Ten przypadek jest czysty i zgodny z opisem w UI.

Scenariusz trzeci (limiter prędkości ~25 km/h): limiter jest **bezstanowy** (`map()`), więc sam
nie dokłada opóźnienia. Powrót po zejściu poniżej limitu to RISE z rampą `iq_rise_fast_ms`
(SPORT: 380 ms pełnej skali). Odczuwana ociężałość powrotu pochodzi z odbudowy ŻĄDANIA przez RUN
(tau = 120 ms w górę) i z samej rampy — nie z limitera.

---

## 4. Pytanie otwarte, którego audyt nie rozstrzyga sam

**Czy podłoga min-Iq + hold (pozycja 7) jest jeszcze potrzebna?**

Powstała (FW-031/032), żeby wspomaganie nie pulsowało w martwych punktach korby, w czasach gdy:

- nie było uśredniania RUN po kącie korby (FW-085 przyszło później),
- nie było filtra asymetrycznego (FW-112.4),
- nie było persistent ARMED_ZERO ani ciągłości PI (QS-2 / STOP-CLICK-C1),
- finalna rampa nie była jednym właścicielem (QS-3D).

Dziś **każdą z tych funkcji ma kto inny**: tętno nogi tłumi pozycja 2, ciągłość momentu zapewnia
PI + ARMED_ZERO, wygładzanie prądu — pozycja 13. Hipoteza z rozdziału 25 mandatu jest więc
**wstępnie potwierdzona źródłowo**: podłoga jest historyczną nakładką. To jednak zmiana odczucia
jazdy, więc decyzja należy do właściciela — karta **STEP 6** w
[planie migracji](EVISTDRIVE_DYNAMICS_MIGRATION_PLAN_PL.md).

---

## 5. Co audyt POTWIERDZIŁ jako czyste (nie otwierać bez nowego dowodu)

- **Finalny owner Iq**: 1 pisarz dynamiczny, `fast_iq_slew_tick()` @16 kHz.
- **PI setpoint**: `PI_iq.setpoint = znak x MS.i_q_setpoint`, jeden producent.
- **PI feedback**: prądy faz, Clarke, Park, `MS.i_q`. Bez przełączania na prąd baterii.
- **ARMED_ZERO**: zwykłe zero momentu nie oznacza MOE OFF ani resetu PI.
- **STOP-CLICK-C1**: usunięcie foreground resetów integratorów PI usunęło klik. Nie przywracać.
- **Limiter prędkości**: bezstanowy, bez własnej rampy — jest wzorcem, nie problemem.
- **Hard cut**: własna, jawna, firmware-owned rampa 200 ms z asercją kompilacji.

## 6. Co audyt USTALIŁ jako NOWE (nie było w poprzednich dwóch audytach)

| ID | Ustalenie | Dowód |
|---|---|---|
| N1 | Uśrednianie RUN po kącie korby jest martwe — wynik nadpisywany co tick | `torque_input.c:803-809` kontra `:462` |
| N2 | Suwak RUN smoothing w stopniach działa tylko jako włącznik 0 / różne od 0 | ten sam blok + `torque_input.c:397` |
| N3 | `assist_run_deadband_mv` nie ma konsumenta sterującego | jedyne użycie: `main.c:3093` do `rearm_delay_diag` |
| N4 | `startup_boost_end_rpm` działa tylko w trybie SPEED | `assist_start.c:109-129` |
| N5 | Podłoga min-Iq omija sufit `max_iq_pct` poziomu | `ride_control.c:718` używa `ride_core_iq_limit`, nie sufitu poziomu |
| N6 | Procent prądu na poziom z Para1 (`phase_current_max_scaled`) nie wpływa na ride core | `ride_control.c:618` nadpisuje `dynamics_iq_scale` |
| N7 | `IQ_RAMP_*_TICKS` oraz `IQ_SLEW_*` z `config.h` mają 0 użyć produkcyjnych | grep: tylko martwy `assist_dynamics.c` |
| N8 | Schemat protokołu odsyła do usuniętej stałej `TQ_GATE_RELEASE` | `evistdrive_config_schema.yaml:585` kontra `config.h:484` |

Szczegóły i priorytety: [rejestr długu](EVISTDRIVE_ARCHITECTURE_DEBT_PL.md).
