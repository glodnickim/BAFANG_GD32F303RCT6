# EVistDrive v3 — FW143 project-ready checkpoint

**Baseline wejściowy:** `EvistDrive06092026v3.zip`  
**Linia rozwoju:** FW139 -> FW143  
**Cel checkpointu:** samowystarczalny projekt do dalszej pracy przez kolejnego agenta, z real-module tests, whole-pipeline regression, supervisory SIL, electrical FOC/PMSM/Hall/QZERO SIL oraz pełnym Walk Assist SIL.  
**Stan:** PC/SIL VERIFIED; exact ARM target build path READY, ale target `.bin` nie został zbudowany w tym runtime z powodu braku Arm GNU 13.2.1.

## 1. Zmiany produkcyjne zachowane z FW139–FW142

### FW139 — PAS
- fizycznie niemożliwe krótkie reverse bounce nie trafia do direction safety;
- prawdziwy reverse nadal natychmiast odbiera permission.

### FW139 — START
- usunięty Hall-gated Gear Preload jako drugi owner normalnego startu;
- finalny `fast_iq_slew` pozostaje jednym właścicielem trajektorii Iq.

### FW140 — cadence
- control używa jednej conditioned cadence;
- raw cadence pozostaje do telemetry/diagnostyki;
- A/B w SIL zmniejszył peak-to-peak Iq z 71 do 19 counts w scenariuszu nierównej korby.

### FW141 — torque timebase
- FAST/RUN filtering jest zależne od rzeczywistego `elapsed_ticks`, nie od liczby wywołań foreground;
- missed-tick regression zachowuje identyczny wynik 35-ms FAST filtra.

### FW142 — testowalny current loop / electrical SIL
- PI Id/Iq + wspólny vector limiter wydzielone do jednego production module `foc_current_loop.c`;
- target firmware i SIL wykonują ten sam kod;
- parity test legacy-vs-helper: 2000 losowych stanów PASS;
- real `FOC.c`, inverse Park, SVPWM, PMSM, physical Hall, `rotor_angle`, QZERO są wykonywane w electrical SIL.

## 2. FW143 — Walk Assist 10–60 rpm

Kontrakt produkcyjny:

```text
normal target: 10..60 chainring/output rpm
default:       30 rpm
70 / 80 / >80: invalid/range/overspeed tests, nie normalne targety Walk
```

Zakres jest scentralizowany w `inc/config.h` i konsumowany przez bank/config validation oraz `walk_assist_motor.c`.

Nie wymagamy laboratoryjnego servo-speed. Niewielkie pływanie pod małym obciążeniem jest metryką jakości, nie FAIL. Twarde kryteria to brak runaway, poprawny start/lifecycle, bezpieczny stall, zero na release/brake/fault/wheel-cut i respektowanie ceiling Iq.

### Full Walk electrical matrix

Real production path:

```text
walk_assist_motor.c
 -> walk_speed_controller.c
 -> final Iq path
 -> real FOC.c / PI / vector limit / SVPWM
 -> virtual PMSM
 -> physical Hall edges
 -> production Hall feedback
 -> Walk governor
```

Macierz deterministyczna:

```text
7 targetów: 10 / 15 / 20 / 30 / 40 / 50 / 60 rpm
3 obciążenia
6 pozycji startowych Hall
= 126 przypadków
```

Wynik:

- **126 / 126 safety/lifecycle PASS**;
- `failures=0`;
- `safeStalls=2` — ciężkie przypadki kończą w bezpiecznym stall zamiast runaway;
- `trackingWarnings=23` — lekkie obciążenie/niskie rpm może pływać; warning, nie FAIL;
- worst first Hall ~206 ms w macierzy Walk;
- invalid targets `0, 9, 61, 70, 80, 100` nie stają się ukrytym targetem Walk i wracają do bezpiecznego default 30 rpm.

## 3. Aktualny gate FW143

### Real-module host suites
- **69 / 69 PASS**.

### Whole-pipeline deterministic regression
- **18 / 18 traces PASS**;
- missed-tick regression PASS;
- repeat RUN_100 byte-identical PASS.

### Fast supervisory closed-loop SIL
- clean/loaded start PASS;
- PAS bounce: 0 false reverse;
- cadence A/B: 71 -> 19 Iq p-p;
- STOP -> restart PASS, first restart Iq = 1 count;
- deterministic fuzz: **10 000 / 10 000 PASS**;
- ASan/UBSan fuzz: **1 000 / 1 000 PASS**.

### Real electrical FOC/PMSM/Hall/QZERO SIL
- real SVPWM geometry sweep through `_U_MAX=1920`: PASS;
- locked 6-sector current tests PASS;
- moving current tracking 50..220 erps PASS;
- Hall start sweep: **24 angles x 2 loads = 48 / 48 PASS**;
- worst normal permission->first Hall ~45 ms;
- initial 3-Hall uncertainty bounded at ~30 electrical degrees;
- full electrical deterministic fuzz: **1 000 / 1 000 PASS**;
- ASan/UBSan electrical fuzz: **100 / 100 PASS**;
- QZERO branch executes; current virtual plant uses safe abort rather than forcing an unproven handback.

### Build/packaging infrastructure
- source manifest: **59/59 production C files listed; 85 total build entries**;
- target-tree/startup/linker/CMSIS/HAL self-check PASS;
- BL820 packager independent CRC/container regression PASS;
- cross-platform builder: `tools/build_firmware.py`;
- Windows one-command gate/build: `VERIFY_AND_BUILD_WINDOWS.bat`.

## 4. Co znajduje się w tej paczce

Paczka jest przygotowana jako nowy workspace projektu i zawiera:

- pełne production `src/` + `inc/`;
- GD32 HAL/CMSIS, startup i linker;
- build/source manifests;
- host tests, whole-pipeline regression i oba SIL-y;
- protocol/config schema;
- pełną historię `.git` tej linii rozwoju;
- `START_HERE.md`, `AGENTS.md`, aktualną mapę architektury i test contract FW143;
- wybrane kluczowe dokumenty reverse G532/Fake Taxi/TSDZ2/M820 w `docs/reference/`.

Nowy agent nie powinien potrzebować wcześniejszych czatów do zrozumienia bieżącej architektury.

## 5. Czego paczka celowo NIE zawiera

Nie bundlujemy zewnętrznego toolchainu wykonywalnego. Do finalnego target build potrzebny jest:

```text
Arm GNU Toolchain arm-none-eabi 13.2 Rel1
GCC 13.2.1
```

Ten wymóg istniał już w oryginalnym build systemie v3.

Do PC verification potrzebne są Python 3 + host C compiler. Git jest potrzebny do normalnej dalszej pracy z historią.

## 6. Jak rozpocząć nowy projekt / nowego agenta

Pierwsza instrukcja dla agenta:

```text
Przeczytaj START_HERE.md oraz AGENTS.md. Traktuj tę paczkę jako authoritative EVistDrive baseline.
Uruchom quick gate przed zmianami. Nie przywracaj historycznych workaroundów bez reprodukcji i testu.
```

Potem:

```bash
python tools/verify_all.py --quick
```

Pełny PC gate:

```bash
python tools/verify_all.py
```

Przed wygenerowaniem kandydata do flashowania:

```bash
python tools/verify_all.py --require-target
```

## 7. Nadal wymagające sprzętu

SIL nie zastępuje ostatniego gate na realnym M820. Po exact target build trzeba zweryfikować przede wszystkim:

1. brak start deadlock/opóźnienia pod realnym obciążeniem;
2. brak ciągłego pump/szarpania w 20..120 rpm;
3. true reverse cut;
4. końcowy STOP click/QZERO na prawdziwej mechanice;
5. ARMED_ZERO restart;
6. Walk 10..60 rpm pod rzeczywistym obciążeniem — drobne pływanie akceptowalne, runaway/stall safety nie;
7. rzeczywisty runtime bank/config dump.

Nie stroić FOC/QZERO/Walk pod parametry wirtualnego PMSM bez potwierdzenia sprzętowego.
