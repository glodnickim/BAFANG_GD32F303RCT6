# EVistDrive — dynamika wspomagania vs TSDZ2
## Audyt ramp-up / ramp-down, toru torque i wytyczne wdrożeniowe

**Status:** dokument projektowy / wytyczne do implementacji  
**Data audytu:** 2026-08-30  
**Język:** PL  
**Zakres:** EVistDrive M820, dynamika rider torque → Iq, porównanie z TSDZ2 OSF  
**Bazowy snapshot EVistDrive:** `Evistdrive 28082026(1).zip`  
**Wersja w `inc/build_version.h`:** `0.0408`  
**SHA-256 snapshotu:** `a1fcbb1c5e2297592e585e0b8c2944a9a099a060ae06f453a2cfb32591193612`

---

# 1. Cel dokumentu

Ten dokument ma być **materiałem wejściowym dla agenta wdrażającego kolejne poprawki EVistDrive**. Nie jest propozycją jednego „magicznego” parametru rampy i nie powinien prowadzić do prostego zwiększenia `ramp_up_ms` / `ramp_down_ms` bez pomiarów.

Problem zgłoszony z jazdy jest następujący:

- przy małej prędkości roweru reakcja potrafi być nadal zaskakująco szybka;
- przy większej zmianie nacisku / żądania momentu wzrost i spadek wspomagania jest odczuwany jako zbyt bezpośredni;
- ustawienia ramp wyglądają na wolne, ale subiektywna reakcja silnika jest szybsza niż sugerują liczby;
- istnieje podejrzenie, że przy przenoszeniu rozwiązań inspirowanych TSDZ2 do FOC EVistDrive zachowano wartości lub logikę adaptacji, ale zmieniła się **domena sterowana**;
- istnieje też podejrzenie, że część zachowania pochodzi z toru czujnika nacisku i zmiany sposobu filtrowania RUN.

Celem audytu jest odpowiedzieć:

1. czy adaptive ramp EVistDrive działa logicznie podobnie do TSDZ2;
2. czy ustawione czasy oznaczają to samo w obu firmware;
3. czy TSDZ2 ma dodatkowe mechanizmy wygładzające, których EVistDrive nie posiada;
4. czy w torze czujnika nacisku po przeniesieniu logiki występuje błąd skali lub normalizacji;
5. co należy zmienić w EVistDrive, aby uzyskać **kontrolowaną, naturalną dynamikę**, bez psucia szybkiego regulatora FOC;
6. jak zrobić to etapami, tak aby każda zmiana miała mierzalnego właściciela i nie mieszała kilku problemów naraz.

---

# 2. Najważniejszy wniosek

## 2.1. EVistDrive nie ma prostego „złego czasu rampy”

Najważniejsza różnica względem TSDZ2 jest architektoniczna:

```text
TSDZ2
rider torque / power
        ↓
target battery current
        ↓
adaptive PWM/duty ramp
        ↓
warunek oparty o zmierzony battery current / phase current
        ↓
rzeczywisty moment
```

W EVistDrive:

```text
rider torque
        ↓
filtered RUN effort
        ↓
mode → Iq request
        ↓
limiters
        ↓
Iq reference ramp
        ↓
szybki current FOC
        ↓
rzeczywisty moment
```

To znaczy:

> **600 ms rampy w TSDZ2 i 600 ms rampy `Iq_ref` w EVistDrive nie są równoważnymi obiektami sterowania.**

TSDZ2 ma dodatkową „bezwładność” wynikającą z tego, że rampa zmienia duty, a wzrost duty jest dodatkowo bramkowany przez rzeczywisty prąd. EVistDrive przekazuje natomiast już ukształtowaną referencję `Iq` do regulatora prądu, którego zadaniem jest właśnie możliwie szybko ją zrealizować.

Dlatego kopiowanie z TSDZ2:

- progów prędkości,
- progów kadencji,
- relacji SLOW/FAST,
- albo samych liczb czasowych

**nie może być traktowane jako pełny port zachowania.**

---

# 3. Źródła i identyfikacja audytu

## 3.1. EVistDrive — źródło podstawowe

Audyt wykonano na dokładnym pliku:

```text
Evistdrive 28082026(1).zip
```

Snapshot zawiera m.in.:

```text
src/assist_dynamics.c
src/assist_start.c
src/assist_modes.c
src/ride_control.c
src/torque_input.c
src/main.c
inc/config.h
inc/torque_input.h
inc/assist_modes.h
inc/ride_control.h
```

`inc/build_version.h`:

```text
EBICS_BUILD_VERSION = 0.0408
```

Kontrolne SHA-256 najważniejszych plików:

| Plik | SHA-256 |
|---|---|
| `src/assist_dynamics.c` | `00f99406c205824740414f7ae9d3a1da7e4c3467977a167bf7d4210b66c131aa` |
| `src/torque_input.c` | `fe4ceca5bd9b6af17dd974dfdd99b3dc0eec5bb20592785f8bfcb9565ce4eabc` |
| `src/assist_modes.c` | `8a8805f9c75e216f599549487affb8db3022cf23a88a99f1194f41d1eef021e0` |
| `src/ride_control.c` | `9c58ebe1d95da4f94136bc7e6ee6ef429d5a6ca0e7fcc4190e1734e3e769211e` |
| `src/assist_start.c` | `a1ee6bc4bea91c679a2c32c545810b1a62a34718a31f35b6965a1892b465752d` |

**Ważne:** skompilowane wartości domyślne nie muszą być identyczne z aktywnym bankiem zapisanym w sterowniku. Przed strojeniem sprzętowym agent ma odczytać i zapisać **rzeczywistą konfigurację runtime** aktywnego banku.

## 3.2. TSDZ2 — źródło porównawcze

Porównanie wykonano z aktualnym publicznym repozytorium:

```text
emmebrusa/TSDZ2-Smart-EBike-1
branch: master
odczyt: 2026-08-30
```

Najważniejsze pliki referencyjne:

```text
src/ebike_app.c
src/motor.c
src/config.h
```

Przy porównaniu należy traktować TSDZ2 jako **wzorzec koncepcji**, a nie źródło stałych do kopiowania. TSDZ2 ma inną architekturę regulatora silnika, inny MCU, inny tor prądu i inne znaczenie „rampy”.

---

# 4. Aktualny tor EVistDrive — od nacisku do momentu

Poniższy łańcuch jest krytyczny. Przy przyszłych zmianach nie wolno analizować „rampy” bez wskazania, na którym dokładnie etapie powstaje skok.

```text
RAW TORQUE ADC
    ↓
torque_input_correct()
    ↓
automatic zero / coast recalibration
    ↓
delta_native
    ↓
assist deadband
    ↓
AFILT — fast filter ~35 ms
    ↓
RUN — ordinary asymmetric filter 120 ms rise / 350 ms fall
    ↓
assist_modes_calculate()
    ↓
startup boost / mode curve / support ratio
    ↓
phase Iq request
    ↓
power/current ceilings
    ↓
ride_control latch / floor / boost / limits
    ↓
optional smooth start
    ↓
gear preload at standstill
    ↓
IQ_PRE_RAMP
    ↓
assist_dynamics_apply()
    ↓
FINAL Iq reference
    ↓
motor_core_set_command()
    ↓
FOC current loop
    ↓
Iq actual / motor torque
```

## 4.1. Skala czujnika nacisku

W aktualnym `inc/torque_input.h` domyślna charakterystyka jest opisana pomiarami konkretnego układu:

```text
zero            ≈ 740 native/mV-domain
6.00 kg         → delta 146
84.00 kg        → delta 1580
public full scale = 60.00 kg
```

Domyślna konwersja jest **piecewise-linear**, a kalibracja użytkownika może przełączyć tor na liniowy span odpowiadający 60 kg.

Nie znaleziono prostego błędu typu:

- ×2,
- ×10,
- błędny znak,
- zamiana kg/centikg,
- błędne 60 kg ↔ 120 kg,
- przypadkowe użycie ADC raw jako gotowego momentu.

Wniosek:

> **Nie ma obecnie dowodu, że główną przyczyną zbyt szybkiej dynamiki jest błąd jednostki lub skali torque sensora.**

Nie oznacza to, że tor torque jest idealny. Problem może dotyczyć **filtrowania i publikowanego sygnału RUN**, a nie skali fizycznej.

## 4.2. AFILT — szybki sygnał

`TORQUE_ASSIST_FILTER_MS = 35 ms`.

Filtr:

- działa w Q8;
- jest aktualizowany w torze zakładającym 4 kHz;
- zbliża się do bieżącego `assist_delta`;
- stanowi szybki, odszumiony sygnał nacisku;
- jest używany m.in. w recovery / rearm.

Istniejący test infrastruktury wykazał ważną cechę: filtr liczy **liczbę wywołań**, a nie rzeczywisty czas. Jeśli wywołania nie przyjdą w idealnym 4 kHz, „35 ms” przestaje być dokładnym czasem fizycznym.

To jest wada architektoniczna do poprawy, ale sama w sobie zwykle **wydłuża** reakcję przy zgubionych wywołaniach, a nie powoduje szybszego narastania.

## 4.3. RUN — ważna zmiana FW-112.4

W kodzie nadal istnieje 180° crank-angle moving average:

```text
TORQUE_RUN_WINDOW_DEG_DEFAULT = 180°
```

Jego pierwotny cel był bardzo dobry: filtr jest clockowany kątem korby, więc przy 20 rpm i 100 rpm obejmuje ten sam fragment obrotu, a nie arbitralny fragment czasu.

Jednak w **ordinary RUN** wynik tego okna nie jest obecnie publikowany jako finalny `assist_delta_run_native`.

FW-112.4 nadpisuje ordinary RUN przez:

```text
rise = 120 ms
fall = 350 ms
```

czyli asymetryczny filtr czasowy.

Ważne rozróżnienie:

```text
180° moving average nadal się liczy
ALE
ordinary RUN publikuje 120/350 ms filter output
```

Recovery/rearm ma własne ścieżki i może publikować FAST bez czekania na ordinary RUN.

### Konsekwencja

Zmiana FW-112.4 usunęła dużą część zależności opóźnienia od kadencji i naprawiła konkretne przypadki zbyt późnego wzrostu momentu. Jednocześnie mogła sprawić, że normalna jazda jest bardziej „czasowa” i mniej związana z mechanicznym cyklem pedałowania.

Dlatego **nie należy jej cofać w ciemno**, ale należy wykonać kontrolowane A/B:

```text
A = obecne RUN 120/350 ms
B = angle-window RUN
C = docelowy hybrydowy estimator, jeśli A/B pokaże taką potrzebę
```

---
# 5. Aktualny `assist_modes` — gdzie powstaje Iq request

## 5.1. POWER_LINEAR nie jest Hybrid TSDZ2

W aktualnym EVistDrive `POWER_LINEAR` liczy żądanie fazowego `Iq` przede wszystkim z obciążenia pedału i support ratio.

Uproszczony model:

```text
assist load
    × support ratio
    ↓
normalized demand
    ↓
phase Iq request
```

Następnie dochodzą:

- cadence compensation;
- per-level `max_iq_pct`;
- motor-power ceiling;
- P/U ceiling przeliczany z mocy na dopuszczalne `Iq` przy bieżącym voltage utilization.

To znaczy, że power path często jest **sufitem**, a nie równoległym generatorem momentu.

Jeżeli:

```text
Iq z torque/load = 200
P/U ceiling      = 270
```

to power filter nie zmienia `Iq_request` — finalnie zostaje 200.

To jest istotne, ponieważ samo ustawienie np. `power_rise_filter_ms = 190 ms` nie oznacza, że moment silnika zawsze narasta z taką dynamiką.

## 5.2. W TSDZ2 Hybrid są dwa rzeczywiste generatory targetu

TSDZ2 Hybrid oblicza oddzielnie:

```text
torque-derived battery-current target
power-derived battery-current target
```

następnie wybiera większy z nich.

Koncepcyjnie:

```text
Torque branch ─┐
               ├── MAX ──> target battery current
Power branch  ─┘
```

W EVistDrive nie należy nazywać obecnego `POWER_LINEAR` pełnym odpowiednikiem Hybrid TSDZ2.

### Wniosek projektowy

Nie mieszać dwóch osobnych tematów:

1. **ride feel / dynamika Iq**, którą należy naprawić w pierwszej kolejności;
2. **semantyka Hybrid**, którą można rozważyć później jako oddzielną funkcję / tryb.

Przebudowa Hybrid w tym samym patchu co rampa uniemożliwi ustalenie, która zmiana faktycznie poprawiła jazdę.

---

# 6. Aktualna adaptive Iq ramp w EVistDrive

## 6.1. Konfiguracja globalna

W `inc/config.h`:

```text
IQ_RAMP_ADAPTIVE  = 1
IQ_RAMP_TIME_MODE = 1
```

Adaptive blending używa:

```text
wheel speed: 4.0 → 20.0 km/h
cadence:     50  → 110 rpm
```

Poniżej dolnego progu `map()` zwraca SLOW. Powyżej górnego progu zwraca FAST.

**Sprawdzone:** `map()` klamruje wejścia, więc nie występuje ekstrapolacja ponad FAST przy np. 130 rpm. To ważne — nie ma ukrytego błędu „kadencja >110 daje jeszcze szybszą rampę”.

## 6.2. Wybór szybszego źródła

Dla rise i fall liczone są osobno:

```text
speed-derived ramp
cadence-derived ramp
```

a następnie wybierany jest **krótszy czas**, czyli szybsza odpowiedź.

Koncepcyjnie:

```text
adaptive_progress = max(progress_from_speed, progress_from_cadence)
```

To jest podobne do TSDZ2.

Przykład:

```text
wheel speed = 2 km/h  → speed sugeruje SLOW
cadence     = 80 rpm  → cadence jest już ~w połowie SLOW→FAST
```

Finalna rampa będzie sterowana kadencją, mimo że rower jedzie wolno.

To **nie jest błąd portu samej logiki**. TSDZ2 również pozwala szybszemu z dwóch kryteriów przejąć sterowanie.

Problem polega na tym, że w TSDZ2 ta adaptive wartość steruje duty/PWM w układzie z dodatkowym feedback-gatingiem, a w EVistDrive bezpośrednio kształtuje referencję prądu dla szybkiego FOC.

---

# 7. Domyślne rampy L1–L5 w snapshot 28.08

Skompilowane profile POWER_LINEAR:

| Poziom | Support | Iq rise slow | Iq rise fast | Iq fall slow | Iq fall fast | Power rise | Power fall |
|---:|---:|---:|---:|---:|---:|---:|---:|
| L1 | 100% | 600 ms | 300 ms | 1000 ms | 180 ms | 150 ms | 375 ms |
| L2 | 200% | 600 ms | 330 ms | 1000 ms | 210 ms | 160 ms | 400 ms |
| L3 | 320% | 650 ms | 380 ms | 1050 ms | 250 ms | 190 ms | 450 ms |
| L4 | 420% | 700 ms | 450 ms | 1100 ms | 300 ms | 220 ms | 500 ms |
| L5 | 520% | 750 ms | 500 ms | 1200 ms | 350 ms | 250 ms | 550 ms |

To jest **dobry kierunek projektowy**: wyższy poziom ma większy gain momentu, więc dostaje wolniejszą dynamikę.

Nie należy usuwać tej zależności bez dowodu.

## 7.1. Ważna uwaga o persisted bank

Tabela powyżej opisuje `compiled defaults`.

Agent ma przed testami zapisać:

```text
active bank
L1..L5 support ratio
power rise/fall
Iq rise slow/fast
Iq fall slow/fast
release_ms
smooth_start enabled/duration
startup boost
cadence compensation
max_iq_pct
max_motor_power_w
```

Jeżeli aktywny bank różni się od defaultów, wszystkie wykresy i porównania mają używać **runtime bank**, nie tabeli z kodu.

---

# 8. Krytyczny finding: co naprawdę oznacza `ramp_ms`

## 8.1. Normalna rampa

W zwykłej ścieżce `assist_dynamics.c` krok jest liczony od **pełnej skali Iq**:

```text
step_per_tick ≈ iq_scale / selected_ramp_ticks
```

To jest klasyczny slew limiter o prawie stałym `dIq/dt`.

Konsekwencja:

```text
czas przejścia ≈ selected_ramp_ms × |ΔIq| / iq_scale
```

Czyli `600 ms` nie oznacza:

> każda zmiana targetu trwa 600 ms.

Oznacza raczej:

> przejście przez całą skalę Iq trwa około 600 ms.

Przykład dla 600 ms:

| Zmiana | Przybliżony czas |
|---:|---:|
| 10% pełnej skali | 60 ms |
| 20% | 120 ms |
| 40% | 240 ms |
| 80% | 480 ms |
| 100% | 600 ms |

To nie jest samo w sobie matematycznym błędem. Tak właśnie działa typowy slew rate limiter.

**Problem użytkowy** polega na tym, że:

1. nazwa parametru w ms łatwo sugeruje „czas przejścia”;
2. w FOC rzeczywisty prąd bardzo skutecznie podąża za `Iq_ref`;
3. slope startuje od razu z maksymalną wartością — brak ograniczenia jerk;
4. TSDZ2, z którego pochodziła inspiracja, ma dodatkowy actuator/current feedback, więc taka sama idea nie daje tego samego odczucia.

## 8.2. Release do zera jest już inne

W kodzie istnieje bardzo ważna poprawka FW-040/FW-072.

Dla normalnego puszczenia pedału:

```text
current Iq at release → 0
```

krok jest liczony od **wartości istniejącej w chwili rozpoczęcia release**, a nie od pełnej skali.

Dlatego:

```text
release_ms = 650
```

ma oznaczać w przybliżeniu rzeczywiste 650 ms od bieżącego poziomu do zera niezależnie od tego, czy startuje z 20%, 50% czy 90% pełnej skali.

To zachowanie należy **zachować**.

Nie wolno przez przypadek zastąpić tego zwykłym full-scale slew podczas przebudowy dynamiki.

---

# 9. Liczbowe przykłady obecnego adaptive ramp

Poniższe liczby dotyczą skompilowanych defaultów i pokazują **czas przejścia przez pełną skalę** po adaptive speed/cadence selection.

## 9.1. Rower 2 km/h, kadencja 30 rpm

Oba wejścia są w SLOW:

| Level | Rise full-scale | Fall full-scale |
|---:|---:|---:|
| L1 | 600 ms | 1000 ms |
| L2 | 600 ms | 1000 ms |
| L3 | 650 ms | 1050 ms |
| L4 | 700 ms | 1100 ms |
| L5 | 750 ms | 1200 ms |

## 9.2. Rower 2 km/h, kadencja 80 rpm

Prędkość nadal sugeruje SLOW, ale cadence wybiera szybszą odpowiedź:

| Level | Rise full-scale | Fall full-scale |
|---:|---:|---:|
| L1 | ~453 ms | ~597 ms |
| L2 | ~468 ms | ~612 ms |
| L3 | ~518 ms | ~657 ms |
| L4 | ~577 ms | ~707 ms |
| L5 | ~627 ms | ~782 ms |

Dla typowej zmiany obejmującej tylko część skali czasy są proporcjonalnie krótsze.

Przy **20% pełnej skali**:

| Level | Rise ~20% | Fall ~20% |
|---:|---:|---:|
| L1 | ~91 ms | ~119 ms |
| L2 | ~94 ms | ~122 ms |
| L3 | ~104 ms | ~131 ms |
| L4 | ~115 ms | ~141 ms |
| L5 | ~125 ms | ~156 ms |

Przy **40% pełnej skali**:

| Level | Rise ~40% | Fall ~40% |
|---:|---:|---:|
| L1 | ~181 ms | ~239 ms |
| L2 | ~187 ms | ~245 ms |
| L3 | ~207 ms | ~263 ms |
| L4 | ~231 ms | ~283 ms |
| L5 | ~251 ms | ~313 ms |

To dobrze tłumaczy sytuację, w której użytkownik widzi „600–1000 ms”, ale podczas jazdy odczuwa reakcję rzędu 100–300 ms.

## 9.3. Kadencja 110 rpm

Przy 2 km/h cadence osiąga FAST i przejmuje rampę.

Dla L1 jest to około:

```text
rise full-scale ≈ 305 ms
fall full-scale ≈ 194 ms
```

Zmiana tylko 20% skali oznacza orientacyjnie:

```text
rise ≈ 61 ms
fall ≈ 39 ms
```

To jest bardzo szybkie dla bezpośredniego `Iq_ref` FOC, nawet jeśli jest logicznie spójne z obecnym algorytmem.

---
# 10. Co dokładnie robi TSDZ2 inaczej

## 10.1. Adaptive ramp jest podobny logicznie

TSDZ2 liczy rampę z:

- wheel speed 4–20 km/h;
- cadence 20–70 rpm;
- i wybiera szybszą odpowiedź z obu.

To jest ten sam rodzaj logiki co w EVistDrive.

Różnica zakresu cadence:

```text
TSDZ2:      20 → 70 rpm
EVistDrive: 50 → 110 rpm
```

EVistDrive przesunął cadence adaptation wyżej, co samo w sobie nie tłumaczy nadmiernej szybkości przy niskiej kadencji. Przy 80–110 rpm cadence zaczyna jednak silnie przyspieszać rampę.

## 10.2. TSDZ2 rampuje actuator, nie `Iq_ref`

W TSDZ2 adaptive wartość trafia do kontrolera PWM duty.

Duty rośnie tylko wtedy, gdy m.in.:

```text
requested duty > actual duty
AND
requested battery current > measured filtered battery current
```

Duty maleje, jeżeli wystąpi m.in.:

- target battery current < measured battery current;
- phase-current limit;
- undervoltage;
- overspeed;
- brake;
- target duty niższy od actual duty.

Czyli występuje dodatkowa warstwa:

```text
target current
     ↓
ramp duty
     ↓
feedback says whether duty may rise
     ↓
actual current
```

W EVistDrive odpowiednikiem nie powinno być kopiowanie duty ramp, ponieważ FOC ma już własny, dużo lepszy current loop.

**Właściwa lekcja do przeniesienia:**

> dynamiczny generator referencji momentu powinien mieć własny łagodny outer-loop governor, a szybki FOC ma pozostać szybki.

## 10.3. TSDZ2 Smooth Start działa wcześniej

TSDZ2 Smooth Start zmienia bezpośrednio `pedal torque delta` przed policzeniem Hybrid/Torque assist.

Uproszczony tor:

```text
torque delta
   ↓
smooth-start envelope
   ↓
Hybrid / Torque / Cadence computation
   ↓
current target
   ↓
PWM/current-gated actuator
```

W EVistDrive smooth start jest później:

```text
mode Iq target
   ↓
limits
   ↓
assist_start_apply_smooth()
   ↓
gear preload
   ↓
assist_dynamics Iq ramp
```

Dodatkowo compiled default ma:

```text
smooth_start.enabled = false
duration = 300 ms
```

Czyli w standardowym profilu ten dodatkowy amortyzator jest wyłączony.

## 10.4. TSDZ2 ma dodatkową obróbkę torque delta

W aktualnym TSDZ2 występują m.in.:

- initial offset learning;
- offset adjustment podczas pedałowania;
- opcjonalny nieliniowy remap kalibracyjny;
- uśrednienie bieżącego torque delta z poprzednią próbką;
- Smooth Start działający na torque delta;
- osobne Torque i Power branches w Hybrid.

Nie należy kopiować tych mechanizmów 1:1, ponieważ czujnik i kalibracja EVistDrive są inne. Istotny jest wzorzec:

```text
sensor
→ normalize
→ remove tiny noise/deadband
→ shape rider intent
→ dopiero potem calculate motor demand
```

---

# 11. Findings — klasyfikacja

## F-01 — Domena rampy różni się od TSDZ2

**Pewność: HIGH / CONFIRMED**

TSDZ2 kształtuje duty/PWM w pętli zależnej od zmierzonego prądu. EVistDrive kształtuje bezpośrednio `Iq_ref` przed FOC.

**Wpływ:** identyczna logika SLOW/FAST może dawać znacznie bardziej bezpośrednią reakcję w EVistDrive.

**Decyzja:** nie kopiować czasów 1:1.

---

## F-02 — Zwykłe `ramp_ms` to full-scale slew time

**Pewność: CONFIRMED**

Normalny step jest liczony z `iq_scale`, nie z `|target-current|`.

**Wpływ:** typowe częściowe zmiany targetu kończą się dużo szybciej niż liczba prezentowana jako `ramp_ms`.

**Klasyfikacja:** nie jest błędem matematycznym, ale jest potencjalnie mylącą semantyką konfiguracji i może być zbyt agresywna dla bezpośredniego FOC.

---

## F-03 — Brak jawnego jerk limitera

**Pewność: CONFIRMED**

Po zmianie targetu `Iq_ref` od pierwszego ticku przyjmuje maksymalny dozwolony slope.

Nie ma osobnego stanu:

```text
dIq/dt
```

który sam narastałby i opadał stopniowo.

**Wpływ:** nawet jeśli czas całkowity wygląda poprawnie, początek dużego przejścia jest natychmiastowy w sensie pochodnej. W FOC jest to dobrze odczuwalne jako „uderzenie”/„natychmiastowe podjęcie”.

**Priorytet wdrożenia:** HIGH.

---

## F-04 — Adaptive wybiera szybsze z speed/cadence

**Pewność: CONFIRMED**

To jest zgodne z filozofią TSDZ2.

**Wpływ:** wolny rower + wysoka kadencja może mieć szybką rampę.

**Ocena:** sam mechanizm nie jest błędnym portem, ale po zmianie domeny z duty na `Iq_ref` jego agresywność wymaga ponownej walidacji.

---

## F-05 — Brak overshootu FAST przez `map()`

**Pewność: CONFIRMED**

`map()` klamruje wejścia poniżej/ponad zakres.

**Wniosek:** 130 rpm nie daje rampy szybszej niż FAST. Nie szukać błędu w ekstrapolacji.

---

## F-06 — Release do zera ma poprawioną amplitude-independent semantykę

**Pewność: CONFIRMED**

Normalny pedal release używa osobnej ścieżki i powinien dojść do zera w `release_ms` niezależnie od startowego Iq.

**Decyzja:** zachować tę własność w każdej nowej architekturze.

---

## F-07 — Smooth Start nie pełni roli stałego ride-feel filtra

**Pewność: CONFIRMED**

EVistDrive Smooth Start:

- jest przeznaczony do launch from standstill;
- nie powinien odpalać przy każdym rolling re-engagement;
- compiled default jest OFF.

**Wniosek:** nie używać Smooth Start jako łaty na wszystkie szybkie przejścia podczas normalnej jazdy.

---

## F-08 — RUN 180° istnieje, ale ordinary RUN go obecnie nie publikuje

**Pewność: CONFIRMED**

FW-112.4 publikuje 120/350 ms time-domain filter w normalnym RUN.

**Wpływ:** możliwa zmiana charakteru per-leg peak/fall względem angle-normalized estimatora.

**Decyzja:** A/B, nie rollback w ciemno.

---

## F-09 — Torque sensor scaling nie wykazuje oczywistego błędu domeny

**Pewność: STRONG**

Skala i centikg conversion są spójne z obecnym mode code.

**Otwarte:** realna charakterystyka konkretnego czujnika pod obciążeniem dynamicznym oraz wpływ current zero/recalibration na shape rider intent.

---

## F-10 — Power filter nie zawsze kształtuje moment

**Pewność: CONFIRMED**

Jeśli P/U ceiling nie jest aktywny, `phase_iq_request` przechodzi bez jego wpływu.

**Wniosek:** nie stroić ride feel samym `power_rise_filter_ms` bez flagi/telemetrii pokazującej, że power ceiling faktycznie ogranicza target.

---

## F-11 — Per-level slower dynamics przy wyższym support są dobrym wzorcem

**Pewność: STRONG**

L4/L5 są celowo wolniejsze niż L1/L2.

**Decyzja:** zachować możliwość różnych dynamics per level.

---

## F-12 — Timebase oparty o liczbę wywołań wymaga uporządkowania

**Pewność: CONFIRMED**

AFILT/RUN/dynamics zakładają 4 kHz w kilku miejscach.

Istniejące host testy potwierdzają, że przy pominiętych wywołaniach „czas” filtra zmienia się.

**Wpływ na obecny symptom:** raczej wtórny, ale krytyczny dla uczciwej konfiguracji parametrów w ms.

---

# 12. Rekomendowana docelowa architektura EVistDrive

## 12.1. Zasada podstawowa

**Nie zwalniać FOC, aby uzyskać łagodny rower.**

FOC powinien nadal możliwie dobrze realizować `Iq_ref`.

Ride feel ma być kształtowany **przed** current loop.

Docelowo:

```text
RAW TORQUE
   ↓
calibration / zero / plausibility
   ↓
FAST rider signal
   ↓
RUN rider-intent estimator
   ↓
assist mode
   ↓
Iq_requested
   ↓
all limits / arbitration
   ↓
Iq_allowed
   ↓
REFERENCE GOVERNOR V2
   ↓
Iq_ref
   ↓
FAST FOC
   ↓
Iq_actual
```

## 12.2. Nowy element: Reference Governor V2

Governor powinien odpowiadać za:

1. ograniczenie `dIq/dt`;
2. opcjonalne ograniczenie `d²Iq/dt²` — jerk;
3. adaptive speed/cadence policy;
4. osobne zachowanie rise/fall;
5. zachowanie istniejącego dokładnego `release_ms` do zera;
6. zachowanie safety semantics;
7. monotoniczne dojście do targetu bez overshootu;
8. pracę w integer/fixed-point;
9. stabilność przy zmianie targetu w trakcie rampy;
10. telemetrię efektywnego slope i stanu governora.

---

# 13. Najbardziej rekomendowany wariant: slew + jerk

Obecny kod ma tylko ograniczenie pozycji referencji na tick.

Docelowo governor powinien utrzymywać także stan prędkości zmian:

```text
Iq_ref
Iq_slew = dIq_ref/dt
```

Koncepcja:

```text
error = Iq_allowed - Iq_ref

slew_target = sign(error) * max_slew_for_current_state

Iq_slew += clamp(
    slew_target - Iq_slew,
    -jerk_down * dt,
    +jerk_up   * dt)

Iq_ref += Iq_slew * dt

if target crossed:
    Iq_ref = Iq_allowed
    Iq_slew = 0
```

## 13.1. Co to daje

Przy skoku targetu:

```text
obecnie:
0 → od razu pełny dIq/dt → target

V2:
0 → narastający dIq/dt → pełny dIq/dt → wygaszenie dIq/dt → target
```

Efekt jest zbliżony do S-curve.

To dokładnie adresuje odczucie:

> „duża amplituda od razu rusza bardzo mocno”.

Bez jerk limitera duży step dostaje maksymalny slope w pierwszym ticku. Z jerk limiterem sam slope potrzebuje krótkiego czasu, aby osiągnąć maksimum.

## 13.2. Czego NIE robić

Nie implementować:

```text
PI current gains × 0.5
```

albo:

```text
wolniejszy PWM update
```

jako sposobu na ride feel.

To pogorszy kontrolę prądu, a nie rozwiąże problemu outer dynamics.

---
# 14. Adaptive policy — co zachować, co ponownie zwalidować

## 14.1. Zachować

- osobne rise/fall;
- per-level parameters;
- zależność od wheel speed;
- zależność od cadence;
- klamrowanie poza zakresem;
- możliwość szybszego zachowania przy dynamicznej jeździe;
- dokładny release-to-zero.

## 14.2. Ponownie zwalidować

Obecne:

```text
progress = max(speed_progress, cadence_progress)
```

jest wierne TSDZ2 na poziomie idei, ale nie musi być najlepsze po przejściu na bezpośredni `Iq_ref`.

Należy przetestować trzy polityki:

### Policy A — CURRENT / fastest wins

```text
progress = max(speed_norm, cadence_norm)
```

Zaleta: aktualne zachowanie, najlepsza responsywność.

Ryzyko: 2 km/h + 100 rpm daje prawie FAST mimo małej prędkości roweru.

### Policy B — weighted blend

Przykład koncepcyjny:

```text
progress = weighted(speed_norm, cadence_norm)
```

Nie ustalać wag bez testów.

Cel: wysoka kadencja może przyspieszyć reakcję, ale nie przejmuje jej w 100% przy bardzo wolnym rowerze.

### Policy C — cadence authority gated by wheel speed

Koncepcja:

```text
cadence_effective = cadence_norm * speed_authority
progress = max(speed_norm, cadence_effective)
```

Przy 0–4 km/h cadence ma ograniczony wpływ; wraz z prędkością odzyskuje pełną władzę.

To jest **bardziej odpowiednie do przetestowania w FOC EVistDrive** niż bezpośrednie kopiowanie TSDZ2, ale nie może zostać uznane za docelowe bez A/B.

---

# 15. Torque RUN — zalecany plan A/B

Nie zmieniać równocześnie RUN i Iq governor.

Najpierw governor, potem estimator.

## 15.1. Baseline A

```text
AFILT: 35 ms
RUN: rise 120 ms / fall 350 ms
```

To jest aktualne zachowanie.

## 15.2. Candidate B — angle window

Publikować istniejący crank-angle average jako RUN.

Testować minimum:

```text
90°
120°
180°
```

180° jest obecnym historycznym defaultem okna, ale nie zakładać, że będzie najlepszy.

### Ryzyko

Powrót do czystego angle-window może przywrócić:

- wolny wzrost po coasting;
- zależność czasu reakcji od prędkości obrotu korby w sensie realnego czasu;
- opóźnione odbudowanie targetu przy małej kadencji.

## 15.3. Candidate C — dual-path RUN

Jeżeli A jest zbyt pikowe, a B zbyt ospałe, dopiero wtedy testować dual-path.

Rekomendowana idea:

```text
angle baseline = crank-angle average
fast signal    = AFILT

rising intent:
    baseline może być doganiany szybciej,
    ale nie skacze wprost do AFILT

falling intent:
    większa waga angle baseline,
    aby pojedynczy trough jednej nogi nie zrzucał momentu
```

Nie używać przypadkowego `max(AFILT, angle_avg)`, bo może to utrzymywać sztucznie wysoki moment.

Lepszy wzorzec:

- bounded convergence;
- angular confirmation;
- jawny rise/fall state;
- brak hidden latches.

---

# 16. Hybrid TSDZ2 — co można wykorzystać później

Nie jest to konieczne do naprawy obecnej dynamiki.

Jeżeli projekt chce kiedyś mieć prawdziwy TSDZ-like Hybrid, należy stworzyć go jawnie jako nowy mode semantics:

```text
Torque branch → Iq_torque
Power branch  → Iq_power

Iq_mode_request = max(Iq_torque, Iq_power)
```

Następnie:

```text
Iq_mode_request
→ common ceilings
→ Iq_allowed
→ common Reference Governor
→ FOC
```

Ważne:

- oba branch muszą być w tej samej domenie `Iq`;
- power branch nie może być ukrytym sufitem, jeżeli mode ma być nazywany Hybrid;
- diagnostyka ma eksportować `Iq_torque`, `Iq_power` i zwycięski branch;
- governor ma pozostać wspólny i działać **po wyborze mode request**, a przed FOC.

Nie implementować Hybrid poprzez dublowanie dwóch różnych ramp.

---

# 17. Rozdzielenie „request”, „allowed” i „reference”

To jest obowiązkowy kontrakt architektoniczny.

## 17.1. `Iq_requested`

Oznacza:

> ile momentu chce assist mode przed limitami ochronnymi / systemowymi.

## 17.2. `Iq_allowed`

Oznacza:

> ile wolno w tym ticku po wszystkich limitach.

## 17.3. `Iq_ref`

Oznacza:

> ile governor faktycznie oddaje do current FOC po ograniczeniu dynamiki.

Docelowy chain:

```text
Iq_requested
    ↓
min / arbitration of limits
    ↓
Iq_allowed
    ↓
Reference Governor
    ↓
Iq_ref
```

Nie wolno mieszać:

```text
limiter response
```

z:

```text
rider-feel ramp
```

bez jawnej polityki.

Przykład: thermal limiter obniża `Iq_allowed`. Governor może zejść do nowej wartości z kontrolowanym fall slew, ale hard overcurrent musi mieć niezależne prawo natychmiastowego wyłączenia.

---

# 18. Normal stop vs safety/fault stop

Nowy governor nie może naruszyć istniejącego rozdzielenia.

## Normal rider release

```text
rider demand → 0
↓
release_ms envelope
↓
Iq_ref → 0
↓
coast / graceful bridge lifecycle
```

## Soft safety derating

Np. temperatura / napięcie / legal speed:

```text
Iq_allowed maleje
↓
governor schodzi kontrolowanie
```

## Hard safety

Np. prawdziwy overcurrent / krytyczny fault:

```text
HARD FAULT
↓
nie czekaj na ride-feel governor
↓
niezależny hard shutdown
```

Nie wolno sprawić, aby jerk limiter opóźniał ochronę sprzętu.

---

# 19. Timebase V2 — parametry w ms muszą znaczyć ms

Obecne moduły często używają:

```text
CONTROL_TICKS_PER_MS = 4
```

oraz aktualizują stan na każde wywołanie.

To działa tylko przy idealnej relacji:

```text
1 call = 250 us
```

Docelowo jeden z dwóch wariantów:

## Wariant preferowany

Governor jest wywoływany z deterministycznego producer ticku i dostaje dokładnie jeden update na 4 kHz hardware tick.

## Wariant alternatywny

Governor dostaje:

```text
elapsed_ticks
```

lub monotoniczny `now_tick` i liczy delta.

### Zakaz

Nie wolno „naprawić” problemu przez zmianę wszystkich stałych z 4 na inną liczbę bez naprawy własności timebase.

---

# 20. Telemetria obowiązkowa przed zmianą algorytmu

Przed wprowadzeniem Governor V2 agent ma rozszerzyć istniejącą diagnostykę o minimalny zestaw obserwowalności.

## 20.1. Sygnały wejściowe

```text
raw_torque
corrected_torque
torque_delta_native
torque_assist_delta
torque_afilt
torque_arun
load_centikg
cadence_rpm
wheel_speed_x100
motor_erps
```

## 20.2. Mode pipeline

```text
support_ratio_pct
startup_boost_active
startup_boost_extra_pct
assist_basis_power_w
raw_motor_power_w
filtered_motor_power_w
Iq_before_PU
Iq_request
```

## 20.3. Ride control

```text
permission
latched
session_state
recovery_state
Iq_after_latch_floor
Iq_after_limits / Iq_allowed
Iq_pre_ramp
smooth_start_active
preload_active
coast_release
```

## 20.4. Dynamics

Dodać:

```text
adaptive_speed_progress
adaptive_cadence_progress
selected_progress
selected_up_ms
selected_down_ms
selected_max_slew_up
selected_max_slew_down
current_slew
current_jerk_state
Iq_ref
```

Dla baseline starego algorytmu co najmniej:

```text
selected_up_ticks
selected_down_ticks
step_q
Iq_scale
abs_delta_to_target
```

## 20.5. Electrical response

```text
Iq_actual
Id_actual
Battery_Current
battery_voltage
motor_power
motor_voltage_utilization
PWM/MOE/bridge_state
```

---

# 21. Najważniejsze metryki

Samo „wydaje się lepiej” nie wystarcza do zamknięcia karty.

Dla każdego kontrolowanego step response liczyć:

```text
t_first_change
t10
t50
t90
t95
settling_time
max_abs_dIq_dt
max_abs_d2Iq_dt2
Iq_overshoot
Iq_actual_tracking_delay
max_abs_dPower_dt
area(|Iq_allowed - Iq_ref|)
```

Dodatkowo dla torque path:

```text
AFILT peak-to-peak
ARUN peak-to-peak
Iq_request peak-to-peak
phase delay vs crank angle
```

---
# 22. Host test matrix — obowiązkowa przed jazdą

## 22.1. Step amplitudes

Testować przynajmniej:

### Rise

```text
0  → 10%
10 → 20%
10 → 50%
10 → 90%
30 → 60%
50 → 90%
```

### Fall

```text
90 → 70%
90 → 40%
90 → 10%
60 → 30%
20 → 10%
```

### Release

```text
20 → 0
50 → 0
90 → 0
```

Release musi osobno weryfikować `release_ms`.

## 22.2. Speed × cadence matrix

Minimum:

```text
wheel speed [km/h]: 0, 2, 4, 6, 10, 15, 20, 25
cadence [rpm]:      0, 30, 50, 60, 80, 100, 110, 120
```

Ważne punkty graniczne:

```text
3.9 / 4.0 / 4.1 km/h
19.9 / 20.0 / 20.1 km/h
49 / 50 / 51 rpm
109 / 110 / 111 rpm
```

Celem jest potwierdzenie:

- clamp;
- monotonic adaptation;
- brak discontinuity na progach;
- brak szybszego niż FAST;
- brak wolniejszego niż SLOW, o ile polityka tego nie przewiduje.

## 22.3. Level matrix

Każdy ważny test uruchomić dla:

```text
L1
L3
L5
```

Pełny regression dla L1–L5 po wyborze finalnego algorytmu.

## 22.4. Target-changes-during-ramp

Testy:

```text
10 → 90%, po 100 ms zmiana na 40%
90 → 10%, po 100 ms zmiana na 70%
20 → 80 → 30 → 60% w krótkich odstępach
```

Wymagania:

- brak skoku `Iq_ref`;
- brak zmiany znaku slew przez niekontrolowany snap;
- governor płynnie przejmuje nowy target;
- żadnego overshootu poza `Iq_allowed`.

---

# 23. Testy torque estimator

## TQ-A — sine-like pedalling ripple

Generator ma symulować powtarzalny nacisk dwóch nóg.

Kadencja:

```text
20 / 40 / 60 / 80 / 100 / 120 rpm
```

Mierzyć:

```text
AFILT ripple
RUN ripple
Iq_request ripple
```

Cel:

- RUN ma redukować trough/peak bez dużej straty średniej;
- wynik nie powinien gwałtownie zmieniać charakteru wraz z cadence.

## TQ-B — genuine slow rise

Nacisk liniowo:

```text
0 → 20 kg
```

w kilku czasach:

```text
250 ms
500 ms
1 s
2 s
```

Celem jest rozdzielenie:

```text
„realna zmiana intencji”
```

od:

```text
„jedna noga weszła w peak”
```

## TQ-C — fast press

```text
2 kg → 20 kg step
```

Sprawdzić, czy governor V2 łagodzi finalny `Iq_ref` nawet jeśli estimator prawidłowo i szybko rozpozna intencję.

To jest ważne: **nie trzeba opóźniać wykrycia nacisku, aby wolniej zbudować moment silnika.**

## TQ-D — quick unload/reload

Symulować krótkie odpuszczenie pedału w środku normalnego pedałowania.

Celem jest uniknięcie:

```text
moment → prawie 0 → moment
```

przy pojedynczym trough jednej nogi.

---

# 24. Timebase fault injection

Powtórzyć kluczowe testy przy:

```text
normal 4 kHz calls
co drugi call pominięty przez krótki burst
co czwarty call pominięty
losowe krótkie gaps
```

Dla wersji V2 parametry opisane jako ms powinny zależeć od **elapsed hardware time**, a nie od liczby szczęśliwie wykonanych main-loop passes.

Kryterium:

```text
różnica t90 / release time między baseline 4 kHz i missed-call case
```

ma być wyjaśniona i ograniczona.

Docelowo w dobrze zrobionym timebase różnica nie powinna wynikać z utraty logicznych ticków governora.

---

# 25. Hardware A/B — scenariusze jazdy

Po przejściu host tests.

## H1 — wolno + wysoka kadencja

To najważniejszy test zgłoszonego problemu.

```text
speed:   2–5 km/h
cadence: 80–100 rpm
```

Wykonać:

- lekki wzrost nacisku;
- duży wzrost nacisku;
- częściowe odpuszczenie;
- pełne odpuszczenie.

Porównać baseline vs Governor V2.

## H2 — wolno + niska kadencja / podjazd

```text
speed:   3–8 km/h
cadence: 30–60 rpm
```

Nie wolno zrobić roweru „martwego”.

Kryterium subiektywne:

- pomoc ma pojawić się wcześnie;
- moment ma budować się bez szarpnięcia;
- brak wymagania przesadnego nacisku przed reakcją.

## H3 — normal trail

```text
speed:   10–20 km/h
cadence: 60–90 rpm
```

Sprawdzić szybkie zmiany nacisku po korzeniach / wyjściu z zakrętu.

## H4 — wysoka dynamika

L4/L5, wysoka kadencja.

Cel:

- brak „gumowego” opóźnienia;
- zachowany sportowy charakter;
- mniejszy jerk niż baseline.

## H5 — start z miejsca

Oddzielnie od normalnej rampy.

Weryfikować:

```text
startup boost
smooth start
preload
neutral dwell / bridge start
Iq governor
```

Nie wolno dopuścić, aby nowy governor zepsuł anti-clunk preload albo zrobił 2-sekundowy crawl.

## H6 — rolling re-engage

Po krótkim odpuszczeniu i ponownym nacisku podczas toczenia.

Nowy governor nie może ponownie wprowadzić problemów FW-112 recovery.

---

# 26. Kryteria akceptacji Governor V2

## MUST PASS — funkcjonalne

- brak overshoot `Iq_ref > Iq_allowed`;
- monotoniczne dojście do stałego targetu;
- exact target convergence;
- release do zera kończy się zgodnie z `release_ms`;
- brake / hard fault zachowują wymagany safety path;
- Walk Assist nie dostaje dodatkowego niezamierzonego lag;
- Hall calibration/service mode nie dostaje stale Iq;
- rolling-rearm same-tick-zero pozostaje zachowane;
- preload nadal ma pierwszeństwo na świeżym starcie;
- brak arithmetic overflow przy maksymalnym Iq;
- brak float/division-heavy work w fast ISR.

## MUST PASS — ride feel

W hardware A/B:

- duża zmiana momentu ma być mniej gwałtowna na początku;
- mała korekta pedałem nie może mieć wielkiego dodatkowego laga;
- wolny podjazd nie może być gorszy niż baseline;
- szybka jazda nie może stać się „gumowa”;
- release nie może przeciągać mocy bardziej niż skonfigurowano.

## MUST PASS — diagnostyka

Dla każdego epizodu musi być możliwe wskazanie:

```text
czy target był szybki,
czy limiter go przyciął,
czy governor go opóźnił,
czy FOC nie nadążył.
```

---

# 27. Kolejność wdrożenia — zalecana karta po karcie

## ETAP A — AUDIT/BASELINE FREEZE

**Bez zmian produkcyjnych.**

1. Zapisz git branch / HEAD / status / diff.
2. Zapisz aktywny runtime bank L1–L5.
3. Uruchom obecne host tests.
4. Wygeneruj baseline matrix z rozdziału 22.
5. Zachowaj CSV i summary.

Output:

```text
ASSIST_DYNAMICS_BASELINE_0.0408.md
baseline/*.csv
```

## ETAP B — OBSERVABILITY

Dodać wyłącznie brakujące pola diagnostyczne:

```text
selected_up_ms
selected_down_ms
step_q
Iq_scale
abs_target_delta
```

oraz późniejsze pola V2.

Nie zmieniać ride feel.

Output ma udowodnić, że trace baseline jest identyczny przed/po patchu poza nową telemetrią.

## ETAP C — REFERENCE GOVERNOR V2, HOST-ONLY ENABLE

Wprowadzić nowy moduł, np.:

```text
assist_reference_governor.c
assist_reference_governor.h
```

Nie wciskać całego algorytmu do `ride_control.c`.

Minimalny API:

```text
input:
  Iq_allowed
  current Iq_ref
  speed
  cadence
  per-level dynamics
  elapsed tick/time
  flags: normal/release/safety/walk/service

output:
  Iq_ref
  debug state
```

Najpierw host build / compile flag.

## ETAP D — JERK A/B

Porównać:

```text
V1 current slew
V2 slew+jerk mild
V2 slew+jerk medium
```

Nie zmieniać torque RUN w tym etapie.

Wybrać najbardziej obiecujący wariant na podstawie metryk i hardware.

## ETAP E — ADAPTIVE POLICY A/B

Dopiero po potwierdzeniu samego governora:

```text
fastest-wins
weighted blend
speed-gated cadence authority
```

Nie stroić równocześnie jerk i adaptive mapping.

## ETAP F — TORQUE RUN A/B

Dopiero gdy finalny Iq governor nadal pozostawia problem per-leg peak/fall.

Porównać 120/350 vs angle window / dual-path.

## ETAP G — OPTIONAL TRUE HYBRID

Osobna funkcja. Nie część poprawki ramp.

---

# 28. Zasady implementacji modułu Governor V2

## 28.1. Jeden owner stanu

Stan:

```text
Iq_ref_q
slew_q
release state
```

ma należeć do jednego modułu.

Nie trzymać części stanu w `ride_control`, części w `main.c`, części w `assist_dynamics`.

## 28.2. Fixed point

Użyć istniejącej filozofii Q-format.

Minimalnie:

```text
Iq_ref_q
slew_q
```

muszą mieć wystarczający zapas, aby nie utracić małych kroków przy długiej rampie.

## 28.3. Exact clamp on target crossing

Po integracji:

```text
if rise crossed target → target
if fall crossed target → target
```

Nie zostawiać oscylacji 1 LSB wokół targetu.

## 28.4. Zero handling

Normal release do zera:

- zachowuje release semantics;
- kończy dokładnie na zero;
- kasuje residual slew.

## 28.5. Target reversal

Jeżeli podczas rise target spadnie poniżej `Iq_ref`, governor nie może natychmiast przełączyć z `+max_slew` na `-max_slew`.

Jerk limiter powinien płynnie wyhamować dodatni slope i dopiero zbudować ujemny.

To jest jeden z najważniejszych przypadków, których obecny prosty slew limiter nie modeluje.

---

# 29. Parametry konfiguracyjne — czego NIE dodawać od razu do UI

Pierwszy patch powinien używać compile-time / internal experimental parameters.

Nie rozszerzać natychmiast persisted 35-B level record o:

```text
jerk_up
jerk_down
cadence_weight
speed_weight
```

zanim nie będzie wiadomo, które parametry naprawdę są potrzebne.

Po wyborze algorytmu można zaprojektować wersjonowany bank migration.

## Zakaz

Nie wolno po cichu zmienić semantyki istniejącego pola `iq_rise_slow_ms` z:

```text
full-scale slew time
```

na:

```text
fixed time for every transition
```

bez:

- migracji;
- nowej dokumentacji;
- aktualizacji UI;
- testu starych zapisanych banków.

---

# 30. Propozycja semantyki parametrów docelowych

Najczytelniejszy wariant:

## `iq_rise_fullscale_ms`

Jawnie oznacza:

> czas odpowiadający przejściu przez 100% dostępnej skali przy ustalonym max slew.

## `iq_fall_fullscale_ms`

Analogicznie dla fall.

## `iq_rise_jerk_ms`

Czas, w którym slew może przejść od 0 do wybranego max rise slew.

## `iq_fall_jerk_ms`

Analogicznie fall.

## `release_ms`

Pozostaje:

> rzeczywisty czas od poziomu w chwili pełnego release do 0, przed coast-release/final lifecycle.

Taka nomenklatura usuwa obecne nieporozumienie, że każda zmiana ma trwać `iq_rise_ms`.

---
# 31. Czego NIE kopiować z TSDZ2

## 31.1. PWM duty ramp 1:1

EVistDrive ma FOC i bezpośrednią regulację `Iq`. Portowanie TSDZ2 duty controllera jako drugiej pętli za FOC stworzyłoby konflikt regulatorów.

**Nie robić:**

```text
Iq PI → duty
plus osobny TSDZ duty ramp manipulator
```

## 31.2. Battery-current target jako główna domena torque

EVistDrive powinien pozostać w domenie `Iq` dla momentu.

Battery current jest limiterem / power constraint, nie zamiennikiem q-axis torque reference.

## 31.3. Stałe ADC / torque z TSDZ2

Inny sensor, inna elektronika, inna kalibracja.

## 31.4. Range cadence 20–70 rpm bez testu

To, że TSDZ2 używa 20–70, nie znaczy, że M820 ma używać tych samych progów.

## 31.5. Smooth Start jako uniwersalny filtr

TSDZ2 Smooth Start jest częścią konkretnej architektury. W EVistDrive launch smoothing, normal ride governor i rolling re-engage mają być rozdzielone.

---

# 32. Co warto przenieść koncepcyjnie z TSDZ2

## 32.1. Multi-stage shaping

Nie jedna rampa „załatwiająca wszystko”, ale:

```text
sensor shaping
→ mode demand
→ limit arbitration
→ actuator/reference shaping
```

## 32.2. Feedback-aware ride feel

Nie kopiować battery-current gate, ale pamiętać, że TSDZ2 nie buduje momentu wyłącznie z otwartego timera.

EVistDrive może uzyskać odpowiednik przez:

- dobry Iq governor;
- `Iq_actual` observability;
- opcjonalną detekcję tracking lag;
- bez zmieniania inner FOC.

## 32.3. Jasne rozdzielenie startup od normal ride

Start z miejsca ma inne wymagania niż zmiana nacisku przy 15 km/h.

## 32.4. Adaptive response

SLOW/FAST zależny od stanu jazdy ma sens. Wymaga jednak ponownego strojenia w nowej domenie.

---

# 33. Opcjonalny tracking-aware governor — dopiero po V2

Jeżeli po slew+jerk nadal będzie potrzebne dodatkowe „TSDZ-like” uspokojenie, można rozważyć governor świadomy tracking error:

```text
tracking_error = Iq_ref - Iq_actual
```

Koncepcja:

```text
jeżeli Iq_actual wyraźnie nie nadąża:
    nie zwiększaj jeszcze szybciej Iq_ref
```

To daje pewne podobieństwo do TSDZ2 current-gated actuator.

### Ale uwaga

Nie implementować tego w pierwszej wersji.

Ryzyka:

- błędny current feedback może sztucznie blokować moment;
- może wejść w interakcję z battery-current limiterem;
- może ukrywać problem current loop;
- może stworzyć dodatkową pętlę dynamiki i oscylację.

Najpierw prosty, mierzalny outer governor.

---

# 34. Interakcja z limiterami

Governor ma pracować na `Iq_allowed`, ale limiter state powinien być obserwowalny.

## 34.1. Limiter entry

Przy spadku limitu:

```text
Iq_requested = 300
Iq_allowed   = 180
```

normalnie governor schodzi do 180 zgodnie z polityką fall.

Jeśli limiter jest krytyczny/safety, może istnieć osobna szybsza fall policy.

## 34.2. Limiter recovery

Przy powrocie limitu:

```text
180 → 300 allowed
```

nie powinien pojawić się natychmiastowy skok ani integrator artefact.

To należy testować jako osobną klasę:

```text
rider demand stały
limit zmienia się
```

nie mieszać z testem rider step.

---

# 35. Interakcja z obecnym gear preload

W `ride_control.c` istnieje:

```text
PRELOAD_IQ_CAP = 10
PRELOAD_TIMEOUT ≈ 300 ms
```

Preload ma za zadanie spokojnie wybrać luz przekładni przy stojącym rotorze.

Nowy governor musi zachować kolejność logiczną:

```text
mode / limits
↓
smooth-start, jeśli aktywny
↓
preload cap
↓
reference governor
```

albo równoważny kontrakt zapewniający, że governor nigdy nie obejdzie preload.

Nie wolno zrobić:

```text
governor najpierw buduje duże Iq_ref
→ preload próbuje ograniczyć już po fakcie
```

---

# 36. Interakcja z coast release i cichym STOP

Aktualny kod ma specjalne mechanizmy kończenia pracy:

- fixed-time release;
- low-speed coast release;
- bridge lifecycle / neutralization;
- osobne prace nad cichym start/stop.

Governor V2 nie może ich zastąpić jednym „ładnym S-curve”.

Sekwencja normalnego stopu powinna pozostać logicznie:

```text
rider releases
↓
normal controlled Iq release
↓
Iq small / zero
↓
low-speed coast / graceful final stop policy
↓
neutral / bridge lifecycle
```

Nie scalać tej karty z przebudową MOE/neutral/Hall stop state.

---

# 37. Interakcja z FW-112 rolling rearm

To obszar wysokiego ryzyka regresji.

Istniejąca logika posiada:

- session state;
- fast-rearm edge;
- torque recovery automaton;
- same-tick zero;
- WAIT_FRESH_LOAD / TRACK_FAST semantics;
- recovery stable window.

Governor V2 musi mieć jasny kontrakt:

## Fast rearm + final demand = 0

```text
Iq_ref = 0 SAME TICK
slew state = 0
```

Nie może zostać stary dodatni slew.

## Fast rearm + real positive demand

Nowy target przechodzi przez normalny governor bez odziedziczonego pre-reverse stanu.

## Recovery collapse

Nie wolno pozostawić dodatniego slope z poprzedniego targetu.

Testy FW-112 muszą wejść do mandatory regression suite.

---

# 38. Interakcja z Walk Assist

Aktualny kod celowo daje WA pełną własność trajektorii `Iq` i omija shared ride ramp.

To ma pozostać.

Nie wprowadzać Governor V2 za Walk Assist bez osobnego projektu, bo:

- WA ma własny speed controller;
- drugi dynamic element może pogorszyć stabilność;
- start/hold/fade WA są inne niż pedal assist.

---

# 39. Minimalny plan zmian w kodzie

## Pliki prawdopodobnie dotknięte

### Nowe

```text
inc/assist_reference_governor.h
src/assist_reference_governor.c
```

### Integracja

```text
src/ride_control.c
inc/ride_control.h          — tylko jeśli trzeba do debug snapshot
```

### Config / bank

Na początku najlepiej **bez zmiany persistent layout**.

### Diagnostics

W zależności od istniejącego miejsca:

```text
fw112 / iq_chain / nowy diag-only snapshot
```

### Tests

```text
tests/host/assist_dynamics/
```

lub zgodnie z istniejącą architekturą testów repo.

## Pliki, których nie ruszać w pierwszym patchu

```text
FOC.c
current sampling
Hall interpolation
PWM timing
battery SOC
CAN protocol produkcyjny, poza diag-only
```

---

# 40. Suggested API — szkic, nie kod do bezmyślnego wklejenia

```c
struct assist_reference_governor_input {
    int32_t iq_allowed;
    uint16_t speed_x100;
    uint16_t cadence_rpm;
    uint32_t elapsed_ticks;

    uint16_t rise_slow_ms;
    uint16_t rise_fast_ms;
    uint16_t fall_slow_ms;
    uint16_t fall_fast_ms;
    uint16_t release_ms;

    bool normal_release;
    bool force_zero;
    bool hard_cut;
};

struct assist_reference_governor_output {
    int32_t iq_ref;
    int32_t slew_q;
    uint16_t effective_rise_ms;
    uint16_t effective_fall_ms;
    uint8_t state;
};
```

Uwaga: nazwy są przykładowe. Najpierw dopasować do konwencji repo.

---

# 41. Pseudocode — baseline-compatible V2

```text
if hard_cut:
    use independent safety path

if force_zero:
    ref = 0
    slew = 0
    reset release state
    return

if walk/service owns Iq:
    synchronize governor state
    bypass normal shaping
    return owner target

compute adaptive full-scale rise/fall policy

if normal_release_to_zero:
    keep current exact release_ms semantics
    optionally jerk-limit only if it does not break guaranteed release time
else:
    desired_slew = full-scale slope from selected profile
    apply jerk limit to slew
    integrate Iq_ref
    exact-clamp on target crossing
```

## Ważna decyzja dla release

Pierwsza implementacja powinna **zachować release path bez eksperymentalnego jerk**, aby nie zmienić jednocześnie dwóch już działających własności.

Po stabilizacji można osobno A/B sprawdzić S-curve release, ale z gwarancją, że total release time nie rośnie ukrycie.

---

# 42. Przykładowe eksperymentalne warianty jerk

Nie są to rekomendowane wartości produkcyjne.

Host A/B może mieć trzy profile:

```text
J0: jerk disabled — obecny slope step
J1: krótki slope ramp
J2: średni slope ramp
```

Ważniejsza jest relacja niż konkretna liczba.

Po testach wybierać na podstawie:

- max dIq/dt;
- max d2Iq/dt²;
- t10/t50/t90;
- odczucia startu dużego step;
- małych korekt pedałem.

Nie wybierać tylko na podstawie całkowitego czasu dojścia.

---

# 43. Czego oczekiwać po prawidłowej poprawce

Dobry wynik nie powinien wyglądać jak:

```text
wszystko wolniejsze
```

Powinien wyglądać jak:

```text
mała korekta pedałem:
  szybko zauważona
  mały i naturalny response

większy step:
  brak natychmiastowego pełnego slope
  płynne zbudowanie siły
  bez gumowego opóźnienia

release częściowy:
  brak gwałtownego dropu
  ale też brak przeciągania

pełny release:
  przewidywalny release_ms
```

---

# 44. Definition of Done dla całego tematu

Temat można uznać za zamknięty dopiero, gdy:

1. istnieje baseline 0.0408 z pełną matrycą;
2. wiadomo, czy symptom powstaje głównie w torque estimator, mode request czy Iq governor;
3. Governor V2 przechodzi host matrix;
4. wszystkie FW-112/rearm regressions przechodzą;
5. start/preload nie jest gorszy;
6. normal release zachowuje przewidywalny czas;
7. hardware A/B potwierdza mniejszy jerk przy dużym step;
8. nie ma zauważalnego pogorszenia małych korekt;
9. L1–L5 są sprawdzone;
10. zapisane banki mają jasną migrację lub zachowują semantykę;
11. dokumentacja opisuje faktyczne znaczenie parametrów;
12. log potrafi jednoznacznie wskazać first divergence.

---

# 45. Gotowy zakres zadania dla agenta

Poniższy blok może zostać użyty jako karta wdrożeniowa po zatwierdzeniu kierunku.

## TASK

**EVISTDRIVE ASSIST DYNAMICS V2 — TSDZ2-INSPIRED FEEL WITHOUT PWM PORT**

## MODE

```text
PRE-AUDIT
→ BASELINE MEASUREMENT
→ DIAGNOSTIC OBSERVABILITY
→ HOST-ONLY GOVERNOR V2
→ A/B TEST
→ HARDWARE CANDIDATE
→ REPORT
```

## PRIMARY GOAL

Zmniejszyć odczuwalny nagły przyrost/spadek momentu EVistDrive, szczególnie dla dużej zmiany rider demand, bez:

- zwalniania current FOC;
- kopiowania PWM ramp TSDZ2;
- zwiększania wszystkich czasów w ciemno;
- psucia rolling rearm;
- psucia cichego start/stop;
- psucia Walk Assist.

## REQUIRED EVIDENCE

Przed zmianą produkcyjną pokaż:

```text
git branch
git HEAD
git status
git diff --stat
active assist bank L1–L5
baseline host response matrix
```

## MUST PRESERVE

```text
release-to-zero exact-time semantics
same-tick zero for FW-112 rearm
Walk Assist ownership of Iq trajectory
service/calibration bypass
hard-fault independent shutdown
gear preload
current FOC gains and loop rate
```

## FIRST IMPLEMENTATION

Dodać outer `Iq reference governor` z:

```text
bounded slew
bounded jerk
adaptive rise/fall
exact target clamp
explicit timebase
```

Nie zmieniać torque RUN w tej samej karcie.

## REQUIRED DIAGNOSTICS

```text
Iq_requested
Iq_allowed
Iq_ref
Iq_actual
selected rise/fall
current slew
adaptive speed/cadence contribution
force_zero/release state
```

## HOST TESTS

Wykonać pełną matrycę z rozdziałów 22–24.

## HARDWARE

Najważniejszy scenariusz:

```text
2–5 km/h
80–100 rpm
mały i duży step nacisku
częściowy i pełny release
```

## STOP CONDITION

Jeżeli host nie pokazuje poprawy jerk przy zachowaniu t50/t90 w akceptowalnym zakresie — **nie flashować**.

Jeżeli hardware pokazuje większy start lag lub gorszy rolling re-engage — wrócić do danych i nie stroić przypadkowymi stałymi.

---

# 46. Otwarte pytania przed finalnym strojeniem

Te pytania nie blokują stworzenia hostowego Governor V2, ale blokują finalne ustawienia produkcyjne.

1. Jakie są dokładne aktywne wartości banku użytkownika podczas zgłaszanego problemu?
2. Jaki jest realny rozkład `ΔIq_request` w typowej jeździe — 10%, 30%, 70% skali?
3. Czy największy subiektywny „strzał” koreluje z `Iq_pre_ramp`, `Iq_ref` czy `Iq_actual`?
4. Czy problem częściej występuje przy wysokiej cadence i niskiej wheel speed?
5. Czy P/U ceiling jest w tych epizodach aktywny?
6. Czy 120/350 ordinary RUN zwiększa short-term peak względem angle-window na realnym logu?
7. Jaki jerk w `Iq_actual` jest jeszcze niewyczuwalny / komfortowy?
8. Czy różne poziomy L1–L5 powinny mieć wspólny jerk time, czy skalowany z support ratio?
9. Czy cadence adaptation w FOC powinna pozostać fastest-wins, czy być ograniczona przy bardzo małej wheel speed?

---

# 47. Decyzje, które można podjąć już teraz

## PEWNE

- nie kopiować TSDZ2 PWM ramp do EVistDrive;
- nie zwalniać inner FOC dla ride feel;
- zwykłe EVistDrive `ramp_ms` jest full-scale slew semantics;
- partial target changes są odpowiednio krótsze;
- pełny release ma osobną, lepszą fixed-time semantykę;
- speed/cadence adaptive logic jest podobna do TSDZ2;
- `map()` jest clamped;
- wolny rower + wysoka kadencja może wybrać szybką rampę;
- Smooth Start EVistDrive nie jest odpowiednikiem całej dodatkowej dynamiki TSDZ2;
- ordinary RUN publikuje 120/350 ms time filter zamiast istniejącego angle-window average;
- nie ma oczywistego błędu jednostki torque sensora.

## NAJBARDZIEJ PRAWDOPODOBNY KIERUNEK POPRAWY

```text
Reference Governor V2
= current absolute slew concept
+ jerk limiting
+ ponownie zwalidowana adaptive policy
+ prawdziwy timebase
```

## DRUGI KROK

A/B torque RUN estimator.

## OSOBNY PRZYSZŁY TEMAT

Prawdziwy TSDZ-like Hybrid jako dwa generatory `Iq`.

---

# 48. Krótkie podsumowanie dla przyszłego agenta

Jeżeli agent ma przeczytać tylko jedną sekcję, niech przeczyta tę.

**Nie szukaj rozwiązania w zwiększaniu `ramp_up_ms`.**

EVistDrive i TSDZ2 nie rampują tej samej wielkości. TSDZ2 ma adaptive PWM duty ramp dodatkowo bramkowaną przez rzeczywisty prąd. EVistDrive rampuje już `Iq_ref`, a szybki FOC realizuje go bez tej dodatkowej bezwładności. Aktualny EVistDrive ma zwykły full-scale slew limiter, więc częściowe zmiany targetu kończą się proporcjonalnie szybciej niż liczba `ramp_ms`. Do tego slope pojawia się natychmiast — brak jerk limitera. To jest najbardziej logiczne miejsce na poprawę ride feel.

Torque sensor scale nie wygląda na źle przeniesioną jednostkę. Natomiast ordinary RUN po FW-112.4 publikuje time-domain 120/350 ms zamiast angle-domain 180° moving average, mimo że okno nadal jest liczone. To powinno zostać później porównane A/B, ale nie równocześnie z nowym Iq governor.

Najbezpieczniejsza kolejność:

```text
1. baseline + telemetry
2. Iq Reference Governor V2: slew + jerk
3. adaptive policy A/B
4. hardware A/B
5. torque RUN A/B
6. dopiero później ewentualny true Hybrid
```

---

# 49. Referencje kodu EVistDrive 0.0408

Najważniejsze miejsca w snapshot 28.08:

| Temat | Plik | Lokalizacja orientacyjna |
|---|---|---:|
| generic full-scale ramp semantics | `src/assist_dynamics.c` | komentarz ok. l.39; step ok. l.172 |
| speed/cadence adaptive selection | `src/assist_dynamics.c` | ok. l.96–116 |
| fixed-time profile release | `src/assist_dynamics.c` | ok. l.134–169 |
| map clamp | `src/main.c` | ok. l.4084–4098 |
| ordinary RUN 120/350 | `src/torque_input.c` / `inc/torque_input.h` | `update_run_asym_filter`; constants l.124–125 headera |
| angle window 180° | `inc/torque_input.h` | `TORQUE_RUN_WINDOW_DEG_DEFAULT` |
| published RUN override | `src/torque_input.c` | ok. l.756–766 |
| default levels dynamics | `src/assist_modes.c` | ok. l.114–130 |
| load→Iq request | `src/assist_modes.c` | `calculate_load_iq_request()` |
| P/U ceiling | `src/assist_modes.c` | `finish_power_request()` |
| smooth start placement | `src/ride_control.c` | ok. l.889 |
| gear preload | `src/ride_control.c` | `PRELOAD_IQ_CAP`, block ok. l.900 |
| final dynamics call | `src/ride_control.c` | ok. l.1004 |
| smooth-start state machine | `src/assist_start.c` | `assist_start_apply_smooth()` |

---

# 50. Publiczne referencje TSDZ2

Repozytorium:

`https://github.com/emmebrusa/TSDZ2-Smart-EBike-1`

Referencyjne pliki na branch `master`:

- `src/ebike_app.c` — `set_motor_ramp()`, `apply_smooth_start()`, `apply_hybrid_assist()`, `get_pedal_torque()`;
- `src/motor.c` — PWM duty controller, battery-current / phase-current / undervoltage gating;
- release history — potwierdza rozwój Smooth Start / Hybrid i zmian w torque sensor calibration.

Dla tego dokumentu publiczne źródło było odczytane 2026-08-30. Przy przyszłym wdrożeniu nie zakładać, że `master` pozostał niezmieniony — zapisać commit/tag użyty do porównania.

---

# 51. Finalna rekomendacja

**Kierunek wdrożenia: TAK — warto poprawić.**

Nie ma dowodu na prostą pomyłkę współczynnika torque sensora. Jest natomiast silny dowód architektoniczny, że przeniesiono z TSDZ2 część idei adaptive ramp do innej domeny sterowania, przez co jej odczuwalny efekt jest inny.

Największą wartość powinien dać:

```text
Iq Reference Governor V2
```

który zachowa szybki FOC, ale ograniczy nie tylko `dIq/dt`, lecz również nagłość zmiany tego slope. Następnie należy osobno zwalidować ordinary RUN filter i dopiero na końcu rozważać prawdziwą semantykę Hybrid.

**Nie wdrażać wszystkich tych zmian w jednym patchu.**

