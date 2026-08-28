# INDEX — router, not a manual

Jeśli pracujesz nad konkretnym tematem, przeczytaj TYLKO dokumenty wskazane niżej — nie
czytaj całej reszty `documentation/`. Cel: minimalny kontekst potrzebny do zadania, zwykle
1-4 krótkie dokumenty. Jeśli okaże się, że potrzebujesz więcej niż wskazano, to sygnał, że
ten routing jest niepełny — dopisz brakującą pozycję zamiast czytać wszystko "na wszelki
wypadek".

Każdy dokument niżej ma stały szablon: PURPOSE / INPUTS / OUTPUTS / STATE / TIMEBASE /
INVARIANTS / TEST SEAMS / RELATED SOURCE FILES / KNOWN ISSUES.

Historia decyzji (karty FW-XXX, CHANGELOG, starsze audyty) NIE jest tu routowana — to inny
poziom kontekstu, patrz `documentation/README.md` ("Przewodnik po dokumentacji"), które
pozostaje źródłem prawdy dla tematów historycznych/kart FW.

## Routing

| Zadanie | Czytaj |
|---|---|
| **PAS** (dekodowanie, cadence, kierunek, re-engagement gate) | [inputs/PAS.md](inputs/PAS.md) + [architecture/TIMEBASES.md](architecture/TIMEBASES.md) |
| **torque filtering / RUN estimator** | [inputs/TORQUE_SENSOR.md](inputs/TORQUE_SENSOR.md) |
| **high cadence power** (czy zachowanie zmienia się z cadence) | [inputs/TORQUE_SENSOR.md](inputs/TORQUE_SENSOR.md) + [assist/POWER_MODE.md](assist/POWER_MODE.md) + [assist/LIMITS.md](assist/LIMITS.md) + [architecture/TIMEBASES.md](architecture/TIMEBASES.md) jeśli problem dotyczy timingu |
| **re-engagement** (cofnięcie pedałowania, powrót assist) | [inputs/PAS.md](inputs/PAS.md) + [assist/START_REARM_RELEASE.md](assist/START_REARM_RELEASE.md) |
| **SOC / bateria** | [inputs/BATTERY.md](inputs/BATTERY.md) |
| **throttle** | [inputs/THROTTLE.md](inputs/THROTTLE.md) + [assist/RIDER_INPUT.md](assist/RIDER_INPUT.md) (sekcja "throttle floor") |
| **brake / hard cut** | [inputs/BRAKE.md](inputs/BRAKE.md) + [assist/ASSIST_DYNAMICS.md](assist/ASSIST_DYNAMICS.md) (sekcja "hard cut ramp") |
| **speed sensor / limit prędkości** | [inputs/SPEED_SENSOR.md](inputs/SPEED_SENSOR.md) + [assist/LIMITS.md](assist/LIMITS.md) |
| **limiter mocy / prądu / napięcia / temperatury** | [assist/LIMITS.md](assist/LIMITS.md) |
| **ride latch / gear preload / rampy Iq** | [assist/ASSIST_DYNAMICS.md](assist/ASSIST_DYNAMICS.md) + [assist/START_REARM_RELEASE.md](assist/START_REARM_RELEASE.md) |
| **finalny Iq/Id do silnika** | [motor/MOTOR_COMMAND.md](motor/MOTOR_COMMAND.md) |
| **FOC / Hall / PWM** | NIE opisane tu jeszcze — poza zakresem tej karty (test infrastructure foundation kończy się na `motor_command_t`, patrz [motor/MOTOR_COMMAND.md](motor/MOTOR_COMMAND.md) "KNOWN ISSUES"). Do czasu napisania osobnej karty czytaj bezpośrednio `src/FOC.c` i `src/main.c` (ISR sekcja) plus `documentation/ARCHITECTURE_AUDIT_MOTOR_AGNOSTIC_PL.md` sekcja 3/4. |
| **rolling no-assist diagnostic** (brak wspomagania podczas jazdy, CASE A/B/C, CAN dump → CSV) | [FW-122_ROLLING_NO_ASSIST_DIAGNOSTIC_PL.md](FW-122_ROLLING_NO_ASSIST_DIAGNOSTIC_PL.md) |
| **kalibracja prądu fazowego** (offsety ADC, sector-dependent Iq error, CASE C) | [FW-125_PHASE_CURRENT_SAME_PATH_CALIBRATION_PL.md](FW-125_PHASE_CURRENT_SAME_PATH_CALIBRATION_PL.md) + [FW-119_CURRENT_CALIBRATION_SAFETY_PL.md](FW-119_CURRENT_CALIBRATION_SAFETY_PL.md) |
| **okno próbkowania ADC / zbocze TIMER0_CH3 / droga do FW-127** | [FW-127_STATUS.md](FW-127_STATUS.md) (zacznij TU — stan bieżący) → [FW-128B1_BATTERY_CURRENT_TIMEBASE_PL.md](FW-128B1_BATTERY_CURRENT_TIMEBASE_PL.md) (bieżąca karta) → [FW-128B0_BATTERY_CURRENT_SCALE_PL.md](FW-128B0_BATTERY_CURRENT_SCALE_PL.md) (bieżąca karta) → [PRE-FW128_PAS_CADENCE_TIMEBASE_PL.md](PRE-FW128_PAS_CADENCE_TIMEBASE_PL.md) (bieżąca karta) → [FW-128C0_PHYSICAL_CURRENT_SCALE_PL.md](FW-128C0_PHYSICAL_CURRENT_SCALE_PL.md) (bieżąca karta) → [FW-128B_CONTRACT_BATTERY_LIMITER_PL.md](FW-128B_CONTRACT_BATTERY_LIMITER_PL.md) (kontrakt, NIE wdrożone) → [FW-128A_CANONICAL_CURRENT_CONTROL_OWNERSHIP_PL.md](FW-128A_CANONICAL_CURRENT_CONTROL_OWNERSHIP_PL.md) → [FW-128_PRE_AUDIT_CURRENT_CONTROL_PL.md](FW-128_PRE_AUDIT_CURRENT_CONTROL_PL.md) → [FW-127_ACQUISITION_REWRITE_PL.md](FW-127_ACQUISITION_REWRITE_PL.md) → [FW-127_PRE_AUDIT_SAMPLE_CONTEXT_COHERENCY_PL.md](FW-127_PRE_AUDIT_SAMPLE_CONTEXT_COHERENCY_PL.md) → [FW-126.7_PRODUCTION_CURRENT_CAL_REPLACEMENT_PL.md](FW-126.7_PRODUCTION_CURRENT_CAL_REPLACEMENT_PL.md) → [FW-126.6_CURRENT_SENSE_VALID_STATE_PL.md](FW-126.6_CURRENT_SENSE_VALID_STATE_PL.md) → [FW-126.5_RAW_DOMAIN_BRIDGE_STATE_PL.md](FW-126.5_RAW_DOMAIN_BRIDGE_STATE_PL.md) → [FW-126_TO_FW-127_AGENT_HANDOFF_PL.md](FW-126_TO_FW-127_AGENT_HANDOFF_PL.md) + [FW-126_HW_TEST_PROCEDURE_PL.md](FW-126_HW_TEST_PROCEDURE_PL.md) |
| **jednostki toru wspomagania** (kg → tryb → moc → Iq, kalibracja czujnika vs odczucie jazdy, dlaczego 320% nie dawało 320%) | [FW-129B_STATE_HYGIENE_REPORT_PL.md](FW-129B_STATE_HYGIENE_REPORT_PL.md) (zacznij TU — stan bieżący) → [FW-129_IMPLEMENTATION_REPORT_PL.md](FW-129_IMPLEMENTATION_REPORT_PL.md) (tabela BEFORE/AFTER, co otwarte) → [FW-129_UNIT_DOMAIN_AUDIT_PL.md](FW-129_UNIT_DOMAIN_AUDIT_PL.md) (audyt: 12 wad D1–D12 z wyprowadzeniami) |
| **filtry mocy nie działają / kto jest właścicielem Iq / co czyści `ride_control_init()`** | [FW-129B_STATE_HYGIENE_REPORT_PL.md](FW-129B_STATE_HYGIENE_REPORT_PL.md) (graf właścicielstwa Iq, tabela stanu runtime, dlaczego `power_rise/fall_filter_ms` są NIEAKTYWNE) |
| **ASSIST TORQUE FULL SCALE / CRANK LENGTH** (dwa globalne ustawienia odczucia jazdy, tuning blob v8) | [FW-129_IMPLEMENTATION_REPORT_PL.md](FW-129_IMPLEMENTATION_REPORT_PL.md) sekcja 3 |
| **przenikanie startowe launch/normal** (dlaczego prąd nie zależy od kadencji, po co `U_LAUNCH`) | [FW-129_IMPLEMENTATION_REPORT_PL.md](FW-129_IMPLEMENTATION_REPORT_PL.md) sekcja 1.1–1.2 + `src/assist_modes.c` komentarz blokowy na górze |
| **diagnostics / CAN recorder / efid / session / budget** | [FW-112-DIAG_WHOLECHAIN_RECORDER_PL.md](FW-112-DIAG_WHOLECHAIN_RECORDER_PL.md) + [FW-119_CURRENT_CALIBRATION_SAFETY_PL.md](FW-119_CURRENT_CALIBRATION_SAFETY_PL.md) |
| **pisanie/uruchamianie testów regresyjnych** | [testing/TEST_ARCHITECTURE.md](testing/TEST_ARCHITECTURE.md) + [testing/TEST_INTERFACES.md](testing/TEST_INTERFACES.md) |
| **dodawanie nowego scenariusza regresji** | [testing/REGRESSION_SCENARIOS.md](testing/REGRESSION_SCENARIOS.md) + [testing/TEST_INTERFACES.md](testing/TEST_INTERFACES.md) |
| **planowanie nowej karty zmian** (jakie moduły/testy dotknie) | [testing/CHANGE_CARD_TEMPLATE.md](testing/CHANGE_CARD_TEMPLATE.md) |

## Co NIE jest tu routowane (świadomie)

- Pełna architektura repo / gotowość motor-agnostyczna → `ARCHITECTURE_AUDIT_MOTOR_AGNOSTIC_PL.md`
  (duży dokument-parasol, czytaj tylko gdy zadanie faktycznie dotyczy reorganizacji, nie
  pojedynczego modułu).
- Wynik konkretnego uruchomienia testów → `TEST_INFRASTRUCTURE_FOUNDATION_REPORT_PL.md`
  (raport z jednego przebiegu prac, nie kontrakt modułu — nie routuj tu przyszłych zadań).
- Historia kart FW-XXX i CHANGELOG → `README.md` w tym katalogu.
