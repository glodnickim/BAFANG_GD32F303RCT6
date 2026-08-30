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

Panel Canable jest obowiązkową częścią karty, ale jego repozytorium
`C:\Projekty\bafang_canable_pro` leży poza zapisywalnym zakresem bieżącej sesji. Nie został
zmieniony. Nie wykonano też canonical build, ponieważ worktree firmware zawiera niezależne,
niestaged zmiany FW-130A w `main.c`, `config.h` i innych plikach; build w tym stanie nie byłby
artefaktem wyłącznie QS-1R.

**B. PARTIAL — zaimplementowano bezpieczny control-plane DIAG, ale brak panelu Canable i
czystego canonical buildu blokuje deklarację gotowości do HW.**

Nie wolno jeszcze kierować użytkownika do sesji START/STOP/RESTART, dopóki panel nie wykona
automatycznej walidacji 48/48 oraz nie zostanie zbudowany czysty artefakt DIAG.
