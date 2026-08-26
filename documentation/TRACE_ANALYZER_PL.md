# eVistDrive Iq & Power Anomaly Tester

To jest analizator zapisanych śladów jazdy. Nie kompiluje firmware, nie
uruchamia symulacji firmware i nie jest replayem kodu sterownika.

## Szybki start

Analiza CSV, JSON albo obsługiwanego surowego logu CAN:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_trace_analyzer.ps1 log.csv `
  --output-dir .build\trace-analysis\ride-01
```

Porównanie dwóch zapisów:

```powershell
.\tools\run_trace_analyzer.ps1 --good good.csv --bad bad.csv
```

Regresje analizatora:

```powershell
.\tools\run_trace_analyzer.ps1 --run-regressions tests\traces
```

Program zwraca kod `1`, jeżeli wykryje krytyczną anomalię. Jest to wynik
diagnostyczny, nie błąd wykonania programu.

## Wejścia

Obsługiwane są:

- zwykłe CSV rozdzielane przecinkiem lub średnikiem, w tym eksport aplikacji z
  `Timestamp (ms)`, `Torque (mV)`, `Cadence (rpm)`, `Current (A)` i `Power (W)`,
- ujednolicony JSON jako lista rekordów albo `{ "samples": [...] }`,
- surowe logi FW-112 DIAG, FW-112 A/B, FW-117 i ROLLING_NO_ASSIST, dekodowane
  przez istniejące skrypty PowerShell.

ROLLING_NO_ASSIST jest dispatchowany wyłącznie po schema byte w HEADER
`0x10248`. Legacy schema v1 oznacza trzy fragmenty DATA i tylko 24 bajty
faktycznie obecne na wire. Finalna FW-122 schema v2 oznacza sześć fragmentów
DATA, 44 bajty logical sample, 250 Hz i osiem próbek confirmation. Decoder
sprawdza te metadata i nie zgaduje layoutu po liczbie ramek. Schema v2 wnosi
osobno software PWM oraz real hardware MOE, lifecycle/current-cal, pełny upper
Iq chain, Hall/angles/direction/ERPS/PI i signed raw Iq.

Metadane mogą znajdować się obok śladu jako `trace.csv.meta.json`,
`trace.meta.json` albo `metadata.json`. Można je też podać opcjami CLI.

## Wynik

Katalog wyniku zawiera:

- `report.txt` — raport dla człowieka z pierwszym znanym odchyleniem,
- `analysis.json` — stabilny schemat maszynowy,
- `event_NNN.svg` — wykres od -500 do +1000 ms wokół eventu, bez zależności
  od matplotlib.

Klasy deterministyczne:

| Klasa | Pierwsze odchylenie | Znaczenie |
|---|---|---|
| A0 | `RIDER_INPUT` | moment jest obecny, ale kadencja/kierunek są niepoprawne |
| A1 | `PERMISSION` / `LATCHED` | aktywny rider nie otrzymał zgody albo latch nie został otwarty |
| A2 | `MODE_DEMAND` | zgoda istnieje, żądanie trybu nie powstało |
| A3 | `IQ_REQUEST` / `IQ_AFTER_LATCH` / `IQ_PRE_RAMP` | żądanie zginęło na P/U, latch/floor albo limiterze |
| A4 | `FINAL_IQ` | `iq_pre_ramp > 0`, ale finalne Iq jest zerowe |
| B | `BRIDGE_ON` | finalne Iq istnieje, ale mostek/PWM nie startuje |
| C | `CURRENT_RESPONSE` | Iq i PWM istnieją, lecz `iq_actual` nie odpowiada |
| D | `POWER_RESPONSE` | telemetria potwierdza oczekiwany assist, ale przez co najmniej jeden obrót korby nie ma odpowiedzi prądowej/mocowej |
| UNKNOWN | `UNKNOWN` / `POWER_RESPONSE` | zapis nie ma sygnałów potrzebnych do potwierdzenia usterki albo oczekiwanego assistu |

Warstwa adaptacyjna dopiero po diagnozie deterministycznej porównuje opóźnienia,
relacje wartości i przejścia stanów z profilem tej samej wersji/configu/loggera.

## Granica względem istniejących testerów

| Narzędzie | Odpowiedzialność | Czy analizator ją powiela? |
|---|---|---|
| `tests/host/run-host-tests.ps1` | kompilowane testy modułów C i testy wiring | nie |
| `tests/host/run_regression.ps1` | symulacja scenariuszy i metryki/golden | nie; CSV może być tylko wejściem |
| `tests/host/run_high_cadence.ps1` | benchmark wysokiej kadencji | nie |
| `tools/trace_analyzer.py` | analiza już zapisanego śladu i uczenie z GOOD | osobny etap offline |

Analizator nie został dopisany do żadnego z tych runnerów ani do skryptów
build. Wiedza o binarnym układzie CAN pozostaje w istniejących dekoderach.

## Baseline GOOD

```powershell
.\tools\run_trace_analyzer.ps1 good.csv --learn-good --session-status GOOD `
  --firmware-version FW-123 --git-commit abc123 `
  --motor-type M820_BL820 --config-hash cfg01 `
  --logger-schema-version fw112ab-2
```

Log trwale oznaczony `BAD` jest odrzucany nawet przy próbie wymuszenia
`--session-status GOOD`; `UNKNOWN` wymaga takiego jawnego potwierdzenia. Log z
niepełnym zestawem metadanych także jest odrzucany. Profil przechowuje
źródła GOOD z SHA-256 i może zostać odtworzony po usunięciu dowolnego źródła.

## Ograniczenie interpretacji

Eksport aplikacji z momentem, kadencją, prądem i mocą nie wystarcza samodzielnie
do krytycznej klasy D. Analizator wymaga także dodatniego `assist_level`, dowodu
permission, braku inhibitów oraz danych o limiterach prędkości, napięcia i
temperatury. Brak tych danych daje co najwyżej `INSUFFICIENT DATA`, nie fałszywy
critical. Do rozróżnienia A1–C potrzebny jest recorder z sygnałami wewnętrznymi.

Persistence D jest wyliczane z mediany rzeczywistych odstępów próbek i kadencji.
Zdarzenie musi zawierać kolejne próbki obejmujące co najmniej jeden obrót korby;
raport podaje liczbę obserwowanych i wymaganych próbek.
