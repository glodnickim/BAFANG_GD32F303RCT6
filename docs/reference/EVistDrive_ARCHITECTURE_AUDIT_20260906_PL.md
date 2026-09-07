# EVistDrive - audyt architektury assist / START / STOP

Data: 2026-09-06
Zakres: aktualny snapshot EVistDrive z poprawka FW138, porownanie ze snapshotami 2026-08-28 i 2026-08-30 oraz odniesienie do odzyskanego zachowania G532.

## 1. Cel

Zachowac bogata funkcjonalnosc EVistDrive (tryby assist, parametry per level, boost, smooth start, release, cadence compensation, limity, walk, torque calibration itd.), ale usunac nakladajace sie mechanizmy, ktore tylko kompensuja skutki innych mechanizmow.

Zasada docelowa:

SENSOR TRUTH -> RIDER INTENT/PERMISSION -> DEMAND -> LIMITS -> ONE FINAL Iq TRAJECTORY -> FOC

Parametry uzytkownika maja zmieniac znaczenie funkcjonalne, a nie tworzyc kolejnych niezaleznych filtrow i state-machine za plecami innych warstw.

## 2. Najwazniejszy werdykt

Obecny kod ma dobra, coraz czystsza warstwe FOC/current-control, ale ride-control nadal niesie historyczny dlug. Sa jednoczesnie:

- stare pola konfiguracji, ktore juz nie steruja niczym,
- stary filtr RUN, ktory nadal jest liczony, ale jego wynik jest nadpisywany,
- recovery state-machine zbudowany glownie wokol problemu stalego starego RUN,
- hold/min-Iq, ktory rozwiazuje ten sam objaw co pozniejsze filtrowanie rider effort,
- osobny Smooth Start i osobny Gear Preload przed finalnym Iq slew,
- stary coast-release zbudowany pod skok kata, ktory canonical angle FW131 juz usunal,
- stary 4 kHz assist_dynamics_apply oraz nowy 16 kHz final slew - dwa opisy tej samej dynamiki, z czego stary nie ma juz produkcyjnego call-site.

To nie jest problem FW138. Wiekszosc tej architektury byla obecna juz w snapshotach 28.08 i 30.08.

## 3. Co bezwzglednie zachowac

### 3.1 Sensor / physical truth

KEEP:
- torque calibration i mapowanie do fizycznego kg/centikg,
- auto-zero/coast recalibration z warunkami bezpieczenstwa,
- krotki filtr szumu sensora,
- PAS quadrature validation, kierunek, liveness i cadence,
- wheel speed / Hall timing,
- start_phase jako osobny fakt 'korba juz ruszyla, cadence jeszcze niezmierzone'.

### 3.2 Funkcje rider-facing

KEEP FUNCTIONALITY:
- Power Linear,
- Power Progressive,
- Power Curve,
- eMTB,
- Torque,
- support ratio / min / max,
- assist torque full scale,
- crank length,
- max motor power,
- max Iq,
- startup boost,
- assist without pedal rotation,
- standstill start load,
- rolling start load,
- cadence compensation,
- Extended Boost (jezeli wlasciciel chce te funkcje),
- Smooth Start jako funkcje, ale nie jako osobny mnoznik przed druga rampa,
- release time,
- acceleration / deceleration,
- Walk Assist: target RPM, current ceiling, wheel-speed ceiling i latch,
- throttle/service paths,
- banki profili.

### 3.3 Limits / safety

KEEP:
- brake/fault/reverse final gate,
- speed limit,
- voltage limit,
- temperature limit,
- battery-current limit jako upstream Iq cap (QS-3C),
- level max power / max Iq,
- hard motor faults omijajace comfort shaping.

### 3.4 Motor control

KEEP:
- one final 16 kHz Iq owner (fast_iq_slew),
- PI Id/Iq + anti-windup,
- current feedback validity / last-valid handling,
- current calibration and synchronized sampling,
- canonical rotor angle FW131,
- FW138 Hall speed/angle confidence separation,
- cold PREPARE + neutral dwell,
- ARMED_ZERO lifecycle,
- no foreground PI reset on ordinary zero torque.

## 4. Elementy ewidentnie martwe / compatibility-only

### 4.1 Globalne stare rampy w tuning_config

src/tuning_config.c:21-24 przechowuje rise_slow_ms/rise_fast_ms/fall_slow_ms/fall_fast_ms.
Komentarz w tym samym pliku mowi wprost, ze po FW069 nic ich nie czyta; sa tylko dla starego formatu CAN/Canable.

Werdykt: REMOVE FROM CONTROL, zachowac tylko pola wire/migration.

### 4.2 power_rise_filter_ms / power_fall_filter_ms

inc/assist_modes.h:74+ i src/assist_modes.c:~900-925 mowia wprost INACTIVE.
Powod: filtr power request robil hidden soft-start przy rise i ghost demand przy fall.

Werdykt: juz poprawnie inactive. Ukryc w UI; zachowac bajty tylko dla zgodnosci. W nowej wersji bank blob mozna je odzyskac jako nowe pola po versioned migration.

### 4.3 run_deadband_mv

Aktualny reader poza serializacja jest tylko w rearm_delay diagnostics (main.c:3394).
Nie bierze udzialu w normalnym sterowaniu assist.

Werdykt: compatibility/diagnostic only. Nie prezentowac jako aktywnego parametru ride feel.

### 4.4 TORQUE_RUN_ATTACK

TORQUE_RUN_ATTACK_STEPS == 0. Komentarz src/torque_input.c:437+ mowi, ze mechanizm powstal dla complaintu, ktory okazal sie bledem klasyfikacji limitera (FW091), nie wolnym filtrem.

Werdykt: usunac produkcyjny kod po utrzymaniu ewentualnej historycznej dokumentacji/testu.

### 4.5 assist_dynamics_apply - stary 4 kHz owner

Search aktualnego source: brak produkcyjnego call-site assist_dynamics_apply().
Nowy owner to ride_final_iq_slew_compute() + fast_iq_slew_tick() 16 kHz.
Mimo tego src/assist_dynamics.c nadal zawiera kompletna druga implementacje rise/fall/release i ride_control_init() nawet resetuje jej nieaktywne state.

Werdykt: HIGH PRIORITY CLEANUP. Przeniesc wspolne definicje/typy do neutralnego iq_trajectory.h, a stara implementacje wyniesc do test/reference albo usunac. Nie utrzymywac dwoch algorytmow, bo sie rozejda.

## 5. Najwazniejszy martwy filtr - torque_run_window_deg

FW085 wciaz liczy moving-average po crank angle i ma parametr 0..360 deg.
Ale src/torque_input.c:~798-817 stwierdza wprost, ze wynik run_filter_step jest w ordinary RUN 'fully superseded below, every tick'.
Normalny RUN publikuje update_run_asym_filter(fast), nie moving average.

To samo bylo juz w snapshotach 28.08 i 30.08.

Skutek:
- bufor, suma, head, fill i parametr sa utrzymywane,
- uzytkownik moze myslec, ze zmienia 180 deg smoothing,
- normalny motor-control tej sredniej nie uzywa.

Werdykt: usunac window z active control. Jezeli chcemy rider-facing 'Torque smoothing', powinien sterowac JEDNYM aktywnym effort-estimatorem, nie martwym oknem.

## 6. 35 ms AFILT + 120/250 ms ARUN

Obecnie realny signal path ma co najmniej:

raw torque -> assist deadband -> 35 ms AFILT -> 120 ms rise / 250 ms fall ARUN -> mode demand -> final Iq trajectory

Komentarz inc/torque_input.h mowi, ze 120/250 wybrano glownie przez host suites, nie tuning na rowerze. 120 ms daje okolo 193 ms do first-positive-demand w opisanym tescie.

To jest prawdziwy rider-feel element i moze tlumaczyc czesc 'lazy start/recatch'.

Werdykt:
- KEEP jeden krotki sensor-noise filter,
- KEEP jeden rider-effort estimator,
- nie dokladac kolejnych power filters,
- przestroic ARUN na sprzecie dopiero po usunieciu redundantnych gate/cap.

## 7. Duplicated START permission

Aktualnie:
- start_phase staje sie true po START_PHASE_STEPS=2,
- cold_start_ready nadal wymaga tuning_config_start_steps(), default 4,
- potem osobny pedal_assist_gate ma komentarz, ze REPLACES old fwd_run >= start_steps gate.

Faktycznie nie zastapil go dla cold start, bo ride_session COLD nie przejdzie do ACTIVE zanim cold_start_ready nie spelni starych 4 krokow.

Werdykt: jedna bramka za duzo.

Docelowo:
- PAS direction validator zostaje,
- start_phase / minimalne potwierdzenie forward zostaje,
- load threshold zostaje,
- ride_session zostaje jako lifecycle/permission,
- pedal gate i start_steps nalezy scalic tak, aby istnial JEDEN owner permission.

Nie zmniejszac bezmyslnie safety. Najpierw rozdzielic 'forward confirmed' od 'motor torque rise'.

## 8. Hold 1400 ms + min-Iq floor 2% - overlapping fix

ride_control.c:~671-750:
- pozytywny mode demand odswieza assist_hold_ticks,
- przez hold czas iq_target jest podnoszony do min_iq,
- WAIT_FRESH_LOAD ma specjalny wyjatek, ktory zeruje hold,
- force_zero_reference ma kolejny specjalny wyjatek, zeby stary ramp state nie wypuscil pradu.

Historycznie hold/min-Iq rozwiazywal pulsing w dead spots. Pozniejsze AFILT/ARUN tez rozwiazuja per-leg ripple.

Werdykt: overlapping responsibility.

Propozycja:
- session latch = tylko permission,
- rider effort estimator = ciaglosc wysilku przez dead spot,
- final Iq shaper = dynamika pradu,
- min-Iq jezeli ma zostac jako funkcja, niech bedzie jawny 'minimum running assist' i dziala tylko przy dodatnim, realnym demand (albo jako advanced option), bez 1.4 s ukrytego podtrzymania zerowego demand.

Po tym recovery WAIT special-cases staja sie w duzej mierze zbedne.

## 9. Rolling rearm recovery - patch chain

WAIT_FRESH_LOAD/TRACK_FAST powstal, aby stale pre-reverse RUN window nie przetrwalo rearmu.
PATCH A potem ominelo redundantny pierwszy WAIT i startuje od TRACK_FAST.
Ride-control dodatkowo:
- substituuje torque_run_filtered = fast podczas recovery,
- zeruje hold w WAIT,
- wymusza same-tick force_zero_reference.

Poniewaz ordinary RUN window i tak zostal zastapiony przez ARUN, to jest bardzo duzy koszt zlozonosci wokol problemu, ktory mozna rozwiazac prosciej.

Docelowo rearm:
1. permission wraca,
2. aktywny ONE effort estimator jest jednorazowo seeded aktualnym fast torque,
3. demand liczy sie normalnie,
4. final Iq shaper odpowiada za bumpless restart.

Werdykt: MERGE/REMOVE po usunieciu starego window i hold-floor side effects.

## 10. Smooth Start + final Iq slew

assist_start_apply_smooth() mnozy iq_target przez czasowa envelope.
Potem finalny 16 kHz slew znowu ogranicza Iq.

To sa dwa shapers tej samej wielkosci.

Werdykt: KEEP funkcje Smooth Start, CHANGE IMPLEMENTATION.
Parametr duration powinien wybierac startowa trajektorie w jednym finalnym Iq ownerze, nie tworzyc upstream multiplier przed druga rampa.

## 11. Gear Preload - mocny kandydat do usuniecia

ride_control.c:~249 i ~1011-1052:
PRELOAD_IQ_CAP=10 (~1 A phase), timeout 300 ms, release po pierwszym Hall edge.
Komentarz w source sam dokumentuje, ze przez dlugi czas preload w ogole nie dzialal z powodu frozen speed, a po FW137 zaczal realnie wplywac na start i odkryl wlasny problem.

To jest osobny state machine przed finalnym slew.
G532 pokazuje prostsza zasade: Q state startuje od zero i pierwszy torque-producing increment jest bardzo maly; nie ma potrzeby czekac na Hall jako sygnal 'backlash done'.

Werdykt: HIGH PRIORITY REPLACE.
Zamiast preload cap:
- final Iq trajectory zaczyna od 0,
- pierwsze kilka procent ma delikatny take-up slope,
- potem szybki normal rise,
- brak 300 ms timeout,
- brak 'first Hall edge releases more torque'.

## 12. Co ciekawe: EVist pierwszy elektryczny krok nie jest za duzy

Przy PH_CURRENT_MAX=700 i shipped standstill rise:
- L1/L2 300 ms,
- L3 325 ms,
- L4 350 ms,
- L5 375 ms.

QS3D daje pierwsze 62.5 us okolo 0.017-0.021% full Iq, czyli MNIEJ niz odzyskane G532 ~0.111% Imax na pierwszy cykl.

Do PRELOAD_IQ_CAP=10 EVist dochodzi w okolo 4.3-5.4 ms.

Wniosek: odczuwane 'uderzenie' nie wynika z gigantycznego pierwszego FOC Iq step. Bardziej pasuje:
- delay permission,
- statyczne tarcie/backlash,
- preload cap/timeout/release,
- duzy upstream target/boost czekajacy za capem,
- nakladanie kilku state machines.

Nie spowalniac calego ride ramp jeszcze bardziej.

## 13. STARTUP BOOST

Startup Boost jest prawdziwa funkcja produktu. Powinien zostac.
Ale ma tylko zmieniac Iq_demand/virtual rider load. Nie powinien miec prawa ominac finalnego Iq trajectory.

Werdykt: KEEP.

## 14. Release vs in-ride fall

release_ms i iq_fall_* maja rozne znaczenie:
- iq_fall: zmiana targetu podczas dalszego pedalowania,
- release_ms: zejscie do 0 po realnym koncu pedalowania.

To sa sensowne dwie funkcje, ale ich wykonawca ma byc jeden final Iq trajectory owner.

Werdykt: KEEP semantics, MERGE implementation.

## 15. Stary coast-release po FW131

RIDE_COAST_RELEASE_ERPS=10 nadal jest opisany jako ochrona przed skokiem kata przy przejsciu interpolated <-> six-step.
Jednak aktywny build ma CANONICAL_ANGLE_ENABLE=1.
FW131 rotor_angle robi jeden canonical angle i bounded transfer.
Host proof FW131/131.1:
- legacy: worst 36 deg, 24 deg nawet na Hall edge,
- current losing trust ~0.077 deg,
- current gaining trust ~0.146 deg.

Czyli historyczna przyczyna FW048 zostala usunieta.

Werdykt: REVALIDATE, prawdopodobnie usunac jako root-cause workaround po A/B na rowerze.
Nie usuwac w ciemno, bo Hall-only przy bardzo malej predkosci nadal ma ograniczona wiedze o pozycji, ale nie wolno dalej uzasadniac tego progiem 'kat skacze 24-36 deg' - to juz nieprawda.

## 16. QZERO tez niesie zalozenie starego kata

quiet_zero.h/c w kilku miejscach nadal uzasadnia low-speed handback 'angle formula jumps'. Po FW131 to uzasadnienie jest stale.
QZERO moze nadal byc potrzebny jako jawna funkcja damping/spindown, ale nie jest fundamentalna czescia poprawnego zero-current FOC.

Werdykt: KEEP TEMPORARILY for current FW138 hardware A/B, potem uproscic:
- najpierw sprawdzic ordinary ARMED_ZERO + canonical angle + FW138 bez old coast assumption,
- jezeli zbyt dlugi run-on nadal wystepuje, zdefiniowac QZERO jako jawny 'coast damping', nie jako latanie starego angle jump,
- zero synthetic speed/BEMF po utracie confidence pozostaje poprawna zasada FW138.

## 17. 200 ms hard-cut ramp

RIDE_HARD_CUT_RAMP_MS=200 powstal, bo snap Iq->0 robil gearbox clunk.
Obejmuje brake/reverse/overtemp/torque fault, podczas gdy real overcurrent zabija bridge natychmiast.

To jest kolejny symptom-fix. Po uporzadkowaniu final Q slew i stop lifecycle nalezy sprawdzic, czy 200 ms jest nadal potrzebne. Safety policy musi pozostac firmware-owned i nie moze byc user comfort parameter.

Werdykt: REVALIDATE. Nie usuwac safety gate; ewentualnie skrocic/zmienic jego final Iq trajectory po pomiarze.

## 18. Docelowa architektura

### A. SENSOR TRUTH
raw torque -> calibration -> small deadband -> ONE short sensor filter
PAS -> legal transition/direction/cadence/start_phase
Hall/wheel -> motion facts

### B. RIDER INTENT / PERMISSION
one state machine:
- brake/fault safe,
- forward confirmed,
- configured load threshold met,
- assist level/mode allowed.

Nie generuje Iq. Tylko YES/NO permission.

### C. DEMAND
ONE mode calculator:
- Power/Torque/eMTB/Curve,
- startup boost,
- cadence compensation,
- assist without rotation,
- extended boost if enabled.

Output: Iq_demand. Zero hidden time filters here.

### D. LIMITS
max Iq, max power, battery current, speed, voltage, temp/legal.
Output: Iq_allowed.

### E. ONE FINAL MOTOR TRAJECTORY (16 kHz)
One owner state:
- START_TAKEUP: tiny first slope, then faster rise,
- RUN_RISE,
- RUN_FALL,
- RELEASE,
- SAFETY_FALL,
- ZERO/ARMED_ZERO.

No gear preload cap.
No separate smooth-start multiplier.
No current-floor owner after demand.
No old 4 kHz ramp.

### F. FOC
Consumes final Iq_ref only.
No knowledge of assist mode, boost, start_steps or rider features.

## 19. Rider-facing parameter model - TSDZ2-like UX, VESC/G532-like ownership

### Basic per level
- Assist mode
- Assist/support strength
- Max motor power
- Max motor current/Iq
- Start load
- Rolling load
- Acceleration
- Deceleration
- Release
- Start smoothness / Take-up
- Startup Boost enable/strength/end cadence

### Advanced per level
- support min/max / curve shape
- eMTB/torque factor
- smooth-start exact ms (maps to START_TAKEUP in final Iq owner)
- extended boost
- assist without rotation

### Global
- torque calibration
- assist torque full scale
- crank length
- sensor smoothing
- PAS forward confirmation (advanced, sensor property)
- cadence compensation

### Walk
- target chainring RPM
- max current
- max wheel speed
- latch behavior

Nie pokazywac jako active settings:
- legacy global ramp slots,
- power_rise_filter_ms / power_fall_filter_ms,
- run_deadband_mv,
- current torque_run_window_deg dopoki nie zostanie podlaczony do jednego prawdziwego estimatora.

## 20. Wire compatibility / 255 B limit

Bank blob ma dokladnie 255 B. Nie trzeba tracic funkcji ani od razu zmieniac transportu.

Mozna w nowej wersji bank/tuning:
- zostawic stare wersje do odczytu,
- dla nowej wersji jawnie zreinterpretowac INACTIVE bytes,
- np. 4 bajty power_rise_filter_ms/power_fall_filter_ms wykorzystac na nowe parametry START_TAKEUP,
- stare global ramp slots i dead run_deadband/window sa kolejnym miejscem na nowe globalne pola.

Warunek: tylko versioned migration. Nigdy nie zmieniac znaczenia bajtow dla starego version bez migracji.

## 21. Kolejnosc prac - bez utraty funkcji

Etap 0: zamrozic aktualny source + runtime bank dump.

Etap 1 - cleanup bez zmiany feel:
- usunac dead 4 kHz assist_dynamics production owner,
- oznaczyc inactive wire fields,
- ukryc dead UI settings,
- usunac disabled TORQUE_RUN_ATTACK implementation,
- zadnych zmian motor-control feel.

Etap 2 - rider-effort cleanup:
- usunac martwy crank-window z control,
- zostawic 35 ms sensor filter + ONE effort estimator,
- uproscic rearm do one-shot seed,
- A/B bez WAIT/TRACK automaton.

Etap 3 - permission cleanup:
- scalic ride_session + pedal gate/start_steps do jednego ownera,
- zachowac forward validation i load thresholds,
- usunac dodatkowy cold-start delay wynikajacy z podwojnej bramki.

Etap 4 - torque trajectory cleanup:
- przeniesc Smooth Start do final 16 kHz shaper,
- zastapic Gear Preload przez START_TAKEUP segment finalnego shaper,
- po tym A/B hold/min-Iq; jezeli niepotrzebny, usunac, jezeli potrzebny, zostawic jako jawna opcje.

Etap 5 - STOP cleanup po FW131/FW138:
- A/B stare RIDE_COAST_RELEASE_ERPS,
- A/B QZERO jako niezalezny damping, nie angle workaround,
- revalidate 200 ms safety fall.

Etap 6 - tuning hardware:
- dopiero tutaj dobierac Start Take-up, Acceleration, Deceleration, Release.

## 22. Minimalne acceptance tests

Kazdy etap musi zachowac:
- brak assist bez legalnego forward + permission,
- brake/fault cannot re-raise demand,
- reverse immediate permission loss,
- no stale Iq after rearm,
- cold start final Iq accumulator starts at 0,
- warm ARMED_ZERO restart bez bridge rearm,
- no click START/STOP,
- no ghost assist,
- no cadence-dependent start delay beyond sensor confirmation,
- all assist modes same steady-state target before/after cleanup,
- limits produce same ceilings,
- old bank blobs migrate deterministically.

## 23. Koncowy kierunek

Bogata funkcjonalnosc EVistDrive nie wymaga bogatego toru motor-control.

TSDZ2 jest dobrym wzorem na to, CO uzytkownik moze ustawic.
VESC/G532 sa lepszym wzorem na to, GDZIE ma byc shaping pradu i jak nie dopuscic do duzego pierwszego torque step.

Docelowo EVistDrive powinien miec wiecej funkcji niz stock Bafang, ale tylko jeden mechanizm odpowiedzialny za kazda rzecz:
- one sensor smoothing,
- one permission owner,
- one demand owner,
- one limit chain,
- one final Iq trajectory,
- one FOC owner.

To zachowuje funkcjonalnosc, a usuwa historyczne plastry.
