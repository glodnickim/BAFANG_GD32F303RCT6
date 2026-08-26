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
