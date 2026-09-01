# QS-3D-R2 - finalna naprawa zrodel finalnego Iq slew 16 kHz

## Status i zakres

- Baseline: `4422f3b25cc429123aa2c5e31520da8112724693` (QS-3C canonical).
- Worktree: `C:\Users\mariuszg\AppData\Local\Temp\opencode\qs3d`, branch `qs3d`.
- Status: **QS-3D-R2 IMPLEMENTED / UNCOMMITTED / EXTERNAL A.PASS / READY TO COMMIT**.
- Ten dokument jest finalizowany przed selektywnym commitem QS-3D; canonical pozostaje poza
  zakresem tego worktree do czasu integracji z glowna linia projektu.
- Hardware: nie jest wymagany przez R1. QS-3E i tuning G532 pozostaja zablokowane.

R2 naprawia architekture bez zmiany map, PI Kp/Ki, MOE policy, FW117, battery-cap
equation/hysteresis ani obecnych EVistDrive czasow ramp. Normalny release pozostaje ok. 650 ms,
a safety release ok. 200 ms.

## Historia: pierwsza proba QS-3D i zewnetrzny STOP

Pierwsza, niecommitowana proba QS-3D przeniosla akumulator Q10 do 16 kHz, lecz niezalezny audit
wydal werdykt **C. STOP**. Audit wykazal:

1. bitfield union byl wiekszy niz raportowany 64-bit payload, a ARM GCC przesuwal zapisy payload
   przed publikacje nieparzystego `seq`;
2. po trzech nieudanych odczytach ISR wykonywal ostatni, niesprawdzony snapshot;
3. `fast_iq_slew_tick()` i `motor_core_set_command()` konkurowaly o `MS.i_q_setpoint`;
4. zmiana rate przy tym samym target/mode byla ignorowana;
5. release byl liczony z `slew_last_published_target`, nie z live Q10 state;
6. `FIS_MODE_SAFETY` nie byl publikowany;
7. sticky `zero_reached` nie byl wlaczony do lifecycle i startowal jako true;
8. reset byl tylko w `ride_control_init()`, mimo twierdzenia o cold PREPARE;
9. testy nie symulowaly przerwania producenta;
10. dokument zawieral niepoparte PASS claims.

R1 traktuje ten STOP jako source of truth. Stary mailbox, release latch, competing raw write i
sticky zero zostaly usuniete; nie sa alternatywnymi sciezkami ani fallbackiem.

## Chronologia auditow i dlaczego stare PASS nie jest dowodem

1. Pierwsza proba QS-3D przeniosla slew do 16 kHz, ale pozostawila niespojny mailbox i
   konkurujacy writer Iq.
2. Pierwszy external audit wydal **C. STOP** i opisal dziesiec defektow wymienionych wyzej.
3. QS-3D-R1 naprawil mailbox, ownership, adaptive command identity, live release, SAFETY,
   ARMED_ZERO i cold reset.
4. Po R1 wewnetrzny raport podal **A. PASS** na podstawie testow host/ARM/debug build.
5. Pozniejszy external source audit wydal **B. PARTIAL**. Ten nowszy audit uniewaznil R1
   A.PASS jako aktualny dowod dla dwoch konkretnych sciezek zrodlowych.
6. Defekt kierunku: dodatni target byl utozsamiany ze wzrostem. Trajektoria live 600 -> target
   250 dostawala krok UP zamiast DOWN.
7. Defekt release: reciprocal estimate mogl byc o jeden ponizej floor quotient; pojedyncze
   dodanie za niezerowa reszte nie zawsze dawalo exact ceil dla fractional Q10.
8. QS-3D-R2 wybiera RISE/FALL/HOLD przez porownanie target Q10 z autorytatywnym live
   `fis.accumulator_q10` i poprawia quotient przed koncowym ceil.
9. R2 dodaje testy przez realne `ride_control_update()` dla targetow i limiterow oraz exhaustive
   practical sweep kazdego live Q10 od 1 do 716800 dla normal/safety release.
10. Po R2 ponownie wykonano ARM GCC 13.2.1 assembly proof, focused tests, wymagane regresje i
    development/debug build.

Historyczne twierdzenie **R1 A.PASS** pozostaje zapisem chronologii, ale NIE jest uzywane jako
current evidence. Aktualny wynik R2 opiera sie na ponownym wykonaniu testow i inspekcji
rzeczywistego zrodla po obu poprawkach.

## Finalna architektura

```text
4 kHz ride/control
  -> demand + permission + assist limits
  -> battery_iq_cap (upstream Iq cap)
  -> Iq_allowed + complete fast command
  -> bounded coherent mailbox
  -> fast_iq_slew_tick() @ 16 kHz       [ONE dynamic final owner]
  -> MS.i_q_setpoint
  -> PI_iq.setpoint = sign * MS.i_q_setpoint
  -> PI_iq.recent_value = MS.i_q
```

`assist_dynamics_apply()` nie jest aktywnym finalnym ownerem. `battery_iq_cap_update()` i
`iq_chain_note_allowed()` wystepuja przed normalna publikacja. Nie istnieje post-slew battery
clamp ani przelaczenie PI do domeny pradu baterii.

## Mailbox R1

Mailbox ma szesc jawnych, naturalnie wyrownanych slow 32-bit:

```c
typedef struct {
    volatile uint32_t seq;
    volatile int32_t  target;
    volatile uint32_t step_mag_8;
    volatile uint32_t mode;
    volatile uint32_t release_ticks_16k;
    volatile uint32_t release_recip_q32;
} fast_iq_slew_mailbox_t;
```

Nie ma bitfield, packingu ani zalozenia o atomowym LDRD/STRD. Cortex-M4 atomowo czyta/zapisuje
kazde wyrownane slowo 32-bit. Protokol jest bounded single-producer/single-consumer seqlock:

- producer: `seq=odd`; `DMB`; wszystkie payload words; `DMB`; `seq=even`; `DMB`;
- consumer: `seq1`; `DMB`; wszystkie payload words do lokalnego candidate; `DMB`; `seq2`;
- candidate jest akceptowany tylko gdy `seq1 == seq2` i oba oznaczaja stabilne even generation;
- maksymalnie trzy proby; po ich wyczerpaniu pozostaje poprzedni `fis.command`;
- niesprawdzony candidate nigdy nie jest kopiowany do aktywnego command.

`volatile` zapewnia rzeczywiste word accesses, ale nie jest traktowane jako bariera. Ordering
zapewnia ARM `dmb 0xF` oraz compiler `memory` clobber w inline assembly.

### ARM GCC assembly proof

Sprawdzono Arm GNU Toolchain `arm-none-eabi-gcc 13.2.1`, Cortex-M4/Thumb, `-O2`.

Publisher ma w tej kolejnosci:

```text
str seq_odd, [mb]
dmb 0xF
str target, [mb,#4]
str step, [mb,#8]
str mode, [mb,#12]
str release_ticks, [mb,#16]
str release_recip, [mb,#20]
dmb 0xF
str seq_even, [mb]
dmb 0xF
```

Reader wykonuje `ldr seq1`, `dmb`, piec payload `ldr`, `dmb`, `ldr seq2`, porownanie oraz
dokladnie trzy bounded attempts. Assembly fast tick nie zawiera `udiv`, `sdiv` ani wywolania
divide helper. Live release uzywa `umull`, `mls`, shift i dwuetapowa remainder correction.
`__aeabi_uldivmod` wystepuje tylko w 4-kHz publisherze podczas przygotowania reciprocal.

## Command identity, kierunek z live Q10 i exact-ceil release

Tozsamosc command obejmuje wszystkie pola zachowania: target, step/rate, mode, release duration
i reciprocal. Zmiana step przy niezmienionym target/mode natychmiast odswieza signed rate;
testy pokrywaja SLOW->FAST i FAST->SLOW.

R2 nie okresla kierunku ze znaku targetu. Producent pobiera jeden wyrownany 32-bit snapshot
`fast_iq_slew_current_accumulator_q10()`; jest to autorytatywny akumulator finalnego ownera.
Porownanie odbywa sie w Q10:

```text
target_q10 > live_accumulator_q10  -> FIS_MODE_RISE  + skonfigurowany up step
target_q10 < live_accumulator_q10  -> FIS_MODE_FALL  + skonfigurowany down step
target_q10 == live_accumulator_q10 -> FIS_MODE_HOLD  + rate 0
```

ISR przy akceptacji command ponownie ustala znak rate z dokladnej relacji Q10. Nie uzywa
previous target, requested target, published target ani target sign.

RISE/FALL zachowuja Q10: stary per-control Q8 step `S` jest dodawany do Q10 co tick 16 kHz.
Po czterech tickach zintegrowany przyrost jest dokladnie jednym starym krokiem 4 kHz. Przyklad
`+7 count / 250 us` daje dodatnie intermediate samples wewnatrz kwartalu i dokladnie `+7` na
granicy 4 kHz; nie ma floor(7/4).

Przy wejsciu w RELEASE lub SAFETY ISR liczy rate z aktualnego `fis.accumulator_q10`, nie z
requested/published target. Publisher przekazuje duration oraz
`floor(2^32 / duration_ticks)`. R2 uwzglednia, ze multiply-high estimate moze byc o jeden
ponizej `floor(live/ticks)`: jesli `remainder >= ticks`, najpierw zwieksza quotient i odejmuje
ticks od remainder, a dopiero potem zwieksza quotient za pozostala niezerowa reszte. Wynik jest
dokladnie `ceil(live_accumulator_q10 / duration_ticks)` dla kazdego praktycznego live Q10.
Obliczenie wystepuje tylko na command edge, bez dzielenia per cycle.

Zmierzona deterministyczna symulacja 16 kHz:

| Scenariusz | Wynik |
|---|---:|
| normal 700 settled -> 0 | 10381 ticks = 648.8125 ms |
| normal interrupted rise (live ok. 264) -> 0 | 10365 ticks = 647.8125 ms |
| normal interrupted fall (live ok. 595) -> 0 | 10310 ticks = 644.375 ms |
| normal immediately after cap change | 10215 ticks = 638.4375 ms |
| safety 700 settled -> 0 | 3198 ticks = 199.875 ms |
| safety during rise | 3128 ticks = 195.5 ms |

Roznice sa oczekiwanym Q10/round-half-up quantization obecnego EVist behavior, nie retuningiem.

## Iq ownership i klasyfikacja wszystkich sciezek

Wszystkie produkcyjne call sites starego `motor_core_set_command()` zostaly przejrzane. Generic
dynamic Iq API usunieto; Motor Core udostepnia tylko `motor_core_set_id_target()`.

| Klasa | R1 ownership |
|---|---|
| NORMAL RIDE | publish RISE/FALL/RELEASE/SAFETY; tylko fast tick zapisuje dynamic Iq |
| WALK ASSIST | `FIS_MODE_BYPASS`, nadal zapis przez fast tick |
| SERVICE position calibration | `FIS_MODE_BYPASS`; Id osobno przez Motor Core |
| COMM LOSS | `FIS_MODE_FORCE_ZERO` przez mailbox |
| CALIBRATION open-loop phase | mutually-exclusive bridge service; cold exact-zero fast state, Id=200 |
| HARD FAULT overcurrent | niezalezny hardware MOE off w `FOC.c`, bez opoznienia przez slew |
| STARTUP | jedyny bezposredni write to boot-only `state->i_q_setpoint = 0` w `motor_core_init()` |
| COLD PREPARE | `fast_iq_slew_cold_prepare()` zeruje mailbox/state/output przy bridge off |

Normalni dynamiczni writerzy `MS.i_q_setpoint`: **dokladnie 1**, `fast_iq_slew_tick()` przez
`&MS.i_q_setpoint`. Nie ma tymczasowego service/comm raw write, ktory kolejny ISR nadpisuje.

## ARMED_ZERO i cold PREPARE

Lifecycle uzywa bezposredniej obserwacji finalnego fast output (option A):

- `RUN -> ARMED_ZERO` dopiero gdy `MS.i_q_setpoint == 0`;
- zero requested nie zmienia lifecycle, dopoki live slew jest dodatni;
- `ARMED_ZERO -> RUN` dopiero gdy finalny fast output staje sie dodatni.

Sticky acknowledgement usunieto. Nie istnieje reset-time `zero_reached=true`, ktore mogloby
potwierdzic pozniejszy request.

Cold gate przy bridge off obserwuje `ride_control_final_iq_requested()`, bo fast ISR nie moze
narastac przy wylaczonym bridge. W autorytatywnej sekwencji tuz przed MOE ON wywolanie
`fast_iq_slew_cold_prepare()` zeruje target, mode, rate inputs, last verified command, Q10
fraction i `MS.i_q_setpoint`. Swiezy publish podczas istniejacego neutral dwell dostarcza demand.
Nie dodano dwell i nie zmieniono theta seed, PI reset, neutral CCR, current validation ani FW117.

## Testy R1 ponowione oraz nowe testy R2

Focused `qs3d_16khz_slew_host.c` uzywa realnego `fast_iq_slew.c` i pokrywa co najmniej:

1. stable rise old-vs-new parity; 2. stable fall; 3. small target; 4. max target;
5. same target SLOW->FAST; 6. FAST->SLOW; 7. reversal; 8. limiter tightening;
9. limiter release; 10-13. settled/interrupted/cap normal releases; 14-16. safety releases;
17. exact zero; 18-20. ACTIVE/ARMED_ZERO/restart; 21. cold reset; 22. service;
23. comm loss; 24. calibration transition; 25. hard fault; 26. retry exhaustion;
27. sequence wrap.

Test-only hook wywoluje realnego consumera: przed update, po `seq=odd`, po kazdym z pieciu
payload stores, bezposrednio przed `seq=even` i bezposrednio po nim. Wynik jest zawsze stary
complete command albo nowy complete command; mixed command = 0. Osobny test utrzymuje odd seq
przez wszystkie trzy proby i potwierdza last verified retention.

- Focused QS-3D-R2 (wraz z ponowieniem R1): PASS.
- Old-vs-new stable 4-kHz boundary equivalence: PASS, max difference 0 count.
- Q10 fractional progression: PASS.
- Migrated ride-control, RUN recovery, throttle safety and state-hygiene harnesses clock real
  fast tick 4x na control period.
- Wszystkie relevant QS-3/QS-3C/FW-126.7/FW-128/FW-129 i lifecycle regressions: PASS.
- Pelny runner ma jeden znany, niezalezny baseline failure:
  `rolling_no_assist_diag_host` oczekuje schema 3, source ma schema 4 (514 T9/T14 failures).
  Pliki tego diagnostycznego mismatch nie zostaly zmienione przez QS-3D-R2.

Nowe dowody R2:

- real production generation (`ride_control_update`, nie reczne `FIS_MODE_*`):
  live 600 -> 250 FALL/down step Q8=53; live 250 -> 600 RISE/up step Q8=77;
  600->599 FALL; 599->600 RISE; 250->250 HOLD;
- real `battery_iq_cap`: 700->250 FALL i cap release 250->700 RISE;
- real temperature limiter target 395: live 600->395 FALL i live 250->395 RISE;
- phase/current ceiling: 700->250 FALL i 250->700 RISE;
- target reversal during active rise/fall wybiera nowy kierunek natychmiast;
- exhaustive practical exact-ceil sweep: wszystkie stany Q10 1..716800 dla 10400 i 3200
  release ticks; zero mismatch;
- boundary/odd/non-divisible/large release simulations: exact zero, brak negative overshoot,
  overflow i dodatkowego ticku;
- normal i SAFETY release ponownie pokrywaja przerwana rise, fall i limiter change;
- mailbox interruption/retry, adaptive rate, one-owner, ARMED_ZERO, cold reset i special
  ownership ponownie PASS;
- pelny runner nadal konczy sie tylko znanym niezaleznym schema mismatch (514 failures), wiec
  complete suite nie jest nazywany PASS; wszystkie wymagane relevant regressions sa PASS.

## Debug build

Build wykonano bez alokowania canonical:

```text
scripts/build-firmware.ps1
  -Target M820_BL820 -Profile debug -Variant normal
  -Version qs3d-r2-debug -OutputDir .build\qs3d-r2-review
```

- Toolchain: ARM GCC 13.2.1.
- Result: PASS.
- FLASH: 106432 B (45.19% of application region; image end 0x0801EFC0).
- RAM: 12632 B (25.70%).
- SHA256: `8BFF9D07399DDA670F79013B4B1CF8BE566EFE0D645103E07FDC464386EB04AB`.
- Warningi signedness w `CAN_Display.c` i unused `fw_ver` sa przedistniejace.
- Production files nie sa staged; commit i canonical nie powstaly.

### Rozroznienie assembly i retained debug artifact

Inspekcja assembly mailboxa i fast tick byla jednorazowym checkiem kompilacji **`-O2`**;
artefakt `.s` nie jest zachowywany jako release artifact. Zachowany debug build jest osobnym
artefaktem **`-O0 -g3`**. Nie nalezy utozsamiac debug BIN z O2 proof ani twierdzic, ze
zachowano osobny O2 artifact.

## Finalny stan i next action

QS-3D-R2 spelnia acceptance criteria source/host/static/ARM/debug-build. Pozniejszy external
review wydal **A. PASS** i zezwolil na selektywny commit oraz integracje Git do glownej linii.
QS-3E pozostaje **NO** do czasu osobnego controlled bike A/B review.

**NEXT ACTION:** selective QS-3D commit, Git integration to the main development tree, canonical
NORMAL build and controlled bike A/B test. Bez tuningu G532 w tej karcie.
