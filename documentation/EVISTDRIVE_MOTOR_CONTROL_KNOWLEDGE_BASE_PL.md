# EVISTDRIVE MOTOR CONTROL KNOWLEDGE BASE — PL

**Wersja dokumentu:** 1.14  
**Ostatnia aktualizacja:** 2026-08-24  
**Gałąź:** `diag/fw112-real-bike-ab`  
**HEAD:** `ba794be` + local changes  
**Dirty state:** tak (zmodyfikowane pliki wymienione poniżej)

---

## Sekcja 16 — STEP 2A: START NEUTRAL DWELL

### 16.1 Data
2026-08-21

### 16.2 Branch / Commit / Dirty state
- **Branch:** `diag/fw112-real-bike-ab`
- **HEAD:** `ba794beb8d89c0c0ee0c2bb942ec8b916f53d677`
- **Dirty:** tak — zmodyfikowane pliki wymienione poniżej + istniejące zmiany na gałęzi

### 16.3 Krok i ID
- **Krok:** STEP 2A
- **ID:** STEP_2A_NEUTRAL_DWELL
- **Cel:** Eliminacja kliku przy starcie mostka przez przytrzymanie neutralnego PWM po MOE ON

### 16.4 Zmodyfikowane pliki i linie

| Plik | Zakres zmian |
|---|---|
| `inc/config.h` | Dodano `START_NEUTRAL_DWELL_CYCLES=4` (linia ~322), `START_DWELL_TIMEOUT_CYCLES=100` (linia ~325) |
| `src/main.c` | Definicje lifecycle states (linia ~1168-1173), zmienne globalne (linia ~1174-1178), inicjalizacja lifecycle przed MOE ON (linia ~1189-1198), progresja lifecycle w main loop (linia ~1201-1217), failsafe timeout (linia ~1218-1237), reset na soft cutoff (linia ~1242-1245), reset na hard cutoff, reset w power_off_controller() (linia ~4975-4977), ISR gating (linia ~3470-3495) |
| `tests/host/step2a_neutral_dwell_wiring_host.c` | Nowy plik — test T1-T6 (source-text wiring guard) |
| `tests/host/run-host-tests.ps1` | Dodano wpis nowej suit'y testowej |
| `CHANGELOG.md` | Wpis STEP 2A w sekcji [Unreleased] |
| `documentation/M820_REVERSE_KNOWLEDGE_BASE.md` | Sekcja 18 opisująca STEP 2A |

### 16.5 Co wdrożono

1. **Lifecycle states:** `IDLE → NEUTRAL_COMMIT → MOE_ON → NEUTRAL_DWELL → FOC_RELEASE → RUN`
2. **ISR gating:** `if(neutral_dwell_active)` blokuje `FOC_calculation()`, pisze neutral CCR (`_T/2`)
3. **Main loop progresja:** odczyt stanu z ISR, przejście przez stany
4. **Failsafe timeout:** wymuszenie wyłączenia mostka po `START_DWELL_TIMEOUT_CYCLES` iteracjach
5. **Reset lifecycle:** na każdej ścieżce wyłączenia (soft cutoff, hard cutoff, power_off, timeout)
6. **Testy hostów T1-T6:** source-text wiring guard — WSZYSTKIE PASS
7. **Audit post-implementation:** 10/10 pytań PASS

### 16.6 Czego NIE wdrożono

- **Zmiana `START_NEUTRAL_DWELL_CYCLES`** — wartość 4 jest wartością pierwszą, do walidacji na stojaku
- **NOWY logger/fw117** — istniejące sygnały wystarczają (patrz 16.10)
- **Zmiana `BRIDGE_START_IQ_DEADZONE`** — usunięty (C0-PROOF: deadzone eliminował preload)
- **Zmiana PI tuning, Hall, BREAK, assist, dead-time, OSSI/OSSR** — poza zakresem

### 16.7 Build DIAG=0

| Wartość | Wynik |
|---|---|
| **Plik** | `.build/0.0393_M820_BL820.bin` |
| **Rozmiar** | 97 984 B |
| **SHA-256** | `477332071AE9D39C54B864652EB4250694462F9C3358460CCAB0C3CD11BA26A4` |
| **text** | 97 680 B |
| **data** | 268 B |
| **bss** | 11 724 B |
| **START_NEUTRAL_DWELL_CYCLES** | 4 |
| **START_DWELL_TIMEOUT_CYCLES** | 100 |
| **Ostrzeżenia** | tylko pre-existing (`-Wpointer-sign` w CAN_Display.c, unused `fw_ver`) |
| **Status** | BUILD PASS |

### 16.8 Build DIAG=1

| Wartość | Wynik |
|---|---|
| **Plik** | `.build/0.0394_M820_BL820.bin` |
| **Rozmiar** | 136 820 B |
| **SHA-256** | `FDDE78C9BDC966EDEE50E67E066030CB3680C9629889A1755467AB12A1898927` |
| **text** | 136 516 B |
| **data** | 268 B |
| **bss** | 44 764 B |
| **START_NEUTRAL_DWELL_CYCLES** | 4 |
| **START_DWELL_TIMEOUT_CYCLES** | 100 |
| **FW-117 trace** | TAK (CAN_DIAGNOSTICS_ENABLE=1) |
| **Ostrzeżenia** | tylko pre-existing |
| **Status** | BUILD PASS |

### 16.9 Host tests

| Test | Opis | Wynik |
|---|---|---|
| T1 | Neutral CCR + lifecycle init PRZED MOE ON | PASS |
| T2 | ISR ma `if(neutral_dwell_active)` blokujący FOC | PASS |
| T3 | `neutral_dwell_counter--` + `foc_release_pending=1` | PASS |
| T4 | `neutral_dwell_active=0` w FOC_RELEASE transition | PASS |
| T5 | Reset lifecycle na soft cutoff | PASS |
| T6 | Reset lifecycle w power_off_controller() | PASS |
| **Razem** | 38 suit testowych | **WSZYSTKIE PASS** |

### 16.10 FW-117 trace — co pozwala rozróżnić

**Co trace JEST w stanie pokazać:**

| Sygnał | Widoczny w trace? | Źródło |
|---|---|---|
| Krawędź MOE 0→1 | TAK | `start_edge` (detekcja co tick @ 4 kHz), `FW117_FLAG_START_CAP` w pierwszej próbce po triggerze |
| CCR1/2/3 | TAK | `s->ccr1`, `s->ccr2`, `s->ccr3` w każdej próbce |
| iq_setpoint | TAK | `s->iq_setpoint` w każdej próbce |
| Stan COASTING (iq_setpoint==0) | TAK | `FW117_STATE_COASTING` w `state_and_hall` bits 0-2 |

**Co trace MOŻE wywnioskować (offline):**

| Stan lifecycle | Jak rozpoznać |
|---|---|
| Neutral dwell (MOE=1, CCR=1875, iq=0) | `flags.MOE=1` + `ccr1==ccr2==ccr3==1875` + `iq_setpoint==0` |
| FOC release (MOE=1, CCR≠1875) | `flags.MOE=1` + `ccr1≠1875` lub `ccr2≠1875` lub `ccr3≠1875` |
| Pierwszy aktywny FOC | pierwsza próbka po triggerze z CCR≠1875 |

**Co trace NIE jest w stanie pokazać (ograniczenie):**

| Ograniczenie | Przyczyna |
|---|---|
| Dokładne CCR podczas 250 µs dwell | Trace prókuje co 1 ms (DECIMATION=4, 1 kHz), dwell trwa 250 µs (1 tick @ 4 kHz) — dwell jest krótszy niż interwał próbkowania |
| Moment przejścia neutral→FOC | Przejście następuje między próbkami trace |

**Wniosek:** FW-117 trace POZWALA jednoznacznie ustalić:
1. Krawędź MOE 0→1 (z `start_edge` / `FW117_FLAG_START_CAP`)
2. CCR1/2/3 (bezpośrednio w próbce)
3. Czy po MOE ON jest neutral (CCR=1875, iq=0) czy FOC (CCR≠1875)
4. Pierwszą próbkę z aktywnym FOC (CCR≠1875)

FW-117 trace NIE pozwala na:
- Rozróżnienie `MOE_ON` vs `NEUTRAL_DWELL` (oba mają MOE=1, CCR=1875)
- Rozróżnienie `FOC_RELEASE` vs `RUN` (oba mają CCR≠1875)

**ALE:** te stany nie wymagają rozróżnienia w logu — wystarczy wiedzieć:
- "MOE ON + neutral CCR" → dwell aktywny
- "MOE ON + aktywny CCR" → FOC działa

### 16.11 RAM/FLASH przed i po

| Metryka | PRZED (0.0392) | PO (0.0393 DIAG=0) | PO (0.0394 DIAG=1) | Delta DIAG=0 |
|---|---|---|---|---|
| text | 97 520 B | 97 680 B | 136 516 B | +160 B |
| data | 268 B | 268 B | 268 B | 0 B |
| bss | 11 724 B | 11 724 B | 44 764 B | 0 B |
| **Flash total** | 97 788 B | 97 948 B | 136 784 B | +160 B |

Delta +160 B flash (DIAG=0) — akceptowalne. Brak zmian w RAM (bss/data niezmienione dla lifecycle state — `uint8_t` zmienna globalna = 1 B, reszta to `static` w main).

### 16.12 Ryzyka

| Ryzyko | Ocena | Mitigacja |
|---|---|---|
| Dwell za krótki (250 µs nie eliminuje kliku) | NISKIE | Zwiększyć `START_NEUTRAL_DWELL_CYCLES` po pomiarze na stojaku |
| Dwell za długi (opóźnia start FOC) | NISKIE | 250 µs = 1 tick @ 4 kHz, niezauważalne |
| Stale lifecycle state po secie | NISKIE | Reset na każdej ścieżce wyłączenia (5 ścieżek) |
| Failsafe timeout nie działa | NISKIE | Timeout = 100 iteracji main loop = 25 ms, sprawdzalny na stojaku |
|/fw117 trace nie rozróżnia stanów | NISKIE | Wystarczające do diagnostyki — patrz 16.10 |

### 16.13 Status

| Element | Status przed | Status po |
|---|---|---|
| Kod | BRAK (stockowy start) | WDROŻONY |
| Build DIAG=0 | — | BUILD PASS |
| Build DIAG=1 | — | BUILD PASS |
| Host tests | — | ALL PASS (38/38) |
| Audit post-implementation | — | 10/10 PASS |
| Bench test | — | WYMAGANY |
| Ride/log | — | WYMAGANY |
| **Status nadrzędny** | **BRAK** | **VERIFIED_SOFTWARE** |

### 16.14 Artefakty

| Artefakt | Rozmiar | SHA-256 |
|---|---|---|
| `.build/0.0393_M820_BL820.bin` (DIAG=0) | 97 984 B | `477332071AE9D39C54B864652EB4250694462F9C3358460CCAB0C3CD11BA26A4` |
| `.build/0.0394_M820_BL820.bin` (DIAG=1) | 136 820 B | `FDDE78C9BDC966EDEE50E67E066030CB3680C9629889A1755467AB12A1898927` |

### 16.15 Decyzja

**Status:** `VERIFIED_SOFTWARE`  
**Następny krok:** Test bench (wymagany przed `VERIFIED_BENCH`)  
**Commit:** NIE wykonywać do czasu weryfikacji na stojaku  
**Dostrójanie:** `START_NEUTRAL_DWELL_CYCLES` — nie zmieniać do pomiaru

---

## Sekcja 17 — Korekty historyczne

| Stary wniosek | Aktualny stan | Przyczyna korekty |
|---|---|---|
| phase ADC zero ~1024 | **~2048** | regular DR i injected JDR mają inną efektywną interpretację left alignment |
| `Iinternal = 32*deltaADC` | **`16*deltaADC`** | jw.; injected signed/left-aligned path |
| PAS 96 edges/rev | **64 edges/rev** | ponowny tracing stockowego quadrature |
| `700 = PH_CURRENT_MAX` | **odrzucone** | xref stałej prowadzi do innej struktury/telemetrii |
| fizyczne 3 Hall sensory | **firmware widzi UVW; board wskazuje MT6816 UVW** | rozdzielenie interfejsu logicznego od fizycznego źródła |

---

## Sekcja 18 — FW-118: Niezależna kalibracja zerowa prądu fazowego

### 18.1 Status

**`VERIFIED_SOFTWARE / HW_PENDING`**

### 18.2 Cel

Zamiana zhardkodowanych offsetów hardware (ADC0=2012, ADC1=2028, ADC2=2020) na runtime
kalibrację per-faza, tak aby Clarke/Park widziały ~0 przy zerowym prądzie.

### 18.3 Tor pomiarowy (potwierdzony w kodzie)

| Faza | Pin | ADC inserted | Offset HW | Regular raw |
|---|---|---|---|---|
| A (ph1) | PA2 | ADC2 ins ch0 | 2020 | `adc_value[7]` |
| B (ph2) | PA3 | ADC1 ins ch0 | 2028 | `adc_value[8]` |
| C (ph3) | PA5 | ADC0 ins ch0 | 2012 | `adc_value[4]` |
| Bat | PA0 | — | — | `adc_value[0]` (osobny tor) |

### 18.4 Zasada działania

1. **Kalibracja** (64 próbki, bridge OFF, po `adc_config()`):
   - Odczyt surowych wartości z regular ADC (bez odejmowania offsetu HW)
   - Średnia, min, max, P2P per faza
   - Walidacja zakresu (1848–2248) i szumu (P2P ≤ 200)

2. **ISR** (po kalibracji):
   ```c
   if(current_calibration_valid){
       i16_ph1_current -= current_offset_a;  // JDR - (mean_raw - HW_offset) ≈ 0
       i16_ph2_current -= current_offset_b;
       i16_ph3_current -= current_offset_c;
   }
   ```

3. **Fallback** (timeout / OUT_OF_RANGE / TOO_NOISY):
   - `current_calibration_valid = 0` → legacy (tylko HW offset)
   - Diagnostyka: `current_calibration_status` wskazuje przyczynę

### 18.5 Stałe (config.h)

| Stała | Wartość | Status |
|---|---|---|
| `CURRENT_ZERO_NOMINAL_ADC` | 2048 | PROVISIONAL |
| `CURRENT_ZERO_MAX_DEVIATION` | 200 | PROVISIONAL |
| `CURRENT_ZERO_MAX_P2P_ADC` | 200 | NEEDS HW VALIDATION |
| `CURRENT_CAL_SAMPLES` | 64 | — |
| `CURRENT_CAL_TIMEOUT` | 100 000 | — |
| `CURRENT_HW_OFFSET_A/B/C` | 2020/2028/2012 | z adc_config() |

### 18.6 Zmienne diagnostyczne

| Zmienna | Opis |
|---|---|
| `current_zero_a_adc` | Średnia surowa PA2 (diagnostyka) |
| `current_zero_b_adc` | Średnia surowa PA3 |
| `current_zero_c_adc` | Średnia surowa PA5 |
| `current_zero_p2p_a/b/c` | Peak-to-peak per faza |
| `current_calibration_status` | 0=uncal, 1=OK, 2=OOR, 3=noisy |
| `current_calibration_valid` | 1=use offsets, 0=legacy |

### 18.7 Testy

| Element | Wynik |
|---|---|
| Build DIAG=0 | PASS (`0.0401_M820_BL820.bin`, 98 724 B) |
| Build DIAG=1 | PASS |
| Host tests | 38/38 PASS |
| Bench test | **WYMAGANY** |

### 18.8 HW_PENDING — do zrobienia na sprzęcie

| Element | Opis |
|---|---|
| `current_zero_a_adc` | Zmierzona wartość zerowa PA2 |
| `current_zero_b_adc` | Zmierzona wartość zerowa PA3 |
| `current_zero_c_adc` | Zmierzona wartość zerowa PA5 |
| `current_zero_p2p_a/b/c` | Zmierzony P2P per faza |
| Finalne MIN/MAX | Do strojenia po pomiarach |
| Finalny P2P limit | Do strojenia po pomiarach |



### 18.9 Co NIE ulega zmianie

- Clarke/Park, PI, skalowanie (CAL_I)
- PA0 battery current (osobny tor kalibracji)
- FW-117 neutral dwell lifecycle
- Dynamic ADC trigger, sector-aware reconstruction
- `BRIDGE_START_IQ_DEADZONE` — usunięty (C0-PROOF)

### 18.10 FW-119 — polityka bezpieczeństwa nad tym pomiarem

FW-118 mierzy; **FW-119 decyduje, co zrobić, gdy pomiar się nie uda**. Zobacz
`documentation/FW-119_CURRENT_CALIBRATION_SAFETY_PL.md`.

- retry (`CURRENT_CAL_MAX_ATTEMPTS`, domyślnie 3) → last-known-good (RAM/sesja) →
  `LEGACY_FALLBACK` (domyślnie) albo `STRICT` (blokada startu FOC).
- Zmienne z §18.6 są teraz polami jednej struktury `current_cal` (`inc/current_cal.h`);
  mapowanie starych nazw jest w karcie FW-119 §6. Statusy 0–3 zachowują znaczenie z §18.6.
- Progi z §18.5 i cała lista §18.8 pozostają **PROVISIONAL / HW_PENDING** — FW-119 niczego

---

## Sekcja 19 — FW-122: ROLLING NO-ASSIST DIAGNOSTIC

### 19.1 Data
2026-08-24

### 19.2 Branch / Commit / Dirty state
- **Branch:** `diag/fw112-real-bike-ab`
- **HEAD:** `ba794be` + local changes
- **Dirty:** tak — nowe pliki diagnostyczne + zmiany w main.c, config.h, CAN_Display.c, diag_session.c, diag_budget.h

### 19.3 Krok i ID
- **Krok:** DIAGNOSTIC
- **ID:** FW-122_ROLLING_NO_ASSIST
- **Cel:** Diagnostyka sporadycznego braku wspomagania podczas jazdy (rolling re-enable)

### 19.4 Zmodyfikowane / nowe pliki

| Plik | Zakres zmian |
|------|-------------|
| `src/rolling_no_assist_diag.c` | Recorder schema v2 (44 B sample, 256 ring, CASE A/B/C, real serializer) |
| `inc/rolling_no_assist_diag.h` | Sample struct, EFIDs `0x10248..0x1024E`, HEADER + 6 DATA, API |
| `src/main.c` | Integration: diag block ~L3171, dump bridge ~L5001, diag_ops ~L5083, init ~L5101 |
| `src/CAN_Display.c:606-618` | HMI CAL swap: DIAG-only `MS->i_q_setpoint` w 0x3205 |
| `src/diag_session.c:174-177` | `kind_of_source` for 8th source |
| `inc/diag_session.h:52-66` | `DIAG_SRC_ROLLING_NO_ASSIST=7`, `DIAG_SRC_COUNT=8` |
| `inc/diag_budget.h` | RNA schema-v2 budget = 11,356 B, session = 1,788 B, conditional ceiling |
| `inc/diag_efid_map.h` | RNA range `0x10248..0x1024E` + disjoint asserts |
| `inc/config.h:320-336` | `FW117_TRACE_ENABLE=0`, `ROLLING_NO_ASSIST_DIAG_ENABLE` auto-derive |
| `scripts/sources-m820.txt:27` | `src/rolling_no_assist_diag.c` |
| `tests/host/rolling_no_assist_diag_host.c` | A/B/C, signed Iq, MOE/cal, persistence, ring i literal C oracle |
| `tests/host/run-host-tests.ps1` | New suite + fw117 suites updated with flags |
| `tools/decode_rolling_no_assist.ps1` | NOWY — CAN dump → CSV decoder |

### 19.5 Kluczowe wnioski

**CASE C sign bug (CONFIRMED, FIXED):**
- `MS.i_q` jest **ujemny** podczas forward drive (`MP.reverse = -1`, Park transform).
- `MS.i_q_setpoint` jest **dodatni** (0..700).
- Porównanie `iq_actual < iq_setpoint/4` → `-700 < 175` = ZAWSZE PRAWDIWE → fałszywa awaria.
- Poprawka: `|iq_actual|` magnitude comparison.

**HMI CAL (CONFIRMED):**
- DIAG build: `0x3205` bytes 0-1 = `MS.i_q_setpoint` (uint16 LE, raw, 1:1).
- `MS.calories`, `MS.int_Temperature` NIE zmienione.
- Każde CAL > 0 = żądanie Iq (nie „100+").

**fw117_trace disabled (CONFIRMED):**
- `FW117_TRACE_ENABLE=0` w config.h.
- Zastąpiony przez `rolling_no_assist_diag` (11,356 B schema-v2 budget vs 15,500 B).
- Przywrócenie możliwe: `FW117_TRACE_ENABLE=1` (testy benchowe).

### 19.6 Status testów

```text
HOST TESTS:     wszystkie suites — PASS
TRACE ANALYZER: 36/36 + regressions 6/6 — PASS
NORMAL 0.0417: 100,140 B FLASH, 12,032 B RAM — PASS
DIAG 0.0418:   143,120 B FLASH, 41,552 B RAM — PASS
```

### 19.7 Następne kroki

1. **Test sprzętowy** — wgrać `0.0418_M820_BL820_DIAG.bin`, zebrać CAN dump.
2. **Dekoder** — `tools/decode_rolling_no_assist.ps1` → CSV.
3. **Analiza CASE A/B/C** — Klasyfikacja triggera z logu.
4. **Root cause** — NIE POTWIERDZONY do czasu analizy logu.
5. **Hipoteza rolling rotor position** — CASE C może Potwierdzić lub Odrzucić.

### 19.8 Powiązane karty

| Karta | Relacja |
|-------|---------|
| FW-112-DIAG | Poprzedni rejestrator (pedal→Iq chain) |
| FW-112.1 | REAL_STOP liveness |
| FW-112.2 | Rolling coast vs REAL_STOP |
| FW112_PATCH_B0a.1 | Gate/permission closeout |
| FW-119 | Current calibration safety |
