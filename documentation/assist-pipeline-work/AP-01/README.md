# AP-01 — Baza pomiarowa i scenariusze całego toru

Historia: V1 `EXEC-EVD-AP-01-001` → review `REVIEW-EVD-AP-01-001` (AP01-R1..R6) → V2
`EXEC-EVD-AP-01-002` → review `REVIEW-EVD-AP-01-002` (AP01-V2-01..04) → V3 `EXEC-EVD-AP-01-003` →
review `REVIEW-EVD-AP-01-003` (AP01-V3-01/02) → **V4 `EXEC-EVD-AP-01-004`** (ten stan). Wszystkie w
`integration/task-reports/`. Karty: `integration/tasks/TASK-EVD-AP-01.md` oraz
`TASK-EVD-AP-01-REWORK-001/002/003.md`.

## REV4 (po review V3) — klasyfikacja wyników

Zawężony rework: **tylko** klasyfikacja/porównanie i powiązane opisy. Stop, obsługa awarii runnera,
inwentarz, okna i liczniki przeszły odbiór i nie były ruszane. 21 CSV i manifest przyjętego biegu
bez zmian — przeliczono wyłącznie metryki pochodne.

- **AP01-V3-01 — trzy etapy zamiast jednej zgadywanej przyczyny.** Limit profilu leży między
  `iq_before_profile_limit` a `iq_mode_request` (potwierdzone w `src/assist_modes.c`: `iq_before_pu`
  jest pobierane tuż przed klamrowaniem do `profile_iq_pct_limit`, a komentarz mówi, że różnica
  oznacza `max_iq_pct` „and nothing else”). REV3 porównywał `iq_before_profile_limit →
  iq_requested`, czyli **pomijał etap**. Teraz raportowane są osobno: `profile_limit`,
  `post_mode_request`, `request_to_allowed`. Tylko pierwszy ma przypisaną przyczynę; dla dalszych
  podawane jest wyłącznie `RAISED/REDUCED/UNCHANGED/MIXED` — floor/gate **nie są zgadywane**.
  Ocena dotyczy właściwego kanału i etapu: `torque_run_native` i `cadence_control_rpm` nie
  dostają już etykiety z toru Iq.
- **Oba floor cases pokazują ten sam wniosek:** `profile_limit` REDUCED 129–179 → **7**, potem
  `post_mode_request` RAISED 7 → **14** albo **175**. Dawna etykieta „CAP_BINDING 174 → 14”
  ukrywała ten błąd kolejności limitów; „174” nie było stałą wartością pre-limit całego okna.
  `without_rotation_repro` ma `profile_limit` UNCHANGED (285 → 285) i dopiero później zerowanie —
  nie jest działaniem capu.
- **AP01-V3-02 — kształt nie wyklucza wyniku.** Plateau bez dowodu zmiany na obserwowanym etapie to
  `PLATEAU_NO_LIMIT_EVIDENCE` (informacja, `comparable=true`). Wykluczenie wymaga
  `LIMITED_UPSTREAM`, czyli faktycznej zmiany wartości na etapie powyżej danego kanału. Test
  rzeczywistego obcięcia zawiera żądanie ponad limit i jego redukcję. `NO_LIMIT_OBSERVED` mówi
  wprost, że nie obejmuje limiterów baterii/termiki/prędkości.
- **Baseline ripple = 0:** nowe tętnienie u kandydata to `RIPPLE_REGRESSED_FROM_ZERO` (regresja,
  procentowa redukcja niezdefiniowana), zero→zero to `UNCHANGED_BOTH_FLAT`.
- **S1 / opisy:** usunięto twierdzenie, że `CONFIRMED` gwarantuje brak wcześniejszego ponownego
  wzrostu — funkcja szuka ostatniego stabilnego odcinka i dopuszcza wcześniejsze przejściowe zero;
  liczba takich zdarzeń jest raportowana osobno jako `excursions_before_confirmation`. Krótszy stop
  nie jest automatycznym dowodem poprawy bezpieczeństwa/mechaniki.

## REV3 (po review V2) — co się zmieniło

Metoda: dla każdego findingu najpierw reprodukcja kontrprzykładu, potem minimalna poprawka, potem
test pozytywny i negatywny. `repro_v2_findings.py` uruchomiony PRZED poprawkami potwierdził 15/15
podproblemów (`results/repro_v2_before_fix.json`), po poprawkach 0/15
(`results/repro_v2_after_fix.json`).

- **AP01-V2-01 (stop / latencja):** `stop_points` wymaga teraz OBSERWOWANEGO utrzymania w tolerancji
  (`min_confirm_s`, kontrakt pomiaru, nie stała firmware) — pojedyncze końcowe zero daje
  `INSUFFICIENT_OBSERVATION`, nie `CONFIRMED`. Niepewność chwili stopu to `transition_bracket_s`
  (para sąsiednich próbek otaczających przejście), a nie mediana próbkowania; opcjonalny
  `max_transition_gap_s` daje status `OBSERVATION_GAP_TOO_LARGE`. `rise_time_10_90_detail` zwraca
  status i nie podaje latencji, gdy przecięcie 10% leży poza zapisem.
- **AP01-V2-02 (kryteria / saturacja / bias):** kryteria rozdzielone na PRESERVATION (budżet
  średniej 5%) i IMPROVEMENT (kierunkowo: mniejsze tętnienie jest lepsze, roboczy cel pilota ≥50%
  redukcji dla ≥40 rpm) — duża poprawa nie jest karana. Stop ma sufit latencji, nie symetryczne
  delta. Ograniczenie rozpoznawane z RZECZYWISTEJ pary kanałów `iq_before_profile_limit` vs
  `iq_requested`; płaskość jest tylko informacją i nie wyklucza porównania. Bias jest LICZONY w
  wynikach względem jawnie nazwanego przypadku odniesienia. Usunięto uzasadnienie oparte na Walk.
- **AP01-V2-03 (test błędu runnera):** test dociera teraz do rzeczywistego błędu native
  (`exit=2`, „invalid/unavailable assist mode 97”) i `RequiredCaseFailed`; `ModuleNotFoundError`
  jest jawną PORAŻKĄ testu. Izolowany jest tylko OUTPUT (`AP01_RESULTS_DIR`), nie lokalizacja kodu.
  Manifest jest oznaczany `run_complete=false` z nowym `run_id` PRZED pierwszym przypadkiem, więc
  awaria przygotowania nie zostawia starego sukcesu; łapane są wszystkie wyjątki; wymagane
  porównania są SPRAWDZANE; ekstraktor odrzuca sprzeczny manifest i nazywa nieaktualny plik metryk.
- **AP01-V2-04 (inwentarz):** dodano `evidence_level` (FILE_EXISTS / READ_IN_THIS_SESSION /
  EXECUTED_IN_THIS_SESSION) — nic nie jest na poziomie EXECUTED, żaden suite nie był uruchomiony.
  Wycofano „already proven exhaustively” itp. Rozdzielono trzy różne wejścia (invalid krok
  kwadratury PAS / `pas_sensor_valid=false` / `torque_sensor_valid=false`) i wskazano częściowe
  pokrycie w `tests/host/fw100_extended_boost_host.c:200-201`. Sprostowano twierdzenie o CSV:
  `sim/l4/virtual_bike_l4.c:473` i `sim/replay/replay_fw.c:76` też emitują CSV (L4 ma `iq_actual`
  i `u_abs`, których Controller Lab nie ma).
- **`test_check_sensitivity.py` (nowy):** cztery kontrole zostały doprecyzowane w trakcie poprawek
  (ich pierwotna forma dopasowywała też zdania WYCOFUJĄCE dawne twierdzenie). Ten plik dowodzi, że
  doprecyzowane kontrole nadal wykrywają defekty V2 — kontrola, która tylko przestaje krzyczeć,
  jest bezwartościowa.

Zachowane bez zmian (na polecenie reviewera): 21 CSV (bitowo identyczne), `results/ap01-results.json`
przyjętego biegu, poprawki okien R2 i liczników R6.

## REV2 (po review V1) — co się zmieniło

- `metrics.py` / `test_metrics.py`: `stop_points` rozdziela teraz `NO_DATA` / `NOT_REACHED` /
  `CONFIRMED` zamiast zwracać czas ostatniej niezerowej próbki jako "czas do zera" (AP01-R1).
  Walidacja wejść (długości, monotoniczność, skończoność, tolerancja > 0). `window()` rzuca błąd
  zamiast cicho skracać przez `zip`. Dodano `relative_ripple`, `relative_bias`, `saturation_flags`.
- `extract_metrics.py`: okna liczone z `ride_start_s`/`ride_stop_s` zapisanych w manifeście per
  przypadek, nie ze stałej 3–7,5 s narzuconej wszystkim plikom (AP01-R2). Czyta wyłącznie własny
  `results/ap01-results.json`, weryfikuje hash CSV na dysku i status `run_complete` (AP01-R5).
- `ap01_runner.py`: błąd wymaganego przypadku przerywa cały bieg (`sys.exit(1)`,
  `run_complete=false`), zamiast zapisać błąd i kontynuować (AP01-R5).
- `test_pipeline_failure_handling.py` — nowy: odtwarza kontrprzykłady z review (stop_points),
  wymuszony błąd przypadku (runner), spreparowany/przestarzały CSV i niekompletny manifest
  (extractor) w izolowanych katalogach tymczasowych, bez dotykania `results/`.
- `tool_inventory_r4.json` — nowy: tabela reuse/extend dla 7 kategorii NEEDS_HARNESS_EXTENSION,
  z konkretnym plikiem/funkcją istniejącego testu i realną luką (AP01-R4). Propozycje CLI
  poprawione na `key=value` (zgodnie z `parse_arg()`), usunięto błędną składnię `--flag`.
- `comparison_criteria_r3.json` — nowy: tabela metryka → definicja/odniesienie → próg → uzasadnienie
  → ograniczenia, w tym otwarte braki nazwane wprost zamiast liczby bez podstawy (AP01-R3).
- `scenario_matrix.json`: liczniki wyliczane z tablicy (11 kategorii, 4 RUN, 7 EXTENSION), poprawiona
  interpretacja fizyczna (mniej, nie więcej, obrotów korby w oknie przy niskiej kadencji — AP01-R6),
  poprawione pola stop_points.

## Co jest w tym katalogu

- `metrics.py` — niezależne prymitywy metryk (p-p/mean, RMS, RMS ripple wokół średniej, bias,
  rise/fall 10-90%, stop_points). Nie duplikuje żadnej matematyki produkcyjnej.
- `test_metrics.py` — AC2: każdy prymityw zweryfikowany na sygnale stałym, znanej fali sinus i
  znanym skoku (analityczne wartości oczekiwane), zanim użyty na CSV Controller Lab.
- `ap01_runner.py` — niezależny runner używający ISTNIEJĄCEGO natywnego Controller Lab
  (`sim/controller_lab/controller_lab.c`) i ISTNIEJĄCEGO serializera CANable
  (`sim/controller_lab/canable_bridge.js`). Nie modyfikuje `run_probe.py` (oryginalny dowód
  audytu, READ_ONLY) — duplikuje tylko domyślne presety tuningu/levelu, bo import z pliku
  audytu naruszałby jego READ_ONLY status.
- `extract_metrics.py` — stosuje `metrics.py` do CSV z `results/`, okno stanu ustalonego 3–7.5s
  (ta sama konwencja co w audycie, dla porównywalności).
- `scenario_matrix.json` — AC5: scenariusz → wejścia → kanały → metryki → próg/otwarta decyzja,
  ze statusem RUN lub NEEDS_HARNESS_EXTENSION (z konkretnym plikiem/zakresem propozycji zamiast
  cichego oznaczenia braku jako PASS).
- `repro_v2_findings.py` — reprodukcja findingów review V2 (przed/po poprawce);
  `--expect-fixed` kończy się błędem, jeśli którykolwiek defekt nadal występuje.
- `test_check_sensitivity.py` — dowód, że doprecyzowane kontrole z powyższego pliku nadal
  wykrywają defekty V2 (mutacyjne wejścia „V2-like”).
- `results/` — CSV, `ap01-results.json` (manifest wykonania z hashami), `ap01-metrics.json`,
  `repro_v2_before_fix.json`, `repro_v2_after_fix.json`.
- `baseline_check_before.json`, `baseline_check_after.json` — wynik porównania hashy z
  `TASK-EVD-AP-01-BASELINE.json` przed i po pomiarach.

## Odtworzenie

Z katalogu `C:\Projekty\eVistDrive`, PowerShell (te same narzędzia co w
`documentation/assist-pipeline-evidence/README.md`):

```powershell
$env:CC='C:\Projekty\tools\w64devkit\bin\gcc.exe'
$env:PATH='C:\Projekty\tools\w64devkit\bin;'+$env:PATH
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-work/AP-01/ap01_runner.py
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-work/AP-01/extract_metrics.py
```

Testy metryk (nie wymagają gcc/node):

```powershell
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-work/AP-01/test_metrics.py
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-work/AP-01/test_check_sensitivity.py
```

Testy ścieżek błędu (wymagają gcc/node — uruchamiają prawdziwy natywny przypadek) oraz kontrola
findingów review V2:

```powershell
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-work/AP-01/test_pipeline_failure_handling.py
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-work/AP-01/repro_v2_findings.py --expect-fixed
```

`AP01_RESULTS_DIR` przekierowuje katalog wyników (używane przez testy negatywne, żeby izolować
wyłącznie OUTPUT); `AP01_NODE` wskazuje interpreter Node. `ap01_runner.py --abort-selftest`
uruchamia celowo niepoprawny przypadek przez prawdziwy entry point.

Wymagane: Python 3.12 pod powyższą ścieżką, gcc z `w64devkit`, Node pod
`C:\Program Files\nodejs\node.exe`, moduł `sim/controller_lab` i sąsiednie repo `canable-web`
(te same zależności co audyt). Build natywnego Controller Lab trafia do `.build/controller_lab/`
i przebudowuje się tylko przy zmianie fingerprintu źródeł — współdzielony z web UI Controller
Lab, nie uruchamiać równolegle innego buildera tego katalogu (patrz WRITE_SCOPE karty).

## Kwalifikacja generatora (AC4)

Jak w audycie: `controller_lab.c` nie zamyka pętli PMSM/FOC. `u_abs` i zmierzony prąd baterii są
zerem na stałe. Cadence/torque/speed to wymuszone syntetyczne wejścia, nie wynik dynamiki roweru.
Wyniki tego katalogu dowodzą KOMPOZYCJI ŻĄDANIA (assist_modes → limits → fast_iq_slew, obserwowane
jako `iq_ref`) dla zadanych wejść — nie osiągów mechanicznych/elektrycznych i nie zachowania HW.
Granica dowodu: forced-input / electrical / Level4 / HW to różne klasy dowodu; `u_abs=0` nie opisuje
rzeczywistej jazdy.

## Znane luki harnessa (nie ukryte jako PASS)

`controller_lab.c` obsługuje jeden skalarny `torque_nm`/`torque_ripple_pct` na cały bieg i
dokładnie jedno zdarzenie `ride_start_s`/`ride_stop_s`. Nie obsługuje: skoku/zmniejszenia
obciążenia w trakcie PAS, restartu przed zakończeniem release, rolling restart, jitter/pominiętych
ticków, prawdziwego cofania kadencji ani wymuszenia `torque_sensor_valid`/`pas_sensor_valid` na
false. Każdy z tych przypadków ma w `scenario_matrix.json` konkretną propozycję rozszerzenia
(plik, zakres, uzasadnienie) i status `NEEDS_HARNESS_EXTENSION` — nie został wykonany w tej sesji
i nie ma przydziału do wspólnego harnessa.
