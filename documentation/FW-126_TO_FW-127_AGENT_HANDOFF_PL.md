# EVistDrive / Bafang M820 — handoff dla agenta
## FW-126 zamknięcie diagnostyczne → test sprzętowy → FW-127

**Stan dokumentu:** 2026-08-25  
**Projekt:** EVistDrive / EBiCS / Bafang M820 / GD32F303RCT6  
**Cel:** zapewnić ciągłość pracy między wątkami i agentami bez ponownego wykonywania zakończonych etapów.

---

# 1. ZASADA NADRZĘDNA

Nie zaczynaj od przebudowy FOC ani strojenia PI.

Obecny priorytet to ustalenie prawdziwego zachowania:

```text
TIMER0_CH3
    ↓
center-aligned PWM half-cycle / edge
    ↓
inserted ADC conversion
    ↓
ADC ISR
    ↓
phase-current sample
```

Dopóki test sprzętowy FW-126 DIAG nie rozstrzygnie tej zależności, **nie wolno wdrażać produkcyjnej dynamicznej geometrii sampling-window**.

Stock M820 dostarcza wzorzec architektoniczny, ale nie rozstrzyga za EVistDrive, która połówka / edge `TIMER0_CH3` faktycznie uruchamia inserted conversion.

---

# 2. SOURCE OF TRUTH

Przed rozpoczęciem pracy przeczytaj aktualne wersje:

```text
M820_REVERSE_KNOWLEDGE_BASE.md
M820_STOCK_REVERSE_KNOWLEDGE_BASE_EVISTDRIVE.md
M820_REVERSE_EVIDENCE_LEDGER.md
M820_OPEN_QUESTIONS_AND_EVISTDRIVE_PORTING.md
documentation/FW-126_PHASE_CURRENT_ACQUISITION_HARDENING_PL.md
```

W razie konfliktu:

1. najnowszy `M820_REVERSE_EVIDENCE_LEDGER.md`,
2. aktualny kod repo,
3. aktualny dokument FW-126,
4. starsze raporty / wcześniejsze wątki.

Nie przywracaj odrzuconych hipotez.

---

# 3. AKTUALNY BASELINE — FW-126.0 ZAKOŃCZONY

FW-126.0 oraz niezależne bezpieczne części FW-126 są już zaimplementowane.

## 3.1 Zaimplementowane

### DIAG-only CH3 sweep podczas neutral dwell

Sweep:

```text
3740
3700
3660
```

Rejestrowane są:

- wejście do ADC ISR,
- stan TIMER0 / CH3 wymagany do rozstrzygnięcia triggera,
- coherency EOIC,
- sekwencja diagnostyczna.

Po teście wykonywany jest:

```text
immediate safety abort
→ przywrócenie CH3
→ brak wejścia w normalny aktywny FOC z testowym ustawieniem
```

### DIAG schema

```text
schema = 7
punkty: 0x10240–0x10246
```

### Iq / Id validity

Dodano reset / invalidation stale Iq/Id:

- przy starcie mostka,
- przy wyłączeniu mostka.

Recorder nie może uznać nieważnego feedbacku za prawidłowy tracking.

### Current calibration hardening

Kalibracja:

- wymaga `MOE = OFF`,
- liczy wyłącznie fresh triples,
- odrzuca przypadek `CURRENT_CAL_MOE_ON`.

### CAL dump

Dodano read-only:

```text
0x602D
```

### FOC START TRACE

DIAG zawiera kompaktowy START TRACE:

```text
72 ms pre-trigger
208 ms post-trigger
```

Nie zmienia zachowania NORMAL.

---

# 4. BUILD BASELINE

## NORMAL

```text
version: v0.0428
FLASH: 101004 B
RAM:   12064 B
SHA256:
7DB8317AE1802ED2CCD2ECFEDC9F5FDAC553D30F210E0097EE9991B32CE3CE94
```

Firmware:

```text
.build/M820_BL820/0.0428_M820_BL820.bin
```

## DIAG

```text
version: v0.0429
FLASH: 150632 B
RAM:   46176 B / 48 KB
RAM usage: 93.95%
SHA256:
05D49FD52F7747CE0EB5E1892B7EA864CFCD3080233341F8B536CB590242F4BA
```

Firmware:

```text
.build/M820_BL820/0.0429_M820_BL820_DIAG.bin
```

Host suite:

```text
PASS
```

w tym nowy test neutral-dwell.

---

# 5. CZEGO FW-126 CELOWO JESZCZE NIE ROBI

Nie zaimplementowano jeszcze produkcyjnie:

```text
dynamic PWM sampling-window selection
SVPWM-sector based current observability
PRIMARY / ALTERNATE / INVALID runtime sampling
latched sampling context N / N+1
bounded last-valid sample age
pełnej stock-like 2-of-3 acquisition pipeline
```

To jest **celowe**.

Nie traktować tego jako brakującego TODO do natychmiastowego dopisania.

Najpierw test sprzętowy.

---

# 6. NAJBLIŻSZE ZADANIE — FW-126 HW VALIDATION

## Tryb pracy

**READ / DIAG / ANALYSIS ONLY dla produkcyjnej logiki FOC.**

Dozwolone są wyłącznie dodatkowe zmiany diagnostyczne, jeśli obecny test nie dostarcza wystarczających danych.

Nie modyfikować produkcyjnej geometrii ADC/PWM przed wynikiem testu.

---

# 7. PROCEDURA TESTU SPRZĘTOWEGO

## Warunki

- rower bezpiecznie uniesiony / koło odciążone,
- brak obciążenia napędu,
- użyć firmware DIAG `v0.0429`,
- nie wykonywać agresywnych testów momentu,
- głównym celem jest neutral dwell / start acquisition.

## Odczytać

1. komplet punktów FW-121 / FW-126 wymaganych przez aktualną dokumentację,
2. `0x602D`,
3. CH3 sweep dla:

```text
3740
3700
3660
```

4. entry-ISR capture,
5. TIMER counter / direction związany z triggerem,
6. EOIC coherency,
7. FOC START TRACE.

---

# 8. PYTANIA, NA KTÓRE TEST MUSI ODPOWIEDZIEĆ

Agent ma zakończyć analizę jednoznaczną tabelą:

| Pytanie | Wynik | Confidence |
|---|---|---|
| Ile inserted conversions przypada na jeden okres PWM? | ? | ? |
| Czy CH3 generuje conversion na UP? | YES/NO | ? |
| Czy CH3 generuje conversion na DOWN? | YES/NO | ? |
| Czy występuje BOTH / dwa triggery? | YES/NO | ? |
| Który compare 3740/3700/3660 przesuwa ISR zgodnie z oczekiwaniem? | ? | ? |
| Czy EOIC odpowiada dokładnie obserwowanej conversion? | YES/NO | ? |
| Czy state/sample sequence jest jednoznaczna? | YES/NO | ? |
| Czy calibration była wykonana wyłącznie przy MOE OFF? | YES/NO | ? |
| Czy offsety A/B/C są stabilne? | YES/NO | ? |
| Czy START TRACE pokazuje stale Iq/Id? | YES/NO | ? |

---

# 9. ABSOLUTE STOP CONDITION

Jeżeli po teście nadal nie da się jednoznacznie powiedzieć:

```text
trigger compare N
→ konkretna połówka PWM
→ konkretna inserted conversion
→ konkretny ADC ISR
```

to:

**STOP.**

Nie wdrażaj FW-127 produkcyjnie.

Zamiast tego przygotuj minimalne rozszerzenie DIAG, które zamknie brakującą obserwację.

Nie zgaduj polarity, edge ani half-cycle.

---

# 10. KRYTERIUM PRZEJŚCIA DO FW-127

FW-127 można rozpocząć tylko jeśli test daje spójną odpowiedź na:

```text
TIMER0_CH3 compare
    ↓
UP / DOWN / BOTH
    ↓
inserted ADC conversion
    ↓
ADC ISR
```

oraz potwierdza, że można bezpiecznie powiązać:

```text
sample N
↔ latched sampling context N
```

---

# 11. POTWIERDZONY WZORZEC STOCK M820

Stock robi:

```text
sample N wygenerowany przez wcześniej ustawiony CCR4
        ↓
consume LATCHED sector/state N
        ↓
2-of-3 reconstruction
        ↓
Clarke / Park / current control
        ↓
SVPWM N+1
        ↓
sampling sector N+1
        ↓
sample_state N+1
        ↓
CCR4 N+1
        ↓
latch razem
```

Najważniejsza zasada:

> Próbki N nie wolno interpretować stanem wyliczonym dopiero po jej pobraniu.

---

# 12. POTWIERDZONE MAPOWANIE STOCK 2-of-3

W stock M820:

```text
SVPWM sector 1 / 6
    direct: B + C
    reconstruct: A = -(B + C)

SVPWM sector 2 / 3
    direct: A + C
    reconstruct: B = -(A + C)

SVPWM sector 4 / 5
    direct: A + B
    reconstruct: C = -(A + B)
```

UWAGA:

Nie zakładaj bez sprawdzenia, że logiczne A/B/C stocku odpowiadają bezpośrednio nazwom fizycznych U/V/W EVistDrive.

Przed produkcyjną implementacją zamknąć mapping:

```text
ADC channels
↔ logical A/B/C
↔ TIMER0 CH0/1/2
↔ motor U/V/W
```

---

# 13. PRIMARY / ALTERNATE / INVALID — WZORZEC STOCK

Stock posiada trzy stany jakości okna:

```text
PRIMARY
ALTERNATE
INVALID
```

Dla stockowego `ARR=2000` odzyskano:

```text
PRIMARY:
    CCR4 = 1999

ALTERNATE:
    CCR4 = selected_compare - 129

INVALID:
    nie używaj świeżej próbki do FOC
```

Stockowe progi:

```text
243
372
129
```

są **sprzętowo/czasowo zależnymi timer counts**.

### BEZWZGLĘDNY ZAKAZ

Nie kopiować do EVistDrive:

```text
243 / 372 / 129
```

jako surowych liczb.

Najpierw wyprowadzić wymagane czasy dla:

```text
TIMER0 = 120 MHz
_T = 3750
center-aligned ≈ 16 kHz
ADC clock
sample time
conversion time
deadtime
amplifier settling
noise settling
required guard time
```

Dopiero potem przeliczyć na ticki EVistDrive.

---

# 14. PLAN FW-127 — DOPIERO PO HW PASS

FW-127 powinien być kartą nadrzędną, ale implementowaną etapami.

---

## FW-127A — Sampling context N/N+1

Wprowadzić jawny obiekt kontekstu:

```c
typedef struct {
    uint8_t svpwm_sector;
    uint8_t sample_state;
    uint8_t direct_mask;
    uint16_t trigger_ccr;
    uint32_t seq;
} current_sample_context_t;
```

Minimum:

```text
current_ctx_latched
current_ctx_next
```

Kontrakt:

```text
ADC sample N
→ consume current_ctx_latched N
→ FOC
→ calculate SVPWM N+1
→ calculate sampling context N+1
→ latch current_ctx_next
→ program PWM / trigger N+1
```

### PASS

Log musi wykazać:

```text
sample sequence N
=
context sequence N
```

bez przesunięcia o jeden cykl.

---

## FW-127B — SVPWM-sector based 2-of-3 reconstruction

Usunąć znaczenie:

```text
char_dyn_adc_state = "highest duty phase"
```

z produkcyjnego current acquisition.

Reconstruction ma zależeć od:

```text
latched SVPWM sampling sector
```

Nie od:

- aktualnego Halla,
- angle sector,
- przypadkowego największego duty,
- wartości obliczonej po pobraniu próbki.

---

## FW-127C — Window validity

Dodać prawdziwe:

```text
PRIMARY
ALTERNATE
INVALID
```

Sampling-window calculator pracuje na **rzeczywiście zastosowanych** PWM compare po wszystkich clampach/ograniczeniach.

Wynik:

```text
SVPWM geometry N+1
→ observability
→ sample_state N+1
→ CH3 N+1
```

---

## FW-127D — INVALID / last-valid / sample_age

Stock przy INVALID nie podaje złej świeżej próbki do FOC.

EVistDrive ma zrobić co najmniej:

```c
if (fresh_valid) {
    current = fresh;
    last_valid = fresh;
    sample_age = 0;
} else {
    current = last_valid;
    sample_age++;
}
```

Dodać jawne quality:

```text
GOOD
ALTERNATE
HELD_LAST_VALID
INVALID
```

EVistDrive ma mieć bounded `sample_age`, którego stock 2.1 nie posiadał na tej ścieżce.

Nie ustalać progów `AGE_WARN/AGE_FAULT` bez runtime histogramów.

---

## FW-127E — START / rolling re-enable integration

Po działającym acquisition:

```text
PREPARE_START
↓
reset PI
↓
neutral CCR
↓
MOE ON
↓
neutral dwell
↓
uzyskaj poprawny current sample/context
↓
dopiero potem aktywne FOC / torque ramp
```

Nie dopuścić stale feedback do pierwszego aktywnego cyklu.

---

# 15. PI NIE JEST CZĘŚCIĄ PIERWSZEJ FAZY FW-127

Nie stroić jeszcze:

```text
Kp
Ki
```

Nie dodawać field weakening.

Nie przebudowywać Hall interpolation.

Najpierw trzeba uzyskać wiarygodne:

```text
Ia / Ib / Ic
→ Id / Iq
```

Dopiero potem strojenie regulatora ma sens.

---

# 16. KOLEJNA KARTA PO ACQUISITION — PI / LIMITERS

Po zamknięciu FW-127 acquisition można otworzyć osobny etap, np. FW-128.

Zakres:

## Battery current limiter

Nie używać jednego `PI_iq` naprzemiennie jako:

```text
Iq controller
battery-current controller
```

Docelowo:

```text
torque demand
↓
Iq_requested
↓
battery-current limit
↓
phase-current limit
↓
thermal/power limits
↓
Iq_allowed
↓
Iq ramp
↓
PI_Iq
```

`PI_Iq` zawsze reguluje:

```text
setpoint = Iq_ref
feedback = Iq_measured
```

## Anti-windup

Obecny conditional integration dla `max_step` i output clamp jest przydatny, ale niewystarczający.

Docelowo PI ma znać różnicę:

```text
Ud/Uq requested
vs
Ud/Uq actually applied after voltage-vector limitation
```

i używać back-calculation / tracking anti-windup.

---

# 17. START / STOP LIFECYCLE

Zachować rozdzielenie:

```text
normal graceful stop
!=
fault/emergency stop
```

Normal:

```text
Iq demand → 0
↓
Iq_ref ramp → 0
↓
FOC pozostaje aktywne
↓
neutralize
↓
MOE OFF
↓
reset transient state
```

Fault:

```text
fault
↓
independent hard MOE OFF
↓
fault latch/state
```

Nie łączyć tych dwóch ścieżek.

---

# 18. ZAKAZY DLA AGENTA

Bez nowego dowodu / osobnej karty nie zmieniaj:

- PI Kp/Ki,
- znaku `q31_u_d_temp = -PI_control(&PI_id)`,
- field weakening,
- Hall angle interpolation,
- fizycznego current gain jako pewnego `100 mA/count`,
- U/V/W mapping na podstawie nazwy zmiennej,
- ADC CH3 polarity/edge na podstawie stocku,
- stockowych `243/372/129` jako stałych EVistDrive,
- fault thresholds stocku jako bezpośrednich wartości EVistDrive.

Nie usuwaj zabezpieczeń ani diagnostyki FW-126 w celu odzyskania RAM bez osobnego audytu.

DIAG ma już ~93.95% RAM — każda rozbudowa diagnostyki musi uwzględnić budżet pamięci.

---

# 19. ZASADA JEDNEJ KARTY / JEDNEGO COMMITU

Każdy etap:

```text
audit
↓
plan
↓
minimal implementation
↓
host tests
↓
build NORMAL + DIAG jeśli dotyczy
↓
hardware test jeśli wymagany
↓
docs
↓
commit
↓
CLOSED
```

Nie implementuj FW-127A/B/C/D/E w jednym dużym diffie.

Każda podkarta musi mieć własny wynik PASS/FAIL.

---

# 20. OBOWIĄZKOWY HANDOFF PO KAŻDYM ETAPIE

Aktualizuj plik statusowy, np.:

```text
documentation/FW-127_STATUS.md
```

Format:

```text
MASTER:
FW-127 Phase Current Acquisition

CURRENT CARD:
FW-127A

LAST COMPLETED:
FW-126 HW VALIDATION — PASS

CONFIRMED:
- CH3 trigger half-cycle = ...
- trigger edge semantics = ...
- inserted conversions/PWM = ...
- ADC ISR sequencing = ...

CURRENT CODE:
branch = ...
HEAD = ...
NORMAL build = ...
DIAG build = ...

HARDWARE:
PASS / FAIL / NOT TESTED

DO NOT CHANGE:
...

OPEN QUESTIONS:
...

NEXT EXACT TASK:
...
```

Na końcu każdej podkarty agent ma przygotować **gotowy prompt do rozpoczęcia następnej podkarty**.

---

# 21. RAPORT WYMAGANY PO NAJBLIŻSZYM TEŚCIE

Po testach v0.0429 agent ma zwrócić raport:

```text
FW-126 HW VALIDATION REPORT

1. TEST CONDITIONS
2. FIRMWARE HASH VERIFIED
3. DIAG SCHEMA VERIFIED
4. CH3=3740 RESULT
5. CH3=3700 RESULT
6. CH3=3660 RESULT
7. CNT/DIR AT ADC ISR
8. INSERTED CONVERSIONS PER PWM
9. UP/DOWN/BOTH VERDICT
10. EOIC COHERENCY
11. SAMPLE SEQUENCE COHERENCY
12. 0x602D CAL DUMP
13. OFFSETS A/B/C
14. MOE DURING CALIBRATION
15. FOC START TRACE
16. STALE IQ/ID CHECK
17. SAFETY ABORT / CH3 RESTORE CHECK
18. CONCLUSION
19. FW-127 GO / NO-GO
20. IF GO: exact FW-127A contract
21. IF NO-GO: minimal DIAG extension only
```

---

# 22. DEFINITION OF DONE — FW-126

FW-126 można oznaczyć jako całkowicie CLOSED dopiero gdy:

- test sprzętowy DIAG został wykonany,
- CH3 trigger semantics są jednoznacznie opisane,
- calibration dump został zweryfikowany,
- start trace nie ujawnia stale feedback,
- testowe CH3 jest zawsze przywracane,
- nie ma regresji NORMAL,
- Evidence Ledger / FW-126 documentation są zaktualizowane.

---

# 23. PUNKT STARTOWY DLA NOWEGO AGENTA

Twoim pierwszym zadaniem **NIE jest implementacja FW-127**.

Twoim pierwszym zadaniem jest:

> Przeczytaj aktualny dokument FW-126 oraz cztery knowledge-base, sprawdź HEAD/build baseline i przygotuj/analizuj pojedynczy test sprzętowy DIAG v0.0429. Na podstawie CH3 sweep `3740/3700/3660`, CNT/DIR/ISR sequence, EOIC oraz `0x602D` wydaj jednoznaczny werdykt `UP / DOWN / BOTH / INCONCLUSIVE`. Jeśli wynik jest INCONCLUSIVE, wolno rozszerzyć tylko DIAG. Jeśli wynik jest jednoznaczny, zamknij FW-126 i przygotuj osobną kartę FW-127A dla latched `sampling context N/N+1`. Nie implementuj kolejnych etapów w tym samym commicie.

---

# 24. GŁÓWNY CEL TECHNICZNY

Docelowo fast loop EVistDrive powinien osiągnąć kontrakt:

```text
ADC conversion N
↓
latched context N
↓
validated/reconstructed Ia/Ib/Ic N
↓
Clarke/Park
↓
Id/Iq
↓
current control
↓
SVPWM N+1
↓
sampling-window geometry N+1
↓
latched context N+1
↓
program PWM + ADC trigger N+1
```

**Najpierw prawidłowy pomiar. Dopiero potem strojenie regulatora.**
