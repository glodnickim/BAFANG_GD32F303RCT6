# M820 reverse engineering — evidence ledger

**Stan:** 2026-08-25  
**Zadanie:** każdy ważny wniosek ma mieć ślad dowodowy oraz jawny falsifier.

| Claim | Główne evidence | Algebra / zachowanie | Confidence | Co może obalić / czego brakuje |
|---|---|---|---|---|
| APP stock 2.1 zaczyna się po 32B header i linkuje od 0x08005000 | vector table po file offset 0x20, SP 0x2000..., reset ~0x0800A3CD | poprawny Cortex-M vector pattern | PEWNE | inny kontener może mieć inny header/base |
| PA2/PA3/PA5 są phase-current channels | ADC injected init + current reconstruction | trzy kanały używane jako Iabc | PEWNE | exact U/V/W order nadal otwarte |
| phase ADC zero ≈2048 | kalibracja 0x0800AF5C + accepted 0x7800..0x8800 + injected alignment | offset_internal=16*ADCzero -> 1920..2176 | PEWNE | — |
| Iinternal=16*(zero-sample) | normal injected read + calibrated offset | JDR left injected path | PEWNE | — |
| EBiCS hardware zero ~2k | EBiCS offsets 2012/2028/2020 | niezależny firmware/hardware cross-check | PEWNE jako cross-check | nie jest dowodem stockowego algorytmu |
| runtime current calibration ma 16 próbek/fazę | stock loop w 0x0800AF5C | 16 accumulations | PEWNE | wait units nadal otwarte |
| 2-of-3 reconstruction | current function around 0x0800B17C | trzecia faza = -(dwie pozostałe) | PEWNE | physical U/V/W order otwarte |
| current sample trigger jest dynamiczny | `0x0800A9E4`: SVPWM -> TIM1 CCR4; constants 1999/243/372/129 | valid primary / alternate / invalid windows | PEWNE | przeliczenie counts->ns zależy od timer clock |
| `0x200004E4` to SVPWM/sampling sector, nie Hall sector | jedyny producer w `0x0800A9E4`, consumer w `0x0800B17C` | sector 1..6 powstaje z geometrii/wektora SVPWM i steruje wyborem shuntów | PEWNE | physical U/V/W order nadal otwarte |
| sample-state jest spójny z triggerem następnej próbki | `0x08007604`: najpierw `0x0800B17C`, potem control, na końcu `0x0800A9E4` | sample N używa previously-latched sector/state; producer tworzy sector/state/CCR4 dla N+1 | PEWNE | — |
| stock 2.1 nie przełącza dynamicznie CH4 polarity w normalnym current path | `0x0800A9E4`: `0x200004F0=0`, finalny CCER bit `0x2000` clear/set wg selektora; brak drugiego realnego producenta | normal path wymusza CC4P=0 | PEWNE dla obrazu 2.1 | exact EVistDrive TIMER0_CH3 half-cycle/edge nadal target-specific |
| invalid current sample nie trafia bezpośrednio do FOC | `0x0800B17C`, `sample_state==0` | last valid currents retained; trzecia faza ponownie rekonstruowana | PEWNE | brak stockowego sample-age/timeout na tej ścieżce |
| overcurrent ma 3 progi | ISR compares abs three phase values z 0x2000035E/60/62 | thresholds 1/2/3 | PEWNE | semantyka niższych dwóch reakcji wymaga pełniejszego opisu |
| hard OC wymaga >5 consecutive hits | najwyższy threshold branch w ISR | counter++ -> >5 -> state9/MOE off | PEWNE | — |
| progi OC są funkcją temperatury | 0x0800E98C input literal 0x200002CE; threshold outputs | piecewise coef -> multipliers | PEWNE | fizyczne uzasadnienie (sensor compensation vs inne) OTWARTE |
| 0x200002CE to temp x10 | NTC LUT writer + helper dzieli /10 i mapuje -40..150 | telemetry + compensation callsites | PEWNE | — |
| normal stop czeka na ramp=0 przed MOE off | state machine around 0x080186B0 | graceful stop | PEWNE | — |
| reset FOC ustawia CCR1/2/3=1000 | reset around 0x08012D90, ARR≈2000 | neutral center vector | PEWNE | physical MOSFET polarity zależy od drivera |
| normal stop używa MOE, nie CEN=0 | TIM_CtrlPWMOutputs helper 0x0800CCF8 | timer pozostaje aktywny | PEWNE | fault paths mogą hard-disable inaczej |
| rotor interface to UVW/Hall-style PC6/7/8 | GPIO read 0x08009E78 + TIM3 hall interface | 6 valid states + sequence | PEWNE | fizyczne źródło sygnału nie wynika z BIN-u |
| fizyczne źródło UVW to prawdopodobnie MT6816 | user pinout/PCB diagram | diagram opisuje encoder UVW mode | BARDZO MOCNE | continuity/schematic would make it PEWNE |
| PAS stock = 64 accepted edges/rev | quadrature tracing | 16 cycles x4 | PEWNE | — |
| PB1 NTC uses LUT, not Beta equation | 191-entry table + interpolation | -40..150, 0.1°C output | PEWNE | exact NTC part unknown |
| PC0/1/2 form phase-voltage/BEMF set | regular ADC + common sector logic | separate baselines + pairwise Hall-dependent deltas | PEWNE jako logiczny set | exact U/V/W and V/count open |
| PC3 is battery/DC-link voltage | ADC processing + conversion 693/4096 | fullscale ~69.3 V interpretation | PEWNE funkcja, unit strongly supported | analog divider physical values not traced |
| PA0 battery current gain = 39.215686 mA/count | stock conversion + CAN 0x3201 A*100 contract | 10/255 A per ADC count | PEWNE | — |
| PA0 zero has temperature compensation | 0x08018850 + 0x0800613C | ~1 raw count per 1.3°C | BARDZO MOCNE | full enable/lifecycle still to finish |
| phase-current physical gain ~95-100 mA/count | EBiCS CAL_I ~95 + stock PA0 cross-cal triangulation | plausible scaling | BARDZO MOCNE/HIPOTEZA exact | need stock physical endpoint or PCB Rshunt*gain |
| TIMER0_CH3 wyzwala inserted ADC na DOLNEJ połówce (DOWN-count match) | FW-126.2 sweep na rowerze, log 2026-08-25 19:28, obraz DIAG 0.0430 | CNT = CCR3 - (CONV+L); nachylenia +1,05 i +0,90 przy krokach CH3 -40; CONV+L = 588 zliczeń = 4900 ns | PEWNE (zmierzone) | inny obraz/timer clock; powtórzyć sweep, gdyby zmieniła się konfiguracja TIMER0 |
| kalibracja prądu (MOE OFF, trigger programowy) czyta ~1850 na wszystkich fazach | 0x602D z trzech niezależnych rozruchów: 1841/1866/1871, 1848/1874/1879, 1852/1879/1884 | P2P 15-19, verify zgodny do 1 LSB, MOE-off potwierdzone | PEWNE (powtarzalne) | — |
| ta sama ścieżka JDR w neutral dwell (MOE ON, trigger CH3) czyta ~0 | surowe JDR z ramek FW-126.0, log 17:44: -10 / -3 / +12 | sprzętowe offsety 2012/2028/2020 są zaprogramowane i w dwellu działają | PEWNE | — |
| przyczyna tej sprzeczności | NIEROZSTRZYGNIĘTA - FW-126.3 etap B ma to zmierzyć | trigger programowy vs CH3 to jedyna różnica wg audytu rejestrów | OTWARTE | pomiar 0x602E |
| `700` is PH_CURRENT_MAX | xref disproved | constant belongs elsewhere | ODRZUCONE | keep as historical false hypothesis |
| assist tables 20..100 and 100..100 are current/speed limits | stock default block + Bafang 0x6011 layout | nine parallel slots | BARDZO MOCNE | finish all runtime xrefs for exact field semantics |

---

## Rejestr korekt

### 2026-08-20 — phase ADC alignment

**Było:** `zero≈1024`, `Iinternal≈32*delta`.  
**Jest:** `zero≈2048`, `Iinternal=16*delta`.  
**Powód:** pomylenie regular ADC DR left alignment z injected JDR signed/left-aligned representation. Cross-check z EBiCS offsetami ~2k ujawnił niespójność i wymusił ponowną analizę.

### PAS

**Było:** 96 edges/rev.  
**Jest:** 64 accepted edges/rev.  
**Powód:** ponowny tracing pełnego stockowego quadrature path.

### Stała 700

**Było:** kandydat phase-current max.  
**Jest:** hipoteza odrzucona.  
**Powód:** producer/consumer xrefs prowadzą do innej struktury; sama liczba podobna do parametru w innym projekcie nie jest dowodem.

### 2026-08-25 — current-sampling sector / CH4 edge

**Było:** mapping 2-of-3 opisywany jako `Hall/sector`, a dokumentacja sugerowała dynamiczne przełączanie CH4 polarity/edge.  
**Jest:** `0x200004E4` jest **SVPWM/sampling sector** produkowanym przez `0x0800A9E4`; `0x08007604` potwierdza latching state/CCR4 na następną próbkę. W stocku 2.1 normalny current path ustawia `0x200004F0=0`, więc CC4P jest wymuszane na 0; nie znaleziono dynamicznego przełączania polarity.  
**Powód:** ponowny producer/consumer tracing `0x08007604 -> 0x0800B17C -> ... -> 0x0800A9E4` i bezpośredni tracing zapisu TIM1 CCER.

### 2026-08-25 — INVALID hold

**Doprecyzowanie:** stock 2.1 przy `sample_state=0` trzyma poprzedni poprawny current state, ale w prześledzonej ścieżce nie znaleziono osobnego `sample_age`/timeoutu. Bounded sample age pozostaje ulepszeniem EVistDrive ponad stock.

### 2026-08-26 — domena wartości „~0" w wybiegu neutralnym (FW-126.5, FAZA 0)

**Było:** otwarta sprzeczność „przy zgaszonym mostku ADC czyta ~1850, a w pracy prąd wychodzi ~0",
z niewykluczoną możliwością, że porównujemy dwie różne domeny (surowy JDR vs wartość po odjęciu
offsetu i po rekonstrukcji 2-z-3).  
**Jest:** obie liczby są **surowym JDR** — odczytem `adc_inserted_data_read()` sprzed odjęcia
`current_cal.offset[]`, w bootcie w którym `current_cal.valid == 0`, więc krok korekty i tak nic
nie odejmował. **Porównanie nigdy nie było międzydomenowe.**  
**Powód:** prześledzenie ścieżki w ISR instrukcja po instrukcji, od `ADC_IDATA0` do zmiennych
`i16_ph1/2/3_current`, z wypisaniem producenta, konsumenta i jednostki każdego kroku.  
**Otwarte:** czy surowa liczba **zmienia się** wraz ze stanem mostka, jak szybko i czy wraca —
to mierzy kampania A0→B0/B1/B2→C0 w DIAG 0.0439 (jeszcze nie na rowerze).

### 2026-08-26 — warstwa adc_trigger_diag usunięta

**Było:** diagnostyka przemiatania CH3 (`0x10240..0x10246`) plus model „ADC0 && ADC1 && ADC2 EOIC",
obalony w FW-126.3.  
**Jest:** warstwa **usunięta z kodu**, nie wyłączona flagą; zero referencji runtime. Zachowane
dokumenty wyników, historyczne logi i dekoder starych schematów 7/8.  
**Powód:** decyzja właściciela (DELETE, nie DISABLE) po regule „nie naprawiaj architektury
kolejnymi wyjątkami". `DIAG_SCHEMA_VERSION` podbity 7 → 8, żeby „brak ramek 0x1024x" nie było
nieodróżnialne od „przemiatanie się nie uzbroiło".

### 2026-08-27 — nasycenie toru prądowego przy zgaszonym mostku (FW-126.6/126.7)

**CONFIRMED:** przy zgaszonym mostku wyjście wzmacniacza prądowego stoi **w nasyceniu przy
szynie dodatniej** — odtworzona konwersja 3874/3910/3898 = 3,12–3,15 V przy 3,3 V, czyli
`V+ − 0,18 V`. To nie jest zero z offsetem, to brak ważnego pomiaru.
**Powód:** rekonstrukcja `raw = JDR + IOFF` z pomiaru FW-126.5 (log 2026-08-26 15:21).

**CONFIRMED:** aktywny mostek neutralny przywraca punkt pracy na **środek skali** — 2004/2023/2020
= 1,615–1,630 V, zgodne z zaprogramowanymi IOFF 2020/2028/2012 w granicach **16 LSB**.
Stałe IOFF od zawsze opisywały stan aktywnego mostka i są poprawne.

**CONFIRMED:** źródło wyzwalania **nie jest** przyczyną — FW-126.4 zmierzył deltę TRGO−SW = 0/0/1
przy rozrzucie własnym 13–15.

**CONFIRMED:** nasycenie jest **ciche** (rozrzut 10 LSB, P2P dumpu 17–19). Mały peak-to-peak
nie może być kryterium ważności kalibracji — to właśnie przez to stara ścieżka przechodziła
kontrole przez cztery karty.

**PRODUCTION FIX — CONFIRMED NA SPRZĘCIE (log 2026-08-27 12:05, DIAG 0.0442).**
FW-126.7 zastąpił ciemną kalibrację pomiarem w istniejącym wybiegu neutralnym FW-117.
Pierwszy przebieg: `state VALID`, `source NEUTRAL_DWELL`, attempts 1, eligible 32/32,
restarts 0, wszystkie 11 kryteriów PASS.

  JDR mean      −18 / −10 / +5      P2P 3 / 2 / 2
  physical ADC  2002 / 2018 / 2017  = 1,613 / 1,626 / 1,625 V   (środek skali)

Odtwarzalność: FW-126.5 zmierzył 2004/2023/2020 w innym boocie — **zgodność 2–5 LSB**.
Wobec starej ciemnej kalibracji (3874/3910/3898) różnica to ~1880 LSB.

**Przelot B0 REALNIE ODRZUCONY:** cycles 41, stable_count 39 → dokładnie 2 cykle spalone, zanim
liczenie stabilności ruszyło. To rampa szyna→środek odrzucona przez bramkę na żywym sprzęcie,
nie w symulacji. Czas kalibracji 41 × 62,5 µs = **2,56 ms**.

`diag_stop=1` — DIAG odmówił zwolnienia FOC, więc po kalibracji nie było momentu.
**ZAMKNIĘTE jazdą na NORMAL 0.0442 (2026-08-27): silnik rusza, jedzie płynnie, bez szarpania.**
Po usunięciu wszystkich ścieżek awaryjnych (LEGACY_FALLBACK, STRICT, LKG) „silnik ruszył" na
NORMAL **logicznie implikuje** CAL VALID: bez niej BRAMKA B odmawia zwolnienia FOC i mostek nie
dochodzi do aktywnego sterowania. Płynna praca przy pierwszych w historii niezerowych offsetach
(−18/−10/+5) jest dodatkowo zgodna z poprawnym mapowaniem faz A=ADC2 / B=ADC1 / C=ADC0.

### 2026-08-27 — FW-127 akwizycja prądu: spójna, potwierdzona sprzętowo

**CONFIRMED:** po przeniesieniu decyzji o oknie próbkowania za SVPWM i wprowadzeniu clampa
geometrii, sesja jazdy (DIAG 0.0446) dała `clamp_total` = 0, `peak_requested` =
`peak_applied` = 3208 przy ARR 3750, i **zero nielegalnych CH3**.

**CONFIRMED:** przy zwykłej jeździe wszystkie trzy boczniki przewodzą przez całą aperturę —
985 743 próbek PRIMARY, **0 ALTERNATE**. Rekonstrukcja 2-z-3 nie była w tej sesji potrzebna
ani razu, więc **nie została na sprzęcie zweryfikowana** (pokrywają ją testy hosta).

**CONFIRMED:** przemodulowanie **nie wystąpiło** w tej jeździe. Szczyt odchylenia 1333 z 1875
dostępnych = 71 % drogi do clampa, zapas ~1,4×. Defekt pozostaje realny (dowód arytmetyczny +
test A4b) — nie zaobserwowany, nie usunięty.

**CONFIRMED:** podmiana last-valid nie zadziałała ani razu (`reuse_count` 0,
`max_sample_age` 0). Wszystkie 7 INVALID to pierwsza próbka po każdym z 7 startów,
natychmiast rozwiązana.

**OBSERWACJA (jakościowa, nie pomiar):** załączanie wspomagania stało się wyraźnie powtarzalne.
Prawdopodobny mechanizm: stary kod zawsze rekonstruował fazę o najwyższym wypełnieniu, więc jedno
wejście Clarke było zawsze sumą obliczoną; teraz oba są pomiarami bezpośrednimi.

### 2026-08-27 — FW-127 ścieżka STARVED: dowód statyczny + jedno utajone zagrożenie

**CONFIRMED (audyt kodu, bez sprzętu):** przy `INVALID && have_last == false`
`current_feedback_update()` zwraca 0 **przed** zapisem wskaźników wyjściowych, więc
`FOC_calculation()` nie jest wołane. Clarke, Park, `runPIcontrol()` i `svpwm()` leżą
wewnątrz tej funkcji, więc żadna podejrzana wartość nie dociera do sterowania, całki PI
pozostają nietknięte, a ISR komenderuje neutralnie (CCR 1875/1875/1875) — zero momentu.
Własność „no active FOC on uninitialized last_valid" = **PASS**.

**OPEN / LATENT HAZARD / NON-BLOCKING:** `consume()` czyści `sector = 0`, a
`sample_window_reconstruct()` czyta 0 jako „odbuduj fazę A" (wartość „nie rekonstruuj" to
`SAMPLE_WINDOW_RECONSTRUCT_NONE = 3`). Dziś martwa dana — obie ścieżki INVALID odrzucają te
wartości przed FOC. Ścieżka wystąpiła 7× na sprzęcie **bez** regresji. Ryzyko dotyczy przyszłego
refaktoru, który zacząłby konsumować świeże prądy przy INVALID.
**NIE jest regresją FW-127 i nie otwiera go ponownie.**
