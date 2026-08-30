# QS-1R — reusable capture: stan implementacji

## Zmiana DIAG-only

QS-1R zachowuje próbkę QS-1 (48 × 44 B) i zgodny wstecz eksport `WRITE 0x6030`, DLC 0.
Dodane API recordera nie dotyka FOC, PI, MOE, FW117, ADC ani sterowania silnikiem.

- `READ 0x6031` (`85116031#`) zwraca `0x822A6031`, DLC 8: schema, stan
  `IDLE/ARMED/TRIGGERED/COMPLETE`, generation, progress, export-ready/busy, trigger events
  i trigger index.
- `WRITE 0x6031` (`85106031#`) przyjmuje wyłącznie COMPLETE poza eksportem. Zleca ISR nowy
  capture; nie niszczy aktywnego capture ani eksportowanego bufora.
- Generation ma wartość 1 przy power-on i zwiększa się po skutecznym NOWY POMIAR. Jest
  eksportowany w istniejącym bajcie `capture_id` nagłówka `0x80010250`, zachowując jego
  dotychczasowe znaczenie dla pierwszego capture.

Reset ringu wykonuje ISR na początku następnego ticku. Parser CAN ustawia tylko pojedynczy
`rearm_pending`, dlatego ISR nie może zobaczyć częściowo wyzerowanego ringu. Status jest
bez skutków ubocznych.

## Test lokalny

`qs_transition_diag_host` przeszedł po zmianie: sprawdza auto-arm, COMPLETE/status,
generation 1→2, ISR-owned re-arm, brak przecieku starej próbki i BUSY podczas TRIGGERED.

## Status dostarczenia

Firmware QS-1R został zbudowany kanonicznie z czystego, odłączonego worktree commitu
`7232f9d58c1b5f33f5d424675a5e7a5e3555a71a` (brak zmian lokalnych):

- NORMAL: eVD `0.0412`, SHA-256
  `8F88ABFE8C39A2F71BA40D6045213D7313D02C4CEE2222AB327703C0ECBB063B`,
  FLASH 104252 B, RAM 12576 B.
- DIAG: eVD `0.0413`, SHA-256
  `7687F636D935346914C6DDD3AF7DCA81994CA1963C07718FCB0AD9252F2AD308`,
  FLASH 153492 B, RAM 48496 B.

DIAG ma ring 48 × 44 B. Względem udokumentowanego QS-1 (48536 B) jego zużycie RAM jest
mniejsze o 40 B. Linker rezerwuje niezależnie 1024 B heapu i 2048 B stosu. W NORMAL kod
recordera kompiluje się do stubów; mapa nie zawiera alokowanego symbolu ringu QS.

`qs_transition_diag_host` oraz pełny `tests/host/run-host-tests.ps1` zakończyły się PASS.
Nie zmieniono plików FW-130A ani sterowania FOC/PI/MOE/FW-117.

**FIRMWARE SIDE: PASS / USER WORKFLOW: PENDING CANABLE.**

Panel Canable jest obowiązkowy przed testem HW: repozytorium
`C:\Projekty\bafang_canable_pro` nie zostało w tej karcie zmienione. Do czasu jego
aktualizacji i walidacji eksportu 48/48 użytkownik nie wykonuje sesji START/STOP/RESTART.
