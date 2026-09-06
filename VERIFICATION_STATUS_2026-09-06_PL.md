# EVistDrive v3 — zweryfikowany checkpoint FW139–FW142

**Baseline:** `EvistDrive06092026v3.zip`
**Cel:** przerwać cykl „unit PASS → flash → nowa regresja” i mieć jeden testowalny tor od ridera do FOC/Hall.
**Stan:** PC/SIL VERIFIED; exact ARM target build READY BUT NOT EXECUTED IN THIS RUNTIME.

## 1. Zmiany produkcyjne

### FW139 — PAS physical-glitch rejection
- odbicie 1–3 ticki nie trafia do automatu `pas_direction` jako prawdziwy reverse;
- rzeczywisty reverse po fizycznie możliwym odstępie nadal natychmiast ustawia direction inhibit;
- naprawa jest przed safety state machine, więc nie osłabia zabezpieczenia reverse.

### FW139 — normal START ma jednego ownera trajektorii prądu
- usunięty Hall-gated Gear Preload (`~1 A -> czekaj na Hall / timeout 300 ms`);
- finalny 16 kHz `fast_iq_slew` pozostaje normalnym właścicielem narastania Iq;
- w zamkniętym SIL ciężki start, który wcześniej czekał ~333 ms permission→Hall, schodzi do dziesiątek ms.

### FW140 — jedna kadencja sterująca
- raw cadence pozostaje do HMI/diagnostyki;
- assist i adaptive Iq używają `cadence_filter`;
- pierwszy wiarygodny pomiar seeduje filtr, więc nie powstaje dodatkowy start delay;
- A/B przy tej samej fizycznej korbie: finalne Iq peak-to-peak 71 -> 19 counts (~73% mniej pompowania).

### FW141 — filtry torque mają prawdziwy timebase
- FAST ~35 ms i RUN ~120/250 ms są catch-upowane według `elapsed_ticks` 4 kHz;
- 35 ms fizycznego wejścia daje ten sam wynik przy gęstych i sparse foreground calls: 247 == 247;
- wynik filtra nie zależy już od chwilowego obciążenia CPU/foreground.

### FW142 — jeden współdzielony current-loop owner
- matematyka PI Id/Iq + wspólny limiter wektora została wydzielona z `main.c` do
  `src/foc_current_loop.c`;
- firmware target i elektryczny SIL linkują TEN SAM moduł;
- parity test: **2000 losowych stanów, exact legacy-vs-helper match PASS**;
- nie zmieniono celowo gainów PI, limitów ani semantyki anti-windup.

## 2. Elektryczny FOC SIL

Pełny backend wykonuje produkcyjny:

```text
PAS/torque/cadence
 -> ride_control
 -> final Iq @16 kHz
 -> FOC.c / Clarke / Park / measured Id/Iq
 -> PI Id/Iq + tracking AW + vector limit
 -> inverse Park
 -> svpwm()
 -> PWM geometry guard
 -> virtual PMSM
 -> Ia/Ib + fizyczny kąt rotora
 -> fizyczne Hall edges
 -> rotor_motion + rotor_angle
 -> z powrotem do FOC/ride_control
```

QZERO jest również wykonywany w pełnym backendzie. W aktualnym modelu testowym STOP:
- QZERO entry: ~156 erps;
- safety abort: ~127 erps;
- test nie wymusza sztucznego handbacku — abort jest akceptowany jako jawna bezpieczna gałąź,
  ponieważ R/L/flux/J w wirtualnym PMSM nie są deklarowane jako pomiar prawdziwego M820.

To jest ważne: simulator ma wykrywać zależność QZERO od fizyki, a nie „stroić prawdę” pod oczekiwany wynik.

## 3. Wyniki pełnego gate PC

### Real-module host
- **69 / 69 PASS**

### Whole-pipeline deterministic regression
- **18 / 18 traces PASS**;
- missed-tick regression PASS;
- repeated RUN_100 byte-identical PASS.

### Fast closed-loop supervisory SIL
- clean start permission→Hall ~33.5 ms;
- loaded start ~37.5 ms;
- PAS bounce: 0 false reverse;
- cadence A/B: 71 -> 19 Iq p-p;
- STOP→RESTART: first restart Iq=1 count;
- deterministic fuzz: **10 000 / 10 000 PASS**;
- ASan/UBSan fuzz: **1 000 / 1 000 PASS**.

### Real FOC/PMSM/Hall/QZERO SIL
- real SVPWM 360° geometry through `_U_MAX=1920`: **0 clamp hits**;
- corrected historical FW127 assumption: normal real SVPWM does NOT exceed ARR at `u_abs~1170`;
- 6-sector locked-rotor current tests PASS;
- moving current tracking 50..220 erps PASS; voltage saturation appears only near the high end of the chosen plant;
- Hall start sweep: **24 electrical angles x 2 loads = 48 / 48 starts PASS**;
- worst permission→first Hall = ~45 ms;
- worst initial Hall-angle uncertainty = 30°, as expected for 3 Hall sector-centre fallback;
- deterministic full electrical fuzz: **1 000 / 1 000 PASS**;
- ASan/UBSan full electrical fuzz: **100 / 100 PASS**.

### BL820 target packaging/build infrastructure
- complete target tree check: **PASS** (85 manifest entries + startup/linker/CMSIS/HAL);
- Python BL820 packager regression: **PASS**;
- CRC/container test uses an independent bitwise reference across many payload lengths: **PASS**;
- cross-platform target builder: `tools/build_firmware.py`;
- one-command target-required gate: `python tools/verify_all.py --require-target`.

## 4. Czego NIE udajemy

W tym runtime nie ma `arm-none-eabi-gcc 13.2.1`, a bezpośrednie pobranie oficjalnego toolchainu jest blokowane.
Dlatego:

```text
PC/SIL verification: PASS
BL820 packaging logic: PASS
target build path: READY/CHECKED
exact target ELF/BIN in this runtime: NOT BUILT
hardware ride verification of FW142: NOT DONE
```

Nie nazywać tej paczki „hardware verified firmware” dopóki exact target build i kontrolowany test na M820 nie przejdą.

## 5. Jak uruchomić

Quick:

```bash
python tools/verify_all.py --quick
```

Full:

```bash
python tools/verify_all.py
```

Pełny gate + wymagany target M820:

```bash
python tools/verify_all.py --require-target
```

Sam build targetu z Arm GNU 13.2.1:

```bash
python tools/build_firmware.py --toolchain "<bin katalog Arm GNU 13.2.1>"
```

## 6. Następny hardware gate

Po uzyskaniu exact `.bin` nie wracamy do losowego strojenia. Pierwszy test roweru ma jedynie zweryfikować rzeczy, których SIL nie może udowodnić z niezmierzonym plantem:

1. start z miejsca pod lekkim i cięższym naciskiem — brak opóźnienia/Hall deadlock;
2. 20/40/60/80 rpm — brak ciągłego pump/szarpania;
3. prawdziwy reverse — natychmiastowy cut;
4. zwykły STOP — klik / czas zatrzymania / QZERO hardware trace;
5. restart z ARMED_ZERO — brak kliku i brak stale Iq;
6. log konfiguracji runtime, żeby odróżnić kod od starego persisted banku.

Dopiero dane z tego jednego gate mogą zmieniać parametry fizycznego modelu lub QZERO. Nie stroić FOC na podstawie samego virtual PMSM.
