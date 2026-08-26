# BAFANG M820 STOCK PHASE-CURRENT CALIBRATION REVERSE

## 0. Najważniejsze wyniki

**CONFIRMED**

- Obraz aplikacji w BIN-ie zaczyna się po **32-bajtowym nagłówku**.
- Baza aplikacji: **`0x08005000`**.
- Reset vector: **`0x0800A3CD`** → kod od `0x0800A3CC`.
- Procedura kalibracji prądów fazowych: **`0x0800AF5C`**.
- Są trzy etapy:
  - `CCER = 0x0880`
  - `CCER = 0x0808`
  - `CCER = 0x0088`
- Każdy etap zbiera **dokładnie 16 próbek**.
- Każda próbka jest liczona jako **`JDR >> 3`**.
- Nie ma końcowego dzielenia przez 16.
- Offset jest **sumą 16 próbek**, przechowywaną jako `uint16`.
- Offsety:
  - A: `0x200004EE`
  - B: `0x200004F0`
  - C: `0x200004F2`
- Źródła:
  - A ← `ADC0 JDR1 @ 0x4001243C`
  - B ← `ADC1 JDR1 @ 0x4001283C`
  - C ← `ADC1 JDR2 @ 0x40012840`
- Walidacja każdego offsetu jest **ściśle**:

`0x7800 < offset < 0x8800`

Granice **nie są akceptowane**.

- Po zakończeniu:
  - `CCER = 0x0DDD`
  - `MOE = 0`
- Normalny tor prądowy używa dokładnie tych samych offsetów w `0x0800B17C`.
- W normalnym torze występuje m.in.:

`current_A = offset_A - 2 * ADC0_JDR1`

co matematycznie wyjaśnia skalę offsetu z kalibracji.

---

# 1. CALIBRATION ENTRY ADDRESS

## Adres

**`0x0800AF5C` — CONFIRMED**

Początek:

```text
0800AF5C  push {r4,lr}
0800AF5E  movs r0,#0
0800AF60  ldr  r1, =0x20000678
0800AF62  strb r0,[r1]
```

### Dlaczego adres jest pewny

BIN zawiera 32-bajtowy nagłówek przed wektorem aplikacji.

Po jego pominięciu:

```text
application base = 0x08005000
initial SP       = 0x2000B808
reset vector     = 0x0800A3CD
```

`0x0800A3CC` trafia w poprawny kod Thumb:

```text
0800A3CC push {r4,lr}
0800A3CE bl   0x08005130
```

Dlatego wcześniejszy punkt orientacyjny `0x0800AF5C` jest **potwierdzony**.

---

# 2. CALLERS / BOOT ORDER

## Call graph

**CONFIRMED**

```text
Reset_Handler
0x0800A3CC
  ↓
0x08005130
  ↓
0x080051EA
  ↓
call application main @ 0x080051F4
  ↓
0x08017F4C
  ↓
0x08017FB0        hardware/application init
  ↓
0x0800B334        motor PWM / ADC initialization
  ↓
interrupt enable:
0x08018052 CPSIE I
0x08018054 CPSIE F
  ↓
return
  ↓
0x0800B860
  ↓
main loop
0x080180E4
  ↓
phase-current calibration
0x0800AF5C
```

### Bezpośrednie calle kalibracji

W `0x080180E4` są **trzy bezpośrednie wywołania**:

| Call instruction | Target |
|---|---|
| `0x08018362` | `0x0800AF5C` |
| `0x0801849A` | `0x0800AF5C` |
| `0x0801884A` | `0x0800AF5C` |

**CONFIRMED**

Przed pierwszym z nich kod sprawdza trzy wartości 16-bitowe względem `0x003C` i dodatkowy warunek równy zero:

```text
0x08018344 ... [base+4]  < 0x3C
0x0801834C ... [base+8]  < 0x3C
0x08018354 ... [base+12] < 0x3C
0x0801835C ... halfword == 0
0x08018362 BL 0x0800AF5C
```

Dokładne fizyczne znaczenie tych czterech warunków nie zostało ustalone z samego tego fragmentu.

**OPEN**

---

# 3. TIMER STATE BEFORE CAL

PWM timer base:

**`0x40012C00` — CONFIRMED**

Inicjalizacja: `0x0800B334`.

## Parametry ustawiane przez firmware

| Element | Wartość | Confidence |
|---|---:|---|
| PSC | `0` | CONFIRMED |
| alignment/control field | `0x20` | CONFIRMED |
| ARR | `2000 / 0x07D0` | CONFIRMED |
| clock/control field | `0x0100` | CONFIRMED |
| repetition | `1` | CONFIRMED |
| CCR1 | `1000 / 0x03E8` | CONFIRMED |
| CCR2 | `1000 / 0x03E8` | CONFIRMED |
| CCR3 | `1000 / 0x03E8` | CONFIRMED |
| CCR4 | `1999 / 0x07CF` | CONFIRMED |
| CH1/2/3 mode field | `0x60` | CONFIRMED |
| CH4 mode field | `0x70` | CONFIRMED |
| preload-related field | `0x08` | CONFIRMED |
| timer enable | `CTL0 bit0 = 1` | CONFIRMED |

Timer zostaje uruchomiony:

```text
0x0800B528 r1 = 1
0x0800B52A r0 = 0x40012C00
0x0800B52C BL 0x0800CC10
```

Helper:

```text
0x0800CC9C LDRH r0,[timer]
0x0800CC9E ORR  r0,#1
0x0800CCA2 STRH r0,[timer]
```

czyli **timer counter jest włączony**.

### Center-aligned

Pole `0x20` jest ustawiane w konfiguracji licznika.

**STRONG:** jest to tryb center-aligned używany przez ten timer.

Nie ma podstaw z samego algorytmu kalibracji, żeby przypisywać mu edge-aligned.

---

# 4. ADC STATE BEFORE CAL

ADC:

```text
ADC0 = 0x40012400
ADC1 = 0x40012800
```

**CONFIRMED**

Oba są inicjalizowane w `0x0800B334` przed rozpoczęciem state machine i przed kalibracją.

Firmware wykonuje również dla obu:

```text
CTL1 bit2 = 1
poll CTL1 bit2 aż zostanie skasowany
```

Adresy:

```text
0x0800B706 → ADC0 → 0x08005EB8
0x0800B70E → ADC1 → 0x08005EB8

0x0800B716..0x0800B72A
poll przez 0x080058A4
```

**CONFIRMED:** hardware bit `0x4` jest uruchamiany i firmware czeka na jego samoczynne wyzerowanie.

**STRONG:** jest to sprzętowa procedura kalibracji ADC peripheral.

---

# 5. FULL CALIBRATION PSEUDOCODE

Odpowiednik kodu `0x0800AF5C`:

```c
done_flag = 0;

if (cal_fault == 0) {
    backup_A = offset_A;
    backup_B = offset_B;
    backup_C = offset_C;
}

offset_A = 0;
offset_B = 0;
offset_C = 0;

wait(10);

/* STAGE 1 */
TIM1_CCER = 0x0880;
MOE = 1;
cal_stage = 1;
wait(100);

counter = 0;
while (counter < 16) {
    wait(10);
    offset_A += ADC0_JDR1 >> 3;
    counter++;
}

MOE = 0;
wait(10);

/* STAGE 2 */
TIM1_CCER = 0x0808;
MOE = 1;
cal_stage = 2;
wait(100);

counter = 0;
while (counter < 16) {
    wait(10);
    offset_B += ADC1_JDR1 >> 3;
    counter++;
}

MOE = 0;
wait(10);

/* STAGE 3 */
TIM1_CCER = 0x0088;
MOE = 1;
cal_stage = 3;
wait(100);

counter = 0;
while (counter < 16) {
    wait(10);
    offset_C += ADC1_JDR2 >> 3;
    counter++;
}

MOE = 0;

TIM1_CCER = 0x0DDD;

/* VALIDATION */
if (
    offset_A > 0x7800 && offset_A < 0x8800 &&
    offset_B > 0x7800 && offset_B < 0x8800 &&
    offset_C > 0x7800 && offset_C < 0x8800
) {
    cal_fault = 0;
    retry_counter = 0;
}
else {
    if (retry_counter < 10)
        retry_counter++;

    if (retry_counter >= 3) {
        cal_fault = 1;
    }
    else {
        offset_A = backup_A;
        offset_B = backup_B;
        offset_C = backup_C;
    }
}

cal_stage = 0;
done_flag = 1;
return;
```

**CONFIRMED**

---

# 6. STAGE 1 HARDWARE STATE

```text
CCER = 0x0880
MOE  = 1
stage flag = 1
wait(100)
16 ×:
    wait(10)
    read 0x4001243C
    offset_A += JDR >> 3
```

Measured path:

```text
ADC0
inserted rank 1
ADC channel 2
JDR1 = 0x4001243C
offset_A = 0x200004EE
```

**CONFIRMED**

Nie przypisuję tego do fizycznego U/V/W.

---

# 7. STAGE 2 HARDWARE STATE

```text
CCER = 0x0808
MOE  = 1
stage flag = 2
wait(100)
16 ×:
    wait(10)
    read 0x4001283C
    offset_B += JDR >> 3
```

Measured path:

```text
ADC1
inserted rank 1
ADC channel 3
JDR1 = 0x4001283C
offset_B = 0x200004F0
```

**CONFIRMED**

---

# 8. STAGE 3 HARDWARE STATE

```text
CCER = 0x0088
MOE  = 1
stage flag = 3
wait(100)
16 ×:
    wait(10)
    read 0x40012840
    offset_C += JDR >> 3
```

Measured path:

```text
ADC1
inserted rank 2
ADC channel 5
JDR2 = 0x40012840
offset_C = 0x200004F2
```

**CONFIRMED**

---

# 9–12. FULL CCER DECODE

Bit layout używany przez kod:

```text
bit 0   CH1E
bit 1   CH1P
bit 2   CH1NE
bit 3   CH1NP

bit 4   CH2E
bit 5   CH2P
bit 6   CH2NE
bit 7   CH2NP

bit 8   CH3E
bit 9   CH3P
bit 10  CH3NE
bit 11  CH3NP

bit 12  CH4E
bit 13  CH4P
```

## `0x0880`

Set bits:

```text
bit 7  CH2NP
bit 11 CH3NP
```

| Signal | State |
|---|---:|
| CH1E | 0 |
| CH1P | 0 |
| CH1NE | 0 |
| CH1NP | 0 |
| CH2E | 0 |
| CH2P | 0 |
| CH2NE | 0 |
| CH2NP | 1 |
| CH3E | 0 |
| CH3P | 0 |
| CH3NE | 0 |
| CH3NP | 1 |
| CH4E | 0 |
| CH4P | 0 |

**CONFIRMED**

## `0x0808`

```text
CH1NP = 1
CH2NP = 0
CH3NP = 1
all E/NE = 0
```

**CONFIRMED**

## `0x0088`

```text
CH1NP = 1
CH2NP = 1
CH3NP = 0
all E/NE = 0
```

**CONFIRMED**

## `0x0DDD`

Każdy z CH1/2/3 ma nibble `0xD = 1101`:

```text
CHxE  = 1
CHxP  = 0
CHxNE = 1
CHxNP = 1
```

CH4:

```text
CH4E = 0
CH4P = 0
```

**CONFIRMED**

### Najważniejszy wniosek z CCER

Podczas wszystkich trzech etapów kalibracji:

**`CH1E = CH1NE = CH2E = CH2NE = CH3E = CH3NE = 0`**

To znaczy:

**CONFIRMED: firmware nie włącza normalnego PWM danego kanału poprzez CCER podczas pobierania offsetu.**

---

# 13. MOE TIMELINE

Helper:

**`0x0800CCF8`**

MOE ON:

```text
0x0800CD3C LDRH [timer+0x44]
0x0800CD40 ORR   #0x8000
0x0800CD44 STRH  [timer+0x44]
```

MOE OFF:

```text
0x0800CD4A LDRH [timer+0x44]
0x0800CD4E UBFX bits 0..14
0x0800CD52 STRH [timer+0x44]
```

Czyli:

```text
BDTR/CCHP = 0x40012C44
MOE       = bit15
```

### Oś

```text
entry
 |
 wait(10)
 |
 CCER = 0x0880
 MOE = 1
 stage = 1
 wait(100)
 16 samples
 MOE = 0
 wait(10)
 |
 CCER = 0x0808
 MOE = 1
 stage = 2
 wait(100)
 16 samples
 MOE = 0
 wait(10)
 |
 CCER = 0x0088
 MOE = 1
 stage = 3
 wait(100)
 16 samples
 MOE = 0
 |
 CCER = 0x0DDD
 validation
 return
```

**CONFIRMED**

Istotne:

**po Stage 3 nie ma `wait(10)` pomiędzy `MOE OFF` a `CCER=0x0DDD`.**

---

# 14. CCR1/2/3/4 TIMELINE

Kalibracja `0x0800AF5C` **nie zapisuje żadnego CCR**.

Wartości po inicjalizacji:

```text
CCR1 = 1000
CCR2 = 1000
CCR3 = 1000
CCR4 = 1999
ARR  = 2000
```

**CONFIRMED**

Procedura modyfikuje:

- CCER
- MOE
- swoje flagi i offsety

ale nie CCR.

---

# 15. PHYSICAL OUTPUT STATE

To miejsce, gdzie szczególnie ważne jest nie zgadywać.

## Twardy fakt

We wszystkich stage:

```text
CH1E/NE = 0
CH2E/NE = 0
CH3E/NE = 0
```

równocześnie:

```text
MOE = 1
```

**CONFIRMED**

Inicjalizacja BDTR składa:

```text
0x0800 + 0x0400 + 0x0019
= 0x0C19
```

czyli raw:

**`BDTR = 0x0C19` przed późniejszymi zmianami MOE.**

**CONFIRMED**

Low byte/dead-time field:

**`0x19` — CONFIRMED**

### Czego firmware nie pozwala jeszcze uczciwie stwierdzić

Nie ma wystarczającego producer/consumer proof, żeby z samego BIN-u nazwać stage:

- „faza do GND”,
- „faza do DC+”,
- „low-side ON”,
- „high-side ON”,
- „faza floating”.

Dlatego:

| Pytanie | Wynik |
|---|---|
| normal PWM enabled? | **NIE — CONFIRMED** |
| konkretny MOSFET fizycznie ON? | **OPEN** |
| faza zwarta do GND? | **OPEN** |
| faza zwarta do DC+? | **OPEN** |
| faza floating? | **OPEN** |
| prąd przez uzwojenie = dokładnie 0? | **OPEN** |

Firmware dowodzi konfiguracji timera, ale nie dowodzi logiki zewnętrznego gate drivera ani elektrycznego poziomu jego wejść.

---

# 16. ADC CONFIGURATION

## Inserted sequence

Oba ADC mają ustawioną długość inserted sequence:

```text
length = 2
```

call:

```text
0x0800B72C ADC0, 2
0x0800B766 ADC1, 2
```

**CONFIRMED**

### ADC0

```text
rank 1 → channel 2 → sample-time encoding 1
rank 2 → channel 0 → sample-time encoding 1
```

Adresy konfiguracji:

```text
0x0800B736..0x0800B740
0x0800B744..0x0800B74E
```

### ADC1

```text
rank 1 → channel 3 → sample-time encoding 1
rank 2 → channel 5 → sample-time encoding 1
```

Adresy:

```text
0x0800B76E..0x0800B798
0x0800B79C..0x0800B7A4
```

**CONFIRMED**

Exact sample-time encoding:

**`1`**

Dokładna liczba cykli ADC wynikająca z kodu `1` nie jest wyliczana w firmware.

**OPEN** jako jednostka fizyczna.

### Alignment

Konfiguracja przekazuje:

```text
0x800
```

do pola sterującego ADC.

**CONFIRMED RAW**

**STRONG:** jest to ustawienie left alignment.

### Resolution

Nie znalazłem w tej ścieżce jawnego przełączania rozdzielczości ADC.

Dlatego dokładne „ADC = N bitów” jako samodzielny fakt wynikający tylko z BIN:

**OPEN**

---

# 17. ADC TRIGGER

Najważniejsze: `0x0800AF5C` **nie wykonuje żadnego software triggera ADC**.

Nie ma w pętlach:

- start conversion,
- JSWSTART,
- SWSTART,
- ręcznego wyzwalania przed każdą próbką.

**CONFIRMED**

Konfiguracja przed kalibracją:

### ADC0

```text
inserted trigger selector field = 0x0000
external inserted trigger enable = 1
```

Adresy:

```text
0x0800B752..0x0800B758
0x0800B75C..0x0800B762
```

### ADC1

```text
inserted trigger selector field = 0x7000
external inserted trigger enable = 1
```

Adresy:

```text
0x0800B7A8..0x0800B7AE
0x0800B7B2..0x0800B7B6
```

### Timer trigger producer

Timer ma ustawione pole master-trigger:

```text
0x70
```

przez:

```text
0x0800B50E r1=0x70
0x0800B512 BL 0x0800E28C
```

Helper wpisuje dokładnie bity `0x70` do `[timer+4]`.

**CONFIRMED**

CCR4:

```text
1999
```

CH4 mode:

```text
0x70
```

**STRONG:** timer generuje TRGO z reference/output compare kanału 4, a master ADC0 otrzymuje ten sprzętowy trigger.

Nie ma dowodu na software-trigger w samej procedurze kalibracji.

---

# 18. ADC MASTER / SLAVE / SYNC

Konfiguracja ADC0 zawiera:

```text
dual/mode field = 0x00010000
```

ADC1 nie dostaje tego samego pola jako własnego master-mode.

**CONFIRMED RAW**

To wskazuje na konfigurację ADC0 jako kontrolującego tryb wielo-ADC.

**STRONG**

Ponieważ:

- ADC0 ma sprzętowy inserted trigger,
- ADC1 jest również używany w tym samym strumieniu,
- calibration routine nie triggeruje ADC1,
- ADC1 JDR1/JDR2 są bezpośrednio konsumowane,

najlepszy wniosek z przepływu kodu jest:

**STRONG: ADC1 pracuje jako partner/slave dla inserted acquisition kontrolowanego przez ADC0.**

Dokładna nazwa trybu z dokumentacji producenta nie występuje jako tekst w firmware:

**OPEN jako nazwa enum**, raw mode `0x10000` pozostaje CONFIRMED.

---

# 19. CHANNEL / JDR MAPPING

| Logical current | ADC | rank | channel | JDR address | offset RAM |
|---|---|---:|---:|---|---|
| A | ADC0 | 1 | 2 | `0x4001243C` | `0x200004EE` |
| B | ADC1 | 1 | 3 | `0x4001283C` | `0x200004F0` |
| C | ADC1 | 2 | 5 | `0x40012840` | `0x200004F2` |

**CONFIRMED**

Fizycznego U/V/W nie przypisuję.

---

# 20. SETTLING DELAYS

Delay helper:

**`0x0800B99C`**

Firmware przekazuje mu argumenty `10`, `100` itd.

Nie ma podstaw, żeby nazwać je automatycznie `µs` albo `ms`.

Dlatego zapisuję je zgodnie z wymaganym standardem jako **wait-units**.

### Sekwencja

| Moment | Delay |
|---|---:|
| przed Stage 1 CCER | 10 units |
| Stage1: MOE ON → first sample | 100 units |
| przed każdą z 16 próbek A | 10 units |
| Stage1 MOE OFF → Stage2 CCER | 10 units |
| Stage2 MOE ON → first sample | 100 units |
| przed każdą próbką B | 10 units |
| Stage2 MOE OFF → Stage3 CCER | 10 units |
| Stage3 MOE ON → first sample | 100 units |
| przed każdą próbką C | 10 units |
| Stage3 MOE OFF → CCER restore | **0 explicit delay** |

**CONFIRMED**

---

# 21–22. SAMPLE LOOP / SAMPLE COUNT

Każdy stage ma osobny identyczny loop.

Przykład A:

```text
counter = 0

loop:
    wait(10)

    r0 = *(uint32_t *)0x4001243C
    r1 = *(uint16_t *)0x200004EE

    r0 = r1 + (r0 >> 3)

    *(uint16_t *)0x200004EE = r0

    counter++
    if counter < 16:
        loop
```

### Liczba

```text
16 × A
16 × B
16 × C
```

łącznie:

**48 bezpośrednich odczytów JDR.**

**CONFIRMED**

### Dummy conversion

Nie ma.

**CONFIRMED**

Pierwsza odczytana próbka jest sumowana.

### EOC/JEOC polling w loop

Nie ma.

**CONFIRMED**

### Flag clear w loop

Nie ma.

**CONFIRMED**

Procedura zakłada, że sprzętowy acquisition stream dostarcza aktualizowane JDR.

---

# 23. OFFSET MATH

To jest istotna korekta względem wcześniejszego uproszczenia „średnia ×16”.

Kod wykonuje **dokładnie**:

```c
uint16_t offset = 0;

for (int n=0; n<16; n++) {
    offset += JDR >> 3;
}
```

Nie wykonuje:

```c
average = sum >> 4;
```

ani:

```c
offset = average << 4;
```

## Dokładna formuła

\[
OFFSET_A =
\sum_{n=0}^{15}
\left(JDR_{A,n} >> 3\right)
\]

analogicznie B/C.

**CONFIRMED**

### Rounding

Logical shift `>>3`, czyli dolne 3 bity każdej próbki są odrzucane **przed dodaniem**.

Nie ma round-to-nearest.

**CONFIRMED**

### Saturation

Nie ma jawnej saturacji.

Akumulator jest zapisywany instrukcją `STRH`, czyli ma 16 bitów.

**CONFIRMED**

---

# 24. A/B/C OFFSET RAM ADDRESSES

| Variable | RAM |
|---|---|
| offset A | `0x200004EE` |
| offset B | `0x200004F0` |
| offset C | `0x200004F2` |
| saved A | `0x200004FC` |
| saved B | `0x200004FE` |
| saved C | `0x20000500` |
| sample counter | `0x2000050C` |
| retry counter | `0x200004FB` |
| calibration stage | `0x20000684` |
| calibration fault | `0x20000688` |
| calibration done | `0x20000678` |

**CONFIRMED**

---

# 25. EXACT VALIDATION LIMITS

Dla A:

```text
0x0800B0B6 CMP offset_A,0x7800
0x0800B0BA BLE fail
```

czyli:

```text
offset_A <= 0x7800 → FAIL
```

następnie:

```text
0x0800B0C0 CMP offset_A,0x8800
0x0800B0C4 BGE fail
```

czyli:

```text
offset_A >= 0x8800 → FAIL
```

To samo dla B oraz C.

## Dokładny warunek

```text
0x7800 < A < 0x8800
0x7800 < B < 0x8800
0x7800 < C < 0x8800
```

czyli:

```text
30720 < offset < 34816
```

**CONFIRMED**

### Co oznacza dawne „1920…2176”

Ponieważ jest 16 próbek:

```text
0x7800 / 16 = 1920
0x8800 / 16 = 2176
```

Zatem próg dotyczy średniej wartości **`JDR >> 3`**:

```text
1920 < mean(JDR >> 3) < 2176
```

Nie jest to automatycznie fizyczny/raw kod przetwornika.

To rozróżnienie jest ważne.

---

# 26. RETRY LOGIC

Nie ma wewnętrznego:

```c
while(calibration_bad)
    calibrate_again();
```

Procedura wykonuje **jedną próbę na jedno wywołanie**.

Po błędzie:

```c
if (retry_counter < 10)
    retry_counter++;

if (retry_counter >= 3)
    cal_fault = 1;
else
    restore_backup();
```

**CONFIRMED**

Czyli:

- failure #1 → counter=1 → restore backup
- failure #2 → counter=2 → restore backup
- failure #3 → counter=3 → `cal_fault=1`
- dalsze błędy → fault pozostaje
- counter przestaje zwiększać się po 10.

Ponowne wywołania pochodzą ze state machine, a nie z lokalnej pętli kalibracji.

---

# 27. LAST-GOOD LOGIC

Na wejściu:

```c
if (cal_fault == 0) {
    backup_A = offset_A;
    backup_B = offset_B;
    backup_C = offset_C;
}
```

**CONFIRMED**

Jeśli nowa kalibracja jest błędna i licznik jest jeszcze `<3`:

```text
saved A → current A
saved B → current B
saved C → current C
```

**CONFIRMED**

To jest rzeczywisty mechanizm **last-known-good** dla pierwszych dwóch kolejnych nieudanych prób.

W trzeciej nieudanej próbie kod nie przywraca backupu.

---

# 28. FAULT LOGIC

Fault RAM:

**`0x20000688`**

Valid calibration:

```text
cal_fault = 0
retry_counter = 0
```

Invalid, retry >=3:

```text
cal_fault = 1
```

**CONFIRMED**

Pierwszy caller natychmiast konsumuje ten flag:

```text
0x08018362 BL calibration
0x08018366 load cal_fault
0x0801836A CBNZ → error/state branch
```

Przy błędzie kod ustawia później state byte na:

```text
0x12
```

w `0x0801838E..0x08018392`.

**CONFIRMED**

Czy `0x12` jest nazwanym „phase-current calibration fault” i jaki ma zewnętrzny error code:

**OPEN**

---

# 29. FINAL RESTORE

Bezpośrednio po ostatniej próbce:

```text
MOE = 0
CCER = 0x0DDD
```

**CONFIRMED**

Kalibracja nie zmienia:

```text
ARR
CCR1
CCR2
CCR3
CCR4
ADC sequence
ADC trigger configuration
ADC sync configuration
IRQ enable
```

w swoim własnym body.

### Końcowe flagi

```text
cal_stage = 0
cal_done  = 1
```

**CONFIRMED**

### Końcowy MOE

**OFF — CONFIRMED**

Procedura nie wykonuje MOE ON po `CCER=0x0DDD`.

---

# 30. CAL OFFSET → NORMAL CURRENT CONSUMER TRACE

To jest jeden z najmocniejszych dowodów w całym reverse.

Normalna funkcja pomiaru prądu:

**`0x0800B17C`**

Czyta te same adresy:

```text
ADC0 JDR1  = 0x4001243C
offset A   = 0x200004EE

ADC1 JDR1  = 0x4001283C
offset B   = 0x200004F0

ADC1 JDR2  = 0x40012840
offset C   = 0x200004F2
```

Przykład A:

```text
0x0800B1B2 load ADC0 JDR1
0x0800B1B6 load offset_A
0x0800B1BA SUB offset_A - (JDR1 << 1)
```

czyli:

```c
current_A_internal =
    offset_A - 2 * ADC0_JDR1;
```

B analogicznie:

```c
current_B_internal =
    offset_B - 2 * ADC1_JDR1;
```

**CONFIRMED**

## Dlaczego suma 16 próbek daje właściwą skalę

Jeżeli zero ADC jest stabilne na `D`:

```text
offset =
16 * (D >> 3)
≈ 2 * D
```

Normalny tor odejmuje:

```text
offset - 2*D
```

więc przy zerowym prądzie:

```text
≈ 0
```

To matematycznie zamyka producer→consumer.

**CONFIRMED**

### Rekonstrukcja trzeciej fazy

`0x0800B17C` wybiera parę mierzoną zależnie od stanu/sector variable.

W odpowiednich branchach trzecia wartość jest wyliczana jako:

```text
-Ia - Ib
```

lub odpowiednia permutacja.

Na przykład:

```text
0x0800B2B0
...
0x0800B2BA SUB
```

tworzy ujemną sumę dwóch pozostałych prądów.

**CONFIRMED**

Czyli stock nie potrzebuje trzech jednoczesnych bieżących pomiarów w każdym stanie.

---

# 30A. NORMAL ADC IRQ TRACE

Vector table:

```text
vector index 34
external IRQ number = 18
handler = 0x08005579
```

kod handlera:

**`0x08005578`**

Sprawdza:

```text
ADC0 status & 0x4
```

a następnie kasuje flagę i przetwarza inserted-data path.

Przy odpowiednim stanie:

```text
0x080055A8 BL 0x08007604
```

a:

```text
0x08007612 BL 0x0800B17C
```

Czyli pełen chain:

```text
ADC event
   ↓
IRQ18 @ 0x08005578
   ↓
0x08007604
   ↓
0x0800B17C
   ↓
JDR - calibrated offsets
   ↓
phase-current reconstruction
   ↓
następne obliczenia sterowania
```

**CONFIRMED**

---

# 31. WHY CCER CHANGES DURING CAL

To jest najciekawszy wynik.

Korelacja jest dokładna:

| Stage | CCER | Jedyny CHxNP = 0 | Current path measured |
|---|---:|---|---|
| 1 | `0x0880` | CH1 | A / ADC0 ch2 |
| 2 | `0x0808` | CH2 | B / ADC1 ch3 |
| 3 | `0x0088` | CH3 | C / ADC1 ch5 |

**CONFIRMED**

Czyli Bafang celowo:

1. wybiera pierwszy logiczny kanał wyjściowy,
2. kalibruje A,
3. wybiera drugi,
4. kalibruje B,
5. wybiera trzeci,
6. kalibruje C.

To nie jest przypadkowe przełączanie.

### Ale co dokładnie robi fizycznie?

W żadnym z tych CCER nie jest ustawione:

```text
CHxE
CHxNE
```

Dlatego firmware **nie dowodzi prostego modelu**:

> „włącza low-side konkretnej fazy”.

Takiego wniosku nie wolno wyciągnąć tylko z `0x0880/0808/0088`.

Najdalej, jak można dojść:

**STRONG:** zmiana `NP` jest celowo zsynchronizowana z wyborem kolejnego toru current sense i prawdopodobnie zmienia stan/off-state właściwego kanału potrzebny do kalibracji.

Natomiast:

- forcing do GND — **OPEN**
- forcing do DC+ — **OPEN**
- floating — **OPEN**
- ustawianie common-mode current amplifier — **OPEN**
- wymuszenie konkretnego low-side — **OPEN**

Nie ma dostatecznego dowodu z samego BIN-u, żeby wybrać jedną z tych fizycznych interpretacji.

---

# 32. CONFIRMED FACTS

1. Calibration entry = `0x0800AF5C`.
2. Funkcja jest wołana z `0x08018362`, `0x0801849A`, `0x0801884A`.
3. PWM/ADC są inicjalizowane wcześniej przez `0x0800B334`.
4. PWM timer base = `0x40012C00`.
5. ARR = 2000.
6. CCR1/2/3 = 1000.
7. CCR4 = 1999.
8. Timer jest uruchomiony.
9. BDTR raw init = `0x0C19`.
10. MOE jest bit15 `timer+0x44`.
11. Stage CCER = `0880 → 0808 → 0088`.
12. Stage 1/2/3 mają MOE ON.
13. Między stage MOE jest OFF.
14. Po Stage3 MOE pozostaje OFF.
15. Final CCER = `0x0DDD`.
16. 16 próbek A.
17. 16 próbek B.
18. 16 próbek C.
19. A = ADC0 JDR1/ch2.
20. B = ADC1 JDR1/ch3.
21. C = ADC1 JDR2/ch5.
22. Każda próbka jest przesuwana `>>3`.
23. Sumowanie jest wykonywane bez dzielenia końcowego.
24. Offsety są 16-bitowe.
25. Validation = strict `(0x7800,0x8800)`.
26. Są osobne backupy A/B/C.
27. Pierwsze dwa błędy przywracają backup.
28. Trzeci kolejny błąd ustawia fault.
29. Retry counter saturuje przy 10.
30. Consumer `0x0800B17C` używa dokładnie tych samych offsetów.
31. Normalna matematyka zawiera `offset - 2*JDR`.
32. Normalny current path wykonuje 2-current measurement + reconstruction third current.
33. IRQ18 prowadzi do tej ścieżki obliczeń.

---

# 33. STRONG FACTS

1. Timer pracuje center-aligned — konfiguracja pola `0x20`.
2. ADC result alignment jest left-aligned — pole `0x800`.
3. Master trigger timer jest generowany przez kanał 4 reference/compare — master selection `0x70`, CCR4=1999.
4. ADC0 jest masterem wielo-ADC; ADC1 uczestniczy jako partner/slave.
5. Trzy zmiany NP są celowo sparowane z kalibracją A/B/C.
6. Nominalne centrum offsetu wewnętrznego wynosi około:

```text
0x8000
```

7. Nominalne centrum JDR w skali używanej przez normalny consumer wynosi około:

```text
0x4000
```

bo:

```text
offset ≈ 2*JDR
```

---

# 34. OPEN QUESTIONS

Po obecnym reverse samego BIN-u nadal **OPEN** pozostają:

1. dokładne fizyczne U/V/W ↔ CH1/2/3;
2. dokładne fizyczne U/V/W ↔ ADC ch2/ch3/ch5;
3. fizyczne napięcie gate driverów przy `CCER=0880/0808/0088`;
4. czy w danym stage konkretna faza jest:
   - floating,
   - GND-clamped,
   - DC+-clamped;
5. czy zmiana NP ma głównie:
   - ustawić common-mode current amplifier,
   - zapewnić zero-current condition,
   - zabezpieczyć mostek,
   - inne znaczenie;
6. fizyczna jednostka `wait(10)` / `wait(100)`;
7. exact sample-time w cyklach wynikający z encoding `1`;
8. dokładna nazwa trybu multi-ADC reprezentowanego przez raw field `0x10000`;
9. fizyczna skala A/count;
10. przełożenie flagi `0x20000688` na publiczny kod błędu HMI/CAN;
11. programowo sterowany opamp/current-amplifier — brak bezspornego sterowania takiego bloku w samej procedurze;
12. dokładny fizyczny stan bramki wynikający z kombinacji `OSSI/OSSR/OIS + polarity + zewnętrzny driver`.

Nie wypełniam tych pól założeniami.

---

# KOŃCOWA TABELA EVIDENCE

| ITEM | STOCK BAFANG FACT | EVIDENCE | CONFIDENCE |
|---|---|---|---|
| Firmware base | `0x08005000` po 32-B header | vector + reset `0x0800A3CD` | CONFIRMED |
| Calibration function | `0x0800AF5C` | direct callers + valid code | CONFIRMED |
| Stage 1 CCER | `0x0880` | `0x0800AF96–AF9C` | CONFIRMED |
| Stage 2 CCER | `0x0808` | `0x0800AFF4–AFFA` | CONFIRMED |
| Stage 3 CCER | `0x0088` | `0x0800B052–B056` | CONFIRMED |
| Restore CCER | `0x0DDD` | `0x0800B0AA–B0B0` | CONFIRMED |
| Samples/channel | 16 | loop compare `#0x10` | CONFIRMED |
| A sample | ADC0 JDR1 `0x4001243C` | `0x0800AFC2` | CONFIRMED |
| B sample | ADC1 JDR1 `0x4001283C` | `0x0800B020` | CONFIRMED |
| C sample | ADC1 JDR2 `0x40012840` | `0x0800B07C–B080` | CONFIRMED |
| Sample math | `JDR >> 3` | `LSR #3` in all stages | CONFIRMED |
| Offset A | `0x200004EE` | calibration + consumer | CONFIRMED |
| Offset B | `0x200004F0` | calibration + consumer | CONFIRMED |
| Offset C | `0x200004F2` | calibration + consumer | CONFIRMED |
| Validation low | `offset > 0x7800` | `CMP/BLE` | CONFIRMED |
| Validation high | `offset < 0x8800` | `CMP/BGE` | CONFIRMED |
| Last good | FC/FE/500 | copy/restore branches | CONFIRMED |
| Fault threshold | 3 failed calls | `CMP #3` | CONFIRMED |
| Retry saturation | 10 | `CMP #0xA` | CONFIRMED |
| MOE ON/OFF | BDTR bit15 | helper `0x0800CCF8` | CONFIRMED |
| End MOE | OFF | final helper call, no later ON | CONFIRMED |
| ARR | 2000 | `0x0800B400–B41A` | CONFIRMED |
| CCR1/2/3 | 1000 | output init | CONFIRMED |
| CCR4 | 1999 | CH4 init | CONFIRMED |
| Consumer | `0x0800B17C` | direct JDR/offset loads | CONFIRMED |
| Current equation | `offset - 2*JDR` | `SUB ..., r0, LSL #1` | CONFIRMED |
| IRQ path | IRQ18 → `0x08005578` → `0x08007604` → `0x0800B17C` | vector + BL chain | CONFIRMED |
| Stage physical GND/DC+/floating | not proven | CCER+MOE insufficient for external bridge state | **OPEN** |
| Current physical A/count | not proven | no physical gain data established | **OPEN** |

# FINAL CONCLUSION

Najważniejszym twardym wynikiem jest to, że stockowy Bafang **nie wylicza zwykłej 16-sample average**.

Tworzy offset w specjalnej skali normalnego toru prądowego:

```text
CAL:
offset = Σ16 (JDR >> 3)

NORMAL:
current = offset - (JDR << 1)
```

Ponieważ:

```text
16 × (JDR >> 3) ≈ 2 × JDR
```

obie strony znajdują się w tej samej skali.

Drugim kluczowym wynikiem jest sekwencja:

```text
CCER 0880 → current A
CCER 0808 → current B
CCER 0088 → current C
```

ale wszystkie `CHxE/CHxNE` są w tych trzech stanach wyzerowane. Dlatego z samego firmware **nie można uczciwie stwierdzić**, że Bafang podczas kalibracji po prostu „włącza po kolei low-side każdej fazy”. Firmware dowodzi selektywnej zmiany konfiguracji polarity/off-state powiązanej 1:1 z kolejnymi torami current sense; dokładny elektryczny powód pozostaje **OPEN**.