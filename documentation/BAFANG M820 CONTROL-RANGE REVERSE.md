# BAFANG M820 CONTROL-RANGE REVERSE

## Zakres

Analiza dotyczy wyłącznie fabrycznego firmware Bafang M820. Nie obejmuje CAN/HMI/BLE ani zmian EVistDrive; zamrożone mechanizmy current-sense nie są ponownie reverse’owane.

Poziomy dowodu:

- **CONFIRMED** — bezpośredni producer/consumer, adresy, stałe i branch/equation.
- **STRONG** — bardzo mocny wniosek z pełnego przepływu, ale bez jednego elementu pozwalającego nazwać go absolutnie.
- **OPEN** — firmware nie daje wystarczającego dowodu.

---

# 1. FINAL THETA VARIABLE

## Finalny kąt elektryczny FOC

```text
RAM:      0x2000054C
type:     int16_t
getter:   0x08007EFC
producer: 0x0800810C
consumer: 0x08009408
```

Getter:

```asm
0x08007EFC  LDR   r0, =0x2000054C
0x08007EFE  LDRSH r0,[r0]
```

Fast current/FOC path:

```text
0x08007604
   ↓
0x0800810C       update theta
   ↓
0x0800B17C       currents
   ↓
0x08006CEC       Clarke
   ↓
0x08007EFC       get theta
   ↓
0x08009408       Park
```

**CONFIRMED**

`0x08009408` używa tego kąta do wyznaczenia sin/cos i wykonania obrotu αβ → rotating frame. Inverse transform w `0x0800A0B4` używa odpowiadających mu zapamiętanych sin/cos.

### Skala

Cały obrót elektryczny:

```text
0x0000 ... 0xFFFF ≈ 0...360°
```

czyli:

```text
65536 counts = 360° electrical
```

Przykłady występujące w stocku:

```text
0x1555 = 29.998°
0x2AAA = 59.996°
0x31C7 = 69.999°
0x8000 = 180°
```

Wrap jest naturalnym wrapem 16-bitowym.

**CONFIRMED**

---

# 2. THETA PRODUCER GRAPH

Pełny odzyskany łańcuch:

```text
PC6 / PC7 / PC8
3-bit Hall/UVW
        │
        ▼
0x08009E78
current Hall state
0x200003EC
        │
        ▼
Hall transition ISR
0x0800C208
        │
        ├── transition direction
        │      0x2000053C
        │
        ├── transition count
        │      0x20000516
        │
        ├── Hall period/history
        │      0x200012C0...
        │
        ├── sector boundary
        │      0x20000526
        │
        └── per-sector correction
               0x200005B0...BA
                     │
                     ▼
period → angle increment
0x08007F08
                     │
                     ▼
Δtheta / FOC tick
0x2000053A
                     │
                     ▼
fast interpolator
0x0800810C
                     │
                     ├── standstill/first-edge center
                     ├── timed interpolation
                     ├── ~70° interpolation clamp
                     └── reverse fallback
                     │
                     ▼
final theta
0x2000054C
                     │
                     ▼
Park 0x08009408
                     │
                     ▼
FOC / rotating voltage
                     │
                     ▼
inverse Park 0x0800A0B4
                     │
                     ▼
SVPWM 0x0800A9E4
```

Do bazowego kąta Halla dochodzi jeszcze:

```text
speed_internal
0x20000370
       ↓
phase advance target
0x200005C4
       ↓
slew limiter
0x200005C0
       ↓
+ base phase offset
0x200005C2
       ↓
global angle offset
0x20000510
```

**CONFIRMED**

Nie znaleziono observera/BEMF produkującego alternatywny `theta`.

---

# 3. STANDSTILL THETA

Statyczny kąt przy braku ruchu produkuje:

```text
0x080082C4
```

Funkcja:

1. czyta aktualny 3-bit Hall przez `0x08009E78`,
2. wybiera jeden z 6 sektorów,
3. dodaje globalny offset,
4. dodaje indywidualną korekcję sektora,
5. ustawia **środek sektora**, a nie jego krawędź.

## Równanie

```text
theta_standstill =
    sector_boundary(Hall)
  + 30° electrical
  + sector_trim(Hall)
  + global_angle_offset
```

modulo 16 bit.

### Stockowe centra sektorów

| Hall | Nominal center |
|---:|---:|
| 4 | ~30° |
| 6 | ~90° |
| 2 | ~150° |
| 3 | ~210° |
| 1 | ~270° |
| 5 | ~330° |

Do tego dochodzi:

```text
global offset @ 0x20000510
+
sector-specific trim
```

Per-sector corrections:

```text
0x200005B0
0x200005B2
0x200005B4
0x200005B6
0x200005B8
0x200005BA
```

**CONFIRMED**

### Najważniejszy wynik

Stock **nie startuje z początku sektora Hall**.

Startuje z:

# **centrum aktualnego sektora = boundary + ~30°**

Nie znaleziono wymuszonego open-loop alignment angle.

**CONFIRMED**

---

# 4. FIRST-MOVEMENT BEHAVIOR

Przed MOE/POEN ON start motor-control wykonuje:

```text
0x08008A84
   ↓
0x080082C4
   ↓
synchronize theta from current Hall
   ↓
0x080083C0
   ↓
MOE ON @ state-machine 0x0801860E
```

Zatem mostek jest uruchamiany **dopiero po ustaleniu kąta na podstawie rzeczywistego Halla**.

**CONFIRMED**

## Pierwsza krawędź Hall

Hall ISR ustawia nową granicę sektora.

Fast updater `0x0800810C` sprawdza:

```c
if (transition_count < 2) {
    if (new_hall_edge)
        theta = hall_boundary + 0x1555;
    else
        theta = static_hall_theta;
}
```

`0x1555 ≈ 30°`.

Czyli:

```text
standstill
    ↓
sector center
    ↓
first Hall edge
    ↓
new sector boundary + 30°
    ↓
still sector center
```

Dopiero po uzyskaniu co najmniej dwóch poprawnych przejść zaczyna się normalna interpolacja czasowa.

**CONFIRMED**

To jest bardzo ważny mechanizm niskiej prędkości: stock nie próbuje wyliczać płynnego kąta z niepewnego pojedynczego okresu Halla.

---

# 5. HALL INTERPOLATION EQUATION

Producer incrementu:

```text
0x08007F08
```

Czas sektora pochodzi z historii przy:

```text
0x200012C0...
```

Funkcje pomocnicze:

```text
0x08007DEC
0x08007E5C
```

obsługują bieżący/zgromadzony sector timing.

## Warunek użycia period

Dla period `P`:

```text
P <= 0x1691       → increment = 0
P >  0x00E883CF   → increment = 0
```

czyli ważny zakres:

```text
0x1691 < P <= 0x00E883CF
```

**CONFIRMED**

## Exact equation

Po co najmniej dwóch poprawnych przejściach:

```text
Δtheta_raw =
    int16(0x029A0000 / Hall_period)
    × direction
```

**CONFIRMED**

Dla pierwszych przejść używany jest bieżący increment.

Po ustabilizowaniu historii — po ponad 12 przejściach — stock uśrednia incrementy z sześciu sektorów:

```text
Δtheta_filtered =
    (Δθ1 + Δθ2 + Δθ3 + Δθ4 + Δθ5 + Δθ6) / 6
```

i zapisuje wynik jako:

```text
0x2000053A
```

Dodatni increment jest dodatkowo ograniczony:

```text
Δtheta <= 0x0FA0 = 4000 counts
```

**CONFIRMED**

---

## Integracja w fast FOC loop

`0x0800810C`:

```c
progress += delta_theta;

if (progress <= 0x31C7)
    interpolated_theta += delta_theta;
```

Limit:

```text
0x31C7 ≈ 70° electrical
```

### Bardzo ważny wynik

Stock **nie clampuje predykcji dokładnie na 60°**.

Pozwala interpolacji dojść do około:

# **70° od ostatniej granicy sektora**

czyli około **10° poza nominalną następną krawędź Halla**, jeżeli kolejny edge się opóźni.

**CONFIRMED**

To zapewnia ciągłość theta przy jitterze/opóźnieniu Halla, ale ma ograniczenie chroniące przed nieograniczonym „uciekaniem” predykcji.

---

# 6. LOW-SPEED SPECIAL BEHAVIOR

Nie znaleziono osobnego „low-speed FOC” ani osobnego transform/PWM.

Specjalne zachowanie niskiej prędkości jest realizowane głównie w **producerze kąta**:

### Region 0 transitions

```text
theta = current Hall sector center
```

### Region 1 transition

```text
theta = new sector boundary + 30°
```

### Region ≥2 valid transitions

```text
theta = boundary + timed interpolation
```

### Po zaniku krawędzi

Stock kasuje estymację czasu/prędkości i wraca do statycznego sector-center.

**CONFIRMED**

### PI gains

Nie znaleziono speed-dependent przełączenia zestawu gainów fast PI dla niskiego RPM.

**STRONG: NO SPECIAL LOW-SPEED PI MODE**

### Startup current boost

Nie znaleziono dedykowanego producenta, którego można uczciwie nazwać `startup current boost`.

Ponieważ upstream torque-request mapping jest poza zakresem obecnego reverse:

**OPEN**

### Static-friction compensation

**OPEN / no dedicated producer proven**

---

# 7. NORMAL-SPEED ANGLE MODEL

Dla poprawnego kierunku forward, po ustabilizowaniu Hall timing:

```text
theta_normal =
    Hall_sector_boundary
  + Hall_time_interpolation
  + sector_trim
  + global_phase_offset
```

gdzie:

```text
Hall_time_interpolation =
    Σ Δtheta_fastloop
```

i:

```text
global_phase_offset =
    base_offset
  + speed_dependent_phase_advance
```

### Finalne równanie logiczne

```text
theta_final =
    boundary(H)
  + trim(H)
  + interpolation(elapsed, Hall_period)
  + base_phase_offset
  + phase_advance(speed)
```

modulo 65536.

**CONFIRMED**

Nie znaleziono:

- PLL angle,
- sensorless observer theta,
- BEMF theta replacement,
- high-speed six-step angle replacement.

---

# 8. HIGH-SPEED ANGLE MODEL

High-speed nie przełącza stocku na inne źródło kąta.

Nadal działa:

```text
Hall
+
sector correction
+
time interpolation
+
phase advance
```

**CONFIRMED/STRONG**

Jedyną potwierdzoną płynną zmianą angle model wraz ze wzrostem Hall-derived speed jest **phase advance**.

Powyżej górnego progu advance osiąga około 30° i pozostaje tam.

Nie znaleziono branchu:

```text
if high_speed:
    theta = BEMF/observer/6step_angle
```

**STRONG: NO HIGH-SPEED ANGLE-SOURCE SWITCH**

---

# 9. BEMF ROLE

Phase-voltage/BEMF channels istnieją w stocku, ale żadna z funkcji finalnego theta:

```text
0x080082C4
0x0800C208
0x08007F08
0x0800810C
0x080188AC...
```

nie pobiera PC0/PC1/PC2 phase-voltage jako korekcji kąta.

Producer graph finalnego theta zamyka się przez:

```text
Hall state
Hall timing
sector trims
speed-derived phase advance
```

### Wynik

**STRONG: NO BEMF ANGLE CORRECTION FOUND**

Nie oznacza to, że BEMF nie jest nigdzie używany przez firmware.

Oznacza dokładnie:

> nie znaleziono producer→consumer path BEMF → final theta / Park angle.

---

# 10. PHASE ADVANCE

To jest jeden z najważniejszych odzyskanych mechanizmów.

Hall-derived speed:

```text
0x08007FAC
   ↓
0x20000370
```

Nie została jeszcze potwierdzona jego fizyczna jednostka, dlatego wartości **140 i 200 nie są nazywane RPM**.

## Target advance

Main-loop producer około:

```text
0x080188AC
```

Równanie:

```c
if (speed_internal < 140)
    advance_target = 0;

else if (speed_internal < 200)
    advance_target =
        ((speed_internal - 140) * 182) / 2;

else
    advance_target = 0x1554;
```

Dla dodatniego speed:

```text
≈ (speed_internal - 140) × 91 angle counts
```

Przy 200:

```text
60 × 91 = 5460 = 0x1554
≈ 29.993°
```

### Krzywa

```text
speed < 140       → 0°
140 ... 200       → linear 0...~30°
speed >= 200      → ~30°
```

**CONFIRMED**

## Slew rate

Actual advance:

```text
0x200005C0
```

Target:

```text
0x200005C4
```

Kod:

```text
actual < target → actual++
actual > target → actual--
```

czyli:

# **maksymalnie 1 angle-count na jedno wykonanie update**

**CONFIRMED**

Finalnie:

```text
global_offset @0x20000510 =
    actual_advance @0x200005C0
  + base_offset @0x200005C2
```

Ten offset jest następnie używany przez każdy sektor Hall.

### Wniosek

# **SPEED-DEPENDENT PHASE ADVANCE = YES — CONFIRMED**

---

# 11. HIGH-SPEED MODE FLAGS

Istnieje dodatkowa flaga:

```text
0x20000698
```

Producer w `0x08007FAC`:

```c
if (speed_internal > 1200)
    flag = 1;
else
    flag = 0;
```

**CONFIRMED**

Jednak przeszukanie bezpośrednich odwołań nie ujawniło consumer chain tej flagi do:

- theta,
- Park,
- PI,
- SVPWM,
- current limit.

### Wniosek

```text
high-speed flag producer = CONFIRMED
motor-control consumer   = OPEN
```

Nie można twierdzić, że próg `1200` przełącza algorytm prowadzenia silnika.

---

# 12. ID_REF PRODUCERS

W fast loop zidentyfikowano dwa komponenty rotating frame, ale **ich kanoniczne przypisanie Id/Iq nie jest w pełni udowodnione z samego BIN-u**.

Dlatego używam:

```text
ROTATING AXIS 0
ROTATING AXIS 1
```

zamiast wymuszać nazwy d/q.

## Axis 0 reference

`0x08007754`:

```text
int16 @0x200004DE
   ↓
clamp 0 ... 0x7FFF
   ↓
0x20000480[0]
```

W fast loop ta wartość trafia bezpośrednio jako pierwszy rotating voltage/control component.

## Axis 1 reference

```text
0x2000069A
   ↓
0x20000480[1]
```

`0x2000069A`:

- jest aktualizowane stopniowo,
- pozostaje nieujemne,
- ma clamp:

```text
<= 0x01F4 = 500
```

i jest reference dla PI drugiej osi.

### Ujemny target

Nie znaleziono producenta:

```text
rotating-current reference < 0
```

zależnego od high-speed/voltage saturation.

### Wynik

Dokładne:

```text
which axis = Id
which axis = Iq
```

pozostaje:

**OPEN**

ale:

**STRONG: NO NEGATIVE Id-LIKE CURRENT TARGET PRODUCER FOUND**

---

# 13. IQ_REF FINAL CONTROL INPUT

Finalny upstream motor-control scalar związany z osią 0 powstaje m.in. jako:

```text
0x200004DE =
    0x2000036E
  + 0x20000674
```

z dalszym thresholdem:

```text
if signed(result) < 500
    result = 0
```

`0x20000674` pochodzi z wolniejszej pętli PI w `0x0800BFA0`.

Natomiast druga oś używa:

```text
0x2000069A
```

jako current-loop reference.

### Kluczowe ograniczenie reverse

Z samego BIN-u i bez wchodzenia w rider-torque/assist mapping nie ma jeszcze uczciwego dowodu, że:

```text
0x200004DE = canonical Iq_ref
```

albo:

```text
0x2000069A = canonical Id_ref
```

### Status

**Final motor-control inputs — CONFIRMED**

**Canonical Id/Iq naming — OPEN**

Nie będę przypisywał osi przez intuicję typowej implementacji FOC.

---

# 14. VECTOR VOLTAGE LIMIT

Po PI i przed inverse Park wywoływane jest:

```text
0x0800A04C
```

Wejście:

```text
x = rotating voltage component 0
y = rotating voltage component 1
```

Kod liczy:

```text
m² = x² + y²
```

i porównuje z:

```text
0x388B1900
```

co jest dokładnie:

```text
30800² = 948640000 = 0x388B1900
```

## Jeśli:

```text
x² + y² <= 30800²
```

brak zmiany.

## Jeśli:

```text
x² + y² > 30800²
```

stock pobiera współczynnik radialnego skalowania z tabeli i wykonuje:

```text
x' = x × k
y' = y × k
```

w fixed-point tak, aby wektor wrócił w okolice promienia 30800.

### Wniosek

To jest:

# **CIRCLE / RADIAL VECTOR LIMITER**

Nie:

- osobny hard clamp Ud,
- osobny hard clamp Uq,
- Id priority,
- Iq priority.

Obie składowe są skalowane proporcjonalnie.

**CONFIRMED**

---

# 15. PI SATURATION / ANTI-WINDUP

Generic PI:

```text
0x0800934C
```

Równanie logiczne:

```c
error = reference - feedback;

P_raw = Kp * error;

I_candidate =
    I_previous + Ki * error;

I =
    clamp(I_candidate,
          integrator_min,
          integrator_max);

P_out = P_raw / P_div;
I_out = I / I_div;

output =
    clamp(P_out + I_out,
          output_min,
          output_max);
```

**CONFIRMED**

Dla zidentyfikowanego fast PI `0x200010B8`:

```text
Kp       = 10
P_div    = 1024
Ki       = 1
I_div    = 1024

output min = -8000
output max = +10000
```

Integrator ma odpowiadające granice około:

```text
-8,192,000
+10,240,000
```

### Anti-windup wewnątrz PI

Integrator jest clampowany.

# **YES — CONFIRMED**

### Czy PI wie o późniejszym circle limiterze 30800?

Nie znaleziono ścieżki:

```text
applied limited Ud/Uq
    ↓
PI back-calculation
```

ani:

```text
vector saturated
    ↓
freeze integrator
```

### Wynik

```text
PI internal integrator clamp       = YES
post-vector saturation feedback    = NO
back-calculation from vector limit = NO
```

**CONFIRMED dla odzyskanej ścieżki**

To oznacza, że stock ma podstawowy anti-windup wewnątrz pojedynczego PI, ale nie ma odzyskanego anti-windup uwzględniającego późniejsze ograniczenie całego voltage vector.

---

# 16. FIELD WEAKENING

Warunek z zadania był jednoznaczny: field weakening można potwierdzić dopiero przy producencie ujemnego `Id_ref`.

W odzyskanych finalnych rotating references:

- nie znaleziono high-speed producer `reference < 0`,
- `0x2000069A` pozostaje nieujemne,
- nie znaleziono voltage-saturation → negative-axis-current path,
- phase advance odbywa się przez **theta**, nie przez ujemny target current.

## Wynik

# **FIELD WEAKENING: STRONG NO**

Dokładniej:

> **NO CONVENTIONAL NEGATIVE Id-LIKE PRODUCER FOUND.**

Nie oznaczam `CONFIRMED NO`, ponieważ kanoniczne mapowanie dwóch osi na Id/Iq nadal jest OPEN.

---

# 17. OVERMODULATION / SIX-STEP

SVPWM producer:

```text
0x0800A9E4
```

ma osobny high-modulation branch.

Sprawdza pierwszy komponent rotating voltage:

```text
> 20000 → mode = 5
< 18000 → mode = 7
18000...20000 → retain previous mode
```

czyli występuje histereza:

```text
ON  threshold = 20000
OFF threshold = 18000
```

**CONFIRMED**

## Mode 5

W każdym sektorze stock przekształca duty przez:

```text
subtract minimum duty from remaining duties
minimum duty = 0
```

np.:

```text
D1 -= Dmin
D2 -= Dmin
Dmin = 0
```

z odpowiednią permutacją zależną od sektora.

To daje **zero-clamped / discontinuous high-modulation PWM**.

**CONFIRMED**

Końcowe CCR są dodatkowo ograniczane do:

```text
ARR = 2000
```

### Overmodulation

# **YES — STRONG**

W firmware istnieje potwierdzony dodatkowy high-modulation/zero-clamp branch zwiększający wykorzystanie dostępnego PWM.

Ścisła akademicka granica „linear SVPWM → formal overmodulation region I/II” nie została jednak w pełni matematycznie odtworzona.

### Six-step PWM

Nie znaleziono:

- przejścia angle source na 6-step,
- osobnego sześciostanowego CCR generatora zastępującego SVPWM,
- switcha z FOC na komutację blokową.

# **SIX-STEP PWM: STRONG NO**

`mode=5` nie jest dowodem fizycznego 6-step.

---

# 18. VERY-LOW-SPEED START

Pełna odzyskana sekwencja:

```text
motor inactive
    ↓
read current Hall
    ↓
theta = current sector center
    ↓
reset relevant angle/control state
    ↓
MOE ON
    ↓
FOC uses static Hall-center theta
    ↓
first Hall transition
    ↓
new sector center
    ↓
second valid transition
    ↓
timed Hall interpolation becomes available
```

**CONFIRMED**

To jest zasadnicza stockowa recepta na płynny start z 3-bit Hall:

> **nie próbować interpolować przed uzyskaniem wiarygodnego czasu sektora.**

Nie znaleziono open-loop electrical-angle sweep.

**STRONG NO OPEN-LOOP START ANGLE SWEEP**

---

# 19. STALL / FIRST-EDGE TIMEOUT

Istnieje Hall no-edge/timeout path około:

```text
0x0800C7E8
```

Licznik:

```text
0x20000515
```

jest zwiększany do maksimum `0x20`.

Po osiągnięciu określonego warunku — m.in. count ≥4 — firmware:

```text
0x20000524 = 1
0x2000052A = 0
0x2000053A = 0
0x20000664 = 0
```

następnie:

```text
BL 0x080082C4
```

czyli ponownie odczytuje aktualny Hall i wraca do statycznego sector-center.

Resetowany jest również:

```text
transition_count @0x20000516 = 0
```

### Wniosek

Po utracie wiarygodnego Hall timing:

```text
predicted speed = 0
interpolation = abandoned
theta = current Hall sector center
```

**CONFIRMED**

Czy ten sam mechanizm generuje oficjalny systemowy `STALL FAULT`:

**OPEN**

Fizyczna jednostka timeoutu:

**OPEN**

---

# 20. ROLLING START

Przed MOE ON stock zawsze wykonuje:

```text
0x080082C4
```

czyli odczytuje **bieżący fizyczny Hall**.

Dlatego już obracający się rotor nie dostaje arbitralnego startup angle.

### Forward rolling rotor

Na wejściu:

```text
theta = current Hall sector center
```

Potem:

```text
first valid edge
→ next sector center

second valid edge
→ normal timed interpolation
```

### Wynik

# **ROLLING ROTOR POSITION SYNCHRONIZATION = YES — CONFIRMED**

Nie wymaga zatrzymania wirnika do znalezienia bezwzględnego sektora.

### Czy stock od razu używa zachowanej poprzedniej prędkości przed pierwszym nowym edge?

Nie jest wystarczająco potwierdzone.

**OPEN**

---

# 21. REVERSE-ROTATION HANDLING

Hall ISR rozpoznaje obie sekwencje kierunku.

Direction:

```text
0x2000053C
```

może być dodatni lub ujemny.

Fast theta updater:

```asm
load signed direction
CMP 0
BGE normal
```

dla ujemnego kierunku wykonuje:

```text
theta =
    reverse_boundary_copy
  + 0x1555
```

czyli:

# **sector center**

Zatem przy kierunku odwrotnym stock **nie kontynuuje forward time interpolation**.

**CONFIRMED**

Dodatkowo Hall-derived speed może być ujemny i jest to rejestrowane w osobnym stanie/liczniku.

### Czy reverse powoduje natychmiastowe odebranie momentu?

Nie odnaleziono bezpośredniego producer→consumer proof:

```text
negative Hall direction
→ torque revoke
```

w obecnym zakresie.

**OPEN**

### Najmocniejszy pewny wniosek

Przy reverse:

```text
angle prediction is degraded to safe Hall-sector-center model
```

zamiast ekstrapolacji w złym kierunku.

**CONFIRMED**

---

# 22. NORMAL STOP

W aktywnym motor state istnieje normalnie wyglądająca ścieżka około:

```text
0x080186B0
```

Warunki:

```asm
config_base + 0x33 == 0
AND
uint16 @0x2000036E <= 100
```

prowadzą do:

```text
motor state = 0
MOE = 0
0x200005EE = 0
0x20000418 = 0
BL 0x08012D90
```

MOE OFF:

```text
0x080186CA
→ 0x0800CCF8(timer,0)
```

**CONFIRMED**

Jeżeli:

```text
0x2000036E > 100
```

sterowanie pozostaje aktywne.

### Bardzo ważne

W tym zidentyfikowanym normalnym stop branch **nie występuje Hall/RPM condition bezpośrednio przed MOE OFF**.

Warunkiem jest zejście scalar control variable:

```text
0x2000036E <= 100
```

plus config gate.

**CONFIRMED**

### Czy stock celowo czeka na prawie zerowe RPM przed MOE OFF?

W tym branchu:

# **NO DIRECT EVIDENCE**

Nie ma:

```text
speed < X
Hall_period > Y
```

w tym bezpośrednim warunku.

Nie wyklucza to, że upstream producer `0x2000036E` pośrednio zachowuje się zależnie od innych warunków.

---

# 23. EXACT MOE-OFF CONDITION

Dla odzyskanej **normalnej** ścieżki:

```c
if (config[0x33] == 0 &&
    control_scalar_0x2000036E <= 100)
{
    motor_state = 0;
    MOE = OFF;
    reset_control_state();
}
```

Adresy:

```text
condition:
0x080186B0 ... 0x080186BE

state zero:
0x080186C0 ... C4

MOE OFF:
0x080186C6 ... CA

control reset:
0x080186D8 → 0x08012D90
```

**CONFIRMED**

Istnieją inne ścieżki MOE OFF związane z innymi state/fault conditions.

Nie są tutaj mieszane z normalnym stop.

### Czy PWM/FOC działa podczas schodzenia do progu?

Tak długo, jak firmware pozostaje w active motor state i powyższy warunek nie wyłącza MOE:

**STRONG YES**

### Czy istnieje długi „Iq=0, ale MOE nadal ON” dwell?

Ponieważ dokładne canonical Iq mapping jest nadal OPEN:

**OPEN**

---

# 24. COMPLETE CONTROL-RANGE STATE MACHINE

Firmware nie ma jednego jawnego enum:

```text
STANDSTILL
LOW
NORMAL
HIGH
SATURATION
```

Dlatego prawidłowym opisem są **LOGICAL OPERATING REGIONS**.

```text
┌─────────────────┐
│   STANDSTILL    │
│ Hall sector     │
│ center          │
└────────┬────────┘
         │ first Hall edge
         ▼
┌─────────────────┐
│ FIRST MOVEMENT  │
│ new sector      │
│ center          │
└────────┬────────┘
         │ >=2 valid transitions
         ▼
┌─────────────────┐
│ VERY LOW /      │
│ INTERPOLATION   │
│ starts          │
└────────┬────────┘
         │ Hall timing valid
         ▼
┌─────────────────┐
│ NORMAL          │
│ Hall + interp   │
│ + trims         │
└────────┬────────┘
         │ speed_internal >140
         ▼
┌─────────────────┐
│ PHASE-ADVANCE   │
│ 0 → ~30°        │
└────────┬────────┘
         │ >=200 internal
         ▼
┌─────────────────┐
│ HIGH SPEED      │
│ same theta      │
│ advance ~30°    │
└────────┬────────┘
         │ voltage demand
         ▼
┌─────────────────┐
│ HIGH MODULATION │
│ / SATURATION    │
│ circle limiter  │
│ + zero-clamp PWM│
└─────────────────┘
```

W dowolnym regionie utrata Hall timing może wykonać:

```text
timed interpolation
        ↓
timeout
        ↓
Δtheta = 0
        ↓
Hall sector center
```

---

# TABLE A — ANGLE BY OPERATING REGION

| REGION | ANGLE SOURCE | EQUATION / BEHAVIOR | CORRECTION | LIMIT | ENTRY | EXIT | CONFIDENCE |
|---|---|---|---|---|---|---|---|
| STANDSTILL | current Hall state | boundary + 30° | sector trim + global offset | fixed sector center | no valid timing | first edge | CONFIRMED |
| FIRST MOVEMENT | Hall | new boundary + 30° | trim + offset | no interpolation | first valid edge | ≥2 transitions | CONFIRMED |
| VERY LOW | Hall + timing | boundary + integrated Δθ | trim + offset | interpolation ≤~70° | ≥2 valid edges | normal continuous timing | CONFIRMED |
| NORMAL | Hall + timing | boundary + interpolation | trim + speed advance | ~70° prediction cap | stable Hall timing | higher speed | CONFIRMED |
| HIGH | same Hall interpolation | same equation | advance saturated ~30° | same prediction cap | speed_internal ≥200 for full advance | speed falls | CONFIRMED |
| SATURATION | same theta | angle unchanged | same advance | voltage vector radius 30800 | voltage vector too large | vector demand falls | CONFIRMED |

---

# TABLE B — HIGH SPEED

| FEATURE | RESULT |
|---|---|
| speed-dependent phase advance | **YES — CONFIRMED** |
| phase advance range | **0 → ~30°** |
| thresholds | **140 / 200 internal speed units** |
| physical RPM equivalents | **OPEN** |
| BEMF angle correction | **STRONG NO** |
| observer/PLL angle switch | **STRONG NO** |
| negative Id-like producer | **STRONG NO** |
| field weakening | **STRONG NO** |
| high-speed flag | **YES, threshold 1200; consumer OPEN** |
| voltage-vector clamp | **YES — radius 30800** |
| proportional vector scaling | **YES** |
| high-modulation zero clamp | **YES** |
| overmodulation extension | **STRONG YES** |
| six-step PWM transition | **STRONG NO** |
| speed-dependent Iq reduction | **OPEN / no dedicated final-path producer found** |
| PI internal anti-windup | **YES** |
| vector-saturation feedback to PI | **NO** |

---

# TABLE C — LOW SPEED

| FEATURE | RESULT |
|---|---|
| absolute angle at zero speed | **YES: Hall sector center** |
| Hall interpolation at standstill | **NO** |
| first-edge interpolation | **NO** |
| interpolation after ≥2 transitions | **YES** |
| interpolation clamp | **~70°** |
| first-edge handling | **sector center** |
| no-edge fallback | **sector center + Δθ=0** |
| startup open-loop angle sweep | **STRONG NO** |
| startup Iq boost | **OPEN** |
| static-friction compensation | **OPEN** |
| Hall timeout handling | **YES — CONFIRMED** |
| system stall fault | **OPEN** |
| rolling-start Hall synchronization | **YES — CONFIRMED** |
| reverse angle handling | **YES: fixed sector-center fallback** |
| reverse torque revoke | **OPEN** |
| special low-speed PI | **STRONG NO** |
| special low-speed angle correction | **YES: center-before-interpolation logic** |

---

# 25. PORTING LESSONS FOR EVISTDRIVE

## HARDWARE / CONTROL NECESSITY

### 1. Absolute Hall-sector angle before PWM

Stock synchronizuje theta **przed MOE ON**.

**PORT TO EVISTDRIVE: HARDWARE/CONTROL NECESSITY**

### 2. Do not interpolate from one Hall interval

Stock wymaga co najmniej dwóch valid transitions, zanim zaufa timingowi.

**PORT TO EVISTDRIVE: HARDWARE/CONTROL NECESSITY**

### 3. Bounded Hall prediction

Interpolacja nie może rosnąć bez końca. Stock stosuje około 70° limitu.

**PORT TO EVISTDRIVE: HARDWARE/CONTROL NECESSITY**

### 4. Timeout → static Hall resynchronization

Przy braku nowych krawędzi:

```text
speed estimate → 0
interpolation → off
theta → Hall center
```

**PORT TO EVISTDRIVE: HARDWARE/CONTROL NECESSITY**

### 5. Voltage-vector circle limitation

```text
sqrt(U0² + U1²) <= limit
```

z proporcjonalnym skalowaniem obu osi.

**PORT TO EVISTDRIVE: HARDWARE/CONTROL NECESSITY**

---

## GOOD STOCK PATTERN

### Per-sector Hall correction

Sześć osobnych trimów pozwala skompensować rzeczywiste mechaniczne/elektryczne rozmieszczenie Halli.

**PORT: GOOD STOCK PATTERN**

### Speed-dependent phase advance

Płynne:

```text
0° → ~30°
```

z slew limiterem zamiast skoku.

**PORT: GOOD STOCK PATTERN**

### High-modulation hysteresis

```text
enter >20000
leave <18000
```

eliminuje szybkie przełączanie mode przy progu.

**PORT: GOOD STOCK PATTERN**

### Integrator clamp

**PORT: GOOD STOCK PATTERN**

---

## OPTIONAL IMPROVEMENT

Stock nie zwraca informacji o późniejszej radialnej saturacji napięcia do PI.

Dla nowej implementacji można rozważyć lepszy anti-windup uwzględniający końcowy applied voltage vector.

**PORT: OPTIONAL IMPROVEMENT — stock tego nie robi.**

---

## DO NOT COPY BLINDLY

Nie kopiować bez własnej kalibracji:

```text
speed thresholds 140 / 200 / 1200
phase advance max 0x1554
PI gains
PI output limits
vector radius 30800
modulation thresholds 18000 / 20000
sector trims
Hall period thresholds
```

To są **stock-specific numbers**, zależne od jego skal fixed-point, timerów i hardware.

---

# 26. CONFIRMED FACTS

1. **Final theta = `int16 @0x2000054C`.**
2. Producer fast theta = `0x0800810C`.
3. Getter = `0x08007EFC`.
4. Direct Park consumer = `0x08009408`.
5. Angle scale = 65536 counts/electrical revolution.
6. Rotor hardware = 3-bit Hall/UVW.
7. Standstill theta = center of current Hall sector.
8. Sector center = boundary + approximately 30°.
9. Six independent per-sector angle trims exist.
10. Stock synchronizes Hall theta before enabling MOE.
11. First Hall edge still uses sector center.
12. Time interpolation does not start until ≥2 valid transitions.
13. Angle increment producer = `0x08007F08`.
14. Increment contains exact numerator `0x029A0000 / period`.
15. Direction sign enters increment.
16. Stable-history filtering averages six sector increments.
17. Positive increment is clamped to `0x0FA0`.
18. Interpolation is allowed to about `0x31C7 ≈70°`.
19. Hall timeout kills interpolation and restores static Hall center.
20. Hall-derived speed stored at `0x20000370`.
21. Speed-dependent phase advance exists.
22. Advance is 0 below 140 internal units.
23. Advance rises linearly between 140 and 200.
24. Advance saturates at `0x1554 ≈30°`.
25. Advance changes only ±1 count per update.
26. No theta-source switch occurs in the recovered high-speed path.
27. BEMF is not a direct final-theta producer in recovered functions.
28. Voltage vector is radially limited to radius 30800.
29. Both vector components are proportionally reduced.
30. Generic PI clamps its own integrator.
31. Final vector saturation is not back-calculated into the PI.
32. High-modulation mode uses 20000/18000 hysteresis.
33. High-modulation branch clamps one duty to zero by common-duty subtraction.
34. No separate six-step angle/PWM generator was identified.
35. Reverse Hall direction is recognized.
36. Negative direction causes fixed sector-center theta instead of normal forward interpolation.
37. Rolling rotor position can be synchronized from current Hall before MOE.
38. One normal stop path switches MOE OFF when `0x2000036E <=100` and its config gate is zero.
39. That MOE-off branch has no direct Hall/RPM predicate.

---

# 27. OPEN QUESTIONS

## A. Physical speed scale

Nie został jeszcze zamknięty przelicznik:

```text
speed_internal @0x20000370
→ mechanical RPM
```

Dlatego:

```text
140
200
1200
```

nie wolno nazywać RPM.

---

## B. Exact canonical d/q assignment

Matematyczny rotating frame jest odzyskany, ale nie ma jeszcze wystarczającego producer proof pozwalającego bez założenia nazwać:

```text
axis0 = Id/Ud
axis1 = Iq/Uq
```

lub odwrotnie w nomenklaturze sterownika Bafanga.

To blokuje absolutnie pewne nazwanie `Id_ref` i `Iq_ref`.

---

## C. Startup torque boost

Nie znaleziono dedykowanego mechanizmu w obecnym zakresie.

Upstream rider-torque mapping celowo nie był reverse’owany.

**OPEN**

---

## D. Reverse torque revoke / rearm

Angle behavior przy reverse jest znany.

Systemowa odpowiedź momentowa:

```text
reverse detected
→ exact torque revoke / rearm condition
```

pozostaje **OPEN**.

---

## E. Complete normal-stop demand ramp

MOE-off endpoint jest znany:

```text
0x2000036E <= 100
```

ale pełny upstream producer/ramp tej zmiennej nie został rozłożony do końca, ponieważ wchodzi w obszar żądania momentu.

---

## F. Strict overmodulation classification

Mechanizm zero-clamp/high-modulation jest twardo potwierdzony.

Pozostaje otwarte wyłącznie akademickie sklasyfikowanie go jako konkretnego regionu formalnego overmodulation.

---

# EVISTDRIVE CONTROL LESSONS

## MUST ADOPT

```text
1. Hall-sector-center theta at zero speed.
2. Synchronize theta before enabling bridge.
3. Do not time-interpolate after only one Hall edge.
4. Start interpolation only after validated transition history.
5. Explicit interpolation/prediction limit.
6. Hall-timeout → zero speed estimate + static sector-center fallback.
7. Per-cycle voltage-vector magnitude limitation.
8. Deterministic theta producer feeding both Park and inverse Park.
```

## SHOULD ADOPT

```text
1. Six independent Hall-sector angle trims.
2. Speed-dependent phase advance with slew limit.
3. High-modulation mode with hysteresis.
4. Strong PI integrator limits.
5. Explicit reverse-direction angle fallback.
6. Consider stronger anti-windup than stock after final vector limiting.
```

## STOCK-SPECIFIC / DO NOT COPY

```text
140 / 200 / 1200 speed thresholds
0x029A0000 timing constant
0x0FA0 increment clamp
0x31C7 interpolation clamp
0x1554 maximum phase advance
30800 voltage-vector radius
18000 / 20000 modulation thresholds
stock PI gains
stock per-sector trim values
```

Bez odtworzenia skal EVistDrive te liczby nie mają bezpośredniej wartości implementacyjnej.

## STILL OPEN

```text
1. speed_internal → physical RPM conversion
2. canonical Id/Iq axis identification
3. full startup torque/current request behavior
4. reverse torque revoke/rearm
5. complete upstream normal-stop demand ramp
6. exact formal classification of zero-clamp modulation
```

---

# FINAL DECISION

## Czy mamy kompletny model prowadzenia rotora:

```text
0 RPM
 ↓
LOW RPM
 ↓
NORMAL
 ↓
HIGH RPM
 ↓
VOLTAGE LIMIT
```

# **PARTIAL — ale model THETA jest już prawie kompletny.**

### Dla forward rotor-angle control odzyskany model jest zasadniczo kompletny:

```text
0 RPM:
Hall sector center

FIRST MOVEMENT:
new Hall sector center

>=2 edges:
Hall time interpolation

NORMAL:
Hall + interpolation + sector trims

HIGH:
ten sam model + speed-dependent advance 0...~30°

late Hall edge:
prediction allowed to ~70°, then frozen

lost Hall timing:
return to static sector center

VOLTAGE LIMIT:
theta unchanged;
voltage vector radially limited;
high-modulation zero-clamp available
```

### Pełnego CONTROL-RANGE modelu nie oznaczam jeszcze jako YES, ponieważ brakuje czterech rzeczy:

1. **fizycznego przeliczenia `speed_internal` na RPM**,  
2. **bezspornego przypisania odzyskanych dwóch rotating axes do canonical Id/Iq**,  
3. **pełnego startup/stop torque-current request path**,  
4. **systemowego torque revoke/rearm przy reverse rotation**.

Najważniejszy wniosek jest jednak zamknięty:

> **Stock Bafang M820 prowadzi rotor od postoju do wysokiej prędkości cały czas z 3-bit Hall. Przy zerowej/małej prędkości używa środka sektora, po uzyskaniu wiarygodnego timing przechodzi płynnie na interpolację, przy wzroście prędkości dodaje do ~30° phase advance, a przy braku napięcia nie zmienia źródła kąta — ogranicza wektor napięcia i zmienia sposób modulacji PWM. Nie znaleziono przejścia na BEMF observer, conventional field weakening ani osobny six-step control mode.**

**STOP — bez implementacji EVistDrive.**