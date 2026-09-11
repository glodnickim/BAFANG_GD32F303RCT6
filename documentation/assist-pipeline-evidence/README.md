# Dowody audytu toru wspomagania — 2026-09-08

Interpretacja: [ASSIST_PIPELINE_MASTER_PLAN.md](../ASSIST_PIPELINE_MASTER_PLAN.md).

- `run_probe.py`: kontrolowane scenariusze wykorzystujące istniejący Controller Lab i rzeczywiste serializery CANable. Nie implementuje alternatywnego algorytmu sterowania. Może odczytać lokalny live endpoint, nie zapisuje do roweru.
- `probe-results.json`: presety, parametry wykonania, bloby wysłane do produkcyjnego parsera, metryki, porównania, hashe CSV oraz obserwacja kompletności ustawień live. 34 przypadki, 31 CSV; trzy odrzucenia mode 4 są oczekiwanym wynikiem parsera.
- `*.csv`: pełne przebiegi co 5 ms, 10 s każdy. Okno statystyk 3–7,5 s. Referencja Iq w counts, nie amperach ani watach.
- `source-manifest.json`: SHA256 139 lokalnych plików C/H oraz źródeł generatora i powiązanych plików CANable; HEAD i status repo w chwili zapisu. Obejmuje zastany WIP. Hash Controller Lab obejmuje jego moduły, nie cały firmware; oba rodzaje identyfikacji są potrzebne.
- `quick-gate.log`: pierwotna bramka `tools/verify_all.py --quick`; PASS etapów do Level4, FAIL replay z WinError 193.
- `replay.log`: PASS powtórzenia samego replay po zachowaniu starego wygenerowanego ELF pod inną nazwą. 24000 wierszy, deterministycznie; zarejestrowanych real-ride przypadków 0.
- `decode.log`: PASS pozostałego etapu CANable FW145 raw-log → canonical → native replay. To test infrastruktury, nie potwierdzenie zgodności zmierzonych i odtworzonych Iq.

Wszystkie etapy quick zostały w ten sposób wykonane; nie ma nowego zbiorczego PASS pojedynczego uruchomienia bramki. Full/sanitizers/target/HW nie wykonano. Ostrzeżenia kompilatora są zachowane w logach. Nie zmieniono accepted baselines.

## Odtworzenie na tym stanowisku

Z katalogu `C:\Projekty\eVistDrive`, PowerShell:

```powershell
$env:CC='C:\Projekty\tools\w64devkit\bin\gcc.exe'
$env:PATH='C:\Projekty\tools\w64devkit\bin;'+$env:PATH
& 'C:\Users\mariuszg\AppData\Local\Programs\Python\Python312\python.exe' motor-controller-firmware/documentation/assist-pipeline-evidence/run_probe.py
```

Wymagane: Python 3.12, gcc z w64devkit, Node pod ścieżką zapisaną w runnerze, lokalne moduły `sim/controller_lab` i sąsiednie repo `canable-web`. Skrypt nadpisuje swoje wyniki w tym katalogu; do porównania po refaktorze zachować kopię dowodów przed uruchomieniem. `source-manifest.json` i logi bramki są osobnym snapshotem audytu i nie aktualizują się automatycznie razem z próbą.

Generator nie zamyka pętli PMSM/FOC: `u_abs` i pomiar prądu baterii pozostają zerowe. Stałe wejścia speed/cadence nie powstają z dynamiki roweru. Z tego względu wyniki dowodzą kompozycji żądania dla określonych wejść, a nie osiągów mechanicznych.
