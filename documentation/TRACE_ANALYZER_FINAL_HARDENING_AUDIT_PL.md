# Trace Analyzer — final hardening audit

Data audytu: 2026-08-24. Zakres: Trace Analyzer, jego testy i dekodery. Kod firmware, sterowania silnikiem i format rejestratora nie został zmieniony.

> Uwaga historyczna: tabela braków w sekcji 5 opisuje schema v1 w chwili tego
> audytu. Karta `FW-122_ROLLING_NO_ASSIST_DIAGNOSTIC_PL.md` następnie naprawiła
> mismatch jako jawne schema v2 (44 B, HEADER + 6 DATA), zachowując decoder v1.
> W finalnym v2 `debug_flags`, MOE, current-cal i rotor evidence są już na wire.

## Wynik

- CLASS C porównuje `abs(iq_actual_raw)` z progiem wyznaczonym z `abs(final_iq)`. Surowy znak Iq pozostaje w raporcie, obok jawnie wyliczonego magnitude.
- Krytyczny CLASS D wymaga teraz potwierdzenia, że odpowiedź silnika była oczekiwana. Samo kręcenie korbą, moment i `assist_level > 0` nie wystarczają.
- Trwałość D jest liczona w kolejnych próbkach z rzeczywistego okresu próbkowania i musi objąć co najmniej jeden obrót korby przy zmierzonej kadencji.
- Dawny event `877.084 s` nie istnieje po hardeningu. Był artefaktem stałej zwłoki 500 ms, a nie udowodnioną anomalią trwającą 151 ms.
- Dekoder FW-122/RNA jest sprawdzany również zamrożonymi bajtami znanej struktury C, a nie wyłącznie round-tripem dwóch zgodnych modeli.

## 1. CLASS C i znak `MS.i_q`

Firmware opisuje i implementuje konwencję Parka: podczas prawidłowej jazdy do przodu `MS.i_q` jest ujemne, a `MS.i_q_setpoint` dodatnie. Zarówno firmware (`src/rolling_no_assist_diag.c`), jak i analizator porównują magnitude.

| `final_iq` | `PWM_ON` | `iq_actual_raw` | magnitude | wynik |
|---:|---:|---:|---:|---|
| 500 | 1 | -500 | 500 | NO C |
| 500 | 1 | -20 | 20 | C — `CURRENT_RESPONSE` |

Test C sprawdza dodatkowo, że evidence zawiera jednocześnie `iq_actual_raw=-20` i `iq_actual_magnitude=20`. Analizator nie gubi więc znaku diagnostycznego.

## 2. Warunki powstania CLASS D

Krytyczny D może powstać tylko przy komplecie dowodów:

1. rider input jest aktywny;
2. `assist_level` istnieje i jest większy od 0;
3. istnieje agregat `assist_permission=true` albo bit 7 `permission_bits`;
4. żaden dostępny inhibit nie jest aktywny: kierunek, hamulec, hard/safety cut, torque fault, blokada FOC przez kalibrację;
5. semantyka limitu prędkości potwierdza brak ograniczenia;
6. ograniczenia napięciowe i termiczne są potwierdzone jako nieaktywne;
7. brak odpowiedzi utrzymuje się przez wymaganą liczbę kolejnych próbek.

Brak dowodu z punktów 2–6 daje `UNKNOWN — INSUFFICIENT DATA`, confidence `LOW`, bez critical D. Jawny inhibit nie tworzy zdarzenia D.

### Rzeczywista semantyka speed limit

`assist_limits_apply()` nie odcina napędu dokładnie na `speed_limit`. Dla potwierdzonego pedałowania:

- do `speed_limit` żądanie pozostaje oczekiwane;
- pomiędzy `speed_limit` i `speed_limit + 2 km/h` trwa legalny taper;
- na końcu tego pasma żądanie może legalnie spaść do zera.

Dlatego próbka z prędkością w taperze lub powyżej jego końca nie może potwierdzić D. Implementacja wynika bezpośrednio z `src/assist_limits.c:21-28`; bit `FW112_PERM_SPEED_LIMIT_OK` potwierdza tylko warunek `speed < speed_limit` (`src/ride_control.c:953`). Agregat `ASSIST_PERMISSION` nie obejmuje downstream limitów napięcia, temperatury i prędkości, dlatego sam bit 7 nie wystarcza (`src/ride_control.c:955-959`).

## 3. Persistence D a sampling rate

Nie ma już stałej „500 ms grace”. Dla każdej ciągłej sekwencji analizator wyznacza:

```text
sample_period_ms = mediana rzeczywistych odstępów próbek
crank_revolution_ms = 60000 / mediana cadence_rpm
required_samples = ceil(crank_revolution_ms / sample_period_ms) + 1
```

D wymaga równocześnie co najmniej `required_samples` kolejnych próbek oraz rzeczywistego span co najmniej jednego obrotu. Raport zdarzenia podaje `confirmation_samples`, `required_confirmation_samples`, sampling rate, kadencję i wymagany czas obrotu.

Fixture D przy 10 Hz zawiera 11 kolejnych próbek. Dla mediany 70 rpm jeden obrót trwa około 857 ms, więc potrzeba 10 próbek obejmujących ten okres. Osobny test ucina sekwencję do 5 próbek i potwierdza NO FALSE POSITIVE dla krótkiego rolling re-enable.

## 4. Ponowna analiza okna 877.084 s

Log: `Logi z jazdy FT/log-w3-2026-03-20-17-14-49-1555520.csv`. Rzeczywisty okres próbkowania: mediana 50 ms (20 Hz).

| faza | timestamp ms | assist | torque mV | cadence rpm | speed km/h | current A | power W |
|---|---:|---:|---:|---:|---:|---:|---:|
| przed | 876326 | 3 | 744 | 0 | 16.24 | 0.00 | 0.00 |
| przed | 876482 | 3 | 744 | 38 | 16.24 | 0.00 | 0.00 |
| przed | 876537 | 3 | 744 | 48 | 16.24 | 0.00 | 0.00 |
| low response start | 876580 | 3 | 912 | 62 | 16.24 | 0.00 | 0.00 |
| w trakcie | 876669 | 3 | 858 | 71 | 16.24 | 0.23 | 9.41 |
| w trakcie | 876831 | 3 | 905 | 74 | 16.24 | 0.23 | 9.38 |
| w trakcie | 876934 | 3 | 1213 | 74 | 15.23 | 0.23 | 9.38 |
| w trakcie | 877035 | 3 | 1901 | 75 | 15.23 | 0.23 | 9.38 |
| dawny event | 877084 | 3 | 1654 | 76 | 15.23 | 0.23 | 9.38 |
| w trakcie | 877165 | 3 | 1405 | 76 | 15.23 | 0.27 | 11.02 |
| w trakcie | 877185 | 3 | 988 | 76 | 15.23 | 0.47 | 19.18 |
| odpowiedź | 877236 | 3 | 862 | 76 | 15.23 | 1.37 | 55.76 |
| po | 877296 | 3 | 937 | 76 | 15.23 | 2.70 | 109.35 |
| po | 877387 | 3 | 1373 | 78 | 15.23 | 4.74 | 191.02 |

Stary detektor rozpoczął D dopiero po stałych 500 ms: `876580 + 500 ms ≈ 877080`, dlatego pierwszą próbką eventu było `877084`. Jego „151 ms” to tylko `877185 - 877084 + 50 ms` — ogon dłuższego przejścia, nie czas fizycznej usterki.

Cała ciągła sekwencja niskiej odpowiedzi ma 13 próbek i span 605 ms. Mediana kadencji wynosi 74 rpm, czyli jeden obrót trwa około 811 ms. Przy 20 Hz potrzeba co najmniej 18 próbek. Sekwencja nie spełnia persistence. Od pierwszego ponownego odczytu kadencji (`876482`) do wyraźnej odpowiedzi (`877236`) mija 754 ms, nadal mniej niż jeden obrót przy tej kadencji.

Sąsiednie krótkie sekwencje niskiej odpowiedzi mają m.in. 100–112 ms około `827.738–828.252 s`, 185 ms około `880.518 s` oraz 220 ms około `899.704 s`. Okno 876.580 s jest dłuższe od nich, ale samo to nie dowodzi awarii. Co ważniejsze, historyczny CSV nie zawiera kierunku, permission/gate, speed limit, hamulca/hard-cut, progu undervoltage ani stanu kalibracji. Nie da się potwierdzić, że w każdej z tych próbek odpowiedź silnika była wymagana.

Wniosek: twierdzenie „151 ms jest anomalią” było niepoprawne. Po hardeningu event `877.084 s` nie występuje: `0 events`, `0 critical`.

## 5. FW-122 / rolling-no-assist: RAM, wire, CSV, Analyzer

Legenda: `TAK*` oznacza wartość tylko wejściową albo istniejącą poza zamrożonym sample; nie jest odzyskiwalna z dumpu.

| SIGNAL | IN RAM | ON WIRE | IN CSV | ANALYZER |
|---|---|---|---|---|
| permission | TAK, wejście; spakowane do flags | TAK, `flags bit 4` | TAK | TAK, używane |
| reason_bits | TAK*, `ride_diag_reason`, poza RNA sample | NIE | NIE | TAK dla innych adapterów; brak z FW-122 |
| iq_before_pu | TAK, sample | TAK, data1[2..3] | TAK | TAK, używane |
| final_iq | TAK jako `iq_setpoint` | TAK, data1[0..1] | TAK | TAK, używane |
| PWM_ON | TAK, spakowane do flags | TAK, `flags bit 7` | TAK | TAK, używane |
| hardware MOE | TAK*, wejście `rna_in.moe`, niekopiowane | NIE | NIE | TAK, jeśli dostarczone; brak z FW-122 |
| bridge_lifecycle | TAK, sample | TAK, data0[6] | TAK | TAK, evidence/C triage |
| hall_state | TAK, sample | TAK, data0[7] | TAK | TAK, evidence/C triage |
| angle_hall | TAK*, global `q31_rotorposition_hall` | NIE | NIE | TAK, jeśli dostarczone; brak z FW-122 |
| angle_absolute | TAK*, global `q31_rotorposition_absolute` | NIE | NIE | TAK, jeśli dostarczone; brak z FW-122 |
| rotor_direction | TAK*, global `i8_recent_rotor_direction` | NIE | NIE | TAK, jeśli dostarczone; brak z FW-122 |
| ERPS | TAK, sample | TAK, data2[2..3] | TAK | TAK, evidence/C triage |
| iq_actual_raw | TAK, sample signed | TAK, data1[4..5] signed BE | TAK | TAK, signed evidence i porównanie magnitude |
| iq_actual_magnitude | DERIVED w triggerze, nie w sample | NIE jako osobne pole; wyliczalne z raw | TAK, wyliczone | TAK, wyliczone |
| PI_q | TAK jako `pi_q_int` | TAK, data1[6..7] | TAK | TAK, evidence/C triage |
| PI_d | TAK jako `pi_d_int` | TAK, data2[0..1] | TAK | TAK, evidence/C triage |
| current_cal_foc_allowed | TAK*, wynik funkcji w motor path; nie w RNA | NIE | NIE | TAK dla D, jeśli dostarczone; brak z FW-122 |
| debug_flags | TAK, sample byte 24 | **NIE — serializer wysyła tylko sample bytes 0..23** | NIE | TAK, jeśli dostarczone; brak z FW-122 |

Najważniejszy wykryty wtedy brak: schema v1 miała 28 B w RAM i zapisywała `debug_flags` na offset 24, lecz serializer wysyłał tylko trzy 8-bajtowe fragmenty danych, czyli offsety 0..23. Czwartą ramką był osobny header. Ten historyczny wynik doprowadził do schema v2 opisanej w karcie FW-122; nie opisuje już finalnego wire format.

## 6. Niezależna weryfikacja dekodera

Dotychczasowy C-host wykonywał encoder↔decoder round-trip tego samego layoutu. Hardening dodaje dwa niezależne kroki:

1. C-host tworzy znany `rolling_no_assist_sample_t` (`tick_abs=0xDEADBEEF`, `iq_setpoint=350`, `iq_before_pu=340`, `iq_actual=-345`, `pi_q=-12000`, `pi_d=500`, `erps=42`, `rpm=80`, threshold=150) i porównuje wynik serializacji z literalnymi bajtami trzech ramek.
2. Zamrożony fixture `tests/traces/adapters/rolling_c_oracle_sample.log` z tymi bajtami jest dekodowany przez `tools/decode_rolling_no_assist.ps1`; test Trace Analyzera porównuje wszystkie pola z wartościami struktury C.

W ten sposób zgodny błąd po obu stronach round-tripu nie daje fałszywego PASS.

### Granice testerów przed kompilacją

- `tests/host/rolling_no_assist_diag_host.c` jest właścicielem dowodu C-struct → literalne wire bytes oraz zachowania realnego modułu C.
- `tests/trace_analyzer/test_trace_analyzer.py` jest właścicielem wire bytes → PowerShell decoder → znormalizowane pola i klasy analizatora.
- `--run-regressions tests/traces` pilnuje stabilnych wyników fixture’ów, nie wykonuje ani nie modeluje firmware.

Nie dodano nowego konkurencyjnego runnera i nie włączono analizatora do kompilacji firmware. Jedyny kontrolowany overlap to granica tych warstw: te same zamrożone bajty są wyjściem oracle C i wejściem dekodera. To celowa integracja, nie duplikacja encoder↔encoder.

## 7. Weryfikacja końcowa

Wyniki pełnego uruchomienia są zapisywane w `.build/trace-analyzer/` i podsumowane w raporcie końcowym zadania.
