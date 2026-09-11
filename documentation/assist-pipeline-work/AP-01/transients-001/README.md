# AP-01 transients-001 — zmiana nacisku, stop i ponowne rozpoczęcie pedałowania

Karta: [`TASK-EVD-AP-01-TRANSIENTS-001`](../../../../../integration/tasks/TASK-EVD-AP-01-TRANSIENTS-001.md).
Raport wykonania: [`EXEC-EVD-AP-01-005`](../../../../../integration/task-reports/EXEC-EVD-AP-01-005.md).
Poprzedni stan przyjęty: [`REVIEW-EVD-AP-01-004`](../../../../../integration/task-reports/REVIEW-EVD-AP-01-004.md).

To **nowy zakres AP-01**, nie poprawka odebranego narzędzia. Przyjęta seria 21 przypadków,
`../results/` i manifesty AP-01 są tu **READ_ONLY** i nie zostały nadpisane.

## Co powstało

| Plik | Rola |
|---|---|
| `snapshot/controller_lab.c.input` | bajtowa kopia wejściowego generatora (SHA256 = wartość z karty), żeby regresję dało się odtworzyć mimo zastanego nieśledzonego WIP |
| `snapshot/README.md.input` | to samo dla README harnessa |
| `scenarios/scenarios.py` | definicje prób; każdy przypadek startuje z **przyjętej** konfiguracji linear72 (import `ap01_runner.TUNING/LEVEL/BASE`) i jawnie wymienia odstępstwa |
| `transients_runner.py` | wykonanie prób, manifest z hashami, dwukrotne powtórzenie każdego przypadku |
| `event_metrics.py` | adapter pomiarowy dla rekordów z **wieloma** zdarzeniami; importuje prymitywy z `../metrics.py`, nie kopiuje ich |
| `analyze.py` | zastosowanie adaptera do własnego manifestu → `results/transients-metrics.json` |
| `make_plots.py` | wykresy całego toru (SVG bez bibliotek, bez wygładzania) |
| `test_transients.py` | testy okien/metryk na sygnałach o znanym wyniku + testy natywnego CLI |
| `baseline_manifest.py` | manifest przed/po z jawnie dozwoloną różnicą harnessa |
| `results/`, `plots/` | wyniki i wykresy tego zadania |

## Rozszerzenie generatora (WRITE_SCOPE)

Zmieniono wyłącznie `sim/controller_lab/controller_lab.c` i `sim/controller_lab/README.md`.
Cztery nowe klucze `key=value`, opisane w [`../../../../sim/controller_lab/README.md`](../../../../sim/controller_lab/README.md):
`torque_step_at_s`, `torque_step_nm`, `ride_restart_s`, `ride_stop2_s`.

- Zmieniają **wyłącznie syntetyczny odczyt siły i ruchu korby**. Żaden filtr, limit, rampa ani stan
  sesji nie jest tu skryptowany ani powielony.
- Przedziały pedałowania są półotwarte. W przerwie **nic nie jest zerowane**: kąt korby, filtry,
  PAS, sesja i rampa zostają w stanie, w jakim zostawił je tor produkcyjny.
- Zegar 4 kHz i cztery kroki 16 kHz bez zmian — to zadanie nie wprowadza jittera.
- Nowe klucze są parsowane ściśle i **odrzucane kodem 2**, nigdy po cichu zamieniane na inny
  scenariusz. Stary parser nie był przebudowywany: dotychczasowe klucze zachowują dawne zachowanie.
- CSV: wszystkie dotychczasowe kolumny i ich znaczenie bez zmian; na końcu dopisane
  `torque_cmd_mean_nm` i `ride_interval`. Diagnostyczne `pedalling` nadal znaczy „nogi się kręcą”,
  a **nie** „sterowanie produkcyjne się zgadza”.

## Kontrakt pomiaru

Parametry **pomiaru**, nie stałe firmware (`analyze.py`, `event_metrics.py`):

```text
pre_window_s        1.0     okno przed zdarzeniem
settle_s            1.0     zapas na przejście; dobrany z ZMIERZONEGO czasu opadania
post_window_s       1.5     okno ustalone po zdarzeniu
zero_tolerance      1.0     jak w przyjętym kontrakcie S1
min_confirm_s       0.10
max_transition_gap_s 0.05
```

- **Żadne okno nie przechodzi przez następne zdarzenie.** `event_windows()` przycina każde okno
  sąsiednim zdarzeniem i **raportuje** przycięcie; okno skrócone nie jest równie mocną obserwacją.
- **Stop oceniany jest wyłącznie na próbkach do następnego zdarzenia.** Późniejszy drugi stop nie
  może potwierdzić pierwszego. Za krótka przerwa daje `INSUFFICIENT_OBSERVATION` — to jest
  odpowiedź, nie usterka do obejścia.
- Brak zaobserwowanego przekroczenia to **brak pomiaru**, nigdy zero.

### Kolejność zdarzeń przy restarcie (poprawione w rework-002)

Kolejność rozstrzyga **przedział przekroczenia zera** `(t_ostatniej_niezerowej, t_pierwszej_zerowej]`
porównany z zadaną chwilą restartu — nie sama wartość ostatniej próbki:

```text
ZERO_OBSERVED_BEFORE_RESTART                          przedział w całości przed restartem:
                                                      KOLEJNOŚĆ ZNANA; niepewna zostaje tylko
                                                      dokładna chwila przekroczenia w przedziale
                                                      i długość potwierdzenia
ORDER_UNDETERMINED_CROSSING_BRACKET_CONTAINS_RESTART   przedział ZAWIERA chwilę restartu:
                                                      eksport nie rozstrzyga kolejności
IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE                 stwierdzenie neutralne; NIE jest dowodem
                                                      opadania ani release
NO_ZERO_OBSERVED_IN_RECORD                            zera nie zaobserwowano w rekordzie
```

- Wcześniejsza wersja definiowała niepewność **odwrotnie** (zero zmierzone przed restartem nazywała
  „kolejnością nierozstrzygniętą”, a przypadek faktycznie nierozstrzygnięty zostawiała bez flagi) —
  patrz `REVIEW-EVD-AP-01-006` T006-01, sprostowanie w `EXEC-EVD-AP-01-007`.
- **Zero przed restartem to nie to samo co potwierdzony stop.** Stop pozostaje osobną metryką
  (`stop_before_restart`, wyłącznie okno `[stop, restart)`).
- Trend opadania, jeśli jest potrzebny, bierze się z **pomiaru** `iq_pre_restart_trend`, nigdy
  z pojedynczej niezerowej próbki.
- Dla kanału już dodatniego przy restarcie `*_resume_s`/`*_latency_s` są `None`; pierwsza dodatnia
  próbka po restarcie jest polem **diagnostycznym**, nie czasem reakcji.
- Przejścia sesji raportowane są **z czasami** (`session_transitions`), nie jako zbiór nazw.

### Tożsamość i kompletność manifestu

Wymagane uruchomienia to próby z `all_cases()` **oraz cztery uruchomienia regresyjne** `regr_*`.
Każde musi wystąpić dokładnie raz, mieć status `OK`, liczbę hashy powtórzeń **nie mniejszą niż
`repeats_per_case`** (jednoelementowa lista jest trywialnie „zgodna” i nie dowodzi powtórzenia),
zgodne hashe oraz — dla wpisu regresji — `new_csv_sha256` równy `csv_sha256` własnego uruchomienia.
Sprzeczność jest twardym odrzuceniem, nigdy cichym obniżeniem statusu.
- 10–90% liczone względem **jawnych** poziomów odniesienia (średnie okna przed/po).
  `ripple_spans_thresholds` nazywa ograniczenie na przebiegu pulsującym: pojedyncze przekroczenie
  progu przez tętnienie to nie to samo, co przejście średniej.

## Odtworzenie

Z katalogu `C:\Projekty\eVistDrive`, PowerShell (te same narzędzia co przyjęta seria AP-01):

```powershell
$env:CC='C:\Projekty\tools\w64devkit\bin\gcc.exe'
$env:PATH='C:\Projekty\tools\w64devkit\bin;'+$env:PATH
$py='C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe'
$b='motor-controller-firmware/documentation/assist-pipeline-work/AP-01/transients-001'

& $py "$b/baseline_manifest.py" before
& $py "$b/transients_runner.py"      # 39 przypadków, każdy 2x
& $py "$b/analyze.py"
& $py "$b/make_plots.py"
& $py "$b/test_transients.py"
& $py "$b/baseline_manifest.py" after
```

Przyjęte zestawy AP-01 (69 testów), uruchamiane bo rozszerzenie dotyka ich zależności:

```powershell
& $py motor-controller-firmware/documentation/assist-pipeline-work/AP-01/test_metrics.py
& $py motor-controller-firmware/documentation/assist-pipeline-work/AP-01/test_check_sensitivity.py
& $py motor-controller-firmware/documentation/assist-pipeline-work/AP-01/test_pipeline_failure_handling.py
& $py motor-controller-firmware/documentation/assist-pipeline-work/AP-01/repro_v2_findings.py --expect-fixed
```

Build natywny trafia do współdzielonego `.build/controller_lab/` — nie uruchamiać równolegle
innego buildera tego katalogu.

## Kwalifikacja dowodu (powtarzać wszędzie, gdzie cytowane są te liczby)

`controller_lab.c` nie zamyka pętli PMSM/FOC. `u_abs` i zmierzony prąd baterii są stale zerem.
Kadencja/moment/prędkość to wymuszone wejścia syntetyczne. Te wyniki opisują **kompozycję żądania**
(`assist_modes → limits → fast_iq_slew`, obserwowane jako `iq_ref`) dla zadanych wejść — **nie**
osiągi mechaniczne/elektryczne, **nie** actual current, bridge ani zachowanie HW. Poprawność
generatora i obserwacja defektów obecnego firmware to dwie różne rzeczy: zły przebieg produkcji nie
oznacza automatycznie błędu nowego generatora.

Reverse, sensor invalid i jitter **pozostają poza tym przydziałem** i nie są tu oznaczone PASS.
