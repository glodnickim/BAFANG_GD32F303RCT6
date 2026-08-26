# BAFANG M820 STOCK CURRENT ADC SYNCHRONIZATION

## WYNIK GŁÓWNY

**Stockowa definicja „NEW COMPLETE CURRENT SAMPLE SET” jest oparta na `ADC0 EOIC`, nie na koniunkcji flag kilku ADC.**

Dla normalnego toru prądowego stock realizuje:

```text
TIMER0 TRGO
    ↓
ADC0 + ADC1 inserted-parallel acquisition
    ↓
rank 1 obu ADC
    ↓
rank 2 obu ADC
    ↓
ADC0 EOIC
    ↓
IRQ18 @ 0x08005578
    ↓
0x08007604
    ↓
0x0800B17C
    ↓
ADC0 JDR1 + ADC1 JDR1 + ADC1 JDR2
```

### Najważniejsza odpowiedź dla FW-126.3

```text
ADC0_EOIC &&
ADC1_EOIC &&
ADC2_EOIC
```

# **NO — NIE JEST TO STOCK-CORRECT**

Stock:

- nie używa ADC2 do zestawu trzech phase-current values,
- nie sprawdza ADC1 EOIC przed wejściem do current path,
- nie wykonuje `ADC0_EOIC && ADC1_EOIC`,
- włącza EOIC interrupt dla **ADC0/master**,
- `IRQ18` sprawdza **tylko `ADC0_STAT.EOIC`**,
- po tym evencie firmware bez dodatkowego gate'u czyta JDR-y ADC0 i ADC1.

Zakres analizy pozostaje dokładnie taki, jak w zadaniu: stock M820 ADC synchronization, bez zmian EVistDrive, bez gate-drivera, CAN i projektowania testów.

---

# 1. ADC0 CONFIG

## Peripheral

```text
ADC0 = 0x40012400
```

**CONFIRMED — BIN**

Konfiguracja zaczyna się w:

```text
0x0800B5C6
```

Kluczowe zapisy:

```text
0x0800B5CC  mode = 0x00010000
0x0800B5D2  scan = 1
0x0800B5DE  field = 0x000E0000
0x0800B5E4  alignment = 0x0800
0x0800B5EA  channel count = 8
0x0800B5F6  ADC config write
```

`0x00010000` w polu sync-mode odpowiada:

```text
ADC_DAUL_REGULAL_PARALLEL_INSERTED_PARALLEL
```

czyli:

**ADC0 i ADC1 pracują w kombinowanym trybie regular-parallel + inserted-parallel.** Jest to dokładna nazwa trybu w oficjalnej bibliotece GD32F30x.

**CONFIRMED**

---

## ADC0 inserted sequence

Długość:

```text
0x0800B72C
r1 = 2
BL 0x08005C58
```

Helper zapisuje:

```text
ISQ.IL = length - 1
```

czyli inserted group ma:

# **2 ranki**

**CONFIRMED**

Konfiguracja:

```text
0x0800B736:
ADC0 rank 1
channel = 2
sample-time encoding = 1

0x0800B744:
ADC0 rank 2
channel = 0
sample-time encoding = 1
```

Zatem:

| Rank | Channel | Data register |
|---|---:|---|
| 1 | ADC channel 2 | ADC0 JDR1 / `0x4001243C` |
| 2 | ADC channel 0 | ADC0 JDR2 / `0x40012440` |

**CONFIRMED**

---

# 2. ADC1 CONFIG

## Peripheral

```text
ADC1 = 0x40012800
```

**CONFIRMED**

ADC1 dostaje konfigurację od:

```text
0x0800B60A
```

Po wyzerowaniu structury nie jest mu ponownie wpisywane `0x10000`.

To jest prawidłowe dla układu master/slave: sync-mode znajduje się po stronie ADC0/master.

---

## ADC1 inserted sequence

```text
0x0800B766
length = 2
```

Ranki:

```text
0x0800B76E:
rank 1
channel = 3
sample-time encoding = 1

0x0800B79C:
rank 2
channel = 5
sample-time encoding = 1
```

Tabela:

| Rank | Channel | Data register | Phase-current usage |
|---|---:|---|---|
| 1 | ADC channel 3 | `ADC1 JDR1 = 0x4001283C` | B |
| 2 | ADC channel 5 | `ADC1 JDR2 = 0x40012840` | C |

**CONFIRMED**

Co ważne, ADC0 i ADC1 mają:

- tę samą długość inserted group = 2,
- ten sam sample-time encoding dla rank1,
- ten sam sample-time encoding dla rank2.

To usuwa problem asymetrycznej długości dwóch równoległych sekwencji.

---

# 3. MULTI-ADC MODE

Raw stock value:

```text
ADC0 sync mode = 0x00010000
```

Semantyka GD32:

```text
ADC_DAUL_REGULAL_PARALLEL_INSERTED_PARALLEL
```

Oficjalny opis:

> ADC0 and ADC1 work in combined regular parallel + inserted parallel mode.



### Wynik

Inserted conversion nie jest dwiema niezależnymi transakcjami ADC.

Jest to:

```text
ADC0 MASTER
     +
ADC1 PARTNER
     ↓
INSERTED PARALLEL
```

**CONFIRMED**

---

# 4. MASTER / SLAVE RELATIONSHIP

## ADC0

Stock ustawia:

```text
0x0800B752
inserted trigger selector = 0x0000
```

Dla GD32F30x jest to:

```text
ADC0_1_EXTTRIG_INSERTED_T0_TRGO
```

czyli:

# **TIMER0 TRGO**

Oficjalna tabela GigaDevice dokładnie mapuje to źródło na TIMER0 TRGO dla inserted group.

Następnie:

```text
0x0800B75C
external inserted trigger enable = 1
```

**CONFIRMED**

---

## ADC1

Stock:

```text
0x0800B7A8
inserted trigger selector = 0x7000
```

czyli selector `111`, odpowiadający programowemu/no-external source dla inserted group.

Jednocześnie:

```text
0x0800B7B2
external inserted trigger enable = 1
```

Ale stockowa procedura **nigdzie nie generuje software inserted trigger dla ADC1** podczas current acquisition.

### Producer proof

Mamy więc jednocześnie:

```text
ADC0 = hardware TIMER0 TRGO
ADC1 = brak własnego niezależnego hardware trigger source
ADC mode = ADC0+ADC1 inserted parallel
```

### Wniosek

ADC1 jest uruchamiany jako partner przez synchronizację z ADC0.

**CONFIRMED jako stock configuration**  
**STRONG jako dokładny hardware master/slave ordering**

---

# 5. TIMER0 TRGO → ADC CHAIN

Z poprzedniego stock audit:

```text
TIMER0
CH3 / użytkowy CH4
CV = 1999
PWM mode 1
        ↓
O3CPRE
        ↓
TIMER0 TRGO
```

ADC0 selector:

```text
TIMER0 TRGO
        ↓
ADC0 inserted trigger
```

Multi-ADC:

```text
ADC0 inserted start
        ↓
ADC0 + ADC1 inserted-parallel
```

Nie występuje tu:

```text
ADC0 trigger
+
ADC1 independent trigger
```

ani:

```text
software trigger ADC0
software trigger ADC1
```

**CONFIRMED**

---

# 6. INSERTED SEQUENCE ORDER

Dla jednego `TIMER0 TRGO`:

```text
TIMER0 TRGO
      ↓
START inserted-parallel
      ↓
┌──────────────────┬──────────────────┐
│ ADC0             │ ADC1             │
├──────────────────┼──────────────────┤
│ rank1: channel 2 │ rank1: channel 3 │
│ → JDR1           │ → JDR1           │
├──────────────────┼──────────────────┤
│ rank2: channel 0 │ rank2: channel 5 │
│ → JDR2           │ → JDR2           │
└──────────────────┴──────────────────┘
      ↓
inserted group complete
```

`EOIC` w nomenklaturze biblioteki GD32F30x to **end of inserted group conversion interrupt**, a więc zdarzenie dotyczące zakończenia grupy, nie pojedynczego ranku.

**CONFIRMED**

---

# 7. JDR UPDATE ORDER

Phase-current registers interesujące stock:

```text
A = ADC0 JDR1
B = ADC1 JDR1
C = ADC1 JDR2
```

Sekwencja oznacza:

```text
rank1:
 ADC0 JDR1 ← new A-related sample
 ADC1 JDR1 ← new B-related sample

rank2:
 ADC0 JDR2 ← new rank2 auxiliary sample
 ADC1 JDR2 ← new C-related sample

then:
 inserted group completion
```

Zatem `C` jest szczególnie istotne:

**ADC1 JDR2 jest drugim rankiem.**

Jeżeli firmware wchodzi do current consumer po completion of inserted group i natychmiast czyta ADC1 JDR2, to jego model świeżości jednoznacznie zakłada zakończenie również rank2 partnera ADC1.

---

# 8. ADC0 FLAGS

Normalny IRQ:

```text
0x08005578
```

początek:

```asm
0x0800557A  load ADC0 base
0x0800557C  LDR ADC0_STAT
0x0800557E  AND #0x4
0x08005582  CMP #0x4
0x08005584  BNE exit
```

Czyli warunek wejścia:

# **ADC0_STAT bit 2 = EOIC**

**CONFIRMED**

Po spełnieniu:

```asm
0x08005586  MVN r0
0x08005588  load ADC0 base
0x0800558A  STR r0,[ADC0_STAT]
```

czyli firmware **jawnie kasuje ADC0 EOIC software'owo**.

Następnie natychmiast:

```text
0x0800558C
ADC0 + 0x40
↓
read ADC0 JDR2
```

To jest dodatkowy bardzo mocny producer→consumer proof:

```text
ADC0 EOIC
↓
ADC0 JDR2 is treated as complete
```

---

# 9. ADC1 FLAGS

W current-sense initialization:

```text
0x0800B7CA
r1 = 0x480
r2 = 1
r0 = ADC0
BL 0x080058EC
```

Helper `0x080058EC`:

```text
uxtb interrupt-mask
OR into ADC_CTL0
```

Low byte:

```text
0x480 & 0xFF = 0x80
```

czyli:

```text
EOICIE
```

jest włączone dla:

# **ADC0**

**CONFIRMED**

Nie występuje odpowiadający temu call w tej inicjalizacji dla ADC1.

Jeszcze ważniejsze: `IRQ18 @ 0x08005578` **nie odczytuje `ADC1_STAT`**.

Nie ma:

```text
ADC1 EOIC?
```

przed wywołaniem current consumer.

### Wynik

**ADC1 EOIC nie jest stockowym freshness gate.**

**CONFIRMED**

---

# 10. FLAG CLEAR SEMANTICS

Stock robi:

```text
ADC0 EOIC detected
↓
write ADC0_STAT
↓
EOIC cleared
↓
read JDR
```

Czyli firmware **nie polega na odczycie JDR jako metodzie skasowania EOIC**.

Oficjalna dokumentacja biblioteki GD32 rozróżnia osobno operacje:

```text
adc_interrupt_flag_get()
adc_interrupt_flag_clear()
```

i identyfikuje EOIC jako flagę końca inserted group.

Dodatkowo dokumentacja ADC GigaDevice tej architektury opisuje EOIC jako ustawiane na końcu ostatniej konwersji grupy i wymagające software clear; sam odczyt data register nie jest traktowany jako jego clear.

### Wniosek praktyczny

`EOIC=1` jest **latched state**, nie „jednocyklowym impulsem świeżości”, który automatycznie znika po odczycie JDR.

Dlatego koniunkcja różnych EOIC ma sens jako freshness criterion tylko wtedy, gdy każdy bit jest odpowiednio zarządzany/zerowany dla każdego cyklu.

Stock czegoś takiego nie robi dla ADC1.

---

# 11. IRQ18 ENTRY CONDITION

Dokładna bramka:

```c
if ((ADC0_STAT & 0x4) != 0x4)
    return;
```

Nie ma:

```c
if (!(ADC0_EOIC && ADC1_EOIC))
    return;
```

Nie ma również:

```c
ADC2_STAT
```

w tej bramce.

### Stock IRQ entry event

# **ADC0 EOIC**

**CONFIRMED**

---

# 12. CURRENT IRQ CALL CHAIN

Po ADC0 EOIC:

```text
IRQ18
0x08005578
   ↓
clear ADC0 EOIC
   ↓
read ADC0 JDR2
   ↓
state checks
   ↓
0x080055A8
BL 0x08007604
   ↓
0x08007606
BL 0x0800810C
   ↓
0x08007612
BL 0x0800B17C
```

Current consumer:

```text
0x0800B17C
```

nie wykonuje kolejnego EOIC gate.

**CONFIRMED**

---

# 13. WHICH EVENT MEANS FRESH SAMPLE?

Dla normalnego toru FOC:

# **ADC0 EOIC**

To właśnie ten event jest przez stock traktowany jako:

```text
NEW INSERTED CURRENT ACQUISITION COMPLETE
```

Dowód software:

1. tylko ADC0 EOIC powoduje wejście do IRQ processing;
2. ADC0 EOIC jest kasowane;
3. bez sprawdzania ADC1 firmware przechodzi do consumer;
4. consumer czyta również ADC1 rank2.

### Status

**CONFIRMED — stock software contract**

---

# 14. ARE ADC0/ADC1 JDRs FRESH AT IRQ ENTRY?

## Stock software answer

# **TAK**

Firmware jednoznacznie tak je traktuje.

Po ADC0 EOIC nie ma:

```text
wait for ADC1
```

ani:

```text
check ADC1 EOIC
```

a mimo to używa:

```text
ADC0 JDR1
ADC1 JDR1
ADC1 JDR2
```

**CONFIRMED jako zachowanie/założenie stock firmware.**

---

## Hardware justification

Warunki:

```text
ADC0+ADC1 = inserted parallel
inserted length ADC0 = 2
inserted length ADC1 = 2
sample-time(rank1) ADC0 = ADC1
sample-time(rank2) ADC0 = ADC1
start = common synchronized event
```

Do tego EOIC oznacza zakończenie inserted group.

Dlatego w momencie zakończenia master sequence partner ADC1 również kończy odpowiadającą sekwencję parallel.

### Hardware freshness

```text
ADC0 JDR1 = fresh
ADC1 JDR1 = fresh
ADC1 JDR2 = fresh
```

przed wejściem do current consumer.

**STRONG**

Nie podnoszę tego do wyższego poziomu tylko dlatego, że dostępny publiczny opis GD32F30x nie podaje cykl-po-cyklowym diagramem relacji EOIC obu przetworników. Zachowanie stocku pokazuje jednak jednoznacznie, że właśnie na tej gwarancji Bafang polega.

---

# 15. DOES STOCK REQUIRE BOTH EOIC?

# **NO**

Stock nie wykonuje:

```text
ADC0 EOIC && ADC1 EOIC
```

Normalny current IRQ sprawdza:

```text
ADC0 EOIC
```

i nic więcej.

Kalibracja również nie sprawdza dwóch EOIC.

### Wynik

**CONFIRMED: STOCK DOES NOT REQUIRE BOTH ADC0 AND ADC1 EOIC AS A FRESHNESS GATE.**

---

# 16. DOES STOCK REQUIRE THREE EOIC?

# **NO**

W zidentyfikowanych ścieżkach:

### Normal current acquisition

```text
ADC0 EOIC only
```

### Calibration

```text
wait
→ read one JDR
```

### ADC2

nie bierze udziału w A/B/C consumer.

Nie ma odpowiednika:

```text
ADC0 EOIC
AND ADC1 EOIC
AND ADC2 EOIC
```

# **CONFIRMED: STOCK DOES NOT USE A THREE-EOIC CONJUNCTION.**

---

# 17. WHY CAL READS JDR WITHOUT EOIC POLLING

To nie przeczy normalnemu modelowi IRQ.

Kalibracja ma inny cel.

Nie próbuje w każdym kroku pobrać jednego atomowego:

```text
A+B+C current set
```

Stage 1 używa tylko:

```text
ADC0 JDR1
```

Stage 2 tylko:

```text
ADC1 JDR1
```

Stage 3 tylko:

```text
ADC1 JDR2
```

Przed każdym odczytem robi:

```text
wait(...)
→ read selected JDR
```

a TIMER0-triggered ADC stream pozostaje aktywny.

### Dlatego kalibracja nie potrzebuje „complete triple”

Potrzebuje tylko:

> aby wybrany JDR został odświeżony przez co najmniej jedną zakończoną konwersję przed odczytem.

JDR zachowuje ostatni zakończony conversion result.

### Czy można twardo powiedzieć, że `wait(10)` > pełen okres triggera?

Nie podaję tego jako faktu bez zamkniętej fizycznej jednostki `wait`.

**OPEN**

Natomiast z samego programu jest potwierdzone:

```text
continuous hardware acquisition stream
+
delayed single-JDR reads
```

**CONFIRMED**

---

# 18. ADC2 ROLE IN STOCK CURRENT PATH

Consumer `0x0800B17C` dla phase currents korzysta z:

```text
ADC0 JDR1
0x4001243C

ADC1 JDR1
0x4001283C

ADC1 JDR2
0x40012840
```

Nie potrzebuje ADC2 do utworzenia A/B/C.

### Wynik

# **ADC2 NIE JEST CZĘŚCIĄ STOCKOWEGO FAST A/B/C PHASE-CURRENT SAMPLE SET.**

**CONFIRMED**

Nie oznacza to, że ADC2 nie może być używany przez firmware do żadnego innego celu.

Oznacza dokładnie:

> ADC2 nie jest trzecim członem synchronizacji stockowego zestawu phase-current A/B/C.

---

# 19. STOCK DEFINITION OF COMPLETE CURRENT SAMPLE

Najlepszy model z zadania:

# **MODEL A**

```text
TIMER0 TRGO
    ↓
synchronized ADC0 + ADC1 inserted acquisition
    ↓
ADC0 completion event
    ↓
IRQ18
    ↓
read ADC0 JDR1
read ADC1 JDR1
read ADC1 JDR2
```

Z jednym ważnym doprecyzowaniem:

**kalibracja jest specjalnym timing-based consumerem pojedynczych JDR i nie używa flag jako per-sample freshness gate.**

Nie zmienia to definicji kompletnego zestawu w normalnym current path.

---

# 20. EVISTDRIVE THREE-EOIC CONDITION

Warunek:

```c
ADC0_EOIC &&
ADC1_EOIC &&
ADC2_EOIC
```

## Verdict

# **WRONG AS A STOCK-EQUIVALENCE CONDITION**

czyli odpowiedź wymagana przez zadanie:

# **NO**

### Dlaczego

Stock architecture ma:

```text
2 ADC peripherals
for 3 current values

ADC0 + ADC1
```

a nie:

```text
3 ADC peripherals
for 3 current values
```

I stock freshness logic ma:

```text
master ADC0 EOIC
```

a nie:

```text
every participating ADC must simultaneously expose EOIC
```

Dlatego warunek trzy-EOIC nie jest fabryczną semantyką Bafanga.

---

# 21. EXACT FACT NEEDED FOR FW-126.3

Bez projektowania poprawki, twardy decision input ze stocku brzmi:

> **Bafang używa jednego master completion event (`ADC0 EOIC`) jako granicy świeżego, zsynchronizowanego zestawu inserted ADC0+ADC1. Nie wymaga osobnej flagi ADC1 ani jakiejkolwiek flagi ADC2.**

Dokładnie ten fakt powinien być traktowany jako referencja przy ocenie równoważności ścieżki kalibracyjnej.

Nie podaję tutaj sposobu implementacji.

---

# 22. CONFIDENCE

| Fakt | Confidence |
|---|---|
| ADC0 base = `0x40012400` | CONFIRMED |
| ADC1 base = `0x40012800` | CONFIRMED |
| ADC0 mode raw `0x10000` | CONFIRMED |
| mode = combined regular parallel + inserted parallel | CONFIRMED |
| ADC0 inserted length = 2 | CONFIRMED |
| ADC1 inserted length = 2 | CONFIRMED |
| ADC0 rank1 = ch2 | CONFIRMED |
| ADC0 rank2 = ch0 | CONFIRMED |
| ADC1 rank1 = ch3 | CONFIRMED |
| ADC1 rank2 = ch5 | CONFIRMED |
| ADC0 inserted trigger = TIMER0 TRGO | CONFIRMED |
| ADC1 has no independent HW trigger in current path | CONFIRMED |
| ADC1 participates as synchronized partner | STRONG |
| EOIC means end of inserted group | CONFIRMED |
| ADC0 EOIC interrupt enabled | CONFIRMED |
| ADC1 EOIC required | **NO — CONFIRMED** |
| IRQ18 checks ADC0 EOIC only | CONFIRMED |
| ADC0 EOIC is stock software completion gate | CONFIRMED |
| all three phase-current JDRs treated fresh at this gate | CONFIRMED |
| silicon-level synchronized freshness guarantee | STRONG |
| ADC2 is part of A/B/C current set | **NO — CONFIRMED** |
| stock waits for ADC0+ADC1 EOIC conjunction | **NO — CONFIRMED** |
| stock waits for three EOIC | **NO — CONFIRMED** |

---

# 23. REMAINING OPEN QUESTIONS

Pozostają tylko drobne kwestie, które **nie zmieniają decyzji FW-126.3**:

1. dokładny fizyczny czas `wait(10)` w calibration routine;
2. dokładne nanosekundowe/cyklowe przesunięcie momentu ustawienia EOIC ADC0 względem ADC1 w trybie inserted parallel;
3. czy ADC1 EOIC pozostaje stale latched między kolejnymi konwersjami, jeśli żadna inna ścieżka go nie kasuje.

Żaden z tych punktów nie zmienia faktu, że stock software:

```text
ADC0 EOIC
```

jest jedyną flagą używaną jako gate normalnego current path.

---

# FW-126.3 DECISION INPUT

```text
STOCK COMPLETION EVENT =
    ADC0 EOIC / ADC0_STAT bit 2
    IRQ18 @ 0x08005578

FRESH JDR SET =
    ADC0 JDR1 @ 0x4001243C
    ADC1 JDR1 @ 0x4001283C
    ADC1 JDR2 @ 0x40012840

ADC0 flag required =
    YES for stock normal IRQ current path

ADC1 flag required =
    NO

ADC2 flag required =
    NO

three-EOIC conjunction =
    NO
    CONFIRMED: STOCK DOES NOT USE A THREE-EOIC CONJUNCTION

STOCK MODEL =
    MODEL A
    synchronized ADC0+ADC1 inserted-parallel
    → master ADC0 completion
    → consume full current set

confidence =
    CONFIRMED for stock software behavior
    STRONG for silicon-level simultaneous-freshness ordering
```

# FINAL DECISION

**`ADC0_EOIC && ADC1_EOIC && ADC2_EOIC` nie jest odpowiednikiem fabrycznej semantyki Bafanga.**

Fabryczny M820 definiuje granicę nowego kompletnego zestawu prądowego przez **completion event mastera ADC0**, po którym bez żadnego sprawdzania ADC1/ADC2 konsumuje dane ADC0+ADC1.

**FW-126.3 powinien więc traktować jako zamknięty fakt stockowy: `ADC0 EOIC = completion gate`, a nie „wszystkie EOIC jednocześnie”.**

STOP — bez implementacji.