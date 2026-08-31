# QS-3 — G532-like final Iq slew parity: porównanie źródłowe i minimalna poprawka

## 1. Baseline i werdykt

- **Baseline:** `aaff688d16e710b2273ffdcedaa4b689e6e62725` (`QS-2: persist ARMED_ZERO lifecycle`).
- **Zakres:** wyłącznie końcowy tor `Iq_allowed -> final ramp -> Iq_ref/MS.i_q_setpoint -> PI_iq`.
- **Werdykt:** **B / PARTIAL**. EVist ma jednego właściciela finalnego Iq, liniowe rampy z
  clampem i mały bezwzględny pierwszy krok, ale przed QS-3 ich czas był zależny od liczby
  przetworzonych wywołań pętli głównej. To jest przypadek **D** z karty: naprawiona została
  wyłącznie baza czasu istniejącej rampy. Nie zmieniono jej strojenia ani architektury.
- **Nowy canonical NORMAL:** `0.0469` (build jest oznaczony `worktree_dirty: true`, bo zawiera
  niezależne, niestagedowane prace SOC obecne przed QS-3; nie jest czystym artefaktem tylko QS-3).

## 2. Odzyskany punkt odniesienia G532

Fabryczny G532 ma rampę Q wewnątrz szybkiej pętli FOC:

| Wielkość | G532 |
|---|---:|
| FOC | 16 000 Hz |
| krok Q narastania / opadania | 10 count/cykl |
| domyślny raw vector-current limit | 55 |
| promień wewnętrzny | `floor(55*16384/100) = 9011` |
| 0→100%, 100→0 | `ceil(9011/10)/16000 = 56.375 ms` |
| pierwszy krok | `10/9011 = 0.111%` |
| średnie pełnoskalowe tempo | `100/56.375 = 1.774%/ms` |
| 50→0 / 20→0 | ok. 28.2 / 11.3 ms |

To jest stałe liniowe `dQ/dt`; `56.375 ms` jest wynikiem skali, kroku i 16 kHz, a nie
literalnym parametrem czasowym do skopiowania do EVist.

## 3. Aktualny tor EVist i własność

Normalny tor ma jedną własność końcowej rampy:

```text
ride_control.c: iq_target po limiterach
  -> iq_chain_note_allowed(iq_target)
  -> assist_dynamics_apply(iq_target, MS.i_q_setpoint, dynamics_input)
  -> motor_core_set_command(): MS.i_q_setpoint = command.iq_target
  -> main.c: PI_iq.setpoint = reverse * i8_reverse_flag * MS.i_q_setpoint
```

- `src/ride_control.c:1044-1053` zapisuje obserwowane `Iq_allowed` bez tworzenia drugiego
  stanu i wywołuje jedyny finalny ramp-owner.
- `src/assist_dynamics.c:39-47,169-210` przechowuje jedyny stan `iq_reference_q` (Q8) i
  `profile_release_step_q`; wartość argumentu `iq_reference` nie jest drugim akumulatorem.
- `src/motor_core.c:13-23` jest jedynym normalnym zapisem `MS.i_q_setpoint`.
- `src/main.c:3655-3657` przekazuje ten Iq_ref do PI. `FOC_calculation()` dostaje ten sam
  setpoint w ISR (`main.c:3989-3992`). Zatem rampa działa **przed PI_iq**.

Istnieją jawne wyjątki, niebędące drugim normalnym ramp-ownerem:

| Ścieżka | Zachowanie | Status QS-3 |
|---|---|---|
| Walk Assist | `walk_active` zwraca własny `iq_target` i synchronizuje Q8 (`assist_dynamics.c:104-108`); WA ma własną regulację prędkości | bez zmian |
| zwolnienie WA | `immediate_cut` natychmiast zeruje Q8 (`:75-82`) | bez zmian |
| rolling fast-rearm / recovery WAIT | `force_zero_reference` natychmiast zeruje Q8 (`:91-98`) | bez zmian |
| coast | przy `coast_release && target==0` dokładne zero (`:146-150`) | bez zmian |
| kalibracja pozycji | gałąź serwisowa wysyła własny command bez ride-feel rampy | bez zmian |
| battery-current limiter | w `BC_limit_flag`, `main.c:3651` podmienia bezpośrednio wejście PI na battery-current setpoint | znany, poza zakresem i jawnie zamrożony przez kartę; nie jest drugim rider-level Iq ramp-ownerem |

## 4. Równania EVist

### Zwykły wzrost i spadek

`src/assist_dynamics.c` używa `CONTROL_TICKS_PER_MS=4`, Q8 i dla zwykłej rampy:

```text
t_rise/fall = selected_ramp_ms * 4
Iq_scale_q  = Iq_scale * 256
step_q      = ceil(Iq_scale_q / t_rise_or_fall)

state_q < target_q: state_q = min(target_q, state_q + step_q * elapsed_ticks)
state_q > target_q: state_q = max(target_q, state_q - step_q * elapsed_ticks)
Iq_ref = round(state_q / 256)
```

Wartość czasu jest adaptowana liniowo po speed (4.0→20.0 km/h) i cadence (50→110 rpm), a
wybierane jest krótsze `up_ticks`/`dn_ticks` (`assist_dynamics.c:119-143`). Stąd tempo jest
między kolumną SLOW i FAST. `min/max` oraz różnica `d` dają dokładny clamp, więc nie ma
overshoot ani ujemnego Iq przy dojściu do zera.

`Iq_scale` ma bieżącą skalę dozwolonego Iq:
`phase_current_max_scaled = MP.phase_current_max * assist_settings[level][current_limit]/100`
(`main.c:1198-1202`). Factory reset daje L1…L5 = 20/40/60/80/100% (`parser.c:337-343`),
czyli przy `PH_CURRENT_MAX=700`: 140/280/420/560/700. Zapisany Para1 i limp mode mogą tę
skalę zmienić; procentowe tempo niżej pozostaje zasadniczo niezależne od skali, poza Q8 i
integer rounding.

### Normalne puszczenie pedału

Gdy `target==0`, nie ma aktywnego pedałowania i `release_ms>0`, owner zatrzymuje wartość
startową i stosuje:

```text
t_release = release_ms * 4
release_step_q = ceil(state_q_at_release_start / t_release)
state_q = max(0, state_q - release_step_q * elapsed_ticks)
```

(`assist_dynamics.c:152-191`). To gwarantuje czas **od aktualnej amplitudy do zera** równy
`release_ms` (z Q8 rounding), nie stałe pełnoskalowe `dIq/dt`. Domyślnie aktywne poziomy mają
`release_ms=650` (`assist_modes.c:112-188`).

### Safety, coast i WA

- `ride_control.c:785-788`: brake/reverse/overtemp/torque-fault ustawia `target=0` oraz
  firmware-owned `RIDE_HARD_CUT_RAMP_MS=200`, nie levelowe `release_ms`.
- `ride_control.c:964-966`: przy końcu jazdy i ERPS poniżej granicy coast przekazuje
  `coast_release`; final owner daje exact zero.
- Walk Assist i jego dead-man release pozostają wymienionymi wyżej bypassami. Żaden z nich
  nie jest dołożonym przez QS-3 filtrem/rampą.

## 5. Normalizacja — factory default L1…L5

Poniższe liczby są dla całej dozwolonej skali `Iq_scale` i nominalnego 4 kHz. Q8 `ceil` może
zmienić ostatnie ułamki procenta. Pierwszy krok oznacza pierwszy **widoczny dodatni** całkowity
increment `MS.i_q_setpoint`: 1 count. Pierwsze wywołanie może jeszcze zwrócić 0, jeśli Q8 nie
przekroczyło progu zaokrąglenia.

| Level (factory Iq scale) | first visible Iq_ref | rise SLOW / FAST ms | rise %/ms SLOW / FAST | fall SLOW / FAST ms | fall %/ms SLOW / FAST | normal release |
|---|---:|---:|---:|---:|---:|---:|
| L1 (140) | 0.714% | 600 / 300 | 0.167 / 0.333 | 1000 / 180 | 0.100 / 0.556 | 650 ms |
| L2 (280) | 0.357% | 600 / 330 | 0.167 / 0.303 | 1000 / 210 | 0.100 / 0.476 | 650 ms |
| L3 (420) | 0.238% | 650 / 380 | 0.154 / 0.263 | 1050 / 250 | 0.095 / 0.400 | 650 ms |
| L4 (560) | 0.179% | 700 / 450 | 0.143 / 0.222 | 1100 / 300 | 0.091 / 0.333 | 650 ms |
| L5 (700) | 0.143% | 750 / 500 | 0.133 / 0.200 | 1200 / 350 | 0.083 / 0.286 | 650 ms |

Normal release, po znormalizowaniu do **całej** allowed scale, ma tempo zależne od amplitudy
startowej: przy starcie 100% wynosi `100/650 = 0.1538%/ms` i FSET 650 ms; przy 50% ma
`0.0769%/ms` i nadal kończy się po 650 ms (FSET 1300 ms); przy 20% `0.0308%/ms`, nadal
650 ms (FSET 3250 ms). To świadoma, historycznie oddzielna semantyka rider release, a nie
G532-owe stałe dQ/dt.

## 6. Tabela porównawcza G532 ↔ EVist

| Właściwość | G532 | EVist po QS-3 |
|---|---|---|
| loop/timebase | FOC 16 kHz | TIMER1 `CONTROL_TIMEBASE_HZ=4000`, `control_time_ticks` |
| domena Iq | promień Q 9011 | Q8 `iq_reference_q`; output int Iq w skali allowed Iq |
| first step | 0.111% | 1 count: 0.143% L5; 0.179/0.238/0.357/0.714% L4…L1 |
| rise slope | 1.774%/ms | 0.133…0.333%/ms (factory extremes) |
| fall slope | 1.774%/ms | 0.083…0.556%/ms (factory extremes) |
| 0→100 equivalent | 56.375 ms | 300…750 ms (profile + speed/cadence) |
| 100→0 ordinary equivalent | 56.375 ms | 180…1200 ms (profile + speed/cadence) |
| 50→0 ordinary | 28.2 ms | połowa zwykłego full-scale time: 90…600 ms |
| 20→0 ordinary | 11.3 ms | 20% zwykłego full-scale time: 36…240 ms |
| normal rider release | stały dQ/dt | 650 ms do zera z każdej amplitudy (default) |
| target clamp / overshoot | bounded | exact clamp / brak overshoot |
| rise/fall osobno | tak, ten sam raw step | tak, osobne per-level SLOW/FAST |
| przed PI | tak | tak w normalnym PI path |
| deterministic timebase | FOC ISR | tak: elapsed `control_time_ticks` delta przekazane do istniejącego ownera |

Wniosek strojenia: EVist nie jest liczbową kopią G532. Jest celowo 3–13× łagodniejszy na
zwykłym wzroście oraz 3–14× łagodniejszy na opadaniu; normal release jest jeszcze wolniejszy
przy małej amplitudzie. To wspiera quiet drivetrain. QS-3 nie kopiuje `10 count/cycle` ani nie
ustawia 56.4 ms, ponieważ oba byłyby błędne w innej domenie Iq i zwiększyłyby bezwzględny
pierwszy krok na aktualnym hardware.

## 7. Timebase i poprawka produkcyjna

Przed QS-3 `assist_dynamics.c` liczył nominalne ticki `ms*4`, ale `ride_control_update()`
wykonywało się z `reg_ADC_flag`, jednego bitu ustawianego przez `TIMER1_IRQHandler`.
`control_time_ticks` zawsze rośnie w ISR (`main.c:2025-2034`), lecz kolejne ustawienie flagi
przed jej skonsumowaniem jest tracone. `main.c:2315-2347` już mierzył `control_delta` i
`missed_control_ticks`, lecz finalna rampa go nie używała. Fizyczny czas rampy mógł więc się
rozciągać wraz z liczbą zgubionych przetworzeń — przypadek D.

Minimalna zmiana QS-3:

1. `main.c` inicjalizuje `control_delta=1`, a po uruchomieniu monitora wylicza
   `control_now-control_prev_processed_tick`; przekazuje go jako `ride_input.elapsed_ticks`.
2. `ride_control.c` tylko przenosi pole do `assist_dynamics_input_t`.
3. `assist_dynamics.c` mnoży **istniejący** wybrany `step_q` przez `elapsed_ticks`, z saturacją
   `INT32_MAX`, zanim wykona obecne `min/max` clampy. Zero od legacy/test caller oznacza jeden
   tick.

Nie powstał drugi owner ani ramp w FOC ISR. Wyjście może zostać zaktualizowane dopiero, gdy
pętla główna znów dostanie CPU — tego nie da się fizycznie odtworzyć bez przenoszenia całego
producenta demand do ISR, co byłoby poza kartą. Od chwili obsługi jest jednak postęp stanu
zgodny z rzeczywiście upływającymi tickami, zamiast nieoznaczonego wydłużenia czasu rampy.

## 8. Zamrożone elementy

- ARMED_ZERO, MOE, FOC/ADC/theta lifecycle: **bez zmian**.
- cold PREPARE, theta seed, FW117 i neutral dwell: **bez zmian**.
- PI Kp/Ki i normalny PI formula: **bez zmian**.
- battery-current limiter, protection, Walk Assist, coast policy oraz safety semantics:
  **bez zmian**. Tylko ich już istniejące release steps korzystają teraz z rzeczywistego
  elapsed-tick input finalnego ownera.

## 9. Testy

Dodano `tests/host/qs3_final_iq_slew_host.c`, wpisany do `tests/host/run-host-tests.ps1`.
Test używa prawdziwego `src/assist_dynamics.c` dla T1–T6 i source-wiring/model dla
niehostowalnego `main.c`:

1. 0→100%: mały pierwszy visible krok, monotoniczność, clamp, brak overshoot — PASS.
2. 100→0: monotoniczny spadek, zero, brak negative overshoot — PASS.
3. 20→0: czas proporcjonalny do amplitudy przy zwykłym stałym slope — PASS.
4. 50→100%: ta sama znormalizowana rampa up — PASS.
5. 100→50%: ta sama znormalizowana rampa down — PASS.
6. zmiana kierunku w trakcie: brak skoku stale accumulator — PASS.
7. ARMED_ZERO→positive: at-most-one-count Iq step; model + wiring bez MOE/PREPARE/FW117 — PASS.
8. normal target→zero→ARMED_ZERO: exact zero i utrzymane armed lifecycle — PASS.
9. hard 200 ms, coast exact zero oraz WA bypass: semantyka bez zmian — PASS.
10. cold start: jedyny modelowy PREPARE/FW117 oraz source link do normalnego PI input — PASS.

Dodatkowa właściwość QS-3: pojedyncze wywołanie z `elapsed_ticks=64` daje identyczny stan Q8,
jak 64 wywołania po jednym ticku.

Pełny `tests/host/run-host-tests.ps1`: **FAIL tylko 1 pre-existing unrelated suite**:
`rolling_no_assist_diag_host` (514 asercji). Jego literalny harness oczekuje schema 3, podczas
gdy baseline `aaff688` ma `ROLLING_NO_ASSIST_DIAG_SCHEMA_VERSION 4U`; QS-3 nie dotyka tej
serializacji ani nagłówka. Wszystkie testy QS-3, QS-2 ARMED_ZERO i kolejne suite’y przeszły.

## 10. Artefakt

- NORMAL debug: `0.0469_M820_BL820.bin`
- SHA-256: `8951B6997B8E4F2E536DAF9E81EFADE24293BD8C608D9D4CB15AA87E879B1564`
- FLASH: 105716 B; RAM: 12608 B; toolchain 13.2.1.
- Build PASS z istniejącymi warningami `CAN_Display.c` signedness oraz unused `fw_ver`.
- Manifest ma `git_commit=aaff688...` i `worktree_dirty=true`; na kod binarny wpływają także
  obecne w drzewie, niezależne zmiany SOC. Artefakt jest numerowany i sprawdzony, lecz nie
  stanowi izolowanego dowodu bitowego samego QS-3.

## 11. Różnice pozostające względem G532 i następna decyzja

1. EVist ma 4 kHz ride-level ramp (nie 16 kHz FOC) i o wiele wolniejszy, per-level tuning.
2. Przy małych factory current limits jeden widoczny count jest procentowo większy niż 0.111%
   G532, ale bezwzględnie pozostaje 1 Iq count; istniejące evidence restartu potwierdzało
   `first Iq_ref=1` i nie uzasadnia zwiększania kroku.
3. EVist normal rider release ma stały czas do zera, nie stałe dIq/dt G532.
4. Znany battery-current PI override pozostaje wyjątkiem normalnego PI path i poza zakresem
   tej karty; nie jest drugą rampą ride-level.

**Następna decyzja:** zachować obecne gentle slew po poprawce deterministycznej bazy czasu.
Nie ma uzasadnienia dla kopiowania 56.375 ms ani kroku 10; ewentualny przyszły tuning powinien
być osobną, świadomą decyzją hardware/ride-feel, a nie „parity” po surowych countach.
