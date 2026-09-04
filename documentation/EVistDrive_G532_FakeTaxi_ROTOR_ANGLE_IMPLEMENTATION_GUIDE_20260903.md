# EVistDrive — G532 + FAKE TAXI + VESC: ROTOR ANGLE / LOW-RPM / HIGH-RPM IMPLEMENTATION GUIDE

**Data:** 2026-09-03  
**Cel dokumentu:** przekazanie kolejnemu agentowi kompletu ustaleń odzyskanych z reverse G532 i Fake Taxi (FT) oraz wniosków z aktualnego open-source VESC FOC, które mają zostać wykorzystane do poprawy toru kąta wirnika w EVistDrive — szczególnie przy starcie, bardzo niskich obrotach, przejściu do normalnej interpolacji, zatrzymaniu oraz wysokich obrotach.

> Ten dokument NIE jest kartą badawczą. To jest handoff wdrożeniowy: co już wiadomo, co jest potwierdzone w firmware i jak tę wiedzę przenieść do EVistDrive bez kopiowania stockowych liczb w ciemno.

---

# 1. GŁÓWNY WNIOSEK

G532 i Fake Taxi rozwiązują problem kąta wirnika na dwa różne sposoby, ponieważ mają różne możliwości sprzętowe.

```text
G532
====
high-resolution rotor position
A/B quadrature + absolute PWM reference
        ↓
4096-count mechanical angle
        ↓
mechanical -> electrical angle
        ↓
omega_e-dependent phase correction
        ↓
FOC

Fake Taxi
=========
3 klasyczne Halle
        ↓
hardware timestamp każdego Hall edge
        ↓
6-sector timing/history
        ↓
sector anchor + per-sector correction
        ↓
continuous interpolation
        ↓
extrapolation bound / timeout
        ↓
speed-dependent phase advance + slew
        ↓
FOC
```

Dla EVistDrive z trzema Hallami **Fake Taxi jest podstawowym wzorcem algorytmicznym**, bo rozwiązuje problem z takim samym typem surowej informacji o położeniu.

G532 pokazuje natomiast, dlaczego nowszy sterownik może zachowywać się jeszcze lepiej przy prawie zerowych obrotach: ma sprzętową informację o położeniu o wysokiej rozdzielczości i nie musi zgadywać kąta między rzadkimi zboczami Halla.

---

# 2. STATUS PEWNOŚCI

W tym dokumencie używamy:

- **CONFIRMED** — wynika bezpośrednio z odzyskanego kodu/dataflow,
- **STRONG** — bardzo mocna interpretacja z kilku śladów,
- **OPEN** — element niezamknięty i nie wolno implementować go jako faktu stocku.

---

# 3. G532 — RZECZYWISTE ŹRÓDŁO KĄTA WIRNIKA

## 3.1. G532 nie opiera normalnego RUN na klasycznych 3 Hallach

**CONFIRMED**

G532 używa sprzętowego toru położenia:

```text
PC6 / PC7
   ↓
TIM8 encoder interface TI1 + TI2
   ↓
CNT = 0...4095
   ↓
high-resolution relative mechanical position
```

TIM8 pracuje jako quadrature encoder counter.

Istotne punkty reverse:

```text
TIM8 config ~0x0800ECC2
TIM8 base   0x40013400
ARR         0x0FFF
CNT range   0...4095
```

Główna funkcja rotor-position około:

```text
0x0800917C
```

czyta bezpośrednio:

```text
TIM8->CNT
TIM8->CR1.DIR
```

Helpery znalezione w obrazie:

```text
0x08010FA0 -> read TIM8 CNT
0x08010FA4 -> read direction
0x08011442 -> write TIM8 CNT
```

To jest zasadnicza różnica względem Fake Taxi.

---

## 3.2. G532 ma również absolute PWM reference

**CONFIRMED**

Drugi tor:

```text
PA1
 ↓
TIM5 PWM-input
 ↓
absolute/reference position
```

Dekoder PWM:

```text
~0x0800DA04
```

W state machine `0x0800917C` występuje sekwencja:

```text
state 0x0A
   ↓
read absolute PWM
   ↓
valid?
   ↓ YES
absolute_count
   ↓
TIM8 CNT = absolute_count
   ↓
state 0x0B RUN
```

Krytyczny fragment znajduje się około:

```text
0x080092D0 ... 0x080092EC
```

Po wejściu w RUN kanał absolute PWM nadal jest używany do plausibility/synchronization supervision: firmware porównuje pozycję PWM z aktualnym TIM8 CNT modulo 4096.

### Wniosek

Funkcjonalnie G532 ma:

```text
absolute reference
+
high-resolution incremental A/B
```

Nie jest jeszcze potwierdzony dokładny model układu magnetycznego na PCB, więc NIE wpisywać konkretnego part-number jako faktu.

---

# 4. G532 — KĄT PRZY BARDZO NISKICH OBROTACH

## 4.1. Bazowy kąt nie zależy od BEMF

**CONFIRMED dla dataflow rotor-position**

W RUN:

```text
CNT = 0...4095
```

jest przeliczany do pełnej 16-bitowej domeny kąta:

```text
mechanical_angle_u16 ~= CNT * 65536 / 4096
```

czyli jeden count to:

```text
360° / 4096 = 0.087890625° mechanicznego
```

Następnie:

```text
base_electrical_angle =
    mechanical_angle * pole_pairs
    + electrical_zero_offset
```

Oznacza to, że przy prawie zerowych obrotach G532 nadal zna położenie wirnika z hardware.

Jeżeli wirnik stoi:

```text
TIM8 CNT stoi
↓
mechanical angle stoi
↓
base electrical angle stoi
```

Nie trzeba uruchamiać osobnej zastępczej formuły Hall/BEMF tylko dlatego, że RPM jest bliski zeru.

---

## 4.2. G532 liczy electrical angular velocity

**CONFIRMED BY FORMULA**

Około:

```text
0x0800926A ... 0x080092AA
```

firmware wykonuje odpowiednik:

```text
omega_e = RPM_mech * pi * pole_pairs / 30
```

Pole `+0x10` struktury rotor-position jest bardzo mocno potwierdzone jako liczba par biegunów.

---

## 4.3. Finalny kąt G532 ma korekcję zależną od prędkości

**CONFIRMED**

Około:

```text
0x080092F4 ... 0x08009310
```

finalny kąt odpowiada:

```text
final_angle =
    base_electrical_angle
    + ((omega_e * angle_speed_coeff) >> 14)
```

Czyli G532 stosuje jawny człon:

```text
angle correction ~ omega_e * K
```

Fizycznie może on kompensować:

- delay czujnika,
- delay dekodowania,
- delay PWM/ADC/control loop,
- phase lag,
- celowy phase advance.

Dokładna fizyczna interpretacja współczynnika pozostaje **OPEN**, ale sama zależność od `omega_e` jest potwierdzona.

### Ważne dla niskiego RPM

Gdy RPM -> 0:

```text
omega_e -> 0
angle_speed_correction -> 0
```

a kąt bazowy nadal pochodzi z fizycznej pozycji enkodera.

---

# 5. G532 — CO Z TEGO WOLNO PRZENIEŚĆ DO EVISTDRIVE

Jeżeli hardware EVistDrive NIE udostępnia A/B + absolute PWM, nie próbować programowo „udawać” 4096-count encodera.

Nie implementować fikcyjnego:

```text
Hall -> ciężki filtr -> pseudo-encoder angle
```

jako odpowiednika G532.

Można natomiast przenieść architekturę:

```text
raw rotor position
↓
canonical cyclic angle
↓
robust direction
↓
robust speed estimate
↓
electrical angle
↓
speed-dependent delay/phase correction
↓
FOC
```

Jeżeli kiedyś zostanie potwierdzone, że hardware silnika/sterownika EVistDrive fizycznie udostępnia A/B albo PWM absolute, wtedy należy ponownie ocenić możliwość przejścia na architekturę G532.

---

# 6. FAKE TAXI — ŹRÓDŁO KĄTA

## 6.1. Fake Taxi nie używa A/B + absolute PWM jak G532

**CONFIRMED**

Fake Taxi używa klasycznych 3 Halli:

```text
PC6
PC7
PC8
 ↓
3-bit Hall state 1...6
```

Funkcja odczytu około:

```text
0x08004E78
```

składa trzy wejścia GPIOC w kod Halla.

Stany:

```text
1...6 = valid Hall states
0 / 7 = invalid/special
```

To zamyka podstawowe pytanie:

```text
Fake Taxi != G532 encoder path
```

---

# 7. FAKE TAXI — SPRZĘTOWY TIMESTAMP HALL

## 7.1. TIM3 pracuje jako hardware Hall timer

**CONFIRMED**

Konfiguracja około:

```text
0x08002FD0
```

łączy:

```text
PC6 / PC7 / PC8
+
TIM3
```

Każde przejście Halla dostaje hardware timestamp.

Tor:

```text
Hall transition
↓
TIM3 capture
↓
CCR1 timestamp
↓
ISR
↓
sector period
```

To jest bardzo ważne dla niskich obrotów: nie wolno opierać precyzyjnego low-speed estimatora na losowym polling GPIO w wolniejszej pętli.

---

# 8. FAKE TAXI — ROZSZERZONY POMIAR CZASU I TIMEOUT

## 8.1. Obsługa TIM3 uwzględnia overflow

**CONFIRMED**

Główna obsługa Hall około:

```text
0x08007208
```

rozróżnia:

```text
capture event
update / overflow event
```

Firmware:

- czyta `TIM3->CCR1`,
- śledzi overflow,
- buduje wydłużony okres pomiędzy zboczami,
- potrafi mierzyć bardzo wolny obrót,
- ma timeout dla bardzo wolnego/zatrzymanego rotora,
- po utracie wiarygodności resetuje/re-anchoruje estimator.

### Kluczowa zasada do EVistDrive

Nie wolno robić:

```text
ostatni period
↓
brak nowego Halla
↓
theta += omega * dt bez końca
```

Musi istnieć:

```text
Hall edge timeout
+
extrapolation validity
+
safe re-anchor
```

---

# 9. FAKE TAXI — LEGAL TRANSITION VALIDATION

**CONFIRMED**

FT sprawdza poprzedni i nowy Hall state w tabeli przejść.

Rozpoznawane są co najmniej:

```text
legal forward
legal reverse
illegal transition
unknown / invalid
```

Dopiero legalny transition aktualizuje timestamp/period/direction.

### Wymaganie EVistDrive

Przed aktualizacją estimatora:

```c
new_hall = read_hall();
transition = hall_transition_table[old_hall][new_hall];

if (transition == LEGAL_FWD || transition == LEGAL_REV) {
    accept_edge();
} else {
    reject_edge();
}
```

Jeden zakłócony edge nie może bezpośrednio wejść do theta Parka.

---

# 10. FAKE TAXI — 6-SECTOR TIMING MODEL

## 10.1. Nie używa tylko ostatniego okresu

**CONFIRMED / STRONG co do nazw pól**

FT przechowuje informacje odpowiadające wszystkim sześciu sektorom.

Po uzyskaniu stabilnej historii:

```text
sector1 increment/period ┐
sector2 increment/period │
sector3 increment/period │
sector4 increment/period ├─ sum / 6 -> averaged angular increment
sector5 increment/period │
sector6 increment/period ┘
```

Średnia jest używana przez interpolator.

### Dlaczego jest to ważne

Realne Halle nie są idealnie rozmieszczone.

Jeżeli sektory fizycznie mają np.:

```text
58°
62°
59°
61°
60.5°
59.5°
```

estymacja oparta tylko na ostatnim sektorze będzie pompowała prędkość i kąt co 60°.

Pełnoobrotowa historia zmniejsza tę modulację.

---

# 11. FAKE TAXI — START / BARDZO NISKIE OBROTY

To jest jeden z najważniejszych fragmentów dla EVistDrive.

## 11.1. Standstill / brak stabilnego lock

**CONFIRMED**

Przed uzyskaniem wiarygodnego timing estimatora FT używa bezpośrednio anchor angle przypisanego do aktualnego Halla.

Przy postoju:

```text
theta = current Hall sector center
```

Czyli nie próbuje udawać precyzyjnej pozycji wewnątrz sektora bez danych.

---

## 11.2. Pierwszy Hall edge

**CONFIRMED z reverse CONTROL-RANGE**

Pierwszy valid Hall edge nadal używa sector-center/anchor modelu.

Geometria wykorzystuje offset około:

```text
0x1555 ~= 30° electrical
```

czyli logicznie:

```text
new sector boundary + ~30°
```

---

## 11.3. Interpolacja dopiero po uzyskaniu historii

**CONFIRMED**

Normalna time interpolation nie startuje natychmiast od pierwszej zmiany Halla.

Odzyskany model:

```text
0 valid transitions
    -> static current-sector center

1 valid transition
    -> new-sector center / anchor

>= 2 valid transitions
    -> timed continuous interpolation may become active
```

### Znaczenie

FT nie próbuje wyliczać wiarygodnej prędkości i theta prediction z jednej niepewnej próbki okresu po postoju.

Dla EVistDrive to jest ważniejszy wzorzec niż agresywny open-loop predictor.

---

# 12. FAKE TAXI — CONTINUOUS INTERPOLATION

## 12.1. Główny interpolator

**CONFIRMED**

Funkcja około:

```text
0x0800310C
```

jest wykonywana z szybkiej ścieżki około:

```text
0x08002606
```

Aktualizuje theta również wtedy, gdy w bieżącym cyklu nie ma nowego Hall edge.

Funkcjonalnie:

```text
sector anchor
+
angular_increment_per_fast_cycle
+
angular_increment_per_fast_cycle
+
...
=
continuous theta
```

Po uzyskaniu lock każdy szybki cykl zwiększa/zmniejsza accumulator kąta zgodnie z kierunkiem i prędkością.

To jest właściwa metoda continuous FOC angle na 3 Hallach.

---

# 13. FAKE TAXI — EXACT INCREMENT / SKALA

Z odzyskanego reverse CONTROL-RANGE:

```text
angle increment producer = 0x08007F08
```

W równaniu występuje licznik:

```text
0x029A0000 / Hall_period
```

Dokładny Q-format i wszystkie warunki należy ponownie sprawdzić przy implementacji, ale ważny jest wzorzec:

```text
measured Hall period
↓
angular increment per fast loop
↓
continuous theta
```

Nie kopiować `0x029A0000` bez upewnienia się, że EVistDrive ma tę samą częstotliwość pętli, timer clock i angle domain.

---

# 14. FAKE TAXI — OGRANICZENIE EKSTRAPOLACJI

**CONFIRMED**

Interpolator ma limit:

```text
0x31C7
```

w domenie:

```text
65536 counts / electrical revolution
```

co odpowiada około:

```text
70° electrical
```

Czyli FT pozwala przewidywaniu przejść mniej więcej:

```text
60° nominalnego sektora
+
~10° marginesu
```

ale nie pozwala theta uciekać dalej bez nowego Halla.

### Wymaganie EVistDrive

Implementować funkcjonalnie:

```c
if (abs(theta_predicted_from_last_edge) <= extrapolation_limit) {
    theta = predicted;
} else {
    theta = safe_hold_or_reanchor;
}
```

Nie kopiować sztywno 70° jako „magicznej prawdy”.

Startowy zakres dla testów może być klasy:

```text
1 sector + mały margines
```

ale finalną wartość dobrać na logach.

---

# 15. FAKE TAXI — HALL TIMEOUT

**CONFIRMED**

Po zbyt długiej ciszy Hall:

```text
prediction validity = false
↓
interpolation/timing state reset
↓
return/re-anchor to static Hall sector center
```

### Najważniejsza zasada

Nie mieszać dwóch niezależnych formuł kąta, które mają różne offsety.

Fallback i normalna ścieżka MUSZĄ być w tej samej domenie:

```text
same sector anchor
same sector correction
same global/base correction
```

Różnica między fallback i interpolated path powinna dotyczyć wyłącznie składnika predykcyjnego, a nie bazowego zera kąta.

To jest krytyczne dla obecnego problemu EVistDrive, gdzie twarde przejście pomiędzy dwiema niespójnymi formułami może generować klik przy starcie lub końcowym zatrzymaniu.

---

# 16. FAKE TAXI — 6 INDYWIDUALNYCH KOREKCJI SEKTORÓW

**CONFIRMED**

FT posiada sześć trwałych korekcji Hall-angle:

```text
0x200005B0
0x200005B2
0x200005B4
0x200005B6
0x200005B8
0x200005BA
```

Funkcja mapowania około:

```text
0x080032C4
```

buduje anchor jako:

```text
nominal sector geometry
+
sector-specific correction
+
common/base correction
```

### Znaczenie

FT jawnie zakłada, że fizyczna geometria Halli nie jest idealna.

EVistDrive nie powinien zakładać, że wszystkie sektory mają idealne 60° i idealnie wspólne zero, jeżeli logi pokazują inaczej.

---

# 17. FAKE TAXI — KOREKCJE SĄ TRWAŁE

**CONFIRMED**

Write path około:

```text
0x08006900
```

zapisuje blok korekcji do pamięci trwałej.

Read path około:

```text
0x0800DE78
```

przywraca go do RAM.

### Rekomendacja EVistDrive

Docelowo przewidzieć:

```text
Hall sector calibration table[6]
```

w konfiguracji trwałej.

Na pierwszym etapie można wdrożyć tabelę i logowanie bez automatycznej procedury kalibracji.

Automatycznego algorytmu kalibracji NIE implementować na podstawie niepełnego reverse.

---

# 18. FAKE TAXI — HIGH-RPM: NIE ZMIENIA ŹRÓDŁA KĄTA

**CONFIRMED / STRONG**

Nie znaleziono przełączenia:

```text
Hall interpolation
-> BEMF angle
```

ani:

```text
Hall FOC
-> sensorless observer
```

ani:

```text
Hall FOC
-> high-speed six-step angle generator
```

Przy wysokich obrotach nadal działa ten sam podstawowy tor:

```text
Hall
+
sector correction
+
time interpolation
+
phase correction
```

To jest ważny wzorzec:

> Nie naprawiać high-RPM przez drugi, całkiem inny generator theta, jeśli ten sam estimator Hall może pozostać źródłem kąta.

---

# 19. FAKE TAXI — SPEED-DEPENDENT PHASE ADVANCE

## 19.1. Producent korekcji

**CONFIRMED dla zależności; STRONG dla jednostki speed**

Około:

```text
0x080138B8 ... 0x08013936
```

firmware bierze speed-derived value:

```text
0x20000370
```

i generuje target phase advance.

Odzyskana logika:

```c
if (speed < 140) {
    target_advance = 0;
}
else if (speed < 200) {
    target_advance = (speed - 140) * 91;
}
else {
    target_advance = 0x1554;
}
```

Maksimum:

```text
0x1554 / 65536 * 360° ~= 30° electrical
```

### Interpretacja speed domain

Z odzyskanego równania `0x20000370`:

```text
increment * 16000 * 10
----------------------
65536 * 7
```

wartość bardzo dobrze pasuje do:

```text
mechanical rotor frequency * 10
```

przy założeniu:

```text
FOC ~16 kHz
pole_pairs = 7
```

Wtedy orientacyjnie:

```text
140 -> ~14 Hz mechanical -> ~840 rotor RPM
200 -> ~20 Hz mechanical -> ~1200 rotor RPM
```

To jest **STRONG**, nie traktować jednostki jako 100% zamkniętej bez ponownego sanity-check.

### NIE kopiować progów 140/200 do EVistDrive

Mają znaczenie tylko jako dowód architektury:

```text
low RPM  -> no advance
mid RPM  -> increasing advance
high RPM -> saturated advance
```

---

# 20. FAKE TAXI — PHASE ADVANCE MA SLEW

**CONFIRMED**

FT nie wpisuje targetu phase advance skokowo.

Aktualny offset przesuwa się o:

```text
+1 count / update
```

lub:

```text
-1 count / update
```

w stronę targetu.

Czyli:

```text
speed changes
↓
new target advance
↓
slew/rate-limit
↓
actual advance
↓
theta final
```

### Krytyczna zasada EVistDrive

NIE robić:

```c
theta_offset = lookup(speed);
```

jeśli lookup może dać skok.

Robić:

```c
target = angle_advance_map(speed);
actual = slew_to(actual, target, configured_step);
```

---

# 21. FAKE TAXI — FINALNY MODEL THETA

Po złożeniu odzyskanych elementów:

```text
Hall PC6/PC7/PC8
      ↓
legal transition validation
      ↓
TIM3 hardware edge timestamp
      ↓
extended sector timing / timeout
      ↓
6-sector period/increment history
      ↓
robust averaged angular increment
      ↓
sector anchor
      +
sector-specific persistent correction
      +
common/global phase correction
      +
continuous timed interpolation (when valid)
      +
speed-dependent phase advance (slewed)
      ↓
FINAL THETA
      ↓
Park / FOC
```

W recovered CONTROL-RANGE finalny theta jest:

```text
RAM      0x2000054C
producer 0x0800810C
getter   0x08007EFC
consumer 0x08009408
```

Domena kąta:

```text
65536 counts = 1 electrical revolution
```

---

# 22. FAKE TAXI — LOW-SPEED SPECIAL BEHAVIOR

Ważne: nie znaleziono osobnego „low-speed FOC” ani osobnego zestawu transformacji/PWM.

Specjalne zachowanie niskiej prędkości siedzi głównie w **producerze kąta**.

Model:

```text
0 transitions:
    theta = current Hall sector center

1 transition:
    theta = new sector center/anchor

>=2 valid transitions:
    theta = sector anchor + timed interpolation

Hall timeout:
    interpolation invalid
    -> reset timing
    -> safe sector-center fallback
```

Nie znaleziono potwierdzonego speed-dependent przełączenia osobnego zestawu gainów fast PI dla low RPM.

**STRONG: NO SPECIAL LOW-SPEED PI MODE**

To sugeruje, że najpierw trzeba naprawić theta, a nie próbować maskować zły kąt innym Kp/Ki.

---

# 23. FAKE TAXI — REVERSE DIRECTION

Z odzyskanego CONTROL-RANGE:

**CONFIRMED**

- reverse Hall direction jest rozpoznawany,
- dla ujemnego kierunku znaleziono ścieżkę używającą fixed sector-center theta zamiast normalnej forward interpolation.

To jest istotne dla EVistDrive, jeżeli reverse/backpedal bierze udział w natychmiastowym STOP.

Nie kopiować zachowania 1:1 bez sprawdzenia obecnej polityki reverse EVistDrive, ale trzeba mieć osobny test regresyjny:

```text
forward low-RPM
reverse movement
backpedal stop
```

---

# 24. DOCELOWA ARCHITEKTURA EVISTDRIVE DLA 3 HALLI

Agent ma dążyć do jednej kanonicznej ścieżki theta, a nie dwóch niespójnych formuł.

Docelowo:

```text
RAW HALL GPIO
    ↓
HARDWARE EDGE CAPTURE
    ↓
LEGAL TRANSITION TABLE
    ↓
DIRECTION
    ↓
EXTENDED TIMESTAMP
    ↓
SECTOR PERIOD[6]
    ↓
ROBUST FULL-CYCLE / MULTI-SECTOR SPEED
    ↓
SECTOR INDEX
    ↓
SECTOR NOMINAL ANCHOR
    +
SECTOR CALIBRATION OFFSET[6]
    +
GLOBAL/COMMON ANGLE OFFSET
    ↓
CANONICAL BASE THETA
    ↓
INTERPOLATION VALIDITY STATE
    ├─ UNLOCKED -> base theta only
    ├─ LEARNING -> base theta only
    ├─ LOCKED   -> base theta + interpolation
    └─ TIMEOUT  -> base theta / safe re-anchor
    ↓
EXTRAPOLATION LIMIT
    ↓
SPEED-DEPENDENT PHASE ADVANCE TARGET
    ↓
PHASE-ADVANCE SLEW
    ↓
FINAL THETA
    ↓
PARK / FOC
```

Najważniejsze:

```text
BASE THETA
```

musi być wspólne dla wszystkich stanów.

Nie wolno mieć:

```text
LOW_SPEED_FORMULA  = anchor A
HIGH_SPEED_FORMULA = anchor B + different offset
```

bo przełączenie może dać natychmiastowy skok wektora prądu.

---

# 25. PROPONOWANA MASZYNA STANÓW THETA W EVISTDRIVE

Nazwy są projektowe, nie pochodzą ze stocku.

```text
ANGLE_UNLOCKED
    |
    | valid Hall state
    v
ANGLE_ANCHORED
    |
    | first legal transition
    v
ANGLE_LEARNING
    |
    | >= required valid history
    v
ANGLE_INTERPOLATED
    |
    | no Hall too long / invalid timing
    v
ANGLE_TIMEOUT
    |
    | safe re-anchor to current Hall
    v
ANGLE_ANCHORED
```

### ANGLE_UN/ANCHORED

```text
theta = canonical_sector_anchor
```

### ANGLE_LEARNING

```text
theta = canonical_sector_anchor
```

Do not enable high-confidence interpolation yet.

### ANGLE_INTERPOLATED

```text
theta = canonical_sector_anchor
      + bounded_interpolation
```

### ANGLE_TIMEOUT

```text
interpolation = invalid
speed confidence = low
angle = canonical sector anchor
```

Ale wszystkie stany nadal stosują:

```text
sector correction
+
global/common angle correction
```

---

# 26. KLUCZOWA ZMIANA DLA OBECNEGO PROBLEMU KLIKU EVISTDRIVE

Jeżeli obecny kod ma dwie formuły theta, które w tej samej fizycznej pozycji zwracają różne kąty:

```text
formula A -> theta_A
formula B -> theta_B
```

nie należy naprawiać problemu wyłącznie przez wolniejsze przełączanie prądu.

Najpierw doprowadzić do:

```text
theta_A(base) == theta_B(base)
```

w punkcie przejścia.

W praktyce:

1. jeden wspólny `canonical_sector_anchor`,
2. jeden wspólny `sector_offset[6]`,
3. jeden wspólny `global_phase_offset`,
4. interpolacja jako dodatkowy składnik, nie alternatywne zero kąta,
5. phase advance jako dodatkowy, rate-limited składnik,
6. przy utracie confidence usuwać/skasować interpolation state bez zmiany bazowego frame of reference.

---

# 27. KOLEJNOŚĆ WDROŻENIA — NIE ROBIĆ WSZYSTKIEGO NARAZ

## ETAP 0 — AUDYT OBECNEGO EVISTDRIVE

Bez zmiany zachowania produkcyjnego odtworzyć:

```text
raw Hall
-> Hall state
-> direction
-> current angle source
-> low-speed angle formula
-> normal-speed angle formula
-> current speed estimate
-> Park theta
```

Znaleźć wszystkie write'y do final theta.

Odpowiedzieć:

- czy istnieje więcej niż jeden owner theta,
- gdzie następuje switch formuły,
- jaka jest różnica `theta_before -> theta_after` w chwili switch,
- czy oba tory stosują ten sam Hall offset/global offset.

---

## ETAP 1 — OBSERVABILITY / LOGGER

Dodać logowanie bez zmiany sterowania.

Minimum:

```text
hall_raw
hall_state
hall_prev
hall_transition_valid
hall_direction
hall_edge_counter
hall_capture_ticks
hall_extended_period
sector_period[0..5]
sector_period_avg
angle_state
angle_confidence
sector_anchor
sector_trim
angle_global_offset
angle_interpolation
angle_extrapolated_since_edge
angle_advance_target
angle_advance_actual
theta_final
theta_delta_per_cycle
speed_raw
speed_filtered / robust
Iq_ref
Iq_meas
Id_meas
MOE/state
```

Szczególnie logować eventy:

```text
FIRST_VALID_HALL
SECOND_VALID_HALL
INTERPOLATION_ENABLE
INTERPOLATION_DISABLE
HALL_TIMEOUT
ANGLE_REANCHOR
DIRECTION_CHANGE
PHASE_ADVANCE_START
PHASE_ADVANCE_SATURATION
```

---

## ETAP 2 — HARDWARE TIMESTAMP + LEGAL TRANSITION

Jeżeli obecny EVistDrive tego jeszcze nie robi w sposób równoważny FT:

1. timestamp każdego legalnego Hall edge w timer capture,
2. overflow extension,
3. legal transition table,
4. reject illegal transitions,
5. jawny direction state.

Nie zmieniać jeszcze phase advance.

---

## ETAP 3 — 6-SECTOR HISTORY

Dodać:

```c
sector_period[6];
```

oraz robust speed/increment bazujący na historii całego cyklu zamiast wyłącznie ostatniego sektora.

Nie musi to być od razu literalne `sum/6`, jeżeli istniejący scheduler wymaga innej implementacji, ale efekt funkcjonalny ma być taki sam:

```text
mniej modulacji speed/theta od nierównej geometrii Hall
```

---

## ETAP 4 — JEDEN CANONICAL BASE ANGLE

To etap krytyczny dla kliku.

Zbudować:

```text
base_theta =
    nominal_sector_anchor
    + sector_trim
    + global_phase_offset
```

Każda gałąź low-speed/normal/timeout ma korzystać z tego samego `base_theta`.

Nie istnieją już dwa różne zera kąta.

---

## ETAP 5 — LOW-SPEED LOCK / LEARNING

Wdrożyć zachowanie klasy Fake Taxi:

```text
standstill:
    base_theta only

first legal edge:
    re-anchor to new sector base_theta
    do not trust normal interpolation yet

sufficient valid history:
    enable interpolation
```

Początkowo użyć co najmniej modelu:

```text
>=2 valid transitions before full interpolation
```

ale finalną politykę można rozszerzyć o confidence, jeśli logi pokażą potrzebę.

---

## ETAP 6 — EXTRAPOLATION BOUND + HALL TIMEOUT

Dodać dwa osobne mechanizmy:

### A. Geometry bound

```text
predicted angle from last Hall edge
<= ~1 sector + margin
```

### B. Time bound

```text
no valid Hall edge too long
-> interpolation invalid
```

Nie wolno dopuścić do nieograniczonego `theta += omega * dt`.

---

## ETAP 7 — PER-SECTOR ANGLE TRIMS

Dodać tablicę:

```c
int16_t hall_angle_trim[6];
```

Na początku:

```text
all = 0
```

Następnie na logach sprawdzić geometry mismatch sektorów.

Dopiero później dodać procedurę kalibracji i zapis trwały.

Nie kopiować wartości FT.

---

## ETAP 8 — HIGH-RPM PHASE ADVANCE

Dopiero po stabilizacji bazowego estimatora.

Implementować model:

```text
LOW SPEED:
    target advance = 0 or calibrated small value

MID SPEED:
    target increases smoothly

HIGH SPEED:
    target saturates at calibrated maximum
```

Actual correction:

```text
actual_advance = slew(actual_advance, target_advance)
```

Nie zmieniać źródła theta przy high RPM.

---

# 28. HIGH-RPM — JAK STROIĆ BEZ STANOWISKA POMIAROWEGO

Jeżeli nie ma hamowni/oscyloskopu, użyć logów i odczucia, ale logi muszą rozdzielać angle od current-loop.

Obserwować:

```text
RPM
Iq_ref
Iq_meas
Id_meas
theta
phase advance
battery current
Vbus
modulation/saturation status
```

### Dobry angle przy high RPM powinien dawać

```text
mniejsze |Id| przy zadanym Iq
stabilniejsze Iq
mniej acoustic roughness
mniej spadku momentu przy wzroście RPM
mniej PI effort/saturation
```

Jeżeli phase advance rośnie, a `|Id|` rośnie i sprawność/odczucie się pogarsza, kierunek/wartość korekcji jest zły.

Nie używać samego „rower jedzie szybciej” jako jedynego kryterium.

---

# 29. LOW-RPM — TESTY OBOWIĄZKOWE

Każdy etap angle estimatora musi przejść:

## Test L1 — cold start

```text
pełny postój
-> lekki nacisk
-> bardzo powolne ruszenie
```

Sprawdzić:

- delta theta przy pierwszym prądzie,
- first Hall edge,
- second Hall edge,
- moment enable interpolation,
- Id/Iq transient,
- brak kliku.

## Test L2 — crawl

Utrzymywać minimalne obroty zębatki.

Sprawdzić:

- brak okresowego pompowania theta,
- brak cyklicznego Id leakage,
- stabilne przejścia sektorów.

## Test L3 — zatrzymanie bez reverse

```text
normal assist
-> release
-> rotor bardzo wolno
-> zero
```

Sprawdzić:

- interpolation disable,
- Hall timeout/re-anchor,
- delta theta przy re-anchor,
- Iq_meas w tym momencie.

## Test L4 — reverse/backpedal

Sprawdzić direction handling i brak dużego theta jump.

## Test L5 — restart zanim rotor całkiem się zatrzyma

Nie wolno resetować estimatora tak agresywnie, żeby krótki re-demand dostał inne zero kąta.

---

# 30. HIGH-RPM — TESTY OBOWIĄZKOWE

## Test H1 — stała kadencja / lekki moment

Obserwować `Id_meas` vs `Iq_meas`.

## Test H2 — rosnące RPM

Obserwować start phase advance i jego slew.

## Test H3 — wysoki RPM + wysoki moment

Sprawdzić:

- voltage saturation,
- Id leakage,
- Iq tracking,
- acoustic roughness.

## Test H4 — szybki spadek RPM

Phase advance musi wracać płynnie, nie skokowo.

---

# 31. KRYTERIA ZALICZENIA TORU THETA

## Low speed PASS

- brak jednocyklicznego skoku theta przy switch stanu estimatora,
- fallback i interpolated path mają wspólny base angle,
- pierwszy i drugi Hall edge nie powodują mechanicznego kliku,
- Hall timeout nie powoduje zmiany bazowego frame of reference,
- interpolation nie ucieka bez ograniczenia,
- reverse transition nie wprowadza niekontrolowanego theta jump.

## Normal speed PASS

- stabilne sector-to-sector theta,
- speed estimate nie pompuje mocno z geometrii pojedynczego sektora,
- brak cyklicznego Id leakage skorelowanego z konkretnym Hall sector.

## High speed PASS

- brak przełączenia na drugi niekompatybilny angle source,
- phase advance zmienia się płynnie,
- phase advance ma clamp,
- `Id` nie rośnie systematycznie z powodu źle dobranej korekcji,
- Iq tracking nie pogarsza się po dodaniu advance.

---

# 32. CZEGO NIE ROBIĆ

1. **Nie kopiować G532 encoder code do Hall-only hardware.**
2. **Nie kopiować stałych FT 140/200/0x1554/0x31C7 bez walidacji domeny.**
3. **Nie używać dwóch niezależnych zero/offset angle formulas.**
4. **Nie przełączać high RPM na BEMF/sensorless tylko dlatego, że „tak powinno być”. FT tego nie robi w odzyskanym torze.**
5. **Nie stroić PI, żeby ukryć skok theta.** Najpierw naprawić angle estimator.
6. **Nie dodawać mocnego LPF na theta jako substytutu dobrej geometrii/timestamp.**
7. **Nie wykonywać nieograniczonej ekstrapolacji po utracie Hall edge.**
8. **Nie wpisywać phase advance bez slew.**
9. **Nie implementować automatycznej kalibracji Hall trims bez osobnego potwierdzenia algorytmu.**
10. **Nie zmieniać równocześnie angle estimatora, PI, current sampling i torque ramp.** Inaczej nie będzie wiadomo, co poprawiło lub zepsuło jazdę.

---

# 33. RÓŻNICA G532 VS FAKE TAXI — TABELA KOŃCOWA

| Funkcja | Fake Taxi | G532 | Znaczenie dla EVistDrive |
|---|---|---|---|
| Podstawowy sensor | 3 Hall | high-resolution A/B | EVist Hall-only powinien wzorować estimator na FT |
| GPIO/timer | PC6/7/8 + TIM3 | PC6/7 + TIM8 encoder | różnica sprzętowa |
| Absolute reference | brak potwierdzonego odpowiednika | PA1 + TIM5 PWM | G532 zna/referencjuje pozycję niezależnie |
| Pozycja przy 0 RPM | sektor Hall | encoder count | G532 ma realną przewagę hardware |
| Hardware timestamp | tak | pozycja liczona sprzętowo | FT nadal jest deterministyczny |
| Startup | sector anchor | absolute/encoder angle | FT wymaga lock sequence |
| Continuous theta | software interpolation | bezpośrednia high-res pozycja + scaling | oba dają ciągły theta inną metodą |
| 6-sector history | tak | niepotrzebna w tej formie | kluczowe dla EVist |
| Per-sector correction | tak, 6 persistent trims | niepotrzebna w tej formie | kluczowe dla EVist |
| Extrapolation bound | ~70° | niepotrzebny analogiczny Hall bound | kluczowe dla EVist |
| Hall timeout | tak | encoder supervision | kluczowe dla EVist |
| High-RPM source switch | nie znaleziono | niepotrzebne | utrzymywać jeden canonical theta path |
| Speed phase correction | tak | tak | implementować po stabilizacji base theta |
| Advance slew | tak | G532 ma własną correction path | brak skoków correction |

---

# 34. NAJWAŻNIEJSZY WNIOSEK WDROŻENIOWY

Nie próbujemy zrobić z EVistDrive „G532 bez enkodera”.

Próbujemy zrobić:

```text
najlepszy możliwy Hall-only estimator
```

wykorzystując odzyskane z Fake Taxi mechanizmy:

```text
1. hardware edge timestamp
2. legal transition validation
3. extended low-speed period measurement
4. Hall timeout
5. 6-sector timing/history
6. robust averaged angular increment
7. canonical sector anchor
8. same base correction in every angle state
9. low-speed anchor before interpolation lock
10. continuous interpolation after sufficient history
11. bounded extrapolation
12. six per-sector angle corrections
13. persistent Hall calibration table
14. one final theta owner
15. speed-dependent phase advance
16. slew of phase advance
17. no high-speed source switch
```

A z G532 przenosimy zasadę architektoniczną:

```text
angle confidence != speed confidence
```

oraz:

```text
base rotor angle
+
small speed-dependent correction
```

zamiast budowania całego theta od prędkości.

---

# 35. PRIORYTET DLA KOLEJNEGO AGENTA

Kolejny agent NIE ma ponownie reverse'ować wszystkiego od zera.

Ma:

1. znaleźć obecny tor Hall/theta w aktualnym source EVistDrive,
2. porównać go punkt po punkcie z sekcjami 24-27 tego dokumentu,
3. wskazać brakujące elementy,
4. najpierw usunąć niespójność dwóch formuł kąta,
5. wdrażać kolejne mechanizmy etapami,
6. po każdym etapie dostarczyć logger i test A/B,
7. nie stroić high-RPM advance dopóki low-RPM base theta nie jest stabilne,
8. nie zmieniać PI/current sampling/torque shaping bez osobnej decyzji.

Pierwsza odpowiedź agenta po audycie ma zawierać tabelę:

| Mechanizm | Jest w EVistDrive? | Zgodny z FT? | Problem | Minimalna zmiana |
|---|---:|---:|---|---|
| hardware Hall timestamp | | | | |
| legal transition table | | | | |
| overflow/extended period | | | | |
| sector_period[6] | | | | |
| robust averaged increment | | | | |
| canonical base theta | | | | |
| single theta owner | | | | |
| first-edge handling | | | | |
| >=2-edge lock | | | | |
| Hall timeout | | | | |
| extrapolation bound | | | | |
| sector trims[6] | | | | |
| common/global offset | | | | |
| phase advance target | | | | |
| phase advance slew | | | | |
| high-RPM same-source policy | | | | |

Dopiero po tej tabeli agent powinien rozpocząć zmiany produkcyjne.

---

# 36. ŹRÓDŁA / ŚLADY REVERSE UŻYTE W TYM HANDOFFIE

## G532

Firmware:

```text
CRX30PC3615F805001.0_G532_250W_25_700-2185_Git-67a37d78_20251117_1756.bin
```

Najważniejsze odzyskane adresy:

```text
0x0800917C  rotor-position / electrical-angle state machine
0x0800ECC2  TIM8 encoder configuration
0x0800DA04  absolute PWM decoder
0x080092D0..0x080092EC absolute PWM -> TIM8 CNT synchronization
0x0800926A..0x080092AA omega_e calculation
0x080092F4..0x08009310 electrical angle + speed correction
0x08010FA0  TIM8 CNT read helper
0x08010FA4  TIM8 DIR read helper
0x08011442  TIM8 CNT write helper
```

## Fake Taxi

Firmware:

```text
FT_2026_05_22_w1.bin
```

Najważniejsze odzyskane adresy / pola:

```text
0x08004E78  raw 3-Hall GPIO reader
0x08002FD0  TIM3 Hall-interface configuration
0x08007208  TIM3 Hall capture/overflow handler
0x0800310C  continuous interpolation / extrapolation bound
0x08007F08  angle increment producer
0x080032C4  Hall sector -> anchor/correction mapping
0x08006900  persistent Hall corrections write path
0x0800DE78  persistent Hall corrections read path
0x080138B8..0x08013936 speed-dependent phase advance producer
0x20000370  speed-derived variable
0x200005B0..0x200005BA six Hall-sector corrections
0x2000054C  final theta
0x0800810C  final theta producer
0x08007EFC  theta getter
0x08009408  direct Park consumer
```

Odzyskane charakterystyczne stałe FT:

```text
0x1555  ~= 30° electrical sector-center geometry
0x31C7  ~= 70° electrical extrapolation bound
0x1554  ~= 30° electrical max recovered phase advance
140     low threshold in stock speed-derived domain
200     upper phase-advance ramp threshold
+/-1    recovered phase-advance slew step per update
```

Te stałe są dowodem działania stocku, NIE gotowymi parametrami EVistDrive.

---

# 37. VESC — TRZECI PUNKT ODNIESIENIA DLA HALL-ONLY FOC

VESC nie jest firmware Bafang i nie należy mieszać jego faktów z reverse G532/FT. Jest jednak bardzo wartościowym punktem odniesienia, ponieważ aktualny open-source VESC realizuje FOC z klasycznymi Hallami oraz równolegle posiada sensorless observer.

Źródła sprawdzone na aktualnej gałęzi `vedderb/bldc` (`master`, stan sprawdzony 2026-09-03):

```text
motor/foc_math.c
motor/foc_math.h
motor/mcpwm_foc.h
confgenerator.c
```

Najważniejsza funkcja dla tego porównania:

```text
foc_correct_hall(...)
```

Konfiguracja związana z tym torem zawiera m.in.:

```text
foc_hall_table[8]
foc_hall_interp_erpm
foc_sl_erpm_start
foc_sl_erpm
```

Stan runtime posiada m.in.:

```text
m_ang_hall
m_ang_hall_rate_limited
m_hall_dt_diff_last
m_hall_dt_diff_now
m_using_hall
```

**CONFIRMED z aktualnego kodu VESC:** Hall angle, interpolacja Hall, rate limiting Hall angle oraz płynne przejście Hall -> observer są jawnie zaimplementowane w jednym torze korekcji kąta.

---

# 38. VESC — BARDZO NISKA PRĘDKOŚĆ: NIE INTERPOLUJ NA SIŁĘ

To jest bardzo ważny wzorzec dla EVistDrive.

VESC wylicza prędkość Halla na podstawie czasu jednego sektora elektrycznego:

```text
rad_per_sec_hall = (pi / 3) / hall_period
```

czyli 60° elektrycznych podzielone przez zmierzony czas sektora.

Jednocześnie posiada jawny próg:

```text
foc_hall_interp_erpm
```

Jeżeli prędkość wynikająca z czasu Hall jest niższa od tego progu, kod robi funkcjonalnie:

```text
VERY LOW SPEED
      ↓
nie kontynuuj normalnej interpolacji
      ↓
use closest calibrated Hall angle
```

Komentarz w VESC wyjaśnia przyczynę: przy bardzo małej prędkości i zmianie kierunku interpolator może zostać około 60° od właściwej pozycji, dlatego bezpieczniej wrócić do najbliższego wiarygodnego kąta Hall.

To jest zgodne kierunkowo z Fake Taxi:

```text
FT:
przed stabilnym timingiem -> Hall-sector anchor

VESC:
poniżej foc_hall_interp_erpm -> closest Hall angle
```

## Wniosek wdrożeniowy dla EVistDrive

Nie wolno traktować `omega * dt` jako zawsze ważniejszego od fizycznego stanu Hall.

Docelowo:

```c
if (!hall_timing_confident || abs(erpm) < HALL_INTERP_MIN_ERPM) {
    theta_hall_target = calibrated_sector_angle[hall_state];
} else {
    theta_hall_target = hall_interpolated_angle;
}
```

Warunek nie może być pojedynczym chaotycznym progiem prowadzącym do skoku `theta_final`. Zmienia się **target / confidence**, a finalny kąt musi pozostać ciągły.

---

# 39. VESC — OCHRONA PRZED UCIECZKĄ INTERPOLACJI

VESC nie pozwala ślepo integrować kąta w nieskończoność.

W `foc_correct_hall()` porównuje bieżący interpolowany kąt z kątem wynikającym z aktualnego stanu Hall.

Jeżeli estymacja pozostaje w rozsądnym obszarze, wykonywana jest normalna predykcja:

```text
theta_hall += omega_hall * dt
```

Jeżeli estymacja oddali się za mocno, VESC nie wykonuje kolejnego dużego skoku, tylko zaczyna miękko ściągać ją w stronę kąta Hall:

```text
theta_hall -= angle_error * 0.01
```

W aktualnym kodzie granica logiczna wykorzystuje różnicę rzędu połowy sektora, czyli około 30° elektrycznych, wraz z kontrolą zgodności kierunku.

## Zestawienie z Fake Taxi

Fake Taxi:

```text
hard extrapolation validity bound ~= 70° electrical
```

VESC:

```text
soft correction toward Hall anchor
+
direction-aware plausibility
```

## Rekomendacja dla EVistDrive

Połączyć oba wzorce:

```text
1. HARD SAFETY BOUND
   prediction nigdy nie może uciekać arbitralnie daleko

2. SOFT ERROR CORRECTION
   zanim dojdziemy do hard bound, phase error ma być łagodnie korygowany
```

Nie wykonywać:

```text
if bad_prediction:
    theta_final = sector_center;   // NIE — możliwy nagły jump
```

Zamiast tego:

```text
prediction invalid
      ↓
freeze / reduce prediction confidence
      ↓
wrapped phase error to Hall anchor
      ↓
bounded correction
      ↓
continuous theta_final
```

---

# 40. VESC — JAWNY ANGLE RATE LIMITER PRZECIW SKOKOM PRĄDU

To jest jeden z najważniejszych nowych wniosków dla obecnego problemu EVistDrive.

VESC ma osobny stan:

```text
m_ang_hall_rate_limited
```

Po wyliczeniu kąta Hall firmware nie podaje go bezpośrednio do dalszego toru. Najpierw ogranicza maksymalną zmianę kąta na jeden krok.

Aktualny kod oblicza limit kroku zależny od prędkości i `dt`, a następnie przesuwa `m_ang_hall_rate_limited` w stronę `m_ang_hall` tylko o dozwolony krok.

Komentarz w źródle VESC mówi wprost, że ma to zmniejszać **current spikes w current controllers, gdy angle estimation zmienia się szybko**.

To jest bezpośrednio związane z objawem EVistDrive:

```text
theta source/formula changes
        ↓
theta jumps
        ↓
Park frame jumps
        ↓
Id/Iq vector rotates instantly
        ↓
current / torque impulse
        ↓
klik / trzask
```

## Wniosek dla EVistDrive

Niezależnie od tego, czy źródłem jest:

```text
Hall anchor
Hall interpolation
re-anchor correction
observer blend
phase advance
```

finalny kąt nie powinien dostać nieograniczonego kroku wynikającego tylko ze zmiany estymatora.

Potrzebna jest osobna ochrona:

```text
theta_candidate
      ↓
wrapped angle difference
      ↓
ANGLE SLEW / RATE LIMIT
      ↓
theta_final
```

**WAŻNE:** rate limiter nie może maskować permanentnie złej kalibracji kąta. Jest zabezpieczeniem przejścia, a nie zamiennikiem poprawnego `Hall angle table`, offsetów sektorowych i timingu.

---

# 41. VESC — PŁYNNE HALL -> OBSERVER ZAMIAST TWARDEGO SWITCHA

To jest najważniejszy wzorzec VESC dla średnich i wysokich obrotów.

VESC posiada dwa progi:

```text
foc_sl_erpm_start
foc_sl_erpm
```

oraz dwa równoległe kąty:

```text
theta_hall
theta_observer
```

Pomiędzy progami wyliczana jest waga Halla:

```text
speed <= foc_sl_erpm_start
    weight_hall = 1.0

speed pomiędzy progami
    weight_hall płynnie 1.0 -> 0.0

speed >= foc_sl_erpm
    weight_hall = 0.0
```

Finalny kąt jest interpolowany funkcją do interpolacji **kątów**, a nie zwykłych liczb, więc respektuje wrap `-pi/+pi`.

Funkcjonalnie:

```text
LOW RPM
100% Hall
0% observer

      ↓

MID RPM
Hall + observer blend

      ↓

HIGH RPM
0% Hall
100% observer
```

To jest dokładnie przeciwieństwo problematycznej architektury:

```c
if (speed < threshold)
    theta = formula_A;
else
    theta = formula_B;
```

jeżeli `formula_A != formula_B` w punkcie przełączenia.

## Docelowa zasada EVistDrive

Jeżeli observer EVistDrive zostanie uznany za wystarczająco wiarygodny przy wyższej prędkości, przejście powinno mieć postać:

```c
err = wrap_angle(theta_observer - theta_hall);
theta_blend = theta_hall + observer_weight * err;
```

Nie robić zwykłego:

```c
theta_blend = hall * (1-w) + observer * w;
```

bez obsługi wrap, bo przejście przez `0/360°` może wygenerować błędny ruch w przeciwną stronę.

Dodatkowo sama `observer_weight` powinna mieć:

```text
confidence gate
+
hysteresis / bounded slew
```

aby jitter prędkości nie powodował szybkiego pompowania udziału dwóch estymatorów.

---

# 42. VESC — KALIBRACJA HALL TABLE

Aktualne API VESC nadal zawiera:

```text
mcpwm_foc_hall_detect(...)
```

oraz konfiguracyjną tablicę:

```text
foc_hall_table[8]
```

Historyczna i współczesna architektura VESC używa procedury wykrywania rzeczywistych kątów stanów Hall podczas kontrolowanego obrotu pola. Istota algorytmu jest następująca:

```text
controlled electrical angle sweep
        ↓
read Hall state repeatedly
        ↓
accumulate angle samples for each Hall code
        ↓
circular average (sin/cos)
        ↓
angle for each valid Hall state
        ↓
persistent foc_hall_table[]
```

To jest bardzo bliskie idei Fake Taxi, gdzie odzyskaliśmy sześć trwałych korekcji sektorowych.

## Wniosek

EVistDrive nie powinien na stałe zakładać idealnej geometrii:

```text
Hall sector = dokładnie 60°
Hall edge = dokładnie nominal angle
```

Docelowa konfiguracja powinna przechowywać przynajmniej:

```c
hall_angle_cal[6];
```

lub równoważne:

```c
hall_sector_offset[6];
```

Najpierw można wdrożyć ręcznie / z logów, a dopiero później automatyczną procedurę serwisową.

**Nie kopiować bezpośrednio formatu VESC 0..200.** EVistDrive powinien pozostać w swojej jednej domenie kąta, najlepiej `uint16/q16 full electrical revolution` albo obecnej spójnej domenie używanej przez Park.

---

# 43. SYNTEZA G532 + FAKE TAXI + VESC — DOCELOWY ESTYMATOR EVISTDRIVE

Po dodaniu VESC obraz jest dużo pełniejszy.

## G532 daje wzorzec docelowej jakości

```text
high-resolution physical position
+
absolute reference
+
one continuous electrical angle
```

Ale tej przewagi hardware nie wolno udawać filtrami, jeżeli EVistDrive ma tylko 3 Halle.

## Fake Taxi pokazuje, jak maksymalnie wykorzystać 3 Halle

```text
hardware timestamp
legal transition validation
extended timing / overflow
6-sector history
robust averaged speed
continuous interpolation
extrapolation bound
6 persistent Hall-angle corrections
speed-dependent phase advance
slew phase advance
```

## VESC pokazuje, jak bezpiecznie zarządzać confidence i zmianą źródła kąta

```text
very low speed -> closest Hall angle, no forced interpolation
soft correction of excessive interpolation error
explicit Hall-angle rate limiter
Hall + observer coexist simultaneously
circular Hall -> observer blend
no hard source switch
```

## Docelowy tor

```text
RAW 3 HALL
    ↓
hardware edge timestamp
    ↓
legal transition + direction validation
    ↓
extended sector period
    ↓
sector_period[6]
    ↓
robust full-cycle speed
    ↓
calibrated Hall angle / 6 sector offsets
    ↓
LOW-SPEED CONFIDENCE
    ├── insufficient timing / ultra-low -> calibrated Hall anchor
    └── valid timing -> continuous Hall interpolation
                         ↓
               soft phase-error correction
                         ↓
               extrapolation safety bound
                         ↓
               Hall angle rate limiter
                         ↓
                    theta_hall
                         │
                         ├─────────────┐
                         │             │
                         │        FOC observer
                         │             │
                         │       theta_observer
                         │             │
                         └──────┬──────┘
                                ↓
                     confidence/speed blend
                    with circular angle math
                                ↓
                    speed/delay phase correction
                                ↓
                         ONE theta_final
                                ↓
                           Park / FOC
```

## Najważniejsza zasada ownership

W całym firmware ma istnieć **jeden finalny właściciel kąta Park**:

```text
theta_final
```

Nie może być sytuacji, że:

```text
low-speed branch zapisuje theta bezpośrednio
normal branch zapisuje theta inaczej
observer branch nadpisuje theta później
phase advance jest dodawany tylko w części ścieżek
```

Wszystkie źródła mają produkować wyłącznie:

```text
candidate angle
confidence
correction
```

a dopiero jeden końcowy blok ma tworzyć `theta_final`.

---

# 44. KOLEJNOŚĆ WDROŻENIA PO DODANIU WNIOSKÓW Z VESC

Nie wdrażać od razu pełnego observer blend. Najpierw naprawić bazowy Hall estimator.

## ETAP A — bez observera

```text
1. one theta ownership audit
2. legal Hall transition validation
3. hardware timestamps / extended period
4. sector_period[6]
5. robust speed estimate
6. calibrated sector angles
7. ultra-low-speed anchor mode
8. continuous interpolation only with confidence
9. soft interpolation-error correction
10. hard extrapolation bound
11. theta rate limiter
12. unified phase offset / correction
```

Ten etap powinien już usunąć dużą część problemu start/stop i zbliżyć EVistDrive do Fake Taxi.

## ETAP B — observer jako logger, nie właściciel

Uruchomić obecny observer równolegle i logować:

```text
theta_hall
theta_observer
wrapped_error
hall_confidence
observer_confidence
rpm/eRPM
Iq_ref
Iq_measured
Id_measured
```

Nie używać jeszcze observera do Park.

Cel:

```text
ustalić od jakiej prędkości observer jest stabilniejszy niż Hall interpolation
```

## ETAP C — bezpieczny blend

Dopiero po danych z jazdy:

```text
Hall dominant
    ↓
blend window
    ↓
observer dominant
```

Warunki wejścia powinny zależeć nie tylko od RPM, ale również od confidence, np.:

```text
observer valid
Hall valid
no illegal transitions
reasonable wrapped phase error
current control stable
```

## ETAP D — high-RPM optimization

Dopiero po zamknięciu ciągłości kąta stroić:

```text
speed-dependent phase advance
omega_e * delay correction
d-q decoupling / feed-forward
field weakening
```

Nie używać high-RPM feature jako sposobu maskowania błędnego Hall angle.

---

# 45. LOGGING WYMAGANY PO DODANIU VESC-STYLE ESTIMATORA

Agent ma dodać log co najmniej:

```text
time
hall_raw
hall_prev
hall_transition_valid
hall_direction
hall_sector
hall_edge_age
hall_period_current
hall_period[0..5]
hall_period_avg
hall_timing_confidence

theta_sector_anchor
theta_hall_interpolated
theta_hall_corrected
theta_hall_rate_limited

theta_observer
observer_confidence
observer_weight
wrapped_hall_observer_error

theta_phase_correction
theta_final

Iq_request
Iq_ref
Iq_measured
Id_measured
MOE
FOC_state
```

Dla problemu kliku szczególnie ważne są eventy:

```text
HALL_EDGE
HALL_INVALID
HALL_TIMEOUT
INTERP_ENABLE
INTERP_DISABLE
EXTRAP_LIMIT
REANCHOR_START
REANCHOR_END
OBSERVER_BLEND_START
OBSERVER_BLEND_END
THETA_RATE_LIMIT_ACTIVE
```

Kryterium jakości:

```text
żaden zwykły transition źródła/confidence nie może powodować nagłego dużego delta(theta_final)
przy istotnym Iq
```

---

# 46. CZEGO NIE KOPIOWAĆ 1:1 Z VESC

Nie kopiować bez własnego strojenia:

```text
foc_hall_interp_erpm
foc_sl_erpm_start
foc_sl_erpm
0.01 soft-correction gain
1.5 hall-angle rate multiplier
VESC hall table scale 0..200
observer gains
motor R/L/flux parameters
open-loop startup parameters
HFI settings
```

VESC jest dowodem **architektury**, nie źródłem gotowych liczb dla M820.

Tak samo jak w Fake Taxi, liczby stockowe są zależne od:

```text
pole pairs
gearbox
Hall geometry
FOC frequency
angle domain
current loop bandwidth
motor R/L/flux
sensor delay
```

---

# 47. KOŃCOWA ZASADA PO PORÓWNANIU TRZECH SYSTEMÓW


Najważniejszy efekt, który chcemy osiągnąć:

```text
POSTÓJ
  ↓
spójny Hall-sector angle
  ↓
pierwszy ruch bez theta jump
  ↓
pierwsze legalne Hall transitions
  ↓
bezpieczne przejście do continuous interpolation
  ↓
stabilny FOC w normalnym zakresie
  ↓
płynnie rosnąca korekcja high-RPM
  ↓
release
  ↓
correction wraca płynnie
  ↓
interpolation traci confidence
  ↓
bezskokowy re-anchor do tej samej bazowej domeny kąta
  ↓
ZERO RPM bez kliku
```

To jest docelowy model EVistDrive po wykorzystaniu najlepszych mechanizmów Fake Taxi, VESC oraz wiedzy o sprzętowej przewadze G532. Fake Taxi jest głównym wzorcem dla jakości Hall-only, VESC dla zarządzania confidence i płynnego Hall->observer, a G532 pokazuje, jak wygląda zachowanie systemu, który dzięki lepszemu sensorowi nie musi zgadywać pozycji przy zerowej prędkości.
