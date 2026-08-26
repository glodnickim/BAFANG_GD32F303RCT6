# M820 reverse engineering — metodologia i odtwarzalność

**Stan:** 2026-08-20  
**Cel dokumentu:** oddzielić *jak doszliśmy do wniosków* od samych wniosków. Ten plik ma umożliwić ponowne zbudowanie bazy wiedzy z innego BIN-u, wersji firmware albo całkiem innego projektu MCU.

---

# 1. Filozofia pracy

Nie zaczynamy od nazywania zmiennych. Zaczynamy od **dowodów mechanicznych**:

```text
adres/rejestr -> producer -> transformacja -> RAM -> consumer -> efekt zewnętrzny
```

Dopiero gdy łańcuch jest spójny, nadajemy nazwę semantyczną.

Dla każdej tezy zapisujemy:

1. skąd pochodzi wejście,
2. gdzie jest zapisane,
3. jak jest matematycznie przetwarzane,
4. kto konsumuje wynik,
5. czy istnieje zewnętrzny cross-check (CAN, HMI, GPIO, oscyloskop, drugi firmware),
6. poziom pewności,
7. co mogłoby tezę obalić.

---

# 2. Narzędzia używane w tej analizie

Minimalny zestaw wystarczający bez Ghidry:

- `sha256sum` — identyfikacja dokładnego obrazu,
- `stat` / `ls` — rozmiar i provenance,
- `od` / `xxd` — nagłówek, wektory, literal pools,
- `strings` — wersje/identyfikatory/tablice ASCII,
- `clang --target=armv7m-none-eabi` — zbudowanie wrappera ELF,
- `ld.lld` — linkowanie surowego BIN-u pod odzyskany adres flash,
- `llvm-objdump` — Thumb/Thumb-2 disassembly,
- `grep`, `sed`, `awk` — targeted xrefs i wycinki,
- małe skrypty Python uruchamiane z shella — szukanie 32-bit literal addresses, dekodowanie little-endian, algebra/tablice,
- logi CAN — walidacja jednostek fizycznych i protokołu,
- drugi/open-source firmware (np. EBiCS) — **wyłącznie cross-check sprzętu**, nigdy jako dowód stocku,
- diagram PCB/pinout — walidacja fizycznego źródła sygnału.

Nie było konieczne OCR ani pełny decompiler C. Kluczowe algorytmy można odzyskać z dobrej analizy assemblera i przepływu danych.

---

# 3. Krok 1 — identyfikacja kontenera i vector table

## 3.1 Obejrzyj początek BIN-u

Przykład:

```sh
od -Ax -tx1 -N96 firmware.bin
```

W badanych obrazach pierwsze 32 bajty są nagłówkiem update. Po offset `0x20` pojawia się:

- pierwsze słowo wyglądające jak `0x2000....` — initial MSP w SRAM,
- kolejne słowo `0x0800.... | 1` — Reset_Handler z bitem Thumb.

To jest bardzo silny marker vector table Cortex-M.

## 3.2 Walidacja

Sprawdź:

```text
SP ∈ SRAM range
Reset_Handler ∈ Flash range
Reset_Handler & 1 == 1
```

Jeżeli te trzy warunki nie zachodzą, offset aplikacji albo architektura są prawdopodobnie błędne.

## 3.3 Ustal flash base

Dla M820/CR X30P 2.1 vector table po nagłówku odpowiada aplikacji linkowanej od:

```text
0x08005000
```

Nie zakładaj tego dla innego firmware. Nowszy obraz może mieć inny bootloader i inny APP base.

---

# 4. Krok 2 — wytnij nagłówek, zbuduj ELF i disassembly

## 4.1 Strip kontenera

```sh
dd if=firmware.bin of=app.bin bs=1 skip=32 status=none
```

## 4.2 Wrapper assembly

```asm
.syntax unified
.thumb
.section .text
.global _app_start
_app_start:
.incbin "app.bin"
```

## 4.3 Zbuduj object

```sh
clang --target=armv7m-none-eabi -c blob.S -o blob.o
```

## 4.4 Linkuj pod odzyskany APP base

```sh
ld.lld -e _app_start -Ttext=0x08005000 blob.o -o app.elf
```

## 4.5 Disassembly Thumb

```sh
llvm-objdump -d --triple=thumbv7m-none-eabi app.elf > app.dis
```

Targeted range:

```sh
llvm-objdump -d --triple=thumbv7m-none-eabi \
  --start-address=0x0800AF00 \
  --stop-address=0x0800B300 \
  app.elf
```

**Pułapka:** jeśli wymusisz złą architekturę albo wejdziesz w literal pool, zobaczysz legalnie wyglądające, ale nonsensowne instrukcje. Nie traktuj każdego `objdump` line jako kodu.

---

# 5. Krok 3 — znajdowanie peryferiów po adresach

Na Cortex-M/GD32/STM32 rejestry peryferiów są bardzo charakterystyczne. Przykładowe anchory użyte w M820:

```text
0x40012C00  TIM1-style advanced timer
0x40012400  ADC1-style
0x40012800  ADC2-style
```

Metoda:

1. znajdź literal z bazą peryferium,
2. zobacz offsets rejestrów,
3. porównaj sekwencję set/clear z dokumentacją/StdPeriph API,
4. nazwij funkcję dopiero po zachowaniu rejestrów.

To pozwoliło zidentyfikować m.in. helper togglujący BDTR.MOE oraz konfigurację ADC/TIM bez symboli.

---

# 6. Krok 4 — literal pools i xrefs bez decompilera

ARM Thumb często ładuje adres RAM tak:

```asm
ldr r1, [pc, #imm]   ; literal pool
ldrsh r0, [r1]
```

Sam `objdump` może zinterpretować słowo w literal pool jako instrukcję. Trzeba je ręcznie odczytać jako little-endian 32-bit.

Przykład skryptu:

```python
from pathlib import Path
b = Path("app.bin").read_bytes()
base = 0x08005000
addr = 0x0800EB28
v = int.from_bytes(b[addr-base:addr-base+4], "little")
print(hex(v))
```

Dla reverse xref do zmiennej RAM można szukać jej 4-bajtowego little-endian wzorca w całym BIN-ie:

```python
value = 0x200002CE
pat = value.to_bytes(4, "little")
```

Potem każdy hit literal pool prowadzi do funkcji, która czyta/zapisuje tę zmienną.

Ta metoda była kluczowa dla:

- temperatury `0x200002CE`,
- phase currents,
- temp-dependent overcurrent thresholds,
- start/stop state variables.

---

# 7. Krok 5 — budowa mapy RAM producer/consumer

Twórz tabelę:

| RAM | Producer | Transformacja | Consumer | Nazwa robocza |
|---|---|---|---|---|
| 0x200002CE | NTC LUT | 0.1°C | fault/telemetry/compensation | motor_temp_x10 |
| 0x2000035E | threshold updater | coeff*k>>8 | ISR abs(Iabc) compare | OC threshold 1 |
| ... | ... | ... | ... | ... |

**Reguła:** nazwa robocza ma być zmieniana, jeśli nowe xref ją obalają. Nie broń wcześniejszej interpretacji.

---

# 8. Krok 6 — algebra z assemblera

Assembler często daje dokładniejsze równanie niż decompiler.

Przykład phase current:

```asm
JDR -> shift
sum 16 samples
... later ...
offset - shifted_JDR
```

Rozpisz każdą operację symbolicznie, zanim podstawisz jednostki.

## 8.1 Worked example: phase-current offset

Kalibracja wykonuje:

```text
offset_internal += JDR >> 3
```

16 razy.

Normalna ścieżka używa równoważnie:

```text
I_internal = offset_internal - (JDR << 1)
```

Błędna interpretacja `JDR = ADC<<4` dawała:

```text
zero ~1024
Iinternal ~32*delta
```

Ale progi walidacji i niezależne EBiCS offsety ~2020 nie pasowały.

Po sprawdzeniu formatu **injected left-aligned signed result** właściwa relacja jest:

```text
JDR = ADC_raw << 3
```

więc:

```text
offset_internal = 16*ADC_zero
I_internal = 16*(ADC_zero-ADC_sample)
```

A stockowy zakres `0x7800..0x8800` daje:

```text
1920..2176 raw ADC
```

co idealnie zgadza się z hardware cross-check.

### Lekcja ogólna

Nigdy nie przenoś założenia o bit alignment między:

- regular DR,
- injected JDR,
- signed/unsigned mode,
- różnymi rodzinami ADC.

Najpierw sprawdź rejestr i format danych w konkretnej konfiguracji.

---

# 9. Krok 7 — identyfikacja algorytmu po zachowaniu, nie po nazwie

Przykład dynamic current sampling:

Nie trzeba znać nazwy funkcji. Wystarczy zobaczyć:

1. obliczane trzy PWM compare,
2. porównania luk między compare,
3. stałe `243`, `372`, `129`,
4. zapis do TIM1 CCR4,
5. zmianę polarity/edge CH4,
6. późniejszy flag `sample_valid`,
7. current reconstruction zależny od flagi.

To jednoznacznie tworzy model:

```text
SVPWM -> observable window -> CCR4 ADC trigger -> sample validity
```

---

# 10. Krok 8 — fizyczne jednostki odzyskuj przez zewnętrzny kontrakt

Najlepszy sposób na `ADC -> A/V` bez aparatury to znaleźć miejsce, gdzie firmware musi wystawić **znaną jednostkę**.

Worked example PA0:

1. stock przetwarza delta ADC,
2. wynik wchodzi do pola CAN `0x3201`,
3. protokół niezależnie potwierdza `current_A = raw/100`,
4. algebra zamyka gain:

```text
1 ADC = 39.215686 mA
```

To jest znacznie mocniejsze niż zgadywanie shunta.

Dla phase-current nadal brakuje analogicznego endpointu w amperach — dlatego 95/100 mA/count pozostaje poniżej poziomu PEWNE.

---

# 11. Krok 9 — cross-check z drugim firmware

Otwarty firmware jest dobrym **sanity check**, ale nie podstawowym dowodem.

Przykład EBiCS M820:

- ADC phase offsets ~`2012 / 2028 / 2020`,
- right-aligned ADC,
- te same fizyczne kanały phase current,
- `CAL_I≈95` jako empiryczna kalibracja.

To mocno wspiera wniosek stocku o zero około 2048, ale nie wolno z tego wywnioskować, że stock używa dokładnie tej samej stałej `CAL_I`.

### Dobra praktyka

```text
STOCK BIN = źródło prawdy o stockowym zachowaniu
OPEN SOURCE = cross-check / inspiracja / hipoteza
PCB = źródło prawdy o fizycznym połączeniu
LOG CAN = źródło prawdy o runtime/protokole
```

---

# 12. Krok 10 — porównanie dwóch stockowych wersji

Porównanie międzywersyjne jest bardzo wartościowe, bo pomaga rozdzielić:

- stałe sprzętowe,
- parametry produktu,
- przypadkowe wartości konfiguracji,
- refaktoryzację kodu.

Procedura:

1. hash obu plików,
2. znajdź vector table i APP base osobno,
3. znajdź te same peripheral anchors,
4. podpisz funkcje po zachowaniu, nie po adresie,
5. porównaj stałe algorytmiczne,
6. jeśli tylko jedna stała zmienia się wraz z nazwą wariantu produktu — kandydat na parametr modelowy,
7. jeśli logika i stałe krytyczne są identyczne — kandydat na hardware invariant.

Nowszy `CR X30P 5.0` jest w kolejce właśnie do takiego diffu.

---

# 13. Jak odtwarzać CAN/protokół

1. Znajdź funkcję budującą 29-bit ID / mailbox.
2. Znajdź stałe command/index.
3. Śledź RAM -> payload byte offsets.
4. Porównaj z prawdziwym logiem CAN.
5. Dopiero potem przypisz jednostkę.

Nigdy nie wnioskuj `A`, `V`, `Nm` wyłącznie z tego, że wartość liczbowo wygląda wiarygodnie.

---

# 14. Jak odtwarzać state machine

Dla START/STOP:

1. znajdź byte/enum stanu,
2. wszystkie write xrefs do niego,
3. dla każdego stanu wypisz:
   - wejścia/guard,
   - side effects,
   - timer/MOE/CCR,
   - reset integratorów,
   - następny stan,
4. zbuduj graf przejść,
5. porównaj ścieżkę normalną z fault path.

To ujawniło, że stock rozdziela graceful stop od awaryjnego hard stop.

---

# 15. Jak odtwarzać zabezpieczenia

Szukaj wzorca:

```text
abs()/range check
-> threshold
-> debounce/persistence counter
-> flag
-> state transition
-> MOE/CEN/gate action
```

Pojedyncze porównanie nie mówi jeszcze, czy to warning, limiter czy hard fault.

Dla overcurrent M820 dopiero znalezienie `>5 consecutive hits -> state9 -> MOE off` pozwoliło zaklasyfikować najwyższy próg jako twarde zabezpieczenie.

---

# 16. Evidence ledger — obowiązkowe przy długim reverse

Dla każdego ważnego wniosku zapisuj:

```text
CLAIM
SOURCE FILE + SHA256
CODE ADDRESSES
RAM ADDRESSES
FORMULA
EXTERNAL CROSS-CHECK
CONFIDENCE
FALSIFIER
DATE
```

Osobny plik `M820_REVERSE_EVIDENCE_LEDGER.md` realizuje tę zasadę.

---

# 17. Rejestr błędów własnej analizy

Reverse engineering bez tego szybko zamienia się w zbiór „prawd”, których nikt już nie umie zweryfikować.

W tym projekcie już wystąpiły trzy pouczające korekty:

1. `PAS 96 -> 64 edges/rev`,
2. `phase zero 1024 / x32 -> 2048 / x16`,
3. `700 = phase current max -> hipoteza odrzucona`.

Każda korekta ma zostać zachowana wraz z powodem. To jest wiedza równie cenna jak poprawny wynik.

---

# 18. Minimalny workflow dla nowego BIN-u

```text
[1] SHA256 + size
[2] hexdump first 128B
[3] locate vector table
[4] infer APP flash base
[5] strip container/header
[6] wrap .incbin -> ELF
[7] Thumb disassembly
[8] identify GPIO/ADC/TIM/CAN anchors
[9] map interrupt vectors
[10] map ADC init + DMA + injected triggers
[11] map RAM producers/consumers
[12] reconstruct control ISR
[13] reconstruct start/stop state machine
[14] reconstruct current/voltage/temp conversions
[15] reconstruct CAN telemetry/params
[16] correlate with runtime logs
[17] compare with second firmware / PCB
[18] update evidence ledger + confidence
```

---

# 19. Czego nie robić

- Nie nadawać nazw funkcjom po jednej stałej.
- Nie utożsamiać „podobnego open source” ze stockiem.
- Nie zgadywać U/V/W po kolejności pinów.
- Nie zakładać A/count z nominalnej mocy silnika.
- Nie mieszać current limit baterii z phase-current limit.
- Nie traktować fault threshold jako normalnego current limit.
- Nie kopiować timer counts do innego PWM frequency bez przeliczenia czasu.
- Nie kopiować NTC LUT do innego NTC/front-end.
- Nie uważać pojedynczego logu za pełny protokół.
- Nie kasować historycznych korekt analizy.

---

# 20. Co jest potrzebne, by przenieść metodę do innego projektu

Dla nowego MCU/firmware nie trzeba znać od razu całego schematu. Wystarczy:

- BIN/HEX/ELF,
- podstawowy typ CPU/architektury albo vector table,
- mapa peryferiów z datasheetu,
- możliwość zrobić disassembly,
- jeden lub dwa runtime logi / znane fizyczne odczyty,
- cierpliwa mapa producer-consumer.

Ta sama metoda działa dla:

- kontrolerów silnika,
- HMI,
- BMS,
- ładowarek,
- sterowników automotive/industrial,
- innych firmware Cortex-M.
