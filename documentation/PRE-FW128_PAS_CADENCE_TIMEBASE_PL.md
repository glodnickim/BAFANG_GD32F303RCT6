# PRE-FW128 — naprawa toru czasu PAS / kadencji

**Status:** wdrożone, testy hosta zielone, **sprzęt jeszcze NIE** (czeka na zgodę)
**NORMAL:** 0.0448 · **DIAG:** 0.0449 · **Wejście:** FW-128C0 `fe2becc`

---

## 0. Najprościej

Kadencja nie była mierzona zegarem. Była liczona **wykonaniami pętli głównej**. Kiedy pętla
zdążyła zrobić przebieg tylko raz zamiast czterech, licznik naliczył 1 zamiast 4 — a ponieważ
obroty to `10000 / okres`, **za krótki okres = za wysoka kadencja**. Stąd skoki na HMI.

Naprawa: piny PAS są teraz odczytywane w przerwaniu 4 kHz, każde zbocze dostaje **znacznik
czasu**, a pętla główna odbiera je w kolejce. Zajęta pętla może teraz **opóźnić** reakcję;
nie może już **zmienić** tego, co i kiedy się stało.

---

## 1. AUDYT — stan zastany

### 1.1 Ścieżka

```
PC12 (A) / PD2 (B)
  └─ odczyt w reg_ADC_processing()          ← PĘTLA GŁÓWNA, bramkowana bitem reg_ADC_flag
      └─ pas_quadrature_step()               (tabela, czysta funkcja — OK)
          ├─ FORWARD → pas_cycle_ticks → MS.cadence = 10000/pas_cycle_ticks
          ├─ REVERSE → pas_direction_on_step(-1), reset fwd_run
          └─ INVALID → pas_direction_on_step(0), reszta ZIGNOROWANA
              └─ MS.cadence → rider_input.cadence_rpm → assist_modes / assist_dynamics
              └─ MS.cadence → CAN_Display.c:758 ramka 0x02F83200 bajt 3 → HMI
```

### 1.2 Gdzie powstawał czas

`TIMER1_IRQHandler` (`main.c:2023`) ustawiał **jednobitową** flagę `reg_ADC_flag=1`.
Pętla główna (`main.c:1095`) robiła `if(reg_ADC_flag) reg_ADC_processing();`.

**Bit nie potrafi liczyć.** Cztery przerwania między dwoma przebiegami pętli ustawiały tę
samą flagę raz → jeden przebieg → **jedna próbka GPIO i jeden przyrost licznika**.

Firmware **już mierzy**, jak często to się dzieje: `missed_control_ticks`,
`missed_control_events`, `worst_missed_burst_ticks` (FW-103/104, `main.c:2303-2311`).
`control_time_ticks` — prawdziwy zegar 4 kHz — **już istniał** w tym samym ISR. PAS po prostu
z niego nie korzystał.

### 1.3 Liczniki zależne od pętli głównej

| licznik | rola | ta karta? |
|---|---|---|
| `pas_cycle_ticks` | okres kadencji | **TAK — naprawione** |
| `pas_idle_ticks` | czas od ostatniego zbocza (stop, adaptacyjny timeout, re-zero momentu) | **TAK** |
| `pas_last_period_ticks` | baza adaptacyjnego timeoutu | **TAK** |
| `pas_liveness` licznik wewnętrzny | werdykt REAL_STOP | **TAK** |
| `PAS_counter` | timeout kadencji (`MP.PAS_timeout`) | **TAK** (§7 karty) |
| `torque_counter` | 1 s bez momentu | **NIE — osobne ustalenie** |
| `tq_fault_ticks` | debounce błędu 25 | **NIE** |
| `uint16_half_rotation_counter` | prędkość | **NIE** |
| `pwm_cutoff_tick` | okno miękkiego odcięcia | **NIE** |
| `ui16_erps_counter` | wiek zdarzenia Halla | **NIE** |
| `slow_loop_counter`, `t3100_counter`, `soc_tick_counter` | telemetria/SOC | **NIE** |

> Osiem liczników poza PAS ma **dokładnie tę samą wadę**. Karta zakazuje ich automatycznego
> rozszerzania, więc są wypisane jako ustalenia, nie naprawione. Najpoważniejszy z nich to
> `pwm_cutoff_tick` — okno odcięcia mocy odmierzane przebiegami pętli.

### 1.4 Rzeczywista rozdzielczość i wzór

`PAS_STEPS_PER_PULSE = 4`, założenie repo: **96 przejść kwadratury na obrót korby**.

```
rpm = kroki_na_impuls × 60 × f_tick / (przejścia_na_obrót × okres)
    = 4 × 60 × 4000 / (96 × okres)
    = 10000 / okres          ← DOKŁADNIE stała, która była w kodzie
```

**Stała 10000 jest poprawna dla tej konfiguracji.** Nie jest już magiczną liczbą — jest
wyprowadzona w `config.h` jako `PAS_CADENCE_RPM_NUMERATOR` z `PAS_TRANSITIONS_PER_REV = 96`.

> **OTWARTE, nie zmieniane:** notatka o rozdzielczości PAS zapisuje niepotwierdzoną
> dwuznaczność 48 vs 96 przejść/obrót — **czynnik 2 na SKALI** kadencji. To pytanie o skalę,
> a skala nie może wywołać skakania. Zmiana wymaga pomiaru (odliczone 60 rpm), więc karta jej
> nie rusza. Zapisane w `config.h` przy stałej, żeby nie zginęło.

### 1.5 Konsumenci kadencji

| konsument | źródło | jednostka | typ | wpływ na Iq |
|---|---|---|---|---|
| `assist_modes` (`calculate_human_power_mw`, `cadence_comp`) | `rider_input.cadence_rpm` = `MS.cadence` | rpm | **RAW** | **TAK** |
| `assist_dynamics` (rampy góra/dół) | `input->cadence_rpm` | rpm | **RAW** | **TAK** |
| `assist_start` | `measured_cadence_rpm` | rpm | RAW | TAK |
| `forward_pedaling` / bramki startu | `MS.cadence > 0` | — | RAW | TAK |
| `MS.p_human` | `MS.cadence × torque_filtered` | mW | RAW | TAK (tryby mocy) |
| **HMI** `0x02F83200[3]` | `MS.cadence` | rpm | **RAW** | nie |
| `ride_control.cadence_filtered_x8` | `uint16_cadence_filtered` | rpm×8 | FILTERED | **żaden — pole jest MARTWE** |

**Dwa ustalenia:**
1. **HMI dostaje wartość SUROWĄ.** Filtr `uint16_cadence_filtered` istnieje, ale nie trafia
   nigdzie, gdzie coś by go czytało.
2. **`cadence_filtered_x8` jest zapisywane i nigdy nie czytane.** Jedyne dwa wystąpienia
   w całym drzewie to przypisanie i deklaracja pola.

### 1.6 Czy istnieje drugi dekoder PAS?

**Nie.** `PAS_processing()` (era M560) usunięto w FW-109 v2. `EXTI_12` na PC12 jest nadal
włączone i `EXTI10_15_IRQHandler` **tylko czyści flagę sprzętową** — nie czyta stanu, nie
podejmuje decyzji. Nie koliduje, ale jest darmowym przerwaniem na każdym zboczu opadającym A.

### 1.7 Warunki STOP z §17 — wszystkie sprawdzone

| warunek | wynik |
|---|---|
| PC12/PD2 to naprawdę dwa kanały PAS | **TAK** — `gpio_init` IPU, komentarz „PD2 Dual PAS2 input pin" |
| TIMER1 naprawdę 4 kHz | **TAK** — prescaler 2 (÷3), period 9999 (÷10000), 120 MHz → **4000 Hz** |
| drugi ISR/EXTI na tych pinach | **EXTI_12 tak, ale nieszkodliwe** (patrz §1.6). `EXTI_2` jest zmapowane na **PB2** (`gpio_exti_source_select(GPIOB, PIN_SOURCE_2)`), czujnik prędkości — **nie** PD2 |
| ryzyko dla deadline ADC/FOC | **NIE** — patrz §5 |
| kroki/obrót zgodne z założeniem repo | **NIEPOTWIERDZONE, nie blokuje** — patrz §1.4 |

> **Utajone zagrożenie (nie w zakresie):** `UART4_init()` konfiguruje **PC12 jako AF push-pull**
> (UART4 TX). Jest skompilowane out (`PRINTDEBUG_UART` zakomentowane w `config.h`), ale
> włączenie tego przełącznika **zamienia kanał A czujnika PAS w wyjście UART**.

---

## 2. Architektura PO

```
TIMER1 @4 kHz  ─────────────────────────────────────────────  ZEGAR, nie harmonogram
   │
   ├─ control_time_ticks++                       (już istniało, FW-103/104)
   ├─ jeden odczyt PC12+PD2
   ├─ pas_sampler_isr_tick(ab, tick)             ← NOWE, produkcja
   │     ├─ pas_quadrature_step()                  TA SAMA tabela, JEDNO wywołanie w firmware
   │     ├─ liczniki forward / reverse / INVALID
   │     ├─ last_transition_tick = tick            zegar bezczynności
   │     └─ kolejka zdarzeń {tick, gap, from→to, step}
   └─ pas_raw_isr_sample(ab, tick)               (DIAG, TEN SAM odczyt)

pętla główna, kiedy zdąży
   └─ while(pas_sampler_pop(&ev))                ← KAŻDE zbocze, w kolejności
        ├─ FORWARD → pas_cadence_forward_step(ev.tick, restart)
        ├─ REVERSE → pas_cadence_break_epoch(0)
        └─ INVALID → pas_cadence_break_epoch(1)
      overflow   → pas_cadence_break_epoch(2)
      pas_idle_ticks = control_now − last_transition_tick     ← WYLICZONE
      PAS_counter    = control_now − pas_cadence_pulse_tick   ← WYLICZONE
      pas_liveness_update(pas_idle_ticks, timeout)
```

## 3. Reguła epoki — sedno karty

Opublikowany pomiar **musi obejmować nieprzerwany ciąg kroków w przód**. Trzy rzeczy ten ciąg
przerywają i wszystkie trzy go unieważniają:

| przyczyna | dlaczego |
|---|---|
| **REVERSE** | okres obejmowałby czas sprzed i po zmianie kierunku |
| **INVALID** | liczba kroków składających okres jest błędna o nieznaną wartość |
| **OVERFLOW** | kolejność zdarzeń nie jest już kolejnością korby |

**Nie ma wartości zastępczej, nie ma clampa, nie ma podtrzymania.** Poprzedni pomiar stoi,
dopóki nowy nie zostanie zasłużony; zeruje go dopiero timeout stopu.

## 4. Konwencja czasu bezczynności — zmiana o JEDEN tick

Stary licznik `pas_liveness` był zerowany przy zboczu, a potem inkrementowany w tym samym
przebiegu → w ticku zbocza pokazywał **1**. Nowy `control_now − last_transition_tick` pokazuje
**0**, bo tyle czasu minęło. REAL_STOP zapada więc **jeden tick 4 kHz później** (250 µs) dla tej
samej fizycznej przerwy. Fizycznie bez znaczenia; testy graniczne S6/S13 dostały o jeden tick
dłuższe przytrzymania, żeby badać **ten sam** próg.

## 5. Koszt ISR i priorytety

Rozbiór `pas_sampler_isr_tick` z faktycznego obrazu 0.0448 (profil **debug = -O0**, tak jak się
wysyła):

| ścieżka | instrukcje |
|---|---|
| tick bez zbocza (przytłaczająca większość) | **29** + ~14 na odczyt GPIO i wywołanie |
| tick ze zboczem | + ~40 (tabela, liczniki, wpis do kolejki) |

≈ **0,36 µs na tick** przy 120 MHz → **0,14 % CPU** przy 4 kHz.

**Priorytety:** grupa `NVIC_PRIGROUP_PRE1_SUB3` = 1 bit wywłaszczenia. `TIMER1_IRQn` = (0,0),
`ADC0_1_IRQn` (FOC) = (0,0) → **równe, więc żadne nie wywłaszcza drugiego**. TIMER1 może co
najwyżej opóźnić ISR FOC o czas własnego wykonania: 0,36 µs wobec okresu 62,5 µs = **0,6 %**.
Okno próbkowania FW-127 ma zapas o rząd wielkości większy. **Deadline ADC/FOC nie jest zagrożony.**

> Ustalenie poboczne: `nvic_irq_enable(EXTI10_15_IRQn, 2U, 0U)` podaje priorytet wywłaszczenia
> **2** przy jednym dostępnym bicie. Nie zmieniane w tej karcie.

## 6. Testy hosta — przeciw PRAWDZIWYM modułom

`tests/host/pre128_pas_timebase_host.c` linkuje `pas_sampler.c`, `pas_cadence.c`
i `pas_quadrature.c`. Generuje **przebieg kwadratury**, próbkuje go przez prawdziwy sampler
i liczy kadencję prawdziwym modułem. **Nic z dekodowania ani z arytmetyki okresu nie jest
w teście powtórzone** — inaczej test zgadzałby się sam ze sobą, a nie z firmware'em.

| scenariusz | wynik |
|---|---|
| **A** 40/60/80/100/120 rpm | 40 / 59 / 80 / 100 / 119 — błąd ≤ 1 rpm, zero INVALID |
| **B obsługa main co 1/2/5/10/37 ticków** | **kadencja 75 IDENTYCZNA, kroki 479 IDENTYCZNE** |
| **C** przejście nielegalne | policzone, **żadnego RPM**, epoka unieważniona, odzysk po 334 tickach z poprawnym 59 |
| **D** reverse w środku okresu | epoka odrzucona, żaden RPM nie obejmuje reverse, następny poprawny |
| **E** stop | bezczynność liczona od zbocza do teraz przy obsłudze main co 50 ticków |
| **F** przewinięcie uint32 | 59 rpm, zero INVALID |
| **G** nieregularna, zagłodzona pętla | 90 rpm przy obsłudze ~1 na 32 ticki |
| **H** przepełnienie kolejki | policzone, zgłoszone **raz**, zbocza nadal liczone dla liveness |
| **I** FW-086 | krok restartu jest POCZĄTKIEM, impuls obejmuje pełny interwał |

**Wynik: ALL CHECKS PASSED.** Baseline bez zmian (`T14`/`T9` w `rolling_no_assist`) →
**0 nowych regresji.**

> **Pułapka z przebiegu:** pierwsza wersja generatora używała podręcznikowej kolejności Graya
> 0→1→3→2. Przy `PAS_DIR_SIGN = -1` produkcyjna tabela nazywa to **wstecz** — test dostał
> idealnie czysty ciąg kroków do tyłu. Kontrola `0b` sprawdza teraz kierunek **prawdziwym
> dekoderem**, zamiast wierzyć komentarzowi.

## 7. Diagnostyka

Dwie nowe ramki zbiorcze (tylko DIAG, bez telemetrii 4 kHz):

- **`0x00010229`** — kroki w przód / wstecz / **nielegalne** (u16) + **przepełnienia kolejki**
  + flagi. Przepełnienie > 0 to bezpośredni dowód, że pętla główna stanęła na tyle długo, by
  stracić **kolejność**.
- **`0x0001022A`** — ostatni zmierzony okres w realnych tickach, `cadence_raw_rpm`,
  `cadence_filtered_rpm`, flagi (`valid` / `epoch_valid` / „seeded" = `start_phase`) oraz
  **epoki unieważnione osobno przez reverse / invalid / overflow**.

Te trzy liczniki odróżniają **szumiący czujnik** (invalid) od **prawdziwego cofania** (reverse)
od **zajętego procesora** (overflow).

`DIAG_AGGREGATE_FIXED_FRAMES` podniesione 14 → 16 (limit `DIAG_AGGREGATE_SNAPSHOT_MAX` = 21).

## 8. Cadence raw / filtered / valid / seeded

- **`cadence_raw_rpm`** = `MS.cadence`, publikowane wyłącznie z nienaruszonej epoki.
- **`cadence_filtered`** = `uint16_cadence_filtered >> 3`, IIR, **wyłącznie prezentacja**.
- **`cadence_valid`** = `pas_cadence_get()->valid`. Ustawiane **tylko** przez rzeczywisty pomiar.
- **`cadence_seeded`** — **żaden seed nie istnieje.** `START_CADENCE_SEED` nie ma nigdzie
  w drzewie; FW-087 usunął fałszywe 1 rpm. Jedyny stan „wierzymy, że pedałuje, ale jeszcze nie
  zmierzyliśmy" to `start_phase`, i to on jest raportowany pod tą flagą.

**HMI nadal dostaje wartość surową.** Karta wymaga najpierw naprawy źródła, a wygładzanie na
HMI to „presentation filtering", nie naprawa — więc **nie zmieniam** tego teraz. Po jeździe
z tym buildem będzie widać, czy surowa kadencja jest już spokojna; jeśli tak, filtr na HMI jest
zbędny, a jeśli nie, ramka `0x0001022A` powie, dlaczego.

## 9. Usunięte

- `pas_qstate` z `main.c` — stan linii ma teraz **jedną** kopię, w samplerze;
- `pas_fwd_steps` z `main.c` — liczenie kroków należy do `pas_cadence.c`;
- `pas_liveness_tick()` i `pas_liveness_transition()` — moduł nie trzyma już własnego licznika;
- `tests/fw086_cadence_first_pulse.js` — jego model dekodera **duplikował algorytm**, czego karta
  zabrania, a jego sekcje 7/8 sprawdzały tekst starego `main.c`. Obie rzeczywiste własności
  (krok restartu jest początkiem; stała wynika z równania) są teraz w scenariuszach **I** i **1**
  nowego testu, **przeciw prawdziwym modułom**.

## 10. Build

| | NORMAL 0.0448 | DIAG 0.0449 |
|---|---|---|
| FLASH | 103 132 B (43,79 %) | 149 588 B (63,51 %) |
| RAM | 12 544 B (25,52 %) | 46 312 B (**94,22 %**) |
| SHA256 | `71012A0D…412DE971` | `64790400…95912B42` |

Delta wobec 0.0447: FLASH **+992 B** / **+1780 B**, RAM **+320 B** obu — kolejka 32 × 8 B plus
stan i liczniki. **Zapas RAM w DIAG spadł do 2840 B** (było 3160). Ciasno.

> Licznik buildów w `.local/build-number.txt` był rozjechany (438 wobec artefaktu 0.0447),
> więc pierwsza próba wyprodukowała numer **0.0439**. Ten artefakt usunąłem, licznik
> zsynchronizowałem do 447 i zbudowałem od nowa. Żaden istniejący plik nie został nadpisany.

## 11. Sprzęt

**NIE wykonany i NIE proszę o niego bez zgody**, zgodnie z §18. Kiedy zapadnie decyzja,
wystarczy **jeden** przejazd z buildem DIAG 0.0449 i ramkami `0x10229`/`0x1022A`.
