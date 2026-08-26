# BAFANG M820 STOCK CAL OUTPUT-STATE AUDIT

## Zakres i standard dowodu

Wartości rejestrów, adresy, kolejność zapisów i konfiguracja GPIO poniżej pochodzą **wyłącznie z fabrycznego `FT_2026_05_22_w1(3).bin`**. Dokumentacja GigaDevice jest używana wyłącznie do przetłumaczenia surowych bitów rejestrów GD32F303 na ich sprzętowe znaczenie. Nie używam EVistDrive/EBiCS jako źródła faktów o stocku i nie projektuję implementacji ani testu. Jest to dokładnie zakres wymagany w zadaniu.

`CONFIRMED` oznacza tutaj wartość/rejestr z BIN-u plus udokumentowaną semantykę GD32, a dla fizycznego pinu dodatkowo potwierdzoną konfigurację GPIO. Stan MOSFET-ów wymaga dodatkowo znanej truth table gate-drivera.

---

# 1. TIMER INSTANCE / PIN MAP

Timer napędu jest **GD32 `TIMER0`**, baza:

```text
TIMER0 = 0x40012C00
```

**CONFIRMED — BIN**

W dokumentacji GD32F303 jest to advanced timer z trzema wyjściami komplementarnymi. Nazewnictwo trzeba przesunąć względem używanego wcześniej zapisu CH1/CH1N:

| Nazwa używana w audycie | Natywna nazwa GD32 | Pin | Stock GPIO |
|---|---|---|---|
| CH1 | `TIMER0_CH0` | **PA8** | skonfigurowany |
| CH1N | `TIMER0_CH0_ON` | **PB13** | skonfigurowany |
| CH2 | `TIMER0_CH1` | **PA9** | skonfigurowany |
| CH2N | `TIMER0_CH1_ON` | **PB14** | skonfigurowany |
| CH3 | `TIMER0_CH2` | **PA10** | skonfigurowany |
| CH3N | `TIMER0_CH2_ON` | **PB15** | skonfigurowany |
| CH4 | `TIMER0_CH3` | PA11 | **nie jest w tej inicjalizacji GPIO faz** |

Pinout TIMER0 jest zgodny z oficjalnym datasheetem GD32F303.

### Dowód z BIN-u

`0x0800B3B0`:

```text
pin mask = 0x0700            ; PA8 | PA9 | PA10
mode     = 0x18
speed    = 3
GPIO     = 0x40010800        ; GPIOA
call     = 0x08007870
```

`0x0800B3CC`:

```text
pin mask = 0xE000            ; PB13 | PB14 | PB15
mode     = 0x18
speed    = 3
GPIO     = 0x40010C00        ; GPIOB
call     = 0x08007870
```

W całym BIN-ie helper remapu AF `0x08007A0C` ma tylko dwa call-site'y, `0x08016236` i `0x0801623E`; nie ma wywołania remapującego TIMER0 w inicjalizacji napędu.

**Wniosek: default TIMER0 pin map — CONFIRMED.**

---

# 2. GPIO AF CONFIG

Helper GPIO `0x08007870` składa dla `mode=0x18`, `speed=3` nibble:

```text
0x8 | 0x3 = 0xB
```

czyli konfigurację:

```text
CTL = 10
MD  = 11
```

Dla GD32F303 oznacza to:

**Alternate Function Output, Push-Pull, max 50 MHz class.**

Oficjalny manual potwierdza, że w AF push-pull bufor wyjściowy GPIO jest sterowany przez peryferium, a nie przez `GPIO_OCTL`.

| Pin | Mode | Driver |
|---|---|---|
| PA8 | AF output | push-pull |
| PA9 | AF output | push-pull |
| PA10 | AF output | push-pull |
| PB13 | AF output | push-pull |
| PB14 | AF output | push-pull |
| PB15 | AF output | push-pull |

**CONFIRMED**

Stock nie wykonuje w tym miejscu osobnego ustawienia HIGH/LOW poprzez `GPIO_OCTL`, ponieważ dla AF-PP poziom pochodzi z TIMER0.

---

# 3. CTL0

Konfiguracja w `0x0800B334`:

```text
PSC   = 0
CAM   = 0x20
CAR   = 2000
CKDIV = 0x0100
CREP  = 1
```

Następnie `0x0800B52C → 0x0800CC10` ustawia:

```text
CTL0.CEN = 1
```

Programowane bity CTL0:

```text
bit 9:8 CKDIV = 01
bit 7   ARSE  = 0
bit 6:5 CAM   = 01
bit 3   SPM   = 0
bit 2   UPS   = 0
bit 1   UPDIS = 0
bit 0   CEN   = 1
```

Bazowa wartość bez dynamicznego `DIR`:

```text
CTL0 = 0x0121
```

W trybie center-aligned `DIR` staje się odzwierciedleniem aktualnego kierunku liczenia, więc podczas pracy odczyt może chwilowo mieć również bit `0x10`.

`CAM=01` oznacza center-aligned mode; `CKDIV=01` daje `fDTS = fTIMER/2`.

**CONFIRMED**

---

# 4. CTL1

Po inicjalizacji:

```text
TIMER0_CTL1 = 0x2A71
```

Decode:

| Bit | Pole | Stock |
|---:|---|---:|
| 14 | ISO3 | 0 |
| 13 | ISO2N | 1 |
| 12 | ISO2 | 0 |
| 11 | ISO1N | 1 |
| 10 | ISO1 | 0 |
| 9 | ISO0N | 1 |
| 8 | ISO0 | 0 |
| 7 | TIOS | 0 |
| 6:4 | MMC | **111** |
| 3 | DMAS | 0 |
| 2 | CCUC | **0** |
| 0 | CCSE | **1** |

Czyli dla trzech faz stock ustawia idle:

```text
main output:          ISOx  = 0
complementary output: ISOxN = 1
```

a `MMC=111` wybiera **O3CPRE jako źródło TRGO**.

### Kluczowy punkt: `CCSE=1`

`0x0800B4D0..0x0800B4DE` jawnie ustawia `CTL1.bit0`.

W GD32F303 oznacza to, że:

```text
CHxEN
CHxNEN
CHxCOMCTL
```

są objęte **commutation-control shadowing**.

Ponieważ:

```text
CCUC = 0
```

nowe wartości trafiają do aktywnej logiki dopiero przy `CMTG`.

**CONFIRMED**

---

# 5. CHCTL0 / CHCTL1

CH0/CH1/CH2, czyli użytkowe CH1/2/3:

```text
COMCTL = 110 = PWM mode 0
COMSEN = 1
CV     = 1000
```

CH3, czyli użytkowy CH4:

```text
COMCTL = 111 = PWM mode 1
COMSEN = 1
CV     = 1999
```

Stąd:

```text
CHCTL0 = 0x6868
CHCTL1 = 0x7868
```

`110` to PWM mode 0, `111` to PWM mode 1; oba tworzą wewnętrzny sygnał `OxCPRE` na podstawie CNT/CV.

**CONFIRMED**

To będzie kluczowe dla CH4/TRGO: **powstawanie `O3CPRE` jest funkcją wewnętrznego compare unit i nie wymaga włączenia fizycznego pinu CH3.**

---

# 6. CCER / CHCTL2

Natywna nazwa GD32:

**`TIMER0_CHCTL2`**

Układ bitów GD32 jest potwierdzony przez manual: EN/P/NEN/NP dla kolejnych kanałów.

## Stage 1 — `0x0880`

```text
CH0EN  = 0
CH0P   = 0
CH0NEN = 0
CH0NP  = 0

CH1EN  = 0
CH1P   = 0
CH1NEN = 0
CH1NP  = 1

CH2EN  = 0
CH2P   = 0
CH2NEN = 0
CH2NP  = 1
```

## Stage 2 — `0x0808`

```text
CH0NP = 1
CH1NP = 0
CH2NP = 1

wszystkie EN/NEN = 0
```

## Stage 3 — `0x0088`

```text
CH0NP = 1
CH1NP = 1
CH2NP = 0

wszystkie EN/NEN = 0
```

## Restore — `0x0DDD`

Dla CH0/1/2:

```text
EN  = 1
P   = 0
NEN = 1
NP  = 1
```

**CONFIRMED — BIN**

---

# 7. BDTR / CCHP `0x0C19` FULL DECODE

Stock inicjalizuje:

```text
TIMER0_CCHP = 0x0C19
```

Podczas stage po ustawieniu POEN:

```text
TIMER0_CCHP = 0x8C19
```

Decode:

| Bit | Pole GD32 | Wartość |
|---:|---|---:|
| 15 | POEN | 0 początkowo / **1 podczas CAL** |
| 14 | OAEN | 0 |
| 13 | BRKP | 0 |
| 12 | BRKEN | 0 |
| 11 | ROS | **1** |
| 10 | IOS | **1** |
| 9:8 | PROT | 00 |
| 7:0 | DTCFG | `0x19` = 25 |

Oficjalny layout i semantyka CCHP są takie właśnie dla GD32F30x.

Ponieważ `DTCFG=0x19` należy do zakresu `0xx`:

```text
dead-time = 25 × tDTS_CK
```

a z `CKDIV=01`:

```text
fDTS = fTIMER / 2
```

Nie przeliczam tego na µs bez potrzeby.

**CONFIRMED**

---

# 8. IOS — IDLE OFF-STATE

Stock:

```text
IOS = 1
```

IOS działa, gdy:

```text
POEN = 0
```

Przy aktywnych `EN=NEN=0`, tabela sprzętowa GD32 nadal określa wyjścia jako **output disabled**.

Przy aktywnych `EN=NEN=1` i `POEN=0`, idle-state może zostać wymuszony przez:

```text
ISOx  = 0
ISOxN = 1
```

czyli logicznie LOW/HIGH zgodnie z idle-state configuration.

**CONFIRMED**

---

# 9. ROS — RUN OFF-STATE

Stock:

```text
ROS = 1
```

ROS jest używany przy:

```text
POEN = 1
```

Ale **ROS=1 nie omija `CHxEN/CHxNEN`**.

Kluczowy dokładny wiersz tabeli GD32 dla stock CAL:

```text
POEN  = 1
ROS   = 1
CHxEN = 0
CHxNEN= 0
```

wynik:

```text
CHx_O  = CHxP
CHx_ON = CHxNP

ALE:
CHx_O  output disable
CHx_ON output disable
```

To jest centralny wynik audytu.

**CONFIRMED**

---

# 10. OIS / ISOx, ISOxN

Stock ustawia dla kanałów fazowych:

| Output | Idle data |
|---|---:|
| CH1 / native CH0 | LOW |
| CH1N | HIGH |
| CH2 / native CH1 | LOW |
| CH2N | HIGH |
| CH3 / native CH2 | LOW |
| CH3N | HIGH |

Ale te wartości nie oznaczają, że podczas stabilnego CAL piny są LOW/HIGH.

Powód:

```text
CAL:
POEN = 1
```

więc obowiązuje gałąź ROS, nie idle POEN=0.

Dodatkowo:

```text
EN=NEN=0
```

więc sprzęt określa obydwa wyjścia jako **disabled**.

**CONFIRMED**

---

# 11. POLARITY P / NP

`P` i `NP` określają logiczną polaryzację sygnałów głównego i komplementarnego.

Dla wyjścia komplementarnego:

```text
NP=0 → high level = active level
NP=1 → low level  = active level
```

ale **`NP` nie jest bitem enable**. Enable to osobny `NEN`.

### Najważniejszy przypadek stock CAL

```text
NEN = 0
POEN = 1
ROS = 1
```

Zmiana NP:

```text
0 ↔ 1
```

zmienia wewnętrzną wartość:

```text
CHx_ON = CHxNP
```

lecz równocześnie:

```text
CHx_ON output disable
```

### Wniosek

**`CHxNP` NIE zmienia fizycznego poziomu sterowanego przez TIMER na CHxN, jeżeli aktywne `CHxNEN=0`.**

**CONFIRMED NO PHYSICAL OUTPUT EFFECT**

To rozstrzyga główną hipotezę z zadania.

---

# 12. MOE / POEN SEMANTICS

GD32 nazywa dawniej używane `MOE`:

```text
POEN — Primary Output Enable
```

Stock CAL:

```text
CCER/CHCTL2 = stage value
POEN = 1
wait(100)
...
POEN = 0
```

Helper:

```text
0x0800CD3C:
CCHP |= 0x8000       ; POEN ON

0x0800CD4A:
CCHP &= 0x7FFF       ; POEN OFF
```

Manual mówi jednoznacznie: `POEN=1` może uruchomić wyjście tylko wtedy, gdy odpowiadający mu `CHxEN` lub `CHxNEN` jest ustawiony.

Zatem:

```text
POEN=1
EN=0
NEN=0
```

**nie oznacza włączenia sześciu pinów PWM.**

POEN wybiera semantykę run/off-state, lecz przy tej kombinacji enable wyjścia pozostają wyłączone.

**CONFIRMED**

---

# 13. E/NEN DISABLED SEMANTICS

Trzy interesujące przypadki:

## A. `POEN=0`

Stock ma `IOS=1`.

Dla `EN=NEN=0`:

```text
timer output enable = OFF
```

Fizyczny pin nie otrzymuje gwarantowanego HIGH ani LOW z TIMER0.

## B. `POEN=1`, `EN=0`, `NEN=0`

To jest **ustalony stan kalibracji**.

```text
internal CHx_O  = P
internal CHx_ON = NP

physical output enable CHx  = OFF
physical output enable CHxN = OFF
```

**CONFIRMED**

## C. `POEN=1`, `EN=1`, `NEN=1`

Normalne wyjścia są aktywne:

```text
CHx_O  = OxCPRE XOR P
CHx_ON = (!OxCPRE) XOR NP
```

i działa logika complementary/dead-time.

---

# 14. CH4 INTERNAL OCREF / O3CPRE

Użytkowy CH4 to GD32:

```text
TIMER0_CH3
```

Stock:

```text
CH3CV     = 1999
CAR       = 2000
COMCTL    = 111 = PWM mode 1
COMSEN    = 1
CH3EN     = 0
```

`CH3EN=0` wyłącza **fizyczny channel output**.

Nie wyłącza comparatora:

```text
CNT ↔ CH3CV
      ↓
    O3CPRE
```

`O3CPRE` jest sygnałem wewnętrznym powstającym z compare logic jeszcze przed finalnym output-enable/polarity stage. Oficjalny opis PWM mode 1 właśnie definiuje zachowanie O3CPRE na podstawie CNT/CH3CV.

### Wynik

**O3CPRE istnieje przy `CH3EN=0`.**

**CONFIRMED**

---

# 15. TRGO PATH

Stock:

```text
CTL1.MMC = 111
```

GD32 definiuje:

```text
MMC=111
→ compare event / source O3CPRE
→ TRGO
```



Pełny producer:

```text
TIMER0 running
   ↓
CNT
   ↓
CH3 compare, CV=1999, PWM mode 1
   ↓
O3CPRE
   ↓
CTL1.MMC=111
   ↓
TIMER0 TRGO
```

**CONFIRMED**

Nie ma w tym łańcuchu:

```text
CH3EN
CH3 physical pin
POEN
```

jako warunku powstania TRGO.

---

# 16. ADC TRIGGER WITH POEN OFF

W stockowym BIN-ie:

```text
0x0800B752:
ADC0 inserted external-trigger selector = 0

0x0800B75C:
inserted external trigger enable = 1
```

Oficjalna biblioteka GD32F30x określa źródło:

```text
ADC0_1_EXTTRIG_INSERTED_T0_TRGO
→ TIMER0 TRGO event
```



Łańcuch:

```text
TIMER0 O3CPRE
→ TIMER0 TRGO
→ ADC0 inserted trigger
```

pozostaje funkcjonalny przy:

```text
POEN = 0
```

ponieważ POEN steruje output stage, nie internal compare/TRGO.

### Wniosek

**`POEN=0` samo w sobie NIE wyłącza sprzętowego triggera ADC.**

**CONFIRMED**

---

# 17. ADC TRIGGER WITH POEN ON

Przy:

```text
POEN = 1
```

ten sam wewnętrzny łańcuch działa identycznie:

```text
O3CPRE
→ TRGO
→ ADC inserted
```

POEN nie dodaje ani nie usuwa source triggera.

### Wniosek

Od strony TIMER0/ADC:

```text
POEN OFF → trigger może działać
POEN ON  → trigger może działać
```

**CONFIRMED**

Zatem **sam fakt, że stock włącza POEN podczas kalibracji, nie jest wymagany do istnienia TIMER0-TRGO.**

Dokładny powód, dlaczego stock mimo wyłączonych EN/NEN ustawia POEN=1, pozostaje **OPEN**.

---

# Ważny stan przejściowy — CCSE / CMTG

To jest istotna korekta do prostego modelu „write CCER → natychmiast nowy stan”.

Stock ma:

```text
CCSE = 1
CCUC = 0
```

Więc:

```text
CHxEN
CHxNEN
CHxCOMCTL
```

są shadowowane i aktywują się przy `CMTG`.

Jednocześnie TIMER0 update IRQ jest włączony przez:

```text
0x0800B7D6..0x0800B7DC
```

Wektor IRQ25:

```text
0x0800BE71
```

Handler `0x0800BE70`:

```text
check TIMER0 update
clear update flag

0x0800BE86:
SWEVG |= 0x20
```

Bit 5 `SWEVG.CMTG` jest właśnie sprzętowym commutation update event i ładuje shadowowane EN/NEN/COMCTL.

### Oś jednego stage

```text
write CHCTL2 = 0x0880 / 0808 / 0088
        ↓
EN/NEN zapisane do shadow
NP zmienia się bez tego shadow mechanizmu
        ↓
POEN = 1
        ↓
krótki stan przejściowy
        ↓
TIMER0 update IRQ
        ↓
CMTG
        ↓
aktywne EN/NEN = 0
        ↓
ustalony stan
        ↓
wait(100)
        ↓
sampling
```

Przed pierwszym CMTG istnieje krótki transient zależny od poprzednio aktywnych EN/NEN.

**Dokładny elektryczny stan tego bardzo krótkiego transientu = OPEN.**

Dla właściwego, ustalonego okna próbkowania po CMTG:

**EN=NEN=0 — STRONG**, ponieważ stock pozostawia TIMER0 update IRQ aktywny i wykonuje długi `wait(100)` przed próbkowaniem.

---

# 18. STAGE 1 PIN TRUTH TABLE

## Ustalony stan podczas próbkowania

```text
CHCTL2 = 0x0880
POEN   = 1
ROS    = 1

CH0NP = 0
CH1NP = 1
CH2NP = 1

EN/NEN wszystkich faz = 0
```

| Output | Pin | Internal data | TIMER output enable | Pin state |
|---|---|---:|---|---|
| CH1 | PA8 | `P=0` | **DISABLED** | Hi-Z/external* |
| CH1N | PB13 | `NP=0` | **DISABLED** | Hi-Z/external* |
| CH2 | PA9 | `P=0` | **DISABLED** | Hi-Z/external* |
| CH2N | PB14 | `NP=1` | **DISABLED** | Hi-Z/external* |
| CH3 | PA10 | `P=0` | **DISABLED** | Hi-Z/external* |
| CH3N | PB15 | `NP=1` | **DISABLED** | Hi-Z/external* |

`*` **TIMER output-disable jest CONFIRMED.** „Hi-Z/external” oznacza, że TIMER nie wymusza HIGH/LOW; rzeczywisty poziom napięcia na węźle może zostać określony przez zewnętrzny gate-driver/pull network, którego BIN nie opisuje.

### Logical current calibrated

```text
A
```

### Expected bridge state

**OPEN** — brak potwierdzonej truth table gate-drivera.

### Torque possible?

Od strony MCU:

**brak aktywnego PWM fazowego — CONFIRMED.**

Od strony mostka:

**OPEN.**

### Confidence

**MCU TIMER STATE: CONFIRMED**  
**PIN ELECTRICAL LEVEL: STRONG / external-dependent**  
**MOSFET STATE: OPEN**

---

# 19. STAGE 2 PIN TRUTH TABLE

```text
CHCTL2 = 0x0808
POEN   = 1

CH0NP=1
CH1NP=0
CH2NP=1

all EN/NEN=0
```

| Output | Pin | Internal data | TIMER OE | Pin state |
|---|---|---:|---|---|
| CH1 | PA8 | P=0 | DISABLED | Hi-Z/external |
| CH1N | PB13 | NP=1 | DISABLED | Hi-Z/external |
| CH2 | PA9 | P=0 | DISABLED | Hi-Z/external |
| CH2N | PB14 | NP=0 | DISABLED | Hi-Z/external |
| CH3 | PA10 | P=0 | DISABLED | Hi-Z/external |
| CH3N | PB15 | NP=1 | DISABLED | Hi-Z/external |

### Logical current calibrated

```text
B
```

Fizyczny stan sześciu **output-enable** jest taki sam jak Stage 1.

**CONFIRMED**

---

# 20. STAGE 3 PIN TRUTH TABLE

```text
CHCTL2 = 0x0088
POEN   = 1

CH0NP=1
CH1NP=1
CH2NP=0

all EN/NEN=0
```

| Output | Pin | Internal data | TIMER OE | Pin state |
|---|---|---:|---|---|
| CH1 | PA8 | P=0 | DISABLED | Hi-Z/external |
| CH1N | PB13 | NP=1 | DISABLED | Hi-Z/external |
| CH2 | PA9 | P=0 | DISABLED | Hi-Z/external |
| CH2N | PB14 | NP=1 | DISABLED | Hi-Z/external |
| CH3 | PA10 | P=0 | DISABLED | Hi-Z/external |
| CH3N | PB15 | NP=0 | DISABLED | Hi-Z/external |

### Logical current calibrated

```text
C
```

Ponownie: z punktu widzenia **fizycznego output-enable TIMER0 wszystkie trzy stage są równoważne.**

**CONFIRMED**

---

# 21. DOES NP MATTER WHEN NEN=0?

## Odpowiedź

**Dla fizycznego pinu w ustalonym stanie CAL: CONFIRMED NO EFFECT.**

NP ma wpływ na wewnętrzną wartość:

```text
CHx_ON = NP
```

ale przy:

```text
POEN=1
ROS=1
NEN=0
```

ten sam wiersz tabeli GD32 mówi:

```text
CHx_ON output disable
```



Czyli zauważona korelacja:

```text
Stage1: jedyny NP=0 → CH1 → A
Stage2: jedyny NP=0 → CH2 → B
Stage3: jedyny NP=0 → CH3 → C
```

**nie jest mechanizmem aktywnie ustawiającym CH1N/CH2N/CH3N na różne poziomy podczas właściwego okna próbkowania.**

W terminologii zadania:

## Wynik = B

`0x0880 / 0x0808 / 0x0088` różnią zawartość polarity/off-state data, ale przy aktywnym `NEN=0` **nie powodują różnego fizycznego sterowania pinami komplementarnymi**.

Po co stock zapisuje akurat taki wzór NP, skoro w ustalonym stanie jest sprzętowo nieaktywny na pinie?

**OPEN**

Nie nazywam tego automatycznie „przygotowaniem na później”, ponieważ firmware tego nie dowodzi.

---

# 22. EXTERNAL GATE DRIVER STATUS

BIN pokazuje:

```text
PA8  → TIMER0_CH0
PB13 → TIMER0_CH0_ON
PA9  → TIMER0_CH1
PB14 → TIMER0_CH1_ON
PA10 → TIMER0_CH2
PB15 → TIMER0_CH2_ON
```

ale nie zawiera identyfikatora, z którego można jednoznacznie odzyskać:

- model gate-drivera,
- truth table jego wejść,
- pull-up/pull-down na PCB,
- zachowanie wejścia drivera, gdy MCU przestaje je aktywnie sterować.

### Status

**EXTERNAL GATE DRIVER MODEL: OPEN**

Zgodnie ze standardem dowodu zatrzymuję w tym miejscu inferencję mostka.

---

# 23. MOSFET STATE IF PROVABLE

Nie jest możliwe potwierdzenie z samego BIN-u:

```text
high-side gate = ON/OFF
low-side gate  = ON/OFF
```

dla stanu, w którym wyjście TIMER jest output-disabled.

Nie wiemy bowiem, czy zewnętrzne wejście drivera:

- ma pull-down,
- ma pull-up,
- ma własny fail-safe,
- interpretuje floating input w określony sposób.

Dlatego:

| Element | Stage 1 | Stage 2 | Stage 3 |
|---|---|---|---|
| MCU phase output driver | OFF | OFF | OFF |
| HS MOSFET gate | OPEN | OPEN | OPEN |
| LS MOSFET gate | OPEN | OPEN | OPEN |
| faza do GND | OPEN | OPEN | OPEN |
| faza do DC+ | OPEN | OPEN | OPEN |
| faza floating | OPEN | OPEN | OPEN |

---

# 24. TORQUE / CURRENT POSSIBILITY

## Co można powiedzieć twardo

W ustalonym oknie kalibracji:

```text
CH1/CH1N output enable = 0
CH2/CH2N output enable = 0
CH3/CH3N output enable = 0
```

Nie ma więc **aktywnego PWM generowanego przez TIMER0 na sześciu wyjściach fazowych**.

**CONFIRMED**

### Shoot-through generowany bezpośrednio przez TIMER?

W ustalonym stanie nie istnieje para aktywnych wyjść PWM, więc TIMER sam nie wydaje komendy jednoczesnego włączenia obu tranzystorów gałęzi.

**STRONG**

### Czy przez uzwojenie może płynąć prąd?

**OPEN na poziomie mostka**, ponieważ stan tranzystorów po output-disable zależy od gate-drivera.

### Czy może powstać moment?

**OPEN na poziomie całego hardware.**

Nie wolno zamienić „TIMER outputs disabled” na automatyczne „wszystkie MOSFET-y OFF”, dopóki nie znamy wejściowej truth table drivera.

### Dead-time

`DTCFG=0x19` jest skonfigurowany, ale w ustalonym CAL oba `EN/NEN=0`, więc nie uczestniczy w normalnym przełączaniu complementary PWM.

Może mieć znaczenie przy przejściach stanów.

---

# 25. SAFETY CONCLUSION

## Główny wynik reverse

Fabryczny M820 podczas stabilnej części każdego z trzech etapów **nie przełącza kolejnych faz do GND/DC+ za pomocą `0x0880 → 0x0808 → 0x0088`.**

Twardo wynika:

```text
Stage 1:
all EN/NEN = 0

Stage 2:
all EN/NEN = 0

Stage 3:
all EN/NEN = 0
```

i zgodnie z tabelą GD32:

```text
POEN=1 + ROS=1 + EN=0 + NEN=0
→ main output disabled
→ complementary output disabled
```



### Zatem wcześniejsza hipoteza:

> zmiana NP wybiera kolejno fizycznie sterowaną fazę

jest **obalona na poziomie wyjść MCU w ustalonym oknie próbkowania**.

### Drugi ważny wynik

Stockowe:

```text
POEN = 1
```

nie jest potrzebne do zachowania:

```text
TIMER0 CH3 compare
→ O3CPRE
→ TRGO
→ ADC inserted trigger
```

CH4/CH3 physical enable oraz POEN są oddzielone od wewnętrznego compare/TRGO.

### Trzeci ważny wynik

Istnieje krótki problematyczny transient związany z:

```text
CCSE=1
→ EN/NEN shadow
→ aktualizacja dopiero przez CMTG
```

więc nie wolno modelować stage jako atomowego:

```text
CCER write → natychmiast EN/NEN=0
```

---

# 26. CONFIRMED FACTS

| ITEM | STOCK FACT | EVIDENCE | STATUS |
|---|---|---|---|
| Timer | `TIMER0 @ 0x40012C00` | BIN literals | CONFIRMED |
| CH1 pin | PA8 / TIMER0_CH0 | GPIO init + GD32 pinout | CONFIRMED |
| CH1N pin | PB13 / TIMER0_CH0_ON | jw. | CONFIRMED |
| CH2 pin | PA9 | jw. | CONFIRMED |
| CH2N pin | PB14 | jw. | CONFIRMED |
| CH3 pin | PA10 | jw. | CONFIRMED |
| CH3N pin | PB15 | jw. | CONFIRMED |
| GPIO mode | AF push-pull | mode 0x18, speed 3 | CONFIRMED |
| Center aligned | CAM=01 | `0x0800B3FA` | CONFIRMED |
| TIMER running | CEN=1 | `0x0800B52C` | CONFIRMED |
| CTL1 | `0x2A71` | init flow | CONFIRMED |
| CCSE | 1 | `0x0800B4D0..B4DE` | CONFIRMED |
| CCUC | 0 | no set after reset | CONFIRMED |
| MMC | 111 | `0x0800B50E/B512` | CONFIRMED |
| TRGO source | O3CPRE | MMC semantics | CONFIRMED |
| CH4 mode | PWM mode 1 | COMCTL=111 | CONFIRMED |
| CH4 compare | 1999 | `0x0800B486` | CONFIRMED |
| CCHP initial | `0x0C19` | init | CONFIRMED |
| ROS | 1 | CCHP bit11 | CONFIRMED |
| IOS | 1 | CCHP bit10 | CONFIRMED |
| Break | disabled | BRKEN=0 | CONFIRMED |
| OAEN | 0 | CCHP bit14 | CONFIRMED |
| Deadtime | raw `0x19` | CCHP | CONFIRMED |
| CAL POEN | 1 | helper `0x0800CCF8` | CONFIRMED |
| Stage1 | `0x0880` | `0x0800AF96` | CONFIRMED |
| Stage2 | `0x0808` | `0x0800AFF4` | CONFIRMED |
| Stage3 | `0x0088` | `0x0800B052` | CONFIRMED |
| E/NEN desired state | all 0 | stage values | CONFIRMED |
| E/NEN shadow | enabled | CCSE=1 | CONFIRMED |
| CMTG producer | TIMER0 update IRQ | `0x0800BE70..BE94` | CONFIRMED |
| Stage settled output OE | all six disabled | GD32 output table | CONFIRMED |
| NP changes internal data | yes | CHCTL2 + table | CONFIRMED |
| NP drives pin with NEN=0 | **NO** | output-disable table | **CONFIRMED NO EFFECT** |
| O3CPRE with physical CH4 disabled | remains internal | compare architecture | CONFIRMED |
| POEN required for TRGO | **NO** | separate internal/output paths | CONFIRMED |
| ADC0 trigger | TIMER0 TRGO | stock selector + GD32 mapping | CONFIRMED |

---

# 27. OPEN QUESTIONS

1. Dokładny stan **w bardzo krótkim oknie między zapisem CHCTL2 a pierwszym CMTG**, ponieważ aktywne EN/NEN mogą jeszcze przez chwilę pochodzić z poprzedniego shadow state.
2. Dokładny elektryczny poziom węzłów PA8/PB13/PA9/PB14/PA10/PB15 po `timer output disable`, jeżeli na PCB istnieją zewnętrzne pull-up/down.
3. Model zewnętrznego gate-drivera.
4. Jego zachowanie dla floating/undriven input.
5. Stan HS/LS MOSFET-ów podczas stabilnego CAL.
6. Czy faza jest wtedy fizycznie floating/GND/DC+.
7. Dlaczego stock rotuje wzór `NP=0` między A/B/C, skoro w ustalonym `NEN=0` nie zmienia to wyjścia pinu.
8. Dlaczego stock jawnie ustawia `POEN=1`, skoro TIMER0 TRGO nie wymaga POEN.

Nie wypełniam tych luk intuicją.

---

# 28. IS STOCK-LIKE HW TEST SAFE TO DESIGN?

# **NO**

Na poziomie MCU udało się udowodnić, że w ustalonym oknie kalibracji wszystkie sześć wyjść TIMER0 jest **output-disabled**, ale nie da się jeszcze przełożyć tego bezpiecznie na stan sześciu bramek MOSFET-ów.

## Dokładnie jedna brakująca informacja, którą trzeba zdobyć jako następną:

**Potwierdzona charakterystyka wejść zastosowanego w M820 gate-drivera w stanie `MCU TIMER output-disable` — jego truth table wraz z zachowaniem wejść wynikającym z pull-up/pull-down na fabrycznej PCB.**