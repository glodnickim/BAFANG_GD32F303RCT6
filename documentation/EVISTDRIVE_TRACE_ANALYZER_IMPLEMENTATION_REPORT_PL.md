# EVISTDRIVE TRACE ANALYZER — IMPLEMENTATION REPORT

## 1. EXISTING LOG FORMATS

Zinwentaryzowano eksport jazdy aplikacji (CSV ze średnikiem), znormalizowane
CSV symulatora hostowego, surowe ramki diagnostyczne FW-112 DIAG, FW-112 A/B,
FW-117 oraz czteroramkowy zapis ROLLING_NO_ASSIST. Wejście JSON obsługuje listę
próbek i obiekt z polem `samples`.

| Format | Charakter | Istniejący parser / sposób wejścia |
|---|---|---|
| eksport aplikacji jazdy | telemetria okresowa, zwykle około 20 Hz | wbudowany adapter CSV z aliasami nagłówków |
| host pipeline / regression CSV | próbki symulacji z tickiem 4 kHz | ten sam adapter CSV; analizator tylko konsumuje wynik |
| FW-112 whole-chain schema 1–3 | event recorder, header + 4 fragmenty | `decode_fw112_diag.ps1` |
| FW-112 whole-chain schema 4 / C0-PROOF | event recorder, header + 3 fragmenty | wersjowa gałąź tego samego dekodera |
| FW-112 A/B, w tym 112.3 | milestone/episode recorder | `decode_fw112_ab.ps1` |
| FW-112.1 / 112.2 | semantyczne warianty sesji/rearm w powyższych recorderach | te same dekodery, nie osobny wire format |
| FW-117 | próbki lifecycle mostka/FOC | `decode_fw117_trace.ps1` |
| ROLLING_NO_ASSIST schema v1 | 250 Hz, HEADER + 3 DATA; tylko 24 B sample było na wire | `decode_rolling_no_assist.ps1`, jawny legacy dispatch |
| ROLLING_NO_ASSIST schema v2 | 250 Hz, HEADER + 6 DATA, logical sample 44 B | ten sam decoder, ścisła walidacja metadata 6/44/250/8 |
| FW-111/rearm/PAS i legacy episode | event/PAS diagnostics | zinwentaryzowane istniejące dekodery; brak automatycznego adaptera, bo nie niosą pełnego łańcucha Iq |

## 2. EXISTING PARSERS REUSED

Analizator wywołuje istniejące:

- `tools/decode_fw112_diag.ps1`,
- `tools/decode_fw112_ab.ps1`,
- `tools/decode_fw117_trace.ps1`,
- `tools/decode_rolling_no_assist.ps1`.

Pierwsze trzy dostały wyłącznie opcjonalny `-OutputJson`. Tryb dotychczasowego
raportu pozostaje domyślny. Decoder ROLLING_NO_ASSIST pozostaje jedynym miejscem
znającym jego wire format. Wybiera v1/v2 tylko po schema byte, waliduje v2
metadata, składa fragmenty po EFID i dekoduje liczby big-endian ze znakiem.

## 3. CONTROL PIPELINE MODEL

Model odwzorowuje kolejność:

`RIDER_ACTIVE → PERMISSION → LATCHED → MODE_DEMAND → IQ_BEFORE_PU → IQ_REQUEST → IQ_AFTER_LATCH → IQ_PRE_RAMP → FINAL_IQ → BRIDGE_ON → CURRENT_RESPONSE → POWER_RESPONSE`.

Mapowanie oparto o `rider_input`/`torque_input`/PAS, `ride_session`,
`pedal_assist_gate`, `ride_control`, `assist_modes`, lifecycle mostka w `main.c`
oraz pętlę FOC/prąd rzeczywisty.

| Etap | Rzeczywisty plik / funkcja lub zmienna |
|---|---|
| rider torque/load | `src/torque_input.c::torque_input_update` |
| cadence / PAS direction | `src/rider_input.c::rider_input_update`, `src/pas_direction.c` i liveness PAS |
| permission / gate / latch / rearm | `src/pedal_assist_gate.c::pedal_assist_gate_update`, `src/ride_session.c::ride_session_update`, `src/ride_control.c::ride_control_update` |
| mode demand i Iq request | `src/assist_modes.c::assist_modes_calculate` |
| P/U ceiling | `src/assist_modes.c::finish_power_request`, `assist_mode_output.iq_before_pu/iq_request` |
| after latch/floor i pre-ramp | `src/ride_control.c::ride_control_update`, snapshot `iq_after_latch_floor/iq_pre_ramp` |
| final Iq / bridge request | `src/main.c`, `MS.i_q_setpoint` i warunek startu mostka |
| PWM / MOE / lifecycle | `src/main.c`, `ui_8_PWM_ON_Flag`, `bridge_lifecycle` |
| FOC / actual current | pętla FOC w `src/main.c`, `MS.i_q` i `PI_iq` |
| electrical response | telemetria `Battery_Current`, napięcie i moc; bez telemetrii wyłącznie CURRENT RESPONSE |

## 4. AVAILABLE SIGNALS

Warstwa aliasów normalizuje czas, moment/load, kadencję/kierunek, permission i
latch, wszystkie obserwowalne etapy Iq, PWM/MOE/bridge state, Hall, `iq_actual`,
integratory PI oraz telemetrię napięcia, prądu i mocy. Raport zawsze podaje
faktycznie dostępny zestaw.

| SIGNAL | available? / format | Pole wejściowe → kanoniczne | Jednostka | Sampling/event | Źródło firmware |
|---|---|---|---|---|---|
| torque/load | app CSV, FW-112 A/B, FW-112 DIAG, host CSV | `Torque (mV)`/`TorqueMv`/`Load` → `torque`/`load` | mV lub centikg | okresowo / event | `torque_input` oraz snapshot `load_centikg` |
| filtered assist delta / RUN | FW-112 A/B schema 2+ | `AfiltNative`/`ArunNative` → `torque_filtered`/`torque_run` | native | milestone | `torque_input` filtry fast/RUN |
| cadence | wszystkie recordery i app CSV | `Cad`, `Cadence`, `rpm` → `cadence` | rpm | okresowo/event | `rider_input_get()->rpm`, `MS.cadence` |
| PAS direction / fwd_run | FW-112 DIAG/A-B | `Dir`, `FwdRun` | stan / kroki | event | `pas_direction`, `rider_input` |
| permission / bits | FW-112 schema 4, RNA | `Permission`, `PermBits` → `permission`, `permission_bits` | bool/bitmask | event / 250 Hz | `ride_control_get_permission_bits()`; gate bit 0 klasyfikuje A1, aggregate bit 7 pozostaje dowodem |
| latch/session/recovery | FW-112 DIAG/A-B | `Latched`, `SesSt`, `Recovery` | stan | event/milestone | `ride_session`, `ride_control` |
| start/required steps, hold, threshold | FW-112 A/B; część także DIAG/RNA | `StartSteps`, `RequiredSteps`, `AssistHold`, `Thr` | kroki/ticki/centikg | milestone/event | snapshot `ride_control` |
| mode demand / iq_request | FW-112 DIAG/A-B, host CSV | `IqReq` → `iq_request` | firmware Iq | event/milestone | `assist_mode_output.iq_request` |
| iq_before_pu | FW-112 schema 4, RNA | `IqBeforePu` → `iq_before_pu` | firmware Iq | event / 250 Hz | `assist_mode_output.iq_before_pu` |
| iq_after_latch_floor | FW-112 A/B | `IqAfterLatch` → `iq_after_latch` | firmware Iq | milestone | `ride_control` arm snapshot |
| iq_pre_ramp | FW-112 DIAG/A-B/FW-117 | `IqPre` → `iq_pre_ramp` | firmware Iq | event/sample | `ride_control` snapshot |
| final iq_setpoint | FW-112, FW-117, RNA, host CSV | `IqSet`/`iq_setpoint` → `final_iq` | firmware Iq | event/sample | `MS.i_q_setpoint` |
| iq_actual | FW-112 DIAG, FW-117, RNA | `IqAct`/`IqMeas` → `iq_actual` | firmware Iq, signed | event/sample | `MS.i_q` |
| PWM/MOE/lifecycle | schema 1–3, A/B, FW-117, RNA v2; **PWM brak w DIAG schema 4** | `pwm_on`, `moe`, `bridge_lifecycle` | bool/stan | event/sample | software flag, real `TIMER_CCHP.POEN`, lifecycle |
| Hall / angles / direction / PI q,d | FW-117 i RNA v2 | `hall_state`, `angle_hall`, `angle_absolute`, `rotor_direction`, `PiQ`, `PiD` | stan/raw16/native | sample | zsynchronizowany rotor/FOC snapshot |
| reason_bits | FW-112 DIAG i RNA v2 | `Rbits` → `reason_bits` | bitmask/nazwy | event/sample | `FW112_REASON_*` z `ride_control` |
| flags2, FINAL_ZERO, PU_CLAMPED | FW-112 DIAG schema 4 | `Flags2` → `flags2` | bitmask | event | `ride_control_get_flags2()` + uzupełnienie w `main.c` |
| debug_flags | host CSV i RNA v2; brak tylko w legacy RNA v1 wire | `debug_flags` | bitmask | sample | `ride_control_get_debug_flags()` |
| motor_voltage_utilization | FW-112 DIAG schema 4 / host bench | `MVU` → `motor_voltage_utilization` | 0..2048 | event/sample | `MS.u_abs` |
| voltage/current/electrical power | app CSV | `Voltage (V)`, `Current (A)`, `Power (W)` | V/A/W | telemetria okresowa | telemetria aplikacji / `MS.Battery_Current` |

## 5. MISSING SIGNALS

Braki nie są imputowane. Przy aktywnym riderze i braku downstream powstaje
`UNKNOWN / INSUFFICIENT DATA`. Dla klasy C raport dodatkowo wypisuje, których
sygnałów (`MOE`, Hall, lifecycle, PI, stan kalibracji) nie da się rozróżnić.

## 6. DETERMINISTIC CLASSES

Zaimplementowano A0, A1, A2, A3, A4, B, C i D. Każdy event ma klasę, nazwę,
czas, długość, pierwszy etap odchylenia, reason bits, dowody, confidence, score
i prawdopodobny obszar kodu. Krótkie fazy poprawnej rampy mają osobne czasy
tolerancji i nie są błędem.

## 7. FIRST DIVERGENCE ALGORITHM

Dla każdej próbki przechodzona jest kolejność pipeline. Zgłaszany jest pierwszy
znany etap, którego warunek przestał zachodzić. Kolejne skutki tej samej awarii
nie zastępują wcześniejszej przyczyny. Sąsiednie próbki tej samej klasy są
grupowane w jeden event.

## 8. LEARNING / GOOD BASELINE

Profil przyjmuje tylko jawnie oznaczone `GOOD` i wymaga pełnych metadanych.
Przechowuje źródło, ścieżkę, SHA-256 i cechy: opóźnienia etapów, relacje wartości
oraz znane przejścia stanów. Agregat można odtworzyć z listy źródeł.

## 9. ANOMALY DETECTION

Najpierw działa wiedza deterministyczna. Dopiero potem profil GOOD wykrywa
opóźnienia ponad zakres p99, relacje wartości poza znanym zakresem i nowe
przejścia stanów. Zdarzenie adaptacyjne przy tym samym czasie i etapie jest
tłumione, jeśli istnieje już diagnoza deterministyczna.

## 10. VERSION ISOLATION

Kluczem profilu jest skrót z firmware version, Git commit, motor type, config
hash i logger schema version. Brak dokładnego dopasowania wyłącza porównanie
adaptacyjne i zapisuje tę decyzję w raporcie.

## 11. CLI

`tools/trace_analyzer.py` obsługuje analizę, `--good/--bad`, `--learn-good`,
`--compare-good`, usuwanie źródła, rebuild profilu i `--run-regressions`.
`tools/run_trace_analyzer.ps1` znajduje Python 3.10+ bez zaszywania konta
użytkownika.

## 12. REPORT OUTPUT

`report.txt` prowadzi od czasu i typu eventu przez `FIRST DIVERGENCE`, klasę,
reason bits, score i dowody do obszaru kodu. Kończy się jawną adnotacją
`TRACE ANALYSIS (not firmware replay)`.

## 13. JSON OUTPUT

`analysis.json` ma `schema_version: 1`, hash wejścia, adapter, metadane,
dostępne/brakujące sygnały, segmenty, eventy i podsumowanie. Nazwy pól są stałe
i pokryte regresjami.

## 14. PLOTS

Dla eventu powstaje samodzielny SVG z dostępnymi sygnałami w oknie -500…+1000
ms i pionowym oznaczeniem pierwszego odchylenia. Nie dodano zależności Python.

## 15. TESTS

`tests/trace_analyzer/test_trace_analyzer.py` realizuje T1–T10 oraz testy obu
wersji FW-112, adaptera RNA, fałszywej detekcji ID w CSV, offsetu torque mV,
latency, segmentacji, porównania, artefaktów, odwracalności profilu i klasy D.
Final hardening rozszerzył zestaw do 34 testów: jawna konwencja signed Iq dla C,
warunki expectation i cadence-scaled persistence dla D, krótki rolling transient
bez false positive oraz zamrożony C-wire oracle dekodera. Biblioteka
`tests/traces` zawiera 6 przypadków regresyjnych. Wynik: 34/34 testów,
6/6 fixture’ów i niezależny `tests/host/run-host-tests.ps1` PASS.

## 16. HISTORICAL LOG RESULTS

Uruchomiono na:

`C:\Projekty\EBICS\Logi z jazdy FT\log-w3-2026-03-20-17-14-49-1555520.csv`

Pierwsza wersja wskazywała event przy 877,084 s. Final hardening wykazał, że jego
151 ms było wyłącznie ogonem liczonym po stałej zwłoce 500 ms. Pełna sekwencja
miała 13 próbek/605 ms; przy medianie 74 rpm wymagany jeden obrót trwa około
811 ms i wymaga 18 próbek przy 20 Hz. Log nie zawiera też permission, kierunku,
speed limit, inhibitów i progów limitera napięcia. Po poprawce wynik wynosi
`0 events`, `0 critical`; szczegółowe próbki i wire audit są w
`TRACE_ANALYZER_FINAL_HARDENING_AUDIT_PL.md`.

Artefakty uruchomienia są w `.build/trace-analyzer/historical-2026-03-20/`.
Firmware nie został zmodyfikowany na podstawie tego wyniku.

## 17. FILES ADDED/MODIFIED

Dodano CLI, bibliotekę `tools/trace_analyzer_lib`, wrapper PowerShell,
dokumentację, katalog profili, testy i fixture’y. Zmieniono cztery istniejące
dekodery tylko w zakresie adaptera maszynowego/poprawności dekodowania. Nie
zmieniono plików firmware, list źródeł, skryptów build ani istniejących runnerów.

## 18. LIMITATIONS

To analiza zapisanych obserwacji, nie wykonanie firmware. Próg momentu w
eksportach mV jest wyznaczany z danego przejazdu. Rzadka telemetria ogranicza
precyzję czasu. Profil wymaga reprezentatywnych, ręcznie potwierdzonych GOOD;
nie uczy się sam z BAD ani UNKNOWN. Wynik D mówi o braku odpowiedzi mocy, lecz
bez sygnałów wewnętrznych nie wskazuje wcześniejszego etapu.

## 19. NEXT DATA FIELDS REQUIRED

Największą wartość diagnostyczną dadzą równocześnie zapisane: `rider_active`,
`permission` i reason bits, `mode_demand`, `iq_request`, `iq_after_latch`,
`iq_pre_ramp`, `final_iq`, `PWM_ON`, `MOE`, bridge lifecycle, Hall,
`iq_actual`, PI q/d, stan kalibracji prądu oraz identyfikatory firmware/commit/
config/logger. Pozwoli to zamienić ogólną klasę D z eksportu aplikacji na
jednoznaczne A1–C.

## Brak nakładania z testerami przed kompilacją

`run-host-tests.ps1` weryfikuje zachowanie kodu C, `run_regression.ps1`
generuje/porównuje symulowane ślady, a `run_high_cadence.ps1` mierzy osobny
scenariusz wydajności. Nowy analizator tylko czyta gotowy zapis. Nie został
wywołany z żadnego z tych skryptów ani ze ścieżki build, a ich wyniki może
opcjonalnie konsumować bez generowania drugiego golden verdictu.
