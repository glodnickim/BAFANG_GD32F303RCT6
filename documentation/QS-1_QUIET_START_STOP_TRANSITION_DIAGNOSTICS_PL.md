# QS-1 — diagnostyka przejść quiet START/STOP

## Zakres i kontrakt

QS-1 dodaje wyłącznie DIAG-only recorder oraz kontrolowany eksport. Nie zmienia MOE, CCR,
FW-117, Hall/theta, ADC, Iq/Id, PI, limiterów ani ramp. Jedyna zmiana poza diagnostyką to
fallback wersji HMI: nie udaje wydania produkcyjnego.

## Audyt i architektura

FW-117 (70×48 B) i rolling no-assist (256×48 B) zapisują z 250 Hz w pętli kontrolnej; pozostają
bez zmian jako długi kontekst, lecz nie są cykl-po-cyklu FOC. QS-1 ma osobny, jeden-writer ring
w `ADC0_1_IRQHandler`: lokalny readonly input po zastosowaniu CCR, stały zapis do RAM, bez CAN,
formatowania, alokacji ani blokad. Reader działa tylko po COMPLETE.

Próbka ma **44 B**: `foc_cycle` u32; sześć bezstratnych prądów i32 (`Iq_requested`,
`Iq_allowed`, `Iq_ref`, `Iq_measured`, `Id_ref`, `Id_measured`); theta Q31 high-word i16; trzy
CCR u16; flagi u16; lifecycle/Hall/sample-state/event u8. `angle_static_legal` jest wyłącznie
obserwacją Hall 1..6 + istniejącej mapy, nie nową bramą sterowania.

48×44 = 2112 B; limit pełnego obiektu recordera 2240 B jest `_Static_assert`. Pretrigger:
12 próbek/0,75 ms; posttrigger: 36/2,25 ms; pełne okno 3 ms @16 kHz. ~100 ms nie mieści się
bezpiecznie w pozostałym DIAG RAM — wykorzystujemy krótkie pełne tempo oraz istniejący FW-117.

Zdarzenia: RUN_RISE, MOE_RISE, FIRST_IQ_REF, IQ_TARGET_ZERO, IQ_REF_ZERO, MOE_FALL,
FAULT_HARD_OFF. Pierwsze uruchamia postwindow; COMPLETE zamraża writer do rearm/init.
Szacowany koszt: stałe load/store i modulo, bez wywołań TX; brak sprzętowego licznika ISR,
konserwatywnie <1% okresu 62,5 µs.

## Eksport

Po COMPLETE wyślij `WRITE 0x6030`, source=5, target=2, DLC=0. ACK=1 przyjmuje zrzut. Pacing,
mailbox ownership i pauza podczas aktywnej sesji są takie jak w istniejącym dumpie. Na próbkę:
nagłówek `0x00010250` (schema=1, capture/index/trigger-index, 44 B, 6 fragmentów) oraz raw
fragmenty 8 B `0x00010251..56`. CAN nie jest wysyłany z ISR.

## HMI version

Canonical builder generuje `build_version.h` przed `inc/`; `CAN 0x6001` serializuje
`eVD %s`, `EBICS_BUILD_VERSION`. Fallback `0.0408` zmieniono na `DEV-NONCANONICAL`, więc IDE
nie identyfikuje się jako stare wydanie. Static test sprawdza macro, użycie 0x6001 i fallback.

## Testy, regresja i build

`qs_transition_diag_host`: wrap/chronologia, trigger, COMPLETE, freeze, rearm, START, STOP,
fault (T1–T9). `qs_hmi_version_static_host`: T-V1–T-V3. NORMAL link dowodzi T10 (bez ringa);
const input i brak wskaźników sterowania dowodzi T11. FW-117 niezmieniony.

Pełna regresja bazowa: wszystkie pakiety poza znanym `rolling_no_assist_diag_host: 514 FAILURES`.
Nie był on dotykany.

| wariant | wersja | SHA-256 | FLASH | RAM | HMI |
|---|---:|---|---:|---:|---|
| NORMAL | 0.0458 | `5DA0AF4FDC8AED65CE53B4D0DD8BF5052818BF11808A09ECBD82FFDB4619553F` | 105500 | 12616 | eVD 0.0458 |
| DIAG | 0.0459 | `C7D6C8A2A028F0E085A69DF5F964D234C337E563D837E61FE7DD527AC75C6369` | 154236 | 48536 (616 wolne) | eVD 0.0459 |

Toolchain: GCC 13.2.1; 79 + startup sources. DIAG linkowy przyrost QS-1: 2176 B; NORMAL: 0 B.

## Hardware capture (DIAG 0.0459)

1. Cold start: postój/bridge inactive → łagodny pedał.
2. Normal release: małe/średnie assist → zwolnij moment.
3. Restart: run → zero około 100 ms → run; powtórz 300 ms i 1 s.
4. Po każdym cyklu odczytaj 0x6030; wyznacz request/MOE/first-Iq/CCR/theta i MOE off/on.

Nie zmieniaj algorytmu, nie stosuj G532 56,4 ms ani ARMED_ZERO. QS-2 może później rozważyć
coherent PREPARE; osobny theta seed nie jest wymagany, ponieważ źródło live theta istnieje.

**A. PASS — QS-1 COMPLETE; TRANSITION DIAGNOSTICS READY FOR HW CAPTURE.**

CONTROL BEHAVIOR CHANGED: NO. FW-117: NO. MOE: NO. Iq ramp: NO. Fallback 0.0408: MADE NON-RELEASE.
