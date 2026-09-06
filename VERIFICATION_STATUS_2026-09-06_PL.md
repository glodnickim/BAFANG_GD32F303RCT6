# EVistDrive v3 - status po etapie FW139-FW141

Baseline: `EvistDrive06092026v3.zip`
Branch: `master`

## Zrealizowane zmiany

1. **Test infrastructure sync** (`a6e9dd9`)
   - przenosny runner host-testow;
   - schema diagnostic v4 zsynchronizowana z testami.

2. **FW139 PAS physical glitch rejection** (`7033c71`)
   - odbicia 1-3 ticki nie sa przekazywane do direction safety;
   - prawdziwy reverse zachowuje natychmiastowe direction inhibit.

3. **FW139 startup trajectory cleanup** (`0be2557`)
   - usuniety Hall-gated gear preload z normalnego startu;
   - finalny 16 kHz Iq trajectory pozostaje jedynym ownerem narastania pradu.

4. **Closed-loop supervisory SIL + fuzz** (`0be2557`, `a3ad8b4`, `989f83f`)
   - prawdziwy PAS -> torque -> rider_input -> ride_control -> final Iq;
   - model ridera, PAS A/B, Hall feedback i breakaway load;
   - scenariusze clean/loaded start, PAS bounce, cadence ripple, pedal 20/40/60/80, stop/restart;
   - deterministyczny fuzz.

5. **FW140 conditioned cadence** (`e6150d9`)
   - wspomaganie i adaptive Iq dynamics korzystaja z jednego filtra kadencji sterujacej;
   - pierwszy realny pomiar seeduje filtr bez dodatkowego start delay;
   - raw cadence zostaje do diagnostyki/HMI.

6. **FW141 elapsed-time torque filtering** (`18d81e0`)
   - 35 ms FAST oraz 120/250 ms RUN sa liczone w realnym czasie 4 kHz, nie liczbie wywolan foreground;
   - missed-tick burst nie rozciaga filtrow torque;
   - caly FAST->RUN cascade jest catch-upowany w poprawnej kolejnosci.

## Aktualny gate PC

### Host real-module suites
- **68 / 68 PASS**

### Whole-pipeline deterministic regression
- 6 scenariuszy x 3 warstwy = **18 traces PASS**
- missed-tick burst: **PASS**
- determinism RUN_100 rerun: **PASS byte-identical**

### Closed-loop supervisory SIL
- clean start: permission -> first Hall ~33.5 ms
- loaded start: permission -> first Hall ~37.5 ms
- PAS bounce: 0 false reverse / 0 inhibit ticks
- cadence raw peak-to-peak Iq: 71 counts
- cadence filtered peak-to-peak Iq: 19 counts
- stop/restart: PASS; first restart Iq=1; max rise step=1
- fuzz: **10000 / 10000 PASS**
- ASan/UBSan fuzz: **1000 / 1000 PASS**

## Wazne ograniczenie aktualnego SIL

Obecny SIL jest zamknieta petla supervisory, ale nie jest jeszcze pelnym modelem elektrycznym PMSM/invertera. Current loop jest stand-inem (~2 ms tracking), a nie realnym `FOC.c` pracujacym przeciwko modelowi faz Ia/Ib/Ic i PWM/ADC.

## Co dalej

1. **Electrical FOC SIL**
   - realny `FOC.c`/PI/SVPWM przeciwko modelowi PMSM;
   - Ia/Ib/Ic, ADC offset/quantization, bus voltage, rotor electrical angle, 3 Hall;
   - wszystkie 6 startowych sektorow Hall;
   - current limit, voltage limit, saturation, anti-windup, zero-current.

2. **FOC lifecycle matrix**
   - COLD -> PREPARE -> ACTIVE -> ARMED_ZERO -> ACTIVE;
   - normal stop bez MOE chatter;
   - fault shutdown niezaleznie;
   - reverse i rolling restart.

3. **Residual architecture cleanup dopiero po electrical SIL**
   - one permission owner;
   - one rider-effort estimator;
   - A/B legacy `min-Iq hold`, QZERO, coast-release;
   - bez zmiany wszystkiego naraz.

4. **Target build gate**
   - exact Arm GNU 13.2.1 + production linker/startup;
   - obecne srodowisko nie ma `arm-none-eabi-gcc` ani PowerShell, wiec target `.bin` nie zostal tu jeszcze zbudowany;
   - `tools/verify_all.py --require-target` jest przygotowany jako twardy gate, gdy toolchain jest dostepny.

## Regula dalszej pracy

`host -> whole-pipeline -> closed-loop SIL -> electrical FOC SIL -> exact target build -> dopiero hardware`
