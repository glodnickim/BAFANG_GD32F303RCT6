# FW-122 ROLLING NO-ASSIST — recorder final przed testem rowerowym

Status: implementacja schema v2 zakończona; końcowe wyniki buildów są wpisywane po pełnej walidacji. Ta karta jest wyłącznie diagnostyczna. Nie zmienia ride-control, assist, FOC, PI, Hall, bridge-start, ramp, preload, neutral dwell, kalibracji prądu ani progów bezpieczeństwa.

## 1. Potwierdzony błąd schema v1

Przed finalizacją `rolling_no_assist_sample_t` miał 28 B, ale bridge w `src/main.c` wysyłał:

| Ramka | EFID | Zawartość |
|---|---:|---|
| HEADER | `0x10248` | metadata; nie była częścią sample |
| DATA 0 | `0x10249` | sample bytes 0..7 |
| DATA 1 | `0x1024A` | sample bytes 8..15 |
| DATA 2 | `0x1024B` | sample bytes 16..23 |

Wysłane były więc tylko 24 z 28 bajtów. `debug_flags` na offset 24 pozostawało w RAM i nigdy nie trafiało do CAN/CSV. Stare twierdzenie „28 B sample = 4 data frames” było nieprawdziwe: czwartą ramką był osobny HEADER.

Audyt wykrył drugi błąd: klasyfikacja i confirmation counter działały co tick 4 kHz, a tylko zapis do ringu był dzielony przez 16. Osiem potwierdzeń oznaczało około 2 ms, nie deklarowane 32 ms. W schema v2 klasyfikacja, confirmation i zapis używają tego samego zegara 250 Hz.

## 2. Wybór transportu

| Kryterium | A: repack do 24 B | B: schema v2, 44 B / 6 DATA |
|---|---:|---:|
| Ring RAM | 6144 B | 11264 B |
| Ramki/sample z HEADER | 4 | 7 |
| Payload dumpu 256 próbek | 8192 B | 14336 B |
| Czas przy 10 ms/ramkę | 10.24 s | 17.92 s |
| Decoder | mały, ale nowy layout | jawny dispatch v1/v2 |
| Utrata danych | konieczne usunięcie dowodów B/C | brak utraty zadeklarowanych bajtów |
| Wartość A/B/C | niewystarczająca dla MOE/cal/rotor | pełne dowody priorytetu 1 i 2 |

Wybrano B. Repack 24 B nie mieści jednocześnie upper-control chain, rozdzielenia PWM/MOE, current-cal i dowodów rotor/PI. Wzrost RAM mieści się w istniejącym limicie 34 KiB i nie dotyczy NORMAL.

## 3. Finalny logical sample — RNA schema v2

Wszystkie liczby wielobajtowe są serializowane big-endian. Struktura ma dokładnie 44 B bez paddingu; offsety są chronione `_Static_assert`.

| Offset | B | Pole | Typ | Znaczenie dla diagnozy |
|---:|---:|---|---|---|
| 0 | 4 | `tick_abs` | u32 | wspólny tick 4 kHz |
| 4 | 1 | `flags` | bits | case A/B/C, rider, permission, load, no-demand, PWM |
| 5 | 1 | `status_flags` | bits | real MOE, current-cal allow, neutral active |
| 6 | 1 | `case_id` | u8 | klasyfikacja próbki |
| 7 | 1 | `bridge_lifecycle` | u8 | IDLE=0 .. RUN=5 |
| 8 | 1 | `hall` | u8 | bezpośredni Hall state |
| 9 | 1 | `permission_bits` | bits | istniejące `FW112_PERM_*` |
| 10 | 1 | `reason_bits` | bits | istniejące `FW112_REASON_*` |
| 11 | 1 | `debug_flags` | bits | istniejące ride-control debug flags |
| 12 | 2 | `iq_before_pu` | i16 | wejście limiterów/PU |
| 14 | 2 | `iq_request` | i16 | żądanie trybu |
| 16 | 2 | `iq_after_latch_floor` | i16 | wynik latch/floor |
| 18 | 2 | `iq_pre_ramp` | i16 | żądanie przed rampą |
| 20 | 2 | `iq_setpoint` / final Iq | i16 | finalne żądanie FOC |
| 22 | 2 | `iq_actual` | i16 signed | surowe `MS.i_q`; znak zachowany |
| 24 | 2 | `pi_q_int` | i16 | stan PI prądu q |
| 26 | 2 | `pi_d_int` | i16 | stan PI prądu d |
| 28 | 2 | `erps` | u16 | elektryczna prędkość rotora |
| 30 | 2 | `rpm` | i16 | cadence |
| 32 | 2 | `load_centikg` | u16 | bieżące obciążenie |
| 34 | 2 | `load_threshold` | u16 | próg latch |
| 36 | 2 | `angle_hall` | u16 | bity 31..16 Q31 Hall angle |
| 38 | 2 | `angle_absolute` | u16 | bity 31..16 Q31 absolute angle |
| 40 | 1 | `rotor_direction` | i8 signed | `i8_recent_rotor_direction` |
| 41 | 1 | `neutral_dwell_counter` | u8 | licznik dwell |
| 42 | 2 | `motor_voltage_utilization` | u16 | `MS.u_abs`, clamp 0..2048 |

Redukcja angles zachowuje górne 16 bitów fazy cyklicznej. Rozdzielczość wynosi dokładnie `360 / 65536 = 0.005493164°/LSB`. To wielokrotnie więcej niż potrzeba do rozpoznania sektora Hall, reseed i kierunku; odrzucone dolne bity opisują tylko sub-LSB interpolacji. CSV zachowuje raw u16 i dodatkowo daje przeliczenie na stopnie.

## 4. Finalny CAN wire format

HEADER jest metadanymi i nie należy do logical sample.

| EFID | Rodzaj | Bajty |
|---:|---|---|
| `0x10248` | HEADER | schema=2, session, DATA count=6, capture, trigger case, sample bytes=44, Hz=250, confirmation=8 |
| `0x10249` | DATA 0 | sample 0..7 |
| `0x1024A` | DATA 1 | sample 8..15 |
| `0x1024B` | DATA 2 | sample 16..23 |
| `0x1024C` | DATA 3 | sample 24..31 |
| `0x1024D` | DATA 4 | sample 32..39 |
| `0x1024E` | DATA 5 | sample 40..43 + cztery jawne zera paddingu |

- LOGICAL SAMPLE SIZE: 44 B.
- CAN DATA FRAGMENTS PER SAMPLE: 6.
- HEADER FRAMES PER SAMPLE: 1.
- TOTAL PAYLOAD PER SAMPLE: 56 B w 7 ramkach.
- PADDING: 4 B wyłącznie na końcu DATA 5, walidowane jako zero przez decoder.

## 5. Signal gap audit — stan przed i po

„RAM” oznacza, że sygnał już istniał w firmware przed tą kartą. „Old sample/wire” opisuje schema v1. Koszt dotyczy schema v2; pola packed współdzielą bajt.

| SIGNAL | RAM | OLD SAMPLE | OLD WIRE | FINAL WIRE / CSV | ANALYZER | A/B/C value | Koszt |
|---|:---:|:---:|:---:|---|:---:|---|---:|
| rider_active | yes | packed | yes | `flags` / yes | yes | kontekst A | 1 bit |
| permission | yes | packed | yes | `flags` / yes | yes | A1/A2 context | 1 bit |
| permission_bits | yes | no | no | raw u8 / yes | yes | dokładny gate chain | 1 B |
| reason_bits | yes | no | no | raw u8 / yes | yes | upper-control inhibit | 1 B |
| iq_before_pu | yes | yes | yes | i16 / yes | yes | A2/A3/A4 | 2 B |
| iq_request | yes | no | no | i16 / yes | yes | pierwszy mode output | 2 B |
| iq_after_latch | yes | no | no | i16 / yes | yes | latch/floor divergence | 2 B |
| iq_pre_ramp | yes | no | no | i16 / yes | yes | ramp input | 2 B |
| final_iq | yes | yes | yes | i16 / yes | yes | granica A/B/C | 2 B |
| PWM_ON | yes | packed | yes | `flags` / yes | yes | software bridge intent | 1 bit |
| hardware MOE | yes, TIMER register | no | no | `status_flags` / yes | yes | B: real power-stage enable | 1 bit |
| bridge_lifecycle | yes | yes | yes | u8 / yes | yes | B substage | 1 B |
| neutral_dwell_active | yes | no | no | `status_flags` / yes | yes | bridge transient context | 1 bit |
| neutral_dwell_counter | yes | no | no | u8 / yes | yes | dwell progress | 1 B |
| current_cal_foc_allowed | yes | no | no | `status_flags` / yes | yes | B safety/cal gate | 1 bit |
| hall_state | yes | yes | yes | u8 / yes | yes | C Hall evidence | 1 B |
| q31 angle_hall | yes | no | no | high16 u16 / raw+deg | yes | C sector/reseed | 2 B |
| q31 angle_absolute | yes | no | no | high16 u16 / raw+deg | yes | C interpolation/reseed | 2 B |
| recent rotor direction | yes | no | no | i8 / yes | yes | C rolling-restart hypothesis | 1 B |
| ERPS | yes | yes | yes | u16 / yes | yes | C rotor speed | 2 B |
| iq_actual_raw | yes | yes | yes | i16 signed / yes | yes | C response | 2 B |
| iq_actual_magnitude | derivable | no | no | derived from raw / yes | yes | sign-independent C compare | 0 B |
| PI_q | yes | yes | yes | i16 / yes | yes | C current-loop state | 2 B |
| PI_d | yes | yes | yes | i16 / yes | yes | C flux-loop state | 2 B |
| debug_flags | yes | yes | **no** | raw u8 / yes | yes | upper-control details | 1 B |
| torque/load | yes | no | no | load u16 / yes | yes | A context | 2 B |
| cadence | yes | yes | yes | i16 / yes | yes | rider/rolling context | 2 B |
| load threshold | yes | yes | yes | u16 / yes | yes | A threshold relation | 2 B |
| case_id | recorder | yes | yes | u8 / yes | yes | A/B/C result | 1 B |
| trigger case | recorder | header | yes | HEADER / yes | yes | capture cause | metadata |
| motor voltage utilization | yes | no | no | u16 / yes | yes | PI saturation context | 2 B |

## 6. Sygnały świadomie nie dodane

| SIGNAL | ON WIRE FINAL? | Dlaczego |
|---|:---:|---|
| pełne 32-bit Q31 angles | no | high16 zachowuje sektor/reseed z 0.00549°/LSB; pełne wartości kosztowałyby dodatkowe 4 B/sample |
| `assist_level` | no | permission bits i rzeczywiste kolejne Iq pokazują decyzję używaną przez A/B/C; level jest kontekstem konfiguracji, dostępnym w FW112 A/B loggerze, ale nie rozstrzyga power-stage/FOC |
| wheel speed + speed limit | no | agregat permission i `SPEED_LIMIT_OK` są faktycznym wynikiem gate; dwie pełne wartości wymagałyby 4 B/sample i nie są potrzebne do rozdziału A/B/C |
| osobne brake/hard-cut/safety booleans | no | obecne bezstratnie w `permission_bits`, `reason_bits` i `debug_flags`; osobne kopie byłyby redundantne |
| voltage/temperature | no | są limiter context, lecz ich wynik jest już w permission/reason; nie rozstrzygają B kontra C po dodatnim final Iq |
| `i_d_actual`, `u_q`, `u_d` | no | przy tej karcie C wymaga raw Iq, PI q/d, rotor i utilization; dodatkowe 6 B podniosłoby format do kolejnej ramki |
| osobne `iq_actual_magnitude` | no | bezpiecznie i dokładnie wyliczane jako `abs(iq_actual_raw)`; raw signed pozostaje źródłem prawdy |

Pozostałe cztery bajty DATA 5 są celowo paddingiem. Wypełnienie ich zwiększyłoby logical sample i ring RAM, mimo że nie zmieniłoby liczby ramek CAN.

## 7. CASE A/B/C

CASE A: rider latch, permission i load context istnieją, ale `final_iq==0` i `iq_before_pu==0`. Recorder tylko obserwuje istniejące wyniki.

CASE B: `final_iq>0`, a co najmniej jeden fakt jest fałszywy: software PWM, hardware MOE, lifecycle przed MOE albo `current_cal_foc_allowed`. Trace Analyzer raportuje evidence oraz deterministyczny subreason, np. `CURRENT_CAL_FOC_NOT_ALLOWED`, `PWM_ON_NOT_SET`, `HARDWARE_MOE_NOT_SET` albo `BRIDGE_LIFECYCLE_BEFORE_MOE`.

CASE C: `final_iq>0`, PWM=1, real MOE=1, lifecycle>=2 i current-cal allow=1, lecz magnitude measured Iq jest mniejsze niż 25% final Iq.

Konwencja znaku:

```text
final_iq=+500, PWM=1, MOE=1, iq_actual_raw=-500 -> NO C
final_iq=+500, PWM=1, MOE=1, iq_actual_raw= -20 -> C
```

Porównanie używa magnitude; sample/CSV/evidence zachowują signed raw Iq.

## 8. Spójny rotor/power snapshot

Co 16 ticków control loop tworzy lokalny input. W jednym krótkim read-only critical section kopiowane są: software PWM, hardware `TIMER_CCHP.POEN`, lifecycle, Hall, oba angles, recent direction, ERPS, final Iq, raw Iq, PI q/d, cadence, neutral state/counter i `u_abs`. Sekcja nie wywołuje funkcji i niczego nie zapisuje do control state. Poprzedni PRIMASK jest zachowany, więc snapshot nie włącza przerwań, jeżeli były wcześniej wyłączone.

Upper-control snapshots, permission/reason/debug, torque i read-only `current_cal_foc_allowed()` są pobierane poza irq-off. Audyt funkcji current-cal potwierdził, że zwraca wyłącznie istniejące `foc_allowed` i nie ma side effects.

## 9. Sample rate, persistence i ring

- control tick: 4 kHz;
- sample: `tick_abs % 16 == 0`, czyli 250 Hz / 4 ms;
- PRE: 100 = 400 ms;
- POST: 156 = 624 ms;
- total: 256 = 1024 ms;
- persistence: 8 kolejnych sklasyfikowanych próbek = 32 ms.

Confirmation nie jest już liczone na niesamplowanych tickach. Test jawnie wykonuje 15 pośrednich ticków po siódmej obserwacji i wymaga braku triggera; ósma obserwacja otwiera capture.

## 10. Decoder i compatibility

`tools/decode_rolling_no_assist.ps1` wybiera layout wyłącznie na podstawie schema byte w HEADER:

- v1: dokładnie 3 DATA; pola z nieprzesłanych offsetów pozostają puste;
- v2: dokładnie 6 DATA oraz obowiązkowe metadata `6/44/250/8`;
- unknown schema, błędne metadata, duplicate fragment albo niezerowy padding: fail closed;
- niepełny sample: warning i pominięcie, bez wymyślania wartości.

CSV eksportuje wszystkie faktycznie przesłane pola, w tym osobno `iq_actual_raw` oraz derived `iq_actual_magnitude`. Stary literalny fixture schema v1 nadal jest dekodowany.

## 11. Niezależny C oracle

Test nie wykonuje decoder↔encoder tego samego modelu. Produkcyjna funkcja C `rolling_no_assist_diag_encode_fragment()` jest wywoływana dla dwóch rozpoznawalnych struktur, a każda ramka jest porównywana z literalnym `[7][8]`:

- sample ON: signed Iq=-500, PI q=-12000, non-zero angles/direction, MOE=1, cal=1;
- sample blocked: Iq=-20, PI d=-32768, other angles, direction=-1, PWM=1, MOE=0, cal=0.

Te same literalne bajty są zamrożone w `tests/traces/adapters/rolling_schema2_c_oracle.log` i niezależnie dekodowane przez PowerShell do oczekiwanych CSV fields.

## 12. Trace Analyzer

Analyzer nie zmienia architektury. Konsumuje final CSV i mapuje: permission/reason/debug, wszystkie etapy Iq, PWM, MOE, lifecycle, current-cal, Hall/angles/direction/ERPS/PI oraz raw signed Iq. CLASS C nadal używa magnitude. CLASS B zachowuje `FIRST DIVERGENCE: BRIDGE_ON` i dodaje tylko subreason oparty na zapisanym fakcie; brak sygnału nie jest zamieniany w pseudo-root-cause.

## 13. HMI live Iq

Nie zmieniono HMI CAL. W DIAG pozostaje finalnym `MS.i_q_setpoint`; `0` oznacza brak final Iq, wartość dodatnia oznacza istniejące żądanie. `MS.calories`, `MS.int_Temperature`, thermal protection i telemetry NORMAL są nietknięte.

## 14. Koszt BEFORE vs AFTER

| Miara | BEFORE schema v1 | AFTER schema v2 | Delta |
|---|---:|---:|---:|
| logical sample RAM | 28 B | 44 B | +16 B |
| ring 256 | 7168 B | 11264 B | +4096 B |
| zmierzony recorder object BSS | — | 11292 B | — |
| RNA budget line | 7296 B | 11356 B | +4060 B |
| total DIAG budget line sum | 26092 B | 30152 B | +4060 B |
| headroom do 34 KiB budget ceiling | 8724 B | 4664 B | -4060 B |
| HEADER + DATA frames/sample | 1+3=4 | 1+6=7 | +3 |
| payload/sample | 32 B | 56 B | +24 B |
| frames/256 samples | 1024 | 1792 | +768 |
| CAN payload/256 samples | 8192 B | 14336 B | +6144 B |
| dump przy 10 ms/frame | 10.24 s | 17.92 s | +7.68 s |
| ARM object text recordera | — | 1206 B | — |
| NORMAL recorder text/data/bss | 0/0/0 B | 0/0/0 B | 0 |
| full DIAG FLASH used (`text+rodata+data load`) | 142008 B (0.0416) | 143120 B (0.0418) | +1112 B |
| full DIAG text bytes | 141740 B (0.0416) | 142852 B (0.0418) | +1112 B |
| full DIAG BSS bez heap/stack | 34124 B (0.0416) | 38216 B (0.0418) | +4092 B |
| full DIAG RAM z heap/stack | 37456 B (0.0416) | 41552 B (0.0418) | +4096 B |

CAN bytes powyżej oznaczają payload 8 B/ramkę. Fizyczny koszt magistrali zależy od extended-frame overhead i bit stuffing. Scheduler wysyła jedną ramkę co 10 ms, stąd czas dumpu jest wyznaczalny dokładnie z liczby ramek.

## 15. Izolacja NORMAL

`inc/config.h` ustawia `ROLLING_NO_ASSIST_DIAG_ENABLE=1` tylko przy diagnostics i `0` w NORMAL. Cały stan/kod modułu jest za:

```c
#if CAN_DIAGNOSTICS_ENABLE && ROLLING_NO_ASSIST_DIAG_ENABLE
```

Niezależny ARM GCC 13.2.1 compile + size:

```text
variant       text  data  bss
NORMAL           0     0    0
DIAGNOSTIC    1206     0 11292
```

Końcowy canonical NORMAL build i symbol check są wymagane przed jazdą.

## 16. Test matrix

| ID | Dowód | Właściciel |
|---|---|---|
| T1 | CASE A trigger | istniejący RNA host |
| T2 | CASE B PWM off | istniejący RNA host |
| T3 | CASE C low magnitude | istniejący RNA host |
| T4 | normal drive no false A/B/C | RNA host magnitude-normal |
| T5 | +500/-500 no C | RNA host + Analyzer |
| T6 | +500/-20 C | RNA host + Analyzer |
| T7 | PWM=1/MOE=0 zachowane | RNA host + v2 oracle |
| T8 | current-cal blocked zachowane i B | RNA host + Analyzer |
| T9 | real C serializer → literal bytes | RNA host |
| T10 | literal bytes → PowerShell → exact CSV | Analyzer adapter tests |
| T11 | PRE100/POST156 i 4 ms spacing | RNA host |
| T12 | header schema/count/bytes/rate/confirm | C literal oracle + decoder validation |
| T13 | 44 declared bytes wysłane, padding jawny | offset asserts + serializer oracle |
| T14 | Analyzer consumes final CSV/evidence/subreason | Analyzer tests |
| T15 | NORMAL zero recorder symbol/size | ARM object + canonical NORMAL build |

Test persistence 8x250 Hz jest dodatkowym dowodem, nie osobnym dublującym harness. Wszystkie zachowania recordera są skupione w istniejącym `rolling_no_assist_diag_host.c`; decoder/Analyzer pozostają w istniejącym suite.

## 17. Canonical build artifacts

Do finalizacji, bez ręcznego `-Version`:

```powershell
.\scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant normal
.\scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant diagnostic
```

- NORMAL: `.build/M820_BL820/0.0417_M820_BL820.bin`, version `0.0417`, PASS,
  FLASH 100140 B, RAM 12032 B. Raw FLASH i RAM są identyczne z baseline 0.0415.
- DIAGNOSTIC: `.build/M820_BL820/0.0418_M820_BL820_DIAG.bin`, version `0.0418`,
  PASS, FLASH 143120 B, RAM 41552 B.
- DIAGNOSTIC SHA256: `DD994A6596BFDE44C6CB0E04E3F887C3BA3C7CAA0DA2D84325D1FF3EA9ECFDE5`.

## 18. Warunek jazdy

Flashować wolno wyłącznie końcowy DIAGNOSTIC artifact wskazany powyżej po PASS host, decoder/oracle, Analyzer regressions, NORMAL build, DIAG build i RAM/flash limits. Następny etap to capture → decoder → Trace Analyzer → FIRST DIVERGENCE. Ta karta nie naprawia hipotezy rotor/Hall ani żadnej innej przyczyny motor-control.

---

## 19. FW-122.1 — schema v3, D2 (redemand-during-soft-cutoff) evidence

**Data:** 2026-08-24. **Status:** wyłącznie instrumentacja diagnostyczna — **zero zmian w
motor-control, START/STOP, FOC, PI, current_cal, Hall/rotor, ride_control**. Zakres:
`inc/rolling_no_assist_diag.h`, `src/rolling_no_assist_diag.c`, snapshot w `src/main.c` (tylko
dodanie odczytu dwóch już istniejących zmiennych — `pwm_cutoff_active`, `pwm_cutoff_tick` —
oraz `uint16_half_rotation_counter` do lokalnego snapshotu, bez dotykania logiki
start/stop-u), `tools/decode_rolling_no_assist.ps1`, `tools/trace_analyzer_lib/core.py`.

### 19.1 Powód karty

Audyt FW-124 (`documentation/FW-124_START_STOP_LIFECYCLE_AUDIT_PL.md`, §6/§13, finding **D2**)
wykazał w kodzie realną, wąską lukę czasową: jeśli nowy dodatni `MS.i_q_setpoint` pojawi się w
trakcie ~10 ms okna `pwm_cutoff_active` (miękkie wygaszanie po ~1 s bezruchu wirnika), warunek
startu `!ui_8_PWM_ON_Flag` w `main.c:1293` jest prawdziwy mimo że hardware MOE nigdy nie zgasło
— sterownik anuluje trwające wygaszanie i wchodzi w pełną sekwencję startu. Schema v2 nie miała
na wire ani `pwm_cutoff_active`, ani postępu tego okna — próbka złapana w tym momencie wyglądała
identycznie jak zwykłe „PWM=0, oczekujemy startu", bez możliwości odróżnienia od prawdziwego
zatrzymanego mostka. Ta karta usuwa tę niejednoznaczność, **przed** jazdą testową szukającą
sporadycznego rolling no-assist.

### 19.2 Co się zmieniło na wire — schema v2 → v3

**CAN cost: ZERO.** Liczba ramek na próbkę (1 HEADER + 6 DATA = 7), EFID-y i liczba bajtów w
każdej ramce (8 B) są identyczne jak w v2. Dwa z czterech bajtów jawnego zero-paddingu we
fragmencie DATA 5 (offsety logiczne 44/45) stają się realnymi polami:

| Pole | Offset | Typ | Znaczenie |
|---|---:|---|---|
| `pwm_cutoff_progress` | 44 | u8 | `min(pwm_cutoff_tick, 255)` — takt licznika miękkiego wygaszania (produkcyjnie 0..40) |
| `hall_timeout_progress` | 45 | u8 | `min(uint16_half_rotation_counter >> 4, 255)` — postęp do progu ~1 s bezruchu (produkcyjnie próg 4000 tick ≫4 = 250) |

Nowy bit w `status_flags` (offset 5, bit 0x08): `RNA_STATUS_PWM_CUTOFF_ACTIVE` — surowy odczyt
zmiennej `pwm_cutoff_active` z `src/main.c`, bez żadnej interpretacji.

`ROLLING_NO_ASSIST_DIAG_SCHEMA_VERSION`: 2 → 3. `ROLLING_NO_ASSIST_SAMPLE_WIRE_BYTES`: 44 → 48
(oba w pełni zdeklarowane pola struktury, zero ukrytego paddingu kompilatora — offsety
zabezpieczone `_Static_assert`; pozostałe 2 B (offsety 46/47) to jawny, zerowany rezerwowy
padding). Dekoder waliduje schema=3 metadata (fragments=6, bytes=48, Hz=250, confirm=8) i
zeruje-sprawdza tylko bajty 46/47 (nie 44..47 jak w v2).

### 19.3 D2 NIE jest nową klasą CASE

`classify_trigger()` w `rolling_no_assist_diag.c` **nie został zmieniony**. Próbka z
`pwm_cutoff_active=1` i `final_iq>0` już dziś spełnia istniejący warunek CASE B
(`!pwm_on`) — bo `pwm_cutoff_active` implikuje `ui_8_PWM_ON_Flag=0` w `main.c`. D2 jest
**podklasą dowodową** (subreason) CASE B, liczoną w całości **poza sterownikiem**, w Trace
Analyzerze (`_bridge_failure_subreason()` w `tools/trace_analyzer_lib/core.py`):

- `REDEMAND_DURING_SOFT_CUTOFF` — próbka ma `pwm_cutoff_active=1` (bezpośredni fakt z wire).
- `CUTOFF_ABORTED_BY_REDEMAND` — dwie kolejne próbki pokazują `pwm_cutoff_active` 1→0 przy
  `moe` nadal 1 i `final_iq>0` — obserwowane przejście, nie zgadywane; przy próbkowaniu 250 Hz
  a oknie ~10 ms może nie złapać każdego takiego przejścia (fałszywy negatyw możliwy, fałszywy
  pozytyw — nie, bo wymaga dwóch realnych, kolejnych próbek).

Priorytet w `_bridge_failure_subreason()`: `CURRENT_CAL_FOC_NOT_ALLOWED` →
`REDEMAND_DURING_SOFT_CUTOFF` → `CUTOFF_ABORTED_BY_REDEMAND` → `PWM_ON_NOT_SET` →
`HARDWARE_MOE_NOT_SET` → `BRIDGE_LIFECYCLE_BEFORE_MOE` → fallback. D2-podklasy stoją **przed**
generycznym `PWM_ON_NOT_SET`, bo są ściślejszym, bardziej informacyjnym opisem tego samego,
już-prawdziwego faktu.

### 19.4 Testy

- **Host (C, real module)**: `tests/host/rolling_no_assist_diag_host.c` — 3 nowe testy
  (`test_case_b_redemand_during_soft_cutoff`, `test_case_b_redemand_after_full_stop`,
  `test_progress_bytes_saturate_not_wrap`) plus rozszerzenie istniejącego CASE C testu o
  asercję „nowe pola nie wpływają na CASE C". Literal-byte oracle (`test_real_c_serializer_
  literal_oracle`) przeliczony na schema v3 realnym serializerem (nie ręcznie). **38/38+3
  = wszystkie zestawy `run-host-tests.ps1` PASS**, w tym pełny transport 256×7 ramek → real
  decoder → strict validation.
- **Decoder**: nowa literalna fixtura `tests/traces/adapters/rolling_schema3_c_oracle.log`
  (3 próbki: „on", „blocked", „d2" — wygenerowane realnym `rolling_no_assist_diag_encode_
  fragment()`, nie ręcznie liczone), zdekodowana bezpośrednio `decode_rolling_no_assist.ps1`
  i zweryfikowana ręcznie (`pwm_cutoff_active`/`progress`/`hall_timeout_progress` poprawne dla
  wszystkich trzech próbek).
- **Trace Analyzer**: `tests/trace_analyzer/test_trace_analyzer.py` — 3 nowe testy
  (dekoder-vs-serializer roundtrip z polami v3, CASE B z subreasonem
  `REDEMAND_DURING_SOFT_CUTOFF`, transition marker `CUTOFF_ABORTED_BY_REDEMAND`). **Nie
  uruchomione lokalnie — na tej maszynie nie ma zainstalowanego interpretera Python.** Zmiany
  w `core.py` sprawdzone ręcznie linia po linii i zweryfikowane pośrednio: cała logika
  wejściowa (nazwy pól, aliasy, CSV) potwierdzona bezpośrednim uruchomieniem
  `decode_rolling_no_assist.ps1` na nowej fixturze (wynik wyżej), więc dane wejściowe do
  Analyzera są potwierdzone poprawne; sama logika Pythona (`_bridge_failure_subreason`,
  `_cutoff_aborted_by_redemand`) pozostaje **niewykonana w tej sesji — do uruchomienia przy
  najbliższej okazji z dostępnym Pythonem, PRZED poleganiem na jej wyniku na realnym logu.**

### 19.5 RAM/flash — ZMIERZONE

`arm-none-eabi-size` na izolowanym obiekcie `src_rolling_no_assist_diag.c.o`:

| Build | text | data | bss |
|---|---:|---:|---:|
| DIAGNOSTIC (schema v3) | 2520 B | 0 B | **12316 B** |
| NORMAL (`ROLLING_NO_ASSIST_DIAG_ENABLE=0`) | 0 B | 0 B | **0 B** |

`12316 B` = dokładnie `256 × 48 B` (ring, 12288 B) `+ 28 B` (reszta stanu `R`) — zgodne co do
bajtu z przewidywaniem sprzed pomiaru. `inc/diag_budget.h`:
`DIAG_BUDGET_ROLLING_NO_ASSIST_DIAG_BYTES` 11356 → **12380 B** (12316 B zmierzone + 64 B
headroom). NORMAL nadal kompiluje cały moduł do zera — potwierdzone tym samym pomiarem, nie
założeniem.

### 19.6 Build

Canonical, bez ręcznego `-Version`:

```powershell
.\scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant normal
.\scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant diagnostic
```

- **NORMAL**: `.build/M820_BL820/0.0421_M820_BL820.bin`, wersja `0.0421`, **PASS**, FLASH
  100140 B, RAM 12032 B. **Bit-identyczne** z bazą sprzed FW-122.1 (0.0417: FLASH 100140 B,
  RAM 12032 B) — potwierdzone realnym buildem, nie tylko `#if`.
- **DIAGNOSTIC**: `.build/M820_BL820/0.0422_M820_BL820_DIAG.bin`, wersja `0.0422`, **PASS**,
  FLASH 143836 B (+716 B vs 0.0418: 143120 B), RAM 42592 B (+1040 B vs 0.0418: 41552 B).
  Oba w granicach budżetu 34 KiB DIAG i 48 KiB SRAM sterownika.
- **DIAGNOSTIC SHA256**: `EA7772E6C5BF8A56901DC283CD4DA0C22CBE4836109ACDEE95E482CC34407FD0`.
- Working tree: `dirty` (oczekiwane — ta karta jeszcze nie jest zacommitowana).

Wszystkie 38+ zestawów `tests/host/run-host-tests.ps1` **PASS**, w tym pełny transport 256×7
ramek przez realny dekoder PowerShell (`Transport PASS: 256 samples, 1792 frames, indices
0..255 continuous, no missing fragments`). `tests/trace_analyzer/test_trace_analyzer.py`
rozszerzony o 3 nowe testy — **NIE uruchomione w tej sesji** (brak Pythona na tej maszynie);
logika wejściowa (CSV z nowymi polami) zweryfikowana bezpośrednim uruchomieniem dekodera na
nowej fixturze `rolling_schema3_c_oracle.log`, wynik poprawny — patrz §19.4.
