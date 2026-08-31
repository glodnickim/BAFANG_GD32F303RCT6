# QS-3B — architektura finalnego Iq slew: 4 kHz kontra 16 kHz

## 1. Baseline commit

- Baseline: `e6ae12c8679b3d14639b99e07fb58e53fd1162a4` (`QS-3: make final Iq slew timebase deterministic`).
- Canonical NORMAL pozostaje `0.0469`. Ta karta nie zmienia kodu produkcyjnego i nie rezerwuje
  nowego numeru.
- QS-2 (`ARMED_ZERO`) i QS-3A (postęp rampy według upływu ticków TIMER1) są zamrożone.
- Drzewo zawiera niezależne prace SOC. Zakres QS-3B to ten raport i wpis w indeksie dokumentacji.

**Werdykt karty: B / PARTIAL, decyzja architektoniczna C — DO NOT IMPLEMENT YET.** Analiza
potwierdza, że mały liniowy tick w ISR byłby tani, ale aktualny kontrakt sterowania nie pozwala
jeszcze przenieść ownera bez naruszenia własności PI, same-tick zero i spójności publikacji.

## 2. Current ownership

### Tor normalnej jazdy

```text
rider_input / assist mode
  -> assist_modes_calculate()                         src/assist_modes.c:941-993
  -> ride_control local iq_target                    src/ride_control.c:558
  -> latch/floor/boost/hard-cut                       src/ride_control.c:650-789
  -> Iq_requested snapshot                           src/ride_control.c:862-864
  -> assist_limits_apply() pedal + throttle          src/ride_control.c:866-884
  -> smooth-start + preload + coast arbitration      src/ride_control.c:912-965
  -> Iq_allowed snapshot                             src/ride_control.c:1042-1044
  -> assist_dynamics_apply()                         src/ride_control.c:1046-1049
  -> motor_core_set_command()                        src/ride_control.c:1050-1056
  -> MS.i_q_setpoint                                 src/motor_core.c:10-23
  -> pi_iq_apply_inputs()                            src/main.c:3624-3657
  -> PI_control(&PI_iq)                              src/main.c:3660-3663
```

`Iq_requested` i `Iq_allowed` nie są osobnymi właścicielami sterowania. Są polami
obserwacyjnymi `static iq_chain_t chain` (`src/iq_chain.c:9-34`). Normalny producent obu to
`ride_control_update()`. ADC ISR odczytuje je wyłącznie do trace w
`src/main.c:4075-4082`. Na ścieżce WA `Iq_requested` nie jest odświeżane, a kalibracja pozycji
wraca przed oboma zapisami; tych snapshotów nie wolno użyć jako przyszłej skrzynki sterującej.

### Właściciele i obejścia

- **Normalna jazda ma dokładnie jednego finalnego slew ownera:** `assist_dynamics.c`, ze stanem
  `static int32_t iq_reference_q` i `profile_release_step_q` (`src/assist_dynamics.c:39-47`).
- **Firmware-wide są wzajemnie wykluczające się trajectory owners:** normalny
  `assist_dynamics` oraz Walk Assist `walk_speed_controller` (`src/walk_speed_controller.c:80-282`).
  Nie są ułożone szeregowo: przy `walk_active` assist dynamics synchronizuje Q8 i zwraca target
  bez drugiego rampu (`src/assist_dynamics.c:101-108`).
- Kalibracja pozycji ma jawny bezpośredni command i omija ride-feel ramp
  (`src/ride_control.c:345-375`). Utrata komunikacji zapisuje command zero bez rampy
  (`src/main.c:1332-1343`). `motor_core_set_command()` pozostaje jedynym miejscem zapisu
  `MS.i_q_setpoint` (`src/motor_core.c:17,22`), lecz ma wiele call sites.
- Argument `int16_i_q_target` przekazywany do `FOC_calculation()` jest w obecnej implementacji
  nieużywany. Rzeczywistym wejściem regulatora jest globalne `MS.i_q_setpoint` odczytane przez
  `pi_iq_apply_inputs()`.

**CURRENT FINAL IQ SLEW OWNERS:** `ONE` dla normalnej jazdy; `MORE THAN ONE` tylko przy liczeniu
wzajemnie wykluczającej się trajektorii WA. Nie istnieją dwie finalne rampy szeregowe.

**CURRENT PI_iq.setpoint WRITERS:**

1. init `PI_iq.setpoint = 0` — `src/main.c:983`;
2. runtime legacy battery-current override — `src/main.c:3651`;
3. runtime normal Iq_ref — `src/main.c:3657`.

Punkty 2 i 3 są dwoma przypisaniami w jednym `pi_iq_apply_inputs()`, ale reprezentują dwie
różne dziedziny fizyczne. Override omija `Iq_allowed`, finalny ramp i `MS.i_q_setpoint`.

## 3. Current 4-kHz architecture

`CONTROL_TIMEBASE_HZ=4000U` (`inc/config.h:153`). `TIMER1_IRQHandler()` inkrementuje
`volatile uint32_t control_time_ticks` i ustawia jednobitowe `reg_ADC_flag`
(`src/main.c:2025-2034`). Pętla główna wywołuje `reg_ADC_processing()`
(`src/main.c:1139`), które bierze snapshot czasu, oblicza `control_delta` i przekazuje go przez
`ride_control_input_t.elapsed_ticks` (`src/main.c:2307-2347,2974-3004`).

Finalny owner działa w processed main context. QS-3A mnoży wybrany krok Q8 przez rzeczywisty
`elapsed_ticks`, dlatego utracone/coalesced wywołania nie rozciągają czasu fizycznego
(`src/assist_dynamics.c:66-70,193-204`). Nadal jednak wynik publikuje się dopiero, gdy main
odzyska CPU; nie ma aktualizacji co 62,5 us.

Zwykły wzrost/spadek używa per-level czasów SLOW/FAST, wybieranych po prędkości i kadencji.
Normalny release używa osobnego wariantu tego samego ownera: latched amplitude dzielona przez
`release_ms*4`. Fabryczne `release_ms` aktywnych poziomów wynosi 650 ms.

## 4. G532 16-kHz architecture

Punkt odniesienia G532:

```text
upstream target/limits
  -> jeden Q slew w domenie FOC 16 kHz
  -> current PI
```

Krok 10 względem skali 9011 daje `10/9011 = 0,11098%` na cykl, około
`1,774% full-scale/ms` i 902 cykle, czyli 56,375 ms od 0 do pełnej skali. Jest to stałe
`dIq/dt`: 20%, 50% i 100% celu zajmuje proporcjonalnie około 11,3, 28,2 i 56,4 ms. Nie jest to
„każdy target osiągnij w 56,4 ms”.

EVist potwierdza 16 kHz źródłowo: TIMER0 jest center-aligned z `_T=3750`, więc
`120 MHz/(2*3750)=16 kHz` (`inc/config.h:19`, `src/main.c:1787-1794`), a injected ADC jest
wyzwalany przez TIMER0 CH3 (`src/main.c:1727-1735`).

## 5. Concurrency analysis

Aktualne konteksty:

| Kontekst | Funkcje / odpowiedzialność |
|---|---|
| TIMER1 4 kHz ISR | `TIMER1_IRQHandler`, hardware tick, flag main, PAS sample, battery-current sample (`main.c:2025-2075`) |
| processed main | `reg_ADC_processing`, rider snapshot, WA, limitery, `ride_control_update`, finalny 4-kHz slew (`main.c:2307-3580`) |
| ADC/FOC 16 kHz ISR | JDR, current calibration/reconstruction, Hall angle, FOC/PI/SVPWM, PWM/context publish, trace (`main.c:3853-4085`) |
| Hall ISR | `TIMER2_IRQHandler`, okres/ERPS, hall state, direction/theta (`main.c:2085-2200+`) |
| inne producenci Iq | comm-loss i service/calibration przez `motor_core_set_command`; WA przez processed main |

Wszystkie TIMER1, TIMER2 i ADC0_1 są skonfigurowane z tym samym priorytetem NVIC
(`src/main.c:1988-1993`). Kod buduje się dla Cortex-M4 (`scripts/build-firmware.ps1:212-213`),
a lokalny CMSIS deklaruje `__CORTEX_M=0x04` i GD32 core r0p1. Naturalnie wyrównany load/store
8/16/32 bit jest pojedynczym transferem na tym rdzeniu. To nie zastępuje `volatile`, nie daje
spójności wielu pól i nie naprawia kolejności kompilatora.

| Stan / przyszłe pole | Typ i dziś | Producent -> konsument | Tearing | Wymagany kontrakt dla 16 kHz |
|---|---|---|---|---|
| `MS.i_q_setpoint` | signed `int32_t`, **nie volatile**, naturalnie wyrównany (`inc/main.h:121`) | dziś main -> ISR; po migracji ISR -> main+ISR | brak word tearing; możliwa niewidoczność/stale na poziomie C | dedykowany `volatile int32_t` output albo atomic getter; jeden runtime writer |
| `iq_reference_q` | signed `int32_t`, static, nie volatile | dziś wyłącznie main | brak współbieżności | po migracji wyłącznie ISR; reset musi użyć kontrolowanego handshake |
| `profile_release_step_q` | signed `int32_t`, static, nie volatile | dziś wyłącznie main | brak współbieżności | po migracji wyłącznie ISR; rate/mode musi przyjść spójnie z targetem |
| `iq_chain.requested/allowed/valid` | 2x signed `int32_t` + `uint8_t`, nie volatile | main -> ISR trace | pojedyncze wordy bez tear, lecz snapshot może mieszać cykle | **nie używać do control handoff** |
| przyszły `iq_target_final` | powinien być signed `int32_t` | main 4 kHz -> ISR 16 kHz | aligned word bez tear | `volatile`/atomowy publish; clamp do domeny Iq przed publikacją |
| przyszły `step_q/mode/flags` | `uint32_t/int32_t` + flags | main -> ISR | każde pole osobno bez tear, cały command może być mieszany | wersjonowany mailbox lub bardzo krótka sekcja krytyczna |
| przyszły zero event | monotonic `uint32_t` | ISR -> main lifecycle | bez tear | sticky sequence/counter, nie chwilowy bool |

Bezpieczny mailbox nie może być seqlockiem, który kręci się w ISR, gdy sequence jest nieparzyste:
main nie może wtedy wznowić publikacji. ISR powinien zrobić jeden non-blocking snapshot i przy
odd/mismatch zachować ostatni poprawny command. Alternatywa to kilka store w bardzo krótkiej
sekcji z wyłączonymi IRQ w main. W obu wariantach potrzebne są jawne compiler ordering i `DMB`
dla całego commandu. Stary `iq_chain` nie spełnia tego kontraktu.

Stale target przez najwyżej jeden cykl 16 kHz jest akceptowalny dla zwykłej jazdy. Nie jest
akceptowalne połączenie nowego targetu ze starym trybem `normal/safety/pass-through/exact-zero`.

## 6. ISR cost analysis

`ADC0_1_IRQHandler()` już wykonuje trzy odczyty JDR pod lokalnym maskowaniem IRQ, offset,
rekonstrukcję 2-of-3, estymację kąta Hall, `FOC_calculation()`, PI, SVPWM, wybór okna ADC,
cztery zapisy CCR, transakcyjny publish kontekstu i QS trace. `FOC_calculation()` wykonuje
Clarke/Park, filtry Iq/Id, hard-overcurrent, PI i inverse Park/SVPWM
(`src/FOC.c:96-178+`).

Statyczna obserwacja artefaktu 0.0469 debug (`-O0`) daje symbole: ADC ISR `0x524` = 1316 B,
`FOC_calculation` `0xA38` = 2616 B, `runPIcontrol` `0xE8` = 232 B. Rozmiar kodu nie jest
pomiarem WCET. W repo nie ma DWT/GPIO instrumentacji czasu ISR ani udokumentowanego zapasu
cykli.

Sam późniejszy tick slew może być mały: snapshot cache, compare target/state, add/sub, clamp,
shift i store. Nie wymaga float, alokacji ani dzielenia. Precompute kroku musi pozostać poza ISR.
Obecny ISR ma już integer division w interpolacji kąta
(`src/main.c:3932-3938`); proponowany slew nie może dodawać kolejnego.

Wniosek: koszt obliczeniowy małego ticka jest prawdopodobnie pomijalny względem obecnego FOC,
ale **dostępny budżet nie jest zmierzony**, więc nie stanowi samodzielnego dowodu wdrożeniowego.

## 7. Normalized step comparison

G532:

```text
cycles_full = 0.056375 * 16000 = 902
step_norm   = 1 / 902 = 0.001108647 = 0.110865% / cykl
slope       = 17.7384 full-scale/s = 1.77384%/ms
```

Najlepszą skalą EVist jest **B — current-vector configured maximum**, czyli
`MP.phase_current_max`, obecnie wymuszone do `PH_CURRENT_MAX=700`
(`src/main.c:999`, `inc/config.h:173`). To domena, w której żyją Iq request/ref i PI.

Nie należy używać:

- `iq_target`/instantaneous `Iq_allowed`: dałoby zawsze podobny czas do aktualnego celu, a więc
  zmienny fizyczny slope;
- `phase_current_max_scaled`: zależy od poziomu i limp mode, więc ten sam fizyczny Iq miałby inny
  `dIq/dt` po zmianie limitera;
- surowego G532 `10`: domeny count są różne.

Dla skali 700 krok idealny wynosi `700/902 = 0,776053` count/cykl. Q16 daje
`ceil(700*65536/902)=50860`, czyli `0,776062` count/cykl i dokładnie 902 cykle do pełnej skali.
Maksymalny normalny stan `700<<16=45 875 200` jest bezpieczny w `int32_t`; nawet parserowy
sanity max około 842 pozostaje daleko od `INT32_MAX`. Q24 w `int32_t` już nie jest bezpieczny.

## 8. First-step comparison

Przy tej samej średniej G532:

| Architektura | Internal normalized step | Co dostaje integer PI przy skali 700 |
|---|---:|---:|
| G532 16 kHz | 0,1109% co 62,5 us | 10/9011 co cykl |
| EVist 4 kHz | 0,4435% co 250 us | pierwszy skok 3 count = 0,4286%; potem skoki 3/4 count |
| EVist 16 kHz Q16 | 0,1109% co 62,5 us | pierwszy widoczny 1 count = 0,1429%, z okresowymi plateau |

4 kHz może odtworzyć średnie `dIq/dt`, ale nie widmo kroków: PI widzi cztery razy rzadsze i
około cztery razy większe zmiany. Między update'ami przez cztery cykle reguluje ten sam target.
16 kHz jest bliższe koncepcji G532, choć integer Iq EVist nadal ogranicza minimalną widoczną
granularność do 1/700.

## 9. Full-scale-equivalent timing

Przy stałym slope z normalizacją do 700:

| Zmiana | Idealny czas 16 kHz | 4 kHz high-resolution accumulator |
|---|---:|---:|
| 0 -> 20% / 20% -> 0 | 11,275 ms; clamp na granicy cyklu ~11,31 ms | ~11,5 ms |
| 0 -> 50% / 50% -> 0 | 28,1875 ms | ~28,25 ms |
| 0 -> 100% / 100% -> 0 | 56,375 ms (902 cykle) | ~56,5 ms (226 update'ów) |

Różnica czasu średniego jest mała. Materialną różnicą 4 kHz jest kwantyzacja w czasie i
amplitudzie, opisana w sekcji 8.

## 10. Normal-release 650-ms ownership

**NORMAL RELEASE 650 ms IS: A — the final Iq slew itself, z wariantem zależnym od stanu.**

`ride_control` przekazuje `profile_pedaling_active=false` i `level->release_ms`
(`src/ride_control.c:585-591`). `assist_dynamics_apply()` przy `target==0` latched'uje
`iq_reference_q` i liczy `ceil(start_reference/(release_ms*4))`
(`src/assist_dynamics.c:152-187`). Nie ma upstream 650-ms envelope przed innym finalnym rampem.

Dlatego zastąpienie tego wariantu stałym slope w jednym przeniesionym ownerze dałoby prawdziwy
G532-like stop: 100/50/20% schodziłoby proporcjonalnie. Samo dołożenie 16-kHz limitera za
obecnym ownerem byłoby błędem i nadal pozostawiłoby 650-ms target/reference.

Hard-cut 200 ms korzysta z tego samego wariantu ownera, tylko z innym `profile_release_ms`.
Przyszły command musi więc rozróżniać co najmniej `NORMAL_FIXED_SLOPE`,
`SAFETY_FIXED_DURATION`, `PASS_THROUGH_WA` i `EXACT_ZERO`.

## 11. Safety paths

| Przypadek | Obecna semantyka | Klasa |
|---|---|---|
| overcurrent `i_d > PH_CURRENT_MAX<<2` | neutral CCR, MOE OFF, lifecycle FAULT, infinite halt (`FOC.c:148-158`) | 1 — bypass ramp, hard off |
| brake, direction inhibit, critical overtemp, torque fault, load calibration | target 0 + firmware-owned 200 ms (`ride_control.c:339,785-789`) | 3 — ten sam final owner, inny rate |
| thermal/voltage/speed limiter, nagły spadek limitu | obniża `Iq_allowed`; zwykły final fall dochodzi do nowego targetu | 3 — ten sam owner, zwykły rate |
| comm loss | bezpośredni command zero (`main.c:1332-1343`) | 1 — bypass normal ride ramp |
| rolling fast-rearm / recovery WAIT | `force_zero_reference`, exact zero w tym samym processed tick (`assist_dynamics.c:91-98`) | 1 — bypass slew w ownerze |
| coast przy niskim ERPS | exact zero i reset Q (`assist_dynamics.c:146-150`) | 1 — bypass slew w ownerze |
| WA aktywny | własny Q8 slew i immediate safety ceilings; normalny owner pass-through | 2/oddzielna, wzajemnie wykluczająca trajektoria |
| puszczenie pełnego żądania WA | `immediate_cut`, Q reset i zero (`assist_dynamics.c:75-82`) | 1 — exact zero |
| position calibration/service | własny command; completion może wyłączyć mostek | 1 — poza normalną jazdą |

Zmiana na normalne 56,4 ms tworzy dodatkową decyzję produktową: istniejące 200 ms safety
release byłoby wolniejsze niż nowe normalne G532-like fall. Karta wymaga zachować 200 ms, więc
nie wolno po cichu zastąpić go normalnym krokiem. Hard overcurrent nadal musi ominąć wszystko.

## 12. ARMED_ZERO integration

Obecnie main przechodzi `RUN -> ARMED_ZERO`, gdy `MS.i_q_setpoint==0`, i
`ARMED_ZERO -> RUN`, gdy staje się dodatnie (`src/main.c:1453-1464`). MOE, FOC, ADC i theta
pozostają aktywne.

Po migracji dokładne zero powstałoby w ADC ISR. Sam polling nie wystarcza: ISR może osiągnąć
zero, a kolejny 4-kHz publish może odwrócić target i rozpocząć wzrost, zanim main odczyta stan.
Wtedy zdarzenie `Iq_ref became exactly zero` zostanie pominięte. Bezpieczny projekt wymaga
sticky monotonic `zero_reached_seq` produkowanego przez ISR. Main konsumuje event i wykonuje
logiczne `RUN -> ARMED_ZERO -> RUN`, jeśli nowy target jest już dodatni, bez MOE edge i bez
cold PREPARE.

Pole output musi być compiler-visible dla main. Dzisiejsze nie-volatile
`MS.i_q_setpoint` nie ma wystarczającego kontraktu po odwróceniu kierunku własności.

## 13. Cold-start integration

Obecny cold gate reaguje na dodatnie `MS.i_q_setpoint` (`src/main.c:1365`), następnie czyści
feedback, oba PI, `u_q/u_d`, ustawia neutral CCR, uruchamia istniejący neutral dwell i dopiero
zwalnia FOC (`src/main.c:1365-1451`). ADC ISR pracuje także przy MOE OFF, bo TIMER0 pozostaje
włączony; w tej gałęzi tylko invaliduje feedback (`src/main.c:4048-4055`).

Prosty tick nowego slew „na każdym ADC IRQ” pozwoliłby akumulatorowi urosnąć, zanim main wejdzie
w PREPARE, i pierwszy aktywny FOC odziedziczyłby niezerowy/stary stan. Z kolei zamrożenie ticka
przy MOE OFF bez zmiany gate tworzy deadlock: `MS.i_q_setpoint` nigdy nie zrobi się dodatnie,
więc mostek nie wystartuje.

Bezpieczna integracja musi:

1. zainicjalizować mailbox/state przed uruchomieniem injected ADC IRQ;
2. pozwolić opublikowanemu demand wywołać istniejący cold gate;
3. atomowo wyzerować slew state w istniejącym PREPARE;
4. zamrozić output na zero przez neutral dwell;
5. wykonać pierwszy slew tick dopiero w pierwszym dozwolonym active FOC cycle.

To nie wymaga nowego dwell ani zmiany MOE policy, ale nie jest prostym przeniesieniem jednej
funkcji i wymaga osobnego testowalnego handshake.

## 14. Option A analysis

**PASS jako bezpieczny stan produkcyjny i rozwiązanie przejściowe; REJECT jako ścisła parity
G532.**

Zalety: zachowuje obecne jednoznaczne ownership, wszystkie special modes, exact-zero,
ARMED_ZERO i cold start; QS-3A zapewnia deterministyczny czas mimo coalesced main calls. Nie
zwiększa ISR i nie wymaga nowej synchronizacji.

Wady: przy slope G532 wymaga `~0,4435%` co 250 us. Dla skali 700 PI dostaje skoki 3/4 count,
nie około 0,111% co 62,5 us; utrzymuje ten sam setpoint przez cztery FOC cycles. Obecne
produkcyjne tuning 300-1200/650 ms jest jeszcze znacznie wolniejsze i nie ma parity czasu.

## 15. Option B analysis

**REJECT w aktualnym baseline; technicznie właściwy kierunek po zamknięciu prerequisite'ów.**

Docelowo B daje jednego ownera w częstotliwości PI, właściwą kwantyzację czasu, naturalny clamp
na target reversal/limiter change i brak drugiego rampu. Q16 wystarcza, a tick może być bez float
i division.

Aktualne blokery:

1. `BC_limit_flag` zmienia bezpośrednio `PI_iq.setpoint` oraz feedback na inną domenę
   (`main.c:3626-3652`). Dopóki limiter nie zostanie przeniesiony upstream do `Iq_allowed`, nie
   istnieje gwarancja „jeden finalny owner -> PI”.
2. `motor_command_t` przenosi target/enable/emergency, ale nie przenosi spójnie policy
   normal/650/200/WA/exact-zero. Kilka call sites ma różną semantykę.
3. Nie istnieje race-safe mailbox target+step+mode ani kontrakt volatile dla outputu.
4. `force_zero_reference` ma kontrakt same-tick w main. Zwykły async target zostałby
   skonsumowany do 62,5 us później, co łamie istniejący kontrakt/test.
5. ARMED_ZERO potrzebuje sticky zero event, a cold PREPARE handshake opisanego wyżej.
6. Brak zmierzonego zapasu czasu ISR.

Wdrożenie B w tej karcie wymagałoby równocześnie naprawy battery limiter ownership i przebudowy
command/lifecycle API. To nie jest minimalny, izolowany patch.

## 16. Option C analysis

Jeżeli „interpolator” tylko trzyma stan i liniowo zbliża się do najnowszego 4-kHz targetu przed
PI, to jest funkcjonalnie tym samym co B. Inna nazwa nie usuwa wymagań ownership ani mailbox.

C może znaczyć coś innego tylko jako interpolacja między poprzednim i następnym 4-kHz punktem
upstream envelope. Następny punkt nie jest znany z góry, a takie opóźnienie lub ekstrapolacja
zmieniłaby safety i target reversal. Nie rekomenduje się osobnej abstrakcji ani układu
`4-kHz ramp + 16-kHz interpolator`, bo stworzyłby dwa elementy dynamiczne.

## 17. Final decision

**DECISION C: DO NOT IMPLEMENT YET.**

Nie jest to odrzucenie koncepcji 16 kHz. Źródło pokazuje, że B jest najlepszym docelowym
odwzorowaniem G532, ale baseline nie spełnia dwóch warunków brzegowych karty: single PI owner
i race-safe, semantycznie kompletny target handoff. Zachowanie obecnego 4-kHz ownera jest
bezpieczniejsze niż częściowa migracja.

## 18. Implementation if any

Brak zmiany produkcyjnej. Nie przeniesiono `assist_dynamics`, nie dodano 16-kHz limitera, nie
zmieniono PI, safety, MOE, ARMED_ZERO ani cold PREPARE.

Zmieniono wyłącznie dokumentację QS-3B i indeks dokumentacji.

## 19. Tests

- Audyt statyczny: wszystkie zapisy `MS.i_q_setpoint`, `PI_iq.setpoint`, call sites
  `motor_core_set_command`, konteksty IRQ i konfiguracja 4/16 kHz — PASS.
- Istniejący focused host `qs3_final_iq_slew_host` — **PASS / ALL CHECKS PASSED**.
- Istniejący focused host `armed_zero_lifecycle_host` — **PASS / ALL CHECKS PASSED**.
- Testy implementacyjne QS-3B 1-20 i numeric fractional accumulator — NOT RUN / NOT APPLICABLE,
  ponieważ zgodnie z Decision C nie powstał algorytm produkcyjny.
- Full suite — nie jest wymagana dla documentation-only decision; ostatni znany pełny run QS-3A
  miał jeden niezależny baseline failure `rolling_no_assist_diag_host` (schema 3 harness kontra
  schema 4 source).

## 20. Build/version if any

Brak builda i brak nowego canonical. Aktualny pozostaje:

- `0.0469_M820_BL820.bin`
- SHA-256 `8951B6997B8E4F2E536DAF9E81EFADE24293BD8C608D9D4CB15AA87E879B1564`

QS-3B nie konsumuje `0.0470`.

## 21. Open risks

1. Legacy battery-current PI override musi najpierw stać się limiterem upstream w tej samej
   domenie Iq; dopiero wtedy `Iq_allowed -> one slew -> PI` będzie prawdą we wszystkich runtime
   modes.
2. Trzeba zaprojektować non-blocking mailbox z jednym właścicielem outputu i pełnym policy
   commandem, nie tylko `int32_t target`.
3. Same-tick exact-zero, sticky zero event i cold PREPARE reset/freeze/release muszą mieć osobne
   host modele oraz source wiring guards.
4. Należy zmierzyć worst-case ADC ISR (DWT lub pin) przed i po zmianie. Rozmiar symbolu nie jest
   dowodem czasowym.
5. Nowe normalne 56,4 ms byłoby dużo ostrzejsze od obecnych 300-1200/650 ms i zmieniłoby ride
   feel. To świadoma zmiana produktu, nie wyłącznie migracja timebase.

**Następna konkretna akcja:** zamknąć upstream battery-current limiter ownership tak, aby
`PI_iq.setpoint` zawsze pochodził z `MS.i_q_setpoint`; potem wrócić do B z atomowym command
mailbox i testami lifecycle 1-20.
