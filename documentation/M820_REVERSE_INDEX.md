# M820 reverse engineering — indeks dokumentacji

**Stan:** 2026-08-20

Dokumentacja została celowo rozdzielona, żeby nie mieszać faktów z metodą ich odzyskania.

## 1. Wiedza / odzyskane algorytmy

`M820_REVERSE_KNOWLEDGE_BASE.md`

Zawiera:

- architekturę ADC,
- current calibration,
- current sampling i reconstruction,
- start/stop,
- overcurrent,
- rotor UVW,
- PAS,
- BEMF/phase voltage,
- battery current/voltage,
- temperature,
- rekomendacje do EVistDrive,
- historyczne korekty.

## 2. Metodologia / jak to odtworzyć

`M820_REVERSE_METHOD_REPRODUCIBILITY.md`

Zawiera:

- użyte narzędzia,
- identyfikację kontenera/vector table,
- tworzenie ELF z surowego BIN-u,
- Thumb disassembly,
- literal pools,
- producer/consumer RAM maps,
- algebra z assemblera,
- odzyskiwanie fizycznych jednostek przez CAN,
- cross-check z drugim firmware,
- pułapki i workflow dla nowego projektu.

## 3. Evidence ledger

`M820_REVERSE_EVIDENCE_LEDGER.md`

Dla każdej ważnej tezy:

- evidence,
- równanie/zachowanie,
- poziom pewności,
- falsifier,
- rejestr błędnych/odrzuconych wcześniejszych wniosków.

## 4. Otwarte pytania i porting

`M820_OPEN_QUESTIONS_AND_EVISTDRIVE_PORTING.md`

Zawiera:

- listę brakujących dowodów,
- priorytety dalszego reverse,
- plan implementacyjny P0/P1/P2,
- rzeczy przenośne i hardware-specific.

## 5. Bootstrap script

`M820_REVERSE_BOOTSTRAP.sh`

Minimalny szablon do:

```text
BIN -> strip header -> .incbin wrapper -> ELF -> Thumb disassembly -> SHA256/metadata
```

Używać po ustaleniu właściwego `HEADER_SIZE` i `APP_BASE` dla konkretnego obrazu.
