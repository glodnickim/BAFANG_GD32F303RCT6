# FW-127 PRE-AUDIT — spójność próbki prądu i kontekstu PWM

**Status:** wyłącznie audyt i projekt. **Zero zmian w kodzie, zero nowych obrazów.**
**Zamrożone:** FW-126.7 = NORMAL 0.0442, checkpoint `1668b00`.

---

## 1. Graf fast-loopu — stan faktyczny

Wszystkie numery linii z `src/main.c` po commicie `1668b00`.

```
  [okres PWM P]  CH3 match na zboczu OPADAJĄCYM (FW-126.2, zmierzone)
        │
        │  CONV + L = 588 zliczeń = 4,9 µs
        ▼
  3708  ADC0_1_IRQHandler — wejście
  3712  foc_adc1_eoic_at_entry  ← jedyny dowód „to jest NOWA konwersja"
  3719  JDR read  (ADC2/ADC1/ADC0)                          ← PRÓBKA N
  3731  cal_jdr[] — kopia RAW dla kalibracji                ← N
  3737  foc_current_feedback_note_fresh()                   ← świeżość, NIE ważność
  3748  odjęcie current_cal.offset[]                        ← N
  3761  dyn_adc_state_select(switchtime, MS.char_dyn_adc_state)   ← czyta switchtime N ✔
  3762  dyn_adc_state_reconstruct(...)                      ← rekonstrukcja N ✔
  3765  Hall / q31_rotorposition_absolute                   ← stan „teraz", nie N
  3792  dyn_adc_trigger_update()   ← czyta switchtime N, PROGRAMUJE CH3 dla N+1  ✘
  3830  FOC_calculation(...)       ← Clarke/Park/PI/inv.Park/SVPWM → switchtime := N+1
  3838  CCR0/1/2 ← switchtime[0..2]                          ← N+1, shadow WYŁĄCZONY
```

`TIMER_OC_SHADOW_DISABLE` na CH0/1/2 i CH3 ([main.c:1808](../src/main.c#L1808) i sąsiednie) —
**każdy zapis compare działa natychmiast**, nie na zdarzeniu update. Granica „okresu" jest więc
programowo rozmyta: druga połowa okresu P już jedzie na wartościach N+1.

---

## 2. Tabela własności N / N+1

| element | opisuje | producent | konsument | kiedy nadpisywany | latch? | ryzyko innego cyklu |
|---|---|---|---|---|---|---|
| `i16_ph1/2/3_current` | **N** | 3719 JDR | 3748, 3762, 3830 | co ISR | nie | — |
| `cal_jdr[]` | **N** | 3731 | `current_cal_sample` | co ISR (const, lokalne) | **tak** | nie |
| `foc_adc1_eoic_at_entry` | **N** | 3712 | 3737 | lokalne | tak | nie |
| `foc_current_sample_seq/tick` | **N** | 3737 | diagnostyka | co ISR | tak | nie |
| `current_cal.offset[]` | stała sesji | `current_cal.c` | 3748 | raz na boot | tak | nie |
| `switchtime[]` | **N przed 3830, N+1 po** | `svpwm()` | 3761, **3792**, 3838 | **w środku ISR** | **NIE** | **TAK** |
| `MS.char_dyn_adc_state` | **N** | 3761 | 3762 | co ISR | **tak** | nie |
| stan w `dyn_adc_trigger_update` | **N** (użyty jako N+1) | 3893 (własne wywołanie) | CH3 | nie zapisany nigdzie | **NIE** | **TAK** |
| `q31_rotorposition_absolute` | „teraz" | 3765–3789 | 3830 | co ISR | nie | częściowo |
| CH3 (`TIMER_CH3CV`) | **N+1** | 3792 | sprzęt | co ISR | sprzętowo | **TAK** |
| CCR0/1/2 | **N+1** | 3838 | sprzęt | co ISR, natychmiast | sprzętowo | — |

---

## 3. GŁÓWNE PYTANIE

> Czy firmware może w jednym ISR połączyć próbkę N z sektorem z innej transakcji PWM i nowym
> switchtime N+1?

**Dla rekonstrukcji: NIE.** FW-120.1 to naprawił — [main.c:3761](../src/main.c#L3761) czyta
`switchtime[]` **zanim** `FOC_calculation()` je nadpisze, więc para 2-z-3 odpowiada geometrii,
w której próbka fizycznie powstała. Ta własność jest zdrowa i **musi przetrwać każdą przebudowę**.

**Dla wyzwalacza: TAK, i to systematycznie.**

```
switchtime[] = geometria N
        │
        ├──► 3761/3762  rekonstrukcja próbki N            ✔ spójne
        │
        └──► 3792  dyn_adc_trigger_update()
                     └─► CH3 := f(geometria N)
                            ▲
                            │  ale próbka, którą ten trigger wyzwoli, powstanie pod:
        3838  CCR0/1/2 := geometria N+1  ────────────────► ✘ NIESPÓJNE
```

Producent→konsument, dokładne lokalizacje:

- **producent geometrii N+1:** `svpwm()` w [FOC.c:246-264](../src/FOC.c#L246-L264), wywołany
  z `FOC_calculation()` w [main.c:3830](../src/main.c#L3830)
- **konsument, który powinien go zobaczyć:** `dyn_adc_trigger_update()`
  w [main.c:3891](../src/main.c#L3891) — ale jest wołany w [main.c:3792](../src/main.c#L3792),
  **38 linii wcześniej**
- **skutek:** okno próbkowania dla akwizycji N+1 wyliczone z rankingu wypełnień z okresu N

---

## 4. Potwierdzone błędy spójności

### B1 — trigger N+1 z geometrii N *(POTWIERDZONE, statyczne)*

Opisany wyżej. Jest **utajony** dopóki B2 trzyma gałąź dynamiczną poza zasięgiem — patrz niżej.

### B2 — `DYNAMIC_ADC_THRESHOLD = _T`: gałąź NIE jest martwa, jest gorzej

Karta pytała, czy `DYNAMIC_ADC_THRESHOLD = _T = 3750` czyni gałąź nieosiągalną. **Nie.**

`switchtime[]` **nie jest nigdzie ograniczany** — między `svpwm()` a
`timer_channel_output_pulse_value_config()` nie ma ani jednego clampa. Arytmetyka (sektor 2&5,
gdzie `Y − Z = U_alpha`):

```
switchtime[0] = ((_T + U_alpha) >> 12) + _T/2
U_alpha       = (_SQRT3 · _T · u_alpha) >> 4 = (28 · 3750 · u_alpha) >> 4 = 6562,5 · u_alpha

switchtime[0] > _T   ⟺   _T + U_alpha > 1875 · 4096 = 7 680 000
                     ⟺   u_alpha > 7 676 250 / 6562,5 = 1169,7
```

Przemiatanie pełnego obrotu elektrycznego po wszystkich trzech sektorach daje wiążący
współczynnik **1,602** (sektor 2&5, `switchtime[0]`), więc odchylenie od środka wynosi
`1,602 · u_abs` i przekracza `_T/2 = 1875` przy **u_abs > 1170**. Innymi słowy: **wystarczy
obserwować `u_abs`** — `u_alpha` nie jest potrzebne, bo `|u_alpha| ≤ u_abs` z równością raz
na obrót.

Ograniczenie kołowe w `runPIcontrol()` ([main.c:3526](../src/main.c#L3526)) dopuszcza
`u_abs ≤ _U_MAX = 1920`, a `|u_alpha| ≤ u_abs`. Zatem przedział **u_alpha ∈ (1170; 1920]** jest
osiągalny — to około **61 % limitu kołowego**, nie egzotyczny róg.

Przy `u_alpha = 1920`: `switchtime[0] = 4952`, czyli **przekracza ARR = 3750**.

Wtedy i tylko wtedy `highest > DYNAMIC_ADC_THRESHOLD` jest prawdą, a gałąź programuje
`CH3 = highest − 10 = 4942` — **też powyżej ARR**. Compare, który nigdy nie dopasuje.

> **Dwa błędy maskują się nawzajem.** B1 jest niewidoczny, bo w normalnej pracy gałąź jest
> nieosiągalna i CH3 stoi na `TRIGGER_DEFAULT = 3740` (zgodne ze wszystkimi logami z roweru).
> Gałąź staje się osiągalna dokładnie wtedy, gdy wartość, którą liczy, jest bezużyteczna.

**Konsekwencja braku triggera:** brak zdarzenia CC3 → brak konwersji → **ISR nie wchodzi** →
`switchtime[]` zostaje zamrożony na wartości przemodulowanej → pętla prądowa martwa.

**Czy się odratuje?** `uint16_half_rotation_counter` (jedyna ścieżka miękkiego odcięcia,
[main.c:1427](../src/main.c#L1427)) rośnie w `reg_ADC_processing()` z pętli głównej, więc żyje
niezależnie od ISR — **ale jest zerowany przy każdym zdarzeniu Hall**
([main.c:2099](../src/main.c#L2099), [:2127](../src/main.c#L2127)). Przy **kręcącym się
wirniku odcięcie nigdy nie zadziała.**

> **DEFEKT UDOWODNIONY, częstość nieznana.** Decyzja właściciela (polityka testów sprzętowych):
> brak clampa, osiągalne `switchtime > ARR`, wynikające z tego `CH3 > ARR` i utrata konwersji
> są **udowodnione statycznie i arytmetycznie**. To, czy `u_abs > 1170` wystąpiło w konkretnej
> jeździe, wpływa na **praktyczną osiągalność i częstość**, nie na **istnienie** defektu.
> Dlatego nie jest to bramka blokująca — patrz §11.

### B3 — dwie niezależne, rozbieżne oceny stanu próbkowania na ISR

- [3761](../src/main.c#L3761): `dyn_adc_state_select(switchtime, MS.char_dyn_adc_state)` — z **historią**
- [3893](../src/main.c#L3893): `dyn_adc_state_select(switchtime, DYN_ADC_STATE_UNDECIDED)` — **bez historii**

Przy remisie wypełnień (`switchtime[i] == switchtime[j]`, brak ostrego maksimum) funkcja zwraca
argument `previous`. Pierwsze wywołanie zwróci wtedy stan zapamiętany, drugie —
`UNDECIDED` → `default: return`, czyli „zostaw CH3 gdzie jest". **Dwie odpowiedzi na to samo
pytanie w jednym ISR**, i żadna nie jest zapisana jako kontekst próbki.

### B4 — brak modelu ważności; zła próbka wchodzi wprost do FOC

Jedyny dostępny predykat to `foc_adc1_eoic_at_entry` → `foc_current_feedback_note_fresh()`
([3737](../src/main.c#L3737)) = **„konwersja się zakończyła"**. To NIE jest dowód, że trigger
trafił w okno przewodzenia dolnych kluczy.

Komentarz w kodzie mówi to wprost ([3834](../src/main.c#L3834)):
*„Do not set `valid` here: a completed conversion still has no derived physical PWM
sampling-window proof, which is the FW-127 work intentionally not guessed."*

Czyli: **dziś nie istnieje PRIMARY / ALTERNATE / INVALID.** Próbka bez dowodu okna idzie prosto
do Clarke → Park → PI.

### B5 — decyzja próbkowania na wartości ŻĄDANEJ, nie zastosowanej

`dyn_adc_state_select()` dostaje `switchtime[]` — wartość wyliczoną. Rzeczywisty `CCR` może się
różnić (§B2: wartości > ARR dają 100 % wypełnienia, a nie „switchtime/ARR"). **Nie ma clampa,
więc nie ma nawet czego porównać.** Geometria, według której wybierany jest sektor, i geometria,
która realnie występuje na mostku, mogą być różne bez żadnego sygnału.

### B6 — brak atomowego kontekstu

`sector` (`MS.char_dyn_adc_state`), `direct_mask` (nie istnieje — jest zaszyty w `switch` wewnątrz
`dyn_adc_state_reconstruct`), `trigger_ccr` (tylko w rejestrze), `seq` (`foc_current_sample_seq`,
osobny licznik) — **cztery rozłączne miejsca**, żadnej struktury opisującej jedną konwersję.

---

## 5. Martwe / nieosiągalne gałęzie

| gałąź | status |
|---|---|
| `if(highest > DYNAMIC_ADC_THRESHOLD)` w `dyn_adc_trigger_update()` | **nieosiągalna w normalnej pracy**, osiągalna tylko przy `u_alpha > ~1170`, gdzie liczy CH3 > ARR |
| `default: return` tamże | osiągalna wyłącznie przy remisie wypełnień; cicho zostawia CH3 z poprzedniego okresu |
| `DYN_ADC_STATE_UNDECIDED` w `dyn_adc_state_reconstruct` | osiągalna; celowo nic nie rekonstruuje |

`TRIGGER_DEFAULT = _T − 10 = 3740` jest więc **efektywnie stałą**, co potwierdzają wszystkie
logi z roweru (FW-126.2, FW-126.5, FW-126.7: CH3 = 3740).

---

## 6. Własność rekonstrukcji

- **wybór pary:** `MS.char_dyn_adc_state`, ustawiany [3761](../src/main.c#L3761), **zatrzaśnięty**
  w `MotorState_t` i konsumowany natychmiast w [3762](../src/main.c#L3762).
- **czy zatrzaśnięty dla konkretnej próbki ADC?** Tak — wartość jest wyliczona i użyta między
  odczytem JDR a `FOC_calculation()`, więc opisuje tę i tylko tę próbkę. **To jest zdrowe.**
- **ale:** jest to zatrzask *implicytny* (kolejność w ISR), nie jawna struktura. Nic nie broni
  przyszłej edycji przed przesunięciem `FOC_calculation()` w górę i cichym zepsuciem tego.
  FW-120.1 opisuje dokładnie ten błąd sprzed swojej naprawy.

---

## 7. Proponowany model ważności (PROJEKT, nie wdrożenie)

```c
typedef enum {
    CURRENT_SAMPLE_INVALID = 0,   /* okno nie zostało dowiedzione            */
    CURRENT_SAMPLE_PRIMARY,       /* wszystkie trzy bocznikami przewodziły    */
    CURRENT_SAMPLE_ALTERNATE      /* dwie ważne, trzecia z rekonstrukcji 2-z-3 */
} current_sample_state_t;

typedef struct {
    uint8_t  sector;        /* geometria, w której ta konwersja powstała     */
    uint8_t  state;         /* current_sample_state_t                        */
    uint8_t  direct_mask;   /* bit na fazę: 1 = zmierzona wprost             */
    uint16_t trigger_ccr;   /* CH3 faktycznie zaprogramowany dla tej konwersji */
    uint32_t seq;           /* numer konwersji, do wykrycia zgubionej        */
} current_sample_context_t;
```

**Polityka INVALID** (bez ustalania jeszcze maksimum wieku):

```
próbka ważna:    last_valid = Iabc;  sample_age = 0
próbka INVALID:  Iabc = last_valid;  sample_age++
```

Maksimum `sample_age` **celowo nierozstrzygnięte** — wymaga pomiaru, nie założenia.

---

## 8. Docelowy przepływ (PROJEKT)

```
kontekst N zatrzaśnięty w poprzednim ISR
        ▼
trigger N (CH3 z kontekstu N) ──► konwersja N ──► IRQ
        ▼
1. snapshot ctx N            (odczyt zatrzaśniętej struktury, nie rejestrów)
2. odczyt + walidacja JDR N  (seq, freshness, okno z ctx N)
3. rekonstrukcja Iabc N      wg ctx N.direct_mask  ← zachowuje własność FW-120.1
4. PRIMARY / ALTERNATE / INVALID  → last_valid / sample_age
5. Clarke / Park / PI / inv. Park
6. SVPWM → switchtime N+1
7. CLAMP do rzeczywistej geometrii  ← NOWE, dziś nie istnieje
8. wyliczenie sektora próbkowania N+1 z wartości PO clampie
9. wyliczenie CH3 N+1 z tej samej geometrii
10. ATOMOWY latch ctx N+1
11. zapis CCR0/1/2/3 N+1
```

Kluczowa różnica: **kroki 7-9 dziś nie istnieją albo dzieją się przed krokiem 6.**

---

## 9. Co usunąć / zastąpić

| plik / funkcja | los |
|---|---|
| `dyn_adc_trigger_update()` ([main.c:3891](../src/main.c#L3891)) | **usunąć** — zastąpione krokami 8-9 po SVPWM |
| drugie wywołanie `dyn_adc_state_select(..., UNDECIDED)` | **usunąć** — jedna ocena na konwersję |
| `MS.char_dyn_adc_state` | **zastąpić** polem `sector` w kontekście |
| `DYNAMIC_ADC_THRESHOLD` | **usunąć** — próg wyprowadzany z geometrii po clampie, nie stała równa `_T` |
| `src/dyn_adc_state.c` | **zachować logikę 2-z-3**, przenieść wybór do producenta kontekstu |
| `foc_current_feedback_*` | **zachować** (świeżość) i **uzupełnić** o ważność okna — to dwie różne rzeczy |
| clamp `switchtime[]` | **dodać** — dziś nie istnieje nigdzie |

---

## 10. Testy hosta (wymagane)

- H-A1 clamp: `switchtime` powyżej `_T` zostaje przycięty; poniżej — nietknięty
- H-B1 kontekst: jeden `seq` na konwersję, brak dziur, wykrycie zgubionej
- H-B2 atomowość: konsument nigdy nie widzi mieszanki pól z N i N+1
- H-C1 sektor liczony z geometrii **po** clampie, nie z żądanej
- H-C2 CH3 dla N+1 wyprowadzony z tej samej geometrii co CCR0/1/2 N+1
- H-C3 **regresja FW-120.1**: rekonstrukcja próbki N nadal używa geometrii N
- H-D1 INVALID → `last_valid` + `sample_age++`; ważna → `sample_age = 0`
- H-D2 rekonstrukcja 2-z-3 zgodna z `direct_mask`
- H-D3 próbka bez dowodu okna **nie może** dojść do Clarke

## 11. Testy sprzętowe — polityka właściciela

### H1 — praktyczna osiągalność B2: **UNKNOWN, NIE jest bramką**

Sprawdzone, czy da się to wyciągnąć z tego, co już istnieje:

| źródło | wynik |
|---|---|
| telemetria NORMAL 0.0442 | **nie zawiera** `u_abs` ani `switchtime` |
| `diag_peak_u_abs` (blok 0x6029) | istnieje, ale **wyłącznie w DIAG** (`CAN_Display.c:874` guard) |
| wszystkie zebrane logi | **zero** odpowiedzi `822C6029` — blok nigdy nie był odczytany |

Wniosek: istniejąca telemetria **nie wystarcza**. Zgodnie z polityką H1 zostaje jako
**UNKNOWN PRACTICAL REACHABILITY**. Firmware **nie jest** modyfikowany, żeby to zebrać,
i **nie planuję** osobnego testu na rowerze.

> To pytanie **nie ginie — wchłania je FW-127A.** Po dodaniu clampa „czy geometria przekroczyła
> ARR" staje się licznikiem zadziałań clampa, który obowiązkowy test #2 zbiera pasywnie, za
> darmo. Odpowiedź przyjdzie później i taniej, zamiast osobnego przejazdu teraz.

### OBOWIĄZKOWY TEST SPRZĘTOWY #1 — jedyny wymagany dziś

```
NORMAL 0.0442
  → produkcyjna kalibracja w wybiegu neutralnym
  → CAL VALID
  → zwolnienie AKTYWNEGO FOC
  → silnik startuje i jedzie normalnie
```

To domyka **zachowanie produkcyjne** FW-126.7. Dowodów samej kalibracji **nie powtarzamy** —
zostały potwierdzone przez DIAG (log 2026-08-27 12:05, 11/11 kryteriów). Ten test odpowiada na
jedno nowe pytanie: czy po zwolnieniu FOC z niezerowymi offsetami silnik zachowuje się poprawnie.

### OBOWIĄZKOWY TEST SPRZĘTOWY #2 — jeden, po całym FW-127

Dopiero po zaimplementowaniu i przetestowaniu na hoście **kompletnego** potoku A+B+C+D.
**JEDEN FLASH, JEDNA SESJA, JEDEN SUROWY LOG.** Instrumentacja **pasywna** — recorder nie może
zmieniać zachowania sterowania.

Musi zebrać w jednym przejeździe:

- brak CCR0/1/2 poza legalnym zakresem ARR (licznik zadziałań clampa — **tu wraca H1**)
- brak CH3 poza legalnym zakresem ARR
- spójność `seq` kontekstu N/N+1
- liczniki PRIMARY / ALTERNATE / INVALID
- ciągłość konwersji ADC (brak zgubionych)
- dowód, że INVALID **nie** wpuszcza podejrzanego prądu do FOC
- działanie last-valid
- rozkład `sample_age` (brak nieoczekiwanego narastania)
- zachowanie przy małym Iq / małym prądzie
- praca przy normalnym i wysokim obciążeniu

### Kiedy wolno poprosić o dodatkowy test

Tylko wtedy, gdy fakt wymagany dla **bezpieczeństwa elektrycznego** nie da się udowodnić
statycznie ani z istniejących dowodów. „Dobrze byłoby zmierzyć" **nie wystarcza.**
Każda prośba musi wprost odpowiedzieć:

1. **JAKA NIEWIADOMA blokuje bezpieczeństwo lub poprawność?**
2. **DLACZEGO nie da się jej udowodnić z kodu / istniejących logów?**
3. **JAKĄ DECYZJĘ odblokuje ten jeden test?**

W przeciwnym razie — nie prosić.

---

## 12. Polityka wdrożenia FW-127

Po zaliczeniu testu #1: **A/B/C/D jako osobne karty i commity**, ale **bez testu na rowerze po
każdej z nich.** Między etapami wyłącznie testy hostowe / statyczne / modelowe.

| karta | zakres | bramka |
|---|---|---|
| **A** | clamp rzeczywistej, zastosowanej geometrii PWM | host |
| **B** | atomowy kontekst próbki N/N+1 | host |
| **C** | sektor / stan / CH3 wyprowadzane z **zaclampowanego** SVPWM N+1 | host |
| **D** | PRIMARY / ALTERNATE / INVALID + last-valid + instrumentacja `sample_age` | host |

Kolejność jest wymuszona: **C nie ma sensu bez A** (nie ma czego clampować),
**D bez B** (nie ma gdzie zapisać werdyktu).

**Zepsutej architektury akwizycji nie zostawiamy obok nowej.** Każda karta zastępuje swój
fragment, a nie dokłada warstwę — ta sama reguła, która rozstrzygnęła FW-126.5 i FW-126.7.

## 13. Kryteria STOP

- **STOP**, jeśli jakakolwiek zmiana naruszy własność FW-120.1 (test H-C3 czerwony) — para
  rekonstrukcji dla próbki w ręku jest jedyną rzeczą, która dziś działa.
- **STOP** przed dotknięciem produkcyjnego FOC, dopóki **test #1 nie przejdzie**: inaczej nie da
  się odróżnić regresji FW-127 od nieprzetestowanego FW-126.7.
- **STOP**, jeśli miałoby powstać drugie miejsce decydujące o oknie próbkowania.
- **STOP** i osobna prośba (wg trzech pytań w §11), jeśli w trakcie wdrożenia wyjdzie fakt
  bezpieczeństwa elektrycznego niedowodliwy statycznie.
---

## ODPOWIEDŹ

> **IS CURRENT ACQUISITION ARCHITECTURALLY COHERENT TODAY?**
>
> ## PARTIAL

**Co jest spójne:** para rekonstrukcji 2-z-3 dla próbki w ręku. FW-120.1 wywalczył tę własność
i ona trzyma — `switchtime[]` czytane w [3761](../src/main.c#L3761) naprawdę opisuje okres,
w którym próbka powstała. Każda przebudowa musi ją zachować, a nie odkryć ponownie.

**Co nie jest spójne:**

1. **Trigger dla N+1 liczony z geometrii N** ([3792](../src/main.c#L3792) przed
   [3830](../src/main.c#L3830)) — udowodnione statycznie, nie hipoteza.
2. **Brak modelu ważności.** Próbka bez dowodu okna wchodzi wprost do FOC; sam firmware to
   przyznaje w komentarzu.
3. **Decyzja na wartości żądanej, nie zastosowanej** — i nie ma clampa, więc te dwie mogą się
   rozjechać bez śladu.
4. **Brak atomowego kontekstu** — sektor, maska, trigger i sekwencja żyją w czterech miejscach.
5. **Gałąź dynamiczna osiągalna tylko tam, gdzie liczy wartość niemożliwą** (CH3 > ARR),
   a utrata triggera przy kręcącym się wirniku **nie ma ścieżki odratowania**.

Punkt 5 jest wynikiem arytmetycznym i **wymaga pomiaru H1, zanim ktokolwiek go nazwie przyczyną
czegokolwiek**. Jeśli się potwierdzi, jest pilniejszy niż całe FW-127.

**Nie implementuję. STOP.**
