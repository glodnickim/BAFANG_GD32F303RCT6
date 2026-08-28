# FW-128B1 — deterministyczny takt próbki prądu baterii

**Wynik: PASS.** **NORMAL 0.0452 · DIAG 0.0453 · Sprzęt NIEWYMAGANY**
**Wejście:** FW-128B0 `5352327` · PAS `3263c4e` **nietknięty**

---

## 0. Najprościej

Prąd baterii był mierzony poprawnie, ale **filtrowany w złym rytmie**. Próbki z przetwornika
przychodziły sprzętowo co 250 µs, a filtr ruszał raz na przebieg pętli głównej — więc każdy
pominięty przebieg **wyrzucał całą próbkę**. Teraz filtr rusza raz na próbkę, w przerwaniu.
**Ani jednej liczby w samym filtrze nie zmieniono** — zmienił się tylko jego zegar.

---

## 1. Graf PRZED / PO

```
PRZED:  TIMER1 CH1 ──sprzęt──► skan ADC ──DMA──► adc_value[0]
                                                     │
                        pętla główna, kiedy zdąży ────┴──► IIR ──► MS.Battery_Current
                        ▲ tu ginęły próbki i tu rozciągała się stała czasowa

PO:     TIMER1 CH1 ──sprzęt──► skan ADC ──DMA──► adc_value[0]
                                                     │
        TIMER1 UPDATE ISR ───────────────────────────┴──► IIR ──► published
                                                                     │
                        pętla główna KONSUMUJE ───────────────────────┴──► ×CAL_BAT_I ──► MS.Battery_Current
```

## 2. Dowód czasu (krok 1 karty)

Zweryfikowane z HEAD: SYSCLK = AHB = APB2 = **120 MHz**, ADC = APB2/6 = **20 MHz**,
APB1 = 60 MHz → zegar TIMER1 = 120 MHz; prescaler 2 (÷3), ARR 9999 (÷10000) = **4000 Hz**.

| zdarzenie | czas od przepełnienia TIMER1 |
|---|---|
| TIMER1 UPDATE (to przerwanie) | **0 µs** |
| TIMER1 CH1 = 2000/10000 → wyzwolenie skanu | **50 µs** |
| **rank 0 = PA0 zapisany przez DMA** (239,5 + 12,5 = 252 cykli = 12,6 µs) | **62,6 µs** |
| koniec skanu 9 kanałów (9 × 12,6 = 113,4 µs) | **163,4 µs** |
| następny UPDATE | 250 µs |

**Zakłócenie od grupy inserted:** wyzwalana TIMER0 CH3 przy 16 kHz (okres 62,5 µs), więc w oknie
113,4 µs mieszczą się **najwyżej 2** wtrącenia. Każde to 55,5 + 12,5 = 68 cykli = 3,4 µs plus
restart przerwanej konwersji regularnej (≤ 12,6 µs) ≈ **16 µs**. Najgorszy przypadek:
skan kończy się o **195,4 µs** — margines **54,6 µs**.

**Dla rank 0 margines jest znacznie większy:** nawet jeśli wtrącenie trafi w pierwszą konwersję,
PA0 jest zapisane o **78,6 µs**, czyli **171 µs przed** następnym UPDATE.

> **Odpowiedź na pytanie z kroku 1: TAK.** Przy przerwaniu UPDATE `adc_value[0]` na pewno należy
> do skanu wyzwolonego 200 µs wcześniej. Rank 0 jest **pierwszym** transferem skanu, więc
> pytanie nie wymaga nawet dowodu na cały skan.

**Odpowiedniość 1:1 jest strukturalna, nie szczęśliwa:** jedno wyzwolenie CH1 na okres TIMER1,
jedno przerwanie UPDATE na okres TIMER1.

## 3. Wybrany właściciel: **MODEL B (TIMER1 UPDATE)** — i dlaczego

| | MODEL A (DMA transfer-complete) | **MODEL B (TIMER1 UPDATE)** |
|---|---|---|
| semantyka | „komplet próbek gotowy" — dokładna | „nowy okres, poprzedni skan dawno gotowy" — **dowiedziona §2** |
| nowe przerwanie | **TAK** (DMA0_Channel0) + priorytet + interakcja z FOC | **NIE** — ISR już istnieje i już próbkuje PAS |
| opóźnienie dla PA0 | zdarzenie na **końcu** skanu (163–195 µs) | wartość odczytana natychmiast, rank 0 gotowy od 62,6 µs |
| dodatkowy koszt IRQ | +1 wektor przy 4 kHz | **0** |

**Wybrano B**, bo: (1) margines czasowy jest o rząd wielkości większy od potrzebnego, (2) nie
dokłada ani jednego przerwania do systemu z ciasnym budżetem FOC, (3) trzyma **jeden** szybki tor
4 kHz dla obu sygnałów (PAS i prąd baterii), zgodnie z precedensem `3263c4e`.

**Świeżość jest weryfikowana, nie zakładana.** ISR czyta licznik DMA
(`DMA_CHCNT(DMA0, DMA_CH0)`); w trybie kołowym przeładowuje się do 9 po zakończeniu rundy, więc
**9 = poprzedni skan zakończony**. Wartość jest przekazywana jako **dowód, nie jako zezwolenie** —
rank 0 jest ważny w obu przypadkach, a odchylenia są liczone (`late_scan_count`).

> Odczyt przez makro rejestru, nie `dma_transfer_number_get()`: biblioteczny akcesor to
> wywołanie funkcji, które najpierw woła helper walidujący peryferium i kanał. W przerwaniu
> 4 kHz po jeden rejestr o stałym adresie to jest **jedno `ldr`**.

## 4. Równanie — **niezmienione co do znaku**

```c
acc -= acc >> 6;   acc += (int32_t)raw - offset;   published = acc >> 6;
```

Test **B1-1** przepuszcza 3000 próbek o celowo niewygodnym przebiegu przez **stare wyrażenie
produkcyjne i nowy moduł równolegle**: **0 rozbieżności**. Wzmocnienie stałoprądowe = **1**
(punkt stały 64·x). Zaokrąglenie: `>>6` zaokrągla w dół, zaniżając o < 1 zliczenie — ta sama
sub-zliczeniowa tendencja co przedtem.

**Częstotliwość próbkowania: dokładnie 4 kHz (250 µs).** Biegun dyskretny **63/64** →
stała czasowa `−1/ln(63/64) = 63,5` próbki = **15,9 ms**. Zmierzone: **63 % po 64 próbkach
(16,00 ms), 99 % po 290 próbkach (72,5 ms)**.

## 5. Cykl życia rozruchu — **wariant A karty**

`battery_current_init()` stoi **przed** `timer1_config()` (przerwanie startuje w następnej linii)
i zostawia moduł **NIEUZBROJONY**: żadna próbka nie jest przyjmowana, publikowana wartość to 0.
`battery_current_set_offset(bat_current_offset)` **uzbraja** go zaraz po kalibracji zera —
niezależnie od tego, czy pomiar wpadł w okno ±200, czy zadziałał fallback, bo w obu wypadkach
to jest offset na całą jazdę.

**Wybrano A, nie B** („zresetuj po fakcie"), bo stan w złej domenie wtedy **w ogóle nie
powstaje**, zamiast powstawać i wymagać korekty. Kalibracja startowa czyta `adc_value[0]`
bezpośrednio i modułu nie potrzebuje, więc nic na tym nie tracimy. Akumulator jest zerowany przed
uzbrojeniem: offset zmierzono przy ~zerowym prądzie, więc **0 jest poprawnym stanem
oczekiwanym**, a każdy inny byłby zgadywaniem gasnącym przez 64 próbki.
Wszyscy konsumenci `MS.Battery_Current` (w tym `soc_init()`) startują po tym punkcie.

## 6. Publikacja i spójność

`acc` jest **prywatne** dla producenta i nigdy nie czytane na zewnątrz. Przez granicę przechodzi
**jedno wyrównane słowo 32-bitowe** `published` — zapis/odczyt jest na tym rdzeniu atomowy
(single-copy), więc konsument nie może zobaczyć połowy aktualizacji. **Żadnego muteksu, żadnego
blokowania** — to zresztą przywróciłoby dokładnie tę zależność, którą karta usuwa.

Mnożenie przez `CAL_BAT_I` (**jedyny float w całym torze**) zostało w `main.c`, świadomie **poza
przerwaniem**.

## 7. Budżet ISR (krok 11)

Z obrazu 0.0452, profil **debug = -O0** (tak jak się wysyła):

| | instrukcje |
|---|---|
| dodane w `TIMER1_IRQHandler` (odczyt `adc_value[0]`, licznik DMA, porównanie, wywołanie) | **11** |
| `battery_current_sample()`, ścieżka uzbrojona | **~51** |
| **razem dodane** | **~62 ≈ 0,52 µs = 0,21 % CPU @ 4 kHz** |
| cały ISR TIMER1 (PAS + bateria + obsługa) | **~125 ≈ 1,05 µs = 0,42 % CPU** |

**Priorytety:** `TIMER1_IRQn` i `ADC0_1_IRQn` (FOC) oba (0,0) przy `NVIC_PRIGROUP_PRE1_SUB3`
(1 bit wywłaszczenia) → **nie wywłaszczają się nawzajem**. TIMER1 może opóźnić ISR FOC najwyżej
o własny czas: **1,05 µs wobec okresu 62,5 µs = 1,7 %**. Okno próbkowania FW-127 ma zapas o rząd
wielkości większy. **Jitter FOC nie rośnie istotnie.**

## 8. Testy hosta

`tests/host/fw128b1_battery_timebase_host.c` linkuje **prawdziwy** `src/battery_current.c`.

| test | wynik |
|---|---|
| **B1-1** zgodność ze starym wyrażeniem | **0 rozbieżności na 3000 próbkach** |
| B1-1b wzmocnienie stałoprądowe | dokładnie 1 |
| B1-2 jedna próbka = jedna aktualizacja | 500/500 |
| **B1-3 niezależność od pętli** (co 1/2/5/10/37/nieregularnie) | **filtr 36 i 4000 aktualizacji — IDENTYCZNIE we wszystkich sześciu** |
| B1-4 odpowiedź skokowa | 63 % po 64 próbkach (16,00 ms), 99 % po 290 (72,5 ms) |
| B1-5 znak (ładowanie/rekuperacja) | −100 → +100 przez zero |
| B1-6 rozruch | nic przed uzbrojeniem, deterministyczne zero po |
| B1-7 zaokrąglenie | ±1 zliczenie symetrycznie |
| B1-8 liczniki | próbki == aktualizacje |
| B1-9 zagłodzona pętla (1 s bez konsumenta) | wynik **identyczny** |
| B1-10 niekompletny skan | policzony, próbka **użyta** |
| B1-11 PAS / B1-12 FW-127 | strażniki źródeł — bez zmian |

**ALL CHECKS PASSED.** Baseline bez zmian (`T14`/`T9` w `rolling_no_assist`) → **0 nowych
regresji**.

> **Dwie pułapki z przebiegu.** (1) Strażnik „dokładnie jedno wywołanie" liczył też **komentarz**
> wspominający funkcję — dodany filtr komentarzy (ta sama lekcja co w FW-128A, inne lekarstwo).
> (2) Asercje A1–A3 w teście FW-128B0 wskazywały, **gdzie** filtr mieszka; B1 go przeniósł, więc
> test poprawnie się wysypał. Równanie jest to samo, więc asercje **zostały** — zmienił się tylko
> ich adres na `src/battery_current.c`.

## 9. Krok 12 — SOC: **OSOBNA WADA, NIE NAPRAWIANA**

Sprawdzone na HEAD: **nie ma** `soc_timer_ticks_total` ani kompensacji upływem ticków.
`main.c:2760` nadal liczy `soc_mAs_acc += MS.Battery_Current / 4000.0f`, a `main.c:2761` uznaje
4000 **wykonań** za sekundę. **SOC TIMEBASE = SEPARATE DEFECT.** Przeniesienie filtru ani tego nie
naprawia, ani nie pogarsza — SOC dostaje teraz *lepszą* wartość prądu, ale całkuje ją nadal
w zmiennym rytmie. Zgodnie z kartą **nie rozszerzam zakresu**.

## 10. Krok 13 — pozostałe liczniki zależne od pętli (wejście do osobnego audytu)

`torque_counter`, `tq_fault_ticks`, `uint16_half_rotation_counter`, `pwm_cutoff_tick`,
`ui16_erps_counter`, `slow_loop_counter`, `t3100_counter`, `soc_tick_counter` — **nietykane**.

## 11. Kroki 6 i 7 — co jest zamrożone

- **Prawo filtru:** współczynnik 1/64 i wzmocnienie 1 — **niezmienione** (test B1-1).
- **`CAL_BAT_I` = 37,0 — NIEZMIENIONE.** FW-128B0 sklasyfikował jego pochodzenie jako
  historyczne/przybliżone, a porównanie ze stockiem jako **PARTIAL**.
  **BEZWZGLĘDNA PEWNOŚĆ SKALI PRĄDU BATERII = PARTIAL** (nadal ~5,65 % niepewności,
  kierunek niebezpieczny — patrz FW-128B0 §F).
  **CYFROWY TAKT PRĄDU BATERII = naprawiony w tej karcie.**
  Kalibracji i czasu **nie miesza się**.

## 12. Build

| | NORMAL 0.0452 | DIAG 0.0453 |
|---|---|---|
| FLASH | 103 448 B (43,92 %) | 149 904 B (63,65 %) |
| RAM | 12 568 B (25,57 %) | 46 344 B (**94,29 %**) |
| SHA256 | `4F83EDEF…E922A573` | `70800058…AE89E1D3` |

Delta wobec 0.0448/0.0449: FLASH **+316 B** / **+316 B**, RAM **+24 B** obu (akumulator, offset,
publikowana wartość, flaga, cztery liczniki). Wolne w DIAG: **2808 B**.

## 13. Sprzęt

**NIEWYMAGANY.** Walidacja sprzętowa pozostaje **skonsolidowana** z kadencją PAS, zachowaniem
prądu baterii, limiterem FW-128B i start/stop.

## 14. Kontrakt przekazania do FW-128B

Od tej karty limiter może zakładać, że `MS.Battery_Current`:

- ma **deterministyczny takt świeżej próbki** — dokładnie **4 kHz**, właściciel udowodniony;
- ma **znaną transmitancję** — IIR 1/64, biegun 63/64, τ = 15,9 ms, wzmocnienie 1;
- jest **publikowany spójnie** (jedno atomowe słowo);
- ma **semantykę ze znakiem** (dodatni = rozładowanie, ujemny = ładowanie/rekuperacja);
- ma **skalę bezwzględną PARTIAL** — to nie zmienia architektury limitera, bo obie strony
  porównania są w tej samej domenie mA.

> **FW-128B jest odblokowane również dla wariantu ze stanem (osobny PI prądu baterii)**, bo `Ki`
> ma teraz określony czas próbkowania. Wybór prawa sterowania i stałych należy do FW-128B.
> **Ta karta limitera nie implementuje.**
