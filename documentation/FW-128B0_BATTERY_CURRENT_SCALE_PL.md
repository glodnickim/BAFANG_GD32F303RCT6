# FW-128B0 — dowód skali i własności prądu baterii

**Wynik: PASS**, ale z jednym **warunkiem wstępnym** dla FW-128B i jednym **odchyleniem
w stronę niebezpieczną**.
**Tryb:** audyt statyczny + matematyka + obliczenie hostowe. **Zero zmian w sterowaniu.**
**Wejście:** PRE-FW128 `3263c4e` (PAS **zamrożony**, nietykany)

---

## 0. Najprościej

Prąd baterii **jest** mierzony w miliamperach i **da się** na nim oprzeć limiter — nieznana skala
prądu fazowego `K` z FW-128C0 **nie jest potrzebna**. Ale audyt znalazł dwie rzeczy:

1. **Stała przeliczająca (37) nie ma żadnego uzasadnienia.** Zmieniano ją cztery razy w pół roku
   („21,3 → 22,5 → 28,0 → 37,0"), zawsze przez autora upstreamu, zawsze z komentarzem w rodzaju
   „poprawka kalibracji". Jeśli poprawna jest liczba wyprowadzona ze stocku (39,216), to firmware
   **zaniża** prąd o 5,65 %, a przy ustawionym limicie 15 A płynie naprawdę **15,9 A**.
2. **Ten sam błąd czasu, co w PAS.** Próbki z przetwornika przychodzą sprzętowo 4 kHz, ale filtr
   przetwarza je **raz na przebieg pętli głównej**. Zmierzone: stała czasowa filtru rośnie
   z 15,5 ms do 77,5 ms, gdy pętla chodzi co 5 ticków.

---

## A. Graf: ADC → `MS.Battery_Current`

| etap | zmienna | typ | jednostka | producent | konsument | tempo | filtr | reset/kalibracja |
|---|---|---|---|---|---|---|---|---|
| pin | **PA0** | analog | A | bocznik/hall płytki | ADC0 | — | — | — |
| ADC | ADC0 **regular rank 0**, `ADC_CHANNEL_0`, próbkowanie **239,5** cykli | u12 | LSB | wyzwalacz **TIMER1 CH1** | DMA0 CH0 | **4 kHz sprzętowo** | — | `adc_calibration_enable` przy starcie |
| DMA | `adc_value[0]` | `uint16_t` | LSB | DMA0 CH0, **circular**, 9 transferów | main | **4 kHz, niezależnie od main** | — | — |
| zero | `bat_current_offset` | `int32_t` | LSB | rozruch, 64 próbki | odejmowanie | raz na boot | średnia z 64 | okno `CAL_BAT_I_OFFSET ± 200` |
| delta | `adc_value[0] − bat_current_offset` | `int32_t` | LSB | `reg_ADC_processing()` | akumulator | **raz na przebieg pętli** | — | — |
| filtr | `battery_current_cumulated` | `int32_t` | LSB × 64 | `reg_ADC_processing()` | konwersja | j.w. | IIR `>>6` | — |
| wynik | **`MS.Battery_Current`** | `int32_t` | **mA** | `main.c:2347` | limiter, SOC, HMI, EBICS | j.w. | — | — |

**ADC0 regular jest wyzwalane sprzętowo (TIMER1 CH1, wartość porównania 2000 przy okresie 9999)
i zapisywane przez DMA w trybie kołowym.** Skan 9 kanałów × (239,5 + 12,5) cykli przy zegarze ADC
20 MHz = **113,4 µs**, czyli mieści się w okresie 250 µs.

## B. Dokładne równanie

```c
raw_delta      = adc_value[0] − bat_current_offset;                    // LSB, ze znakiem
cumulated     -= cumulated >> 6;   cumulated += raw_delta;             // IIR, punkt stały 64×
MS.Battery_Current = (int32_t)((float)(cumulated >> 6) * CAL_BAT_I);   // mA
```

**`CAL_BAT_I` jest w miliamperach na zliczenie ADC.** Zweryfikowane hostowo: 100 zliczeń → 3700 mA
przy `CAL_BAT_I = 37,0`.

**Wzmocnienie filtru = dokładnie 1.** Punkt stały akumulatora to 64·x, więc `>>6` odzyskuje x.
Filtr wnosi wyłącznie opóźnienie, nie skalę. Zaokrąglenie: `>>6` zaokrągla w dół, więc wynik jest
zaniżony o < 1 zliczenie = **< 37 mA**; konwersja `float→int32` obcina, dodając < 1 mA.

## C. Offset to nie wzmocnienie — sprawdzone

Kalibracja startowa uśrednia 64 próbki i **przyjmuje wynik tylko wewnątrz okna
`CAL_BAT_I_OFFSET ± 200` zliczeń** (2035 ± 200 = 1835…2235); poza nim `bat_current_offset`
zostaje przy stałej kompilacyjnej 2035. **Nie dotyka `CAL_BAT_I`.** Test hostowy potwierdza:
dwa różne zera (1900 i 2150) dają dla tej samej fizycznej delty **identyczne** 1850 mA.

**Zanieczyszczenie zera — istniejące zabezpieczenia:**

| źródło | czy okno ±200 je łapie |
|---|---|
| prąd rozładowania (silnik) | **nie dotyczy** — kalibracja jest przed startem PWM |
| pobór postojowy sterownika/HMI/świateł | **NIE** — mieści się w oknie i zostanie odjęty jako „zero" |
| prąd ładowarki | **częściowo** — duży prąd wypadnie z okna i zostanie odrzucony, mały nie |
| rekuperacja | nie dotyczy przy starcie |

**Okno ±200 zliczeń to ±7,40 A.** To okno **wiarygodności**, nie dowód braku prądu. Nie zmieniam
go w B0.

## D. Pochodzenie `CAL_BAT_I` — **klasyfikacja D**

Pełna historia z repozytorium (autor: **stancecoke**, upstream EBiCS):

| commit | data | `CAL_BAT_I` | `CAL_BAT_I_OFFSET` | komunikat |
|---|---|---|---|---|
| `06dc59d` | 2025-11-09 | **21,3** | 1360 | „update some calibrations and display content" |
| `91a3c2c` | 2026-01-24 | **22,5** | 1360 | — |
| `014ea3b` | 2026-04-10 | **28,0** | 1360 | — |
| `72e9e7e` | 2026-05-13 | 28,0 | **2025** | — |
| `430d746` | 2026-05-16 | **37,0** | 2025 | „Adjust current calibration" |
| `9db3b36` | 2026-05-17 | 37,0 | **2035** | — |

**Cztery różne wartości wzmocnienia w pół roku, żadnego wyprowadzenia, żadnej notatki pomiarowej,
żadnego komentarza.** Skok 28,0 → 37,0 to **+32 % w trzy dni** przy niezmienionym offsecie — to
dopasowanie empiryczne, nie pomiar toru analogowego.

**Klasyfikacja: D — przybliżona stała historyczna.**
Nie A (brak wyprowadzenia fizycznego), nie B (brak udokumentowanego pomiaru), nie C (nie jest
stałą znanej płytki — offset zmienił się z 1360 na 2035, czyli tor analogowy **zmienił się** po
drodze). Nie E, bo jest wyraźny ślad świadomych korekt.

> **Poszlaka potwierdzająca:** w tym samym commicie `430d746` `CAL_I` skoczyło 70 → 95.
> `95/70 = 1,357`, `37/28 = 1,321` — obie stałe poruszają się razem, dokładnie tak, jak każe
> komentarz przy `CAL_I` („wstecznie wyliczone z prądu baterii"). **`CAL_I` dziedziczy więc
> każdy błąd `CAL_BAT_I`** — to domyka ustalenie z FW-128C0.

## E. Porównanie ze stockiem — **PARTIAL**

Wyprowadzenie stockowe (`M820_REVERSE_KNOWLEDGE_BASE.md` §11.1):
```
CAN_current = delta_ADC × 1000 / 255      (konwersja ze stockowego binarium)
CAN field   = current_A × 100             (kontrakt protokołu 0x3201)
⇒ 1 zliczenie PA0 = 10/255 A = 39,215686 mA
```

| aspekt | stock | EVistDrive | zgodne? |
|---|---|---|---|
| pin wejściowy | **PA0** | **PA0** | **TAK** |
| MCU / rozdzielczość ADC | GD32F303RCT6, 12 bit | to samo | **TAK** |
| tor analogowy / płytka | **ta sama fizyczna płytka** | **TAK** |
| domena napięcia odniesienia | VDDA, brak korekty VREFINT | brak VREFINT (sprawdzone) | **TAK** |
| konwencja zera | kompensacja temperaturowa zera PA0 | stałe 2035 ± kalibracja startowa | **różne, ale to OFFSET** |
| polaryzacja | dodatnia = rozładowanie (założenie) | dodatnia = rozładowanie | prawdopodobnie tak |
| **cyfrowy pre-scale przed konwersją** | **NIEZWERYFIKOWANE** | brak (surowe zliczenia) | **← jedyna luka** |

**WERDYKT: PARTIAL.** Tor sprzętowy jest ten sam — to **ta sama płytka** — więc różnica 5,65 %
**nie może pochodzić ze sprzętu**. Jedno założenie pozostaje niepotwierdzone: czy `delta_ADC`
w stockowej konwersji to **surowe zliczenia**, a nie wewnętrzna reprezentacja z pre-skalą.
To nie jest czepialstwo: **stockowa ścieżka prądu FAZOWEGO ma pre-skalę ×16**
(`Iinternal = 16·(zero − sample)`, ledger). Gdyby tor baterii miał podobną, wyprowadzone
39,216 byłoby o ten czynnik przesunięte.

> Zauważmy jednak: gdyby pre-skala była ×16, prawdziwe wzmocnienie wynosiłoby 2,45 mA/zliczenie
> — czterokrotnie i pół rzędu wielkości od 37, co byłoby jawnie sprzeczne z tym, że firmware
> pokazuje wiarygodne prądy. Więc pre-skala ×16 jest **praktycznie wykluczona**; to, czego brakuje,
> to formalny dowód, że pre-skali nie ma **żadnej**.

## F. Fizyczna konsekwencja 37 wobec 39,215686

Stosunek `39,215686 / 37,0 = **1,05988**`. Firmware **ZANIŻA** o **5,65 %**.

| firmware pokazuje | prawdziwy prąd (jeśli stock ma rację) | różnica |
|---|---|---|
| 5 A | **5,30 A** | +0,30 A |
| 10 A | **10,60 A** | +0,60 A |
| 12 A | **12,72 A** | +0,72 A |
| 15 A | **15,90 A** | +0,90 A |
| 20 A | **21,20 A** | +1,20 A |

**Skonfigurowany limit** (`BATTERYCURRENT_MAX = 15000` mA, konfigurowalny 1–40 A przez
`parser.c:161`):

> **Limit 15,0 A przepuszcza w rzeczywistości 15,90 A — o 0,90 A / 6,0 % za dużo.**
> Kierunek błędu jest **niebezpieczny**: limiter zwalnia hamulec dopiero, gdy zaniżony odczyt
> dobije do progu, więc naprawdę płynie **więcej**, niż użytkownik ustawił.

**To nie jest powód, żeby teraz zmienić stałą.** 39,216 jest wciąż tylko kandydatem (§E), a
podniesienie `CAL_BAT_I` bez dowodu zamieniłoby jeden nieudowodniony błąd na drugi. Właściwe
domknięcie to pomiar cęgami albo odczyt bocznika ze schematu — **ta sama luka, co `K` w FW-128C0**.

## G. Czas i filtr — **ten sam błąd klasy, co w PAS**

| pytanie | odpowiedź |
|---|---|
| **1. Czy regular ADC próbkuje niezależnie od main?** | **TAK.** Wyzwalanie sprzętowe TIMER1 CH1, zapis przez DMA kołowe. `adc_value[0]` jest zawsze świeże. |
| **2. Czy filtr dostaje każdą próbkę?** | **NIE.** `battery_current_cumulated` jest aktualizowane **raz na wykonanie `reg_ADC_processing()`** i czyta tylko **najświeższą** wartość DMA. Próbki między przebiegami są **tracone bezpowrotnie** — to aliasing, nie tylko opóźnienie. |
| **3. Czy koalescencja zmienia stałą czasową filtru?** | **TAK, proporcjonalnie.** Zmierzone hostowo (63 % skoku): **15,5 ms** przy 1 przebiegu/tick, **31,0 ms** przy co 2, **77,5 ms** przy co 5. |
| **4. Czy zmienia czas reakcji limitera?** | **TAK.** Legacy czyta `MS.Battery_Current` przy 16 kHz (`runPIcontrol` z ISR FOC), ale **nie może odzyskać informacji, której main nigdy nie pobrał**. |

**KLASYFIKACJA: pomiar prądu baterii JEST zależny od pętli głównej — dokładnie tak jak PAS.**
Nie naprawiam tego w B0.

> **Ile to jest w praktyce?** Zgłoszony objaw sprzed naprawy PAS daje liczbę. HMI pokazywało
> około **140 rpm**, a `MS.cadence = 10000/okres` przy strażniku `okres > 70` — czyli **140 rpm
> to dokładnie sufit tego strażnika** (10000/71 = 140,8). Żeby prawdziwa kadencja ~30 rpm
> (333 ticki) policzyła się jako 71, pętla główna musiała wykonywać się **raz na ~4,7 ticka**.
> Przy takim współczynniku stała czasowa filtru prądu baterii wynosiła ~**75 ms zamiast 16 ms**,
> a około **cztery piąte próbek ADC** nigdy nie trafiały do filtru.
> To wnioskowanie z objawu, nie pomiar — ale to jedyna liczba, jaką mamy, i jest duża.

## H. Legacy limiter — co faktycznie konsumuje

Lokalizacja po FW-128A: `pi_iq_apply_inputs()` w `main.c`, blok `LEGACY_BC_OVERRIDE`.

| element | wartość | jednostki |
|---|---|---|
| prąd mierzony | `MS.Battery_Current` | **mA** |
| maksimum konfigurowane | `MP.battery_current_max` | **mA** |
| **warunek wejścia** | `MS.Battery_Current > MP.battery_current_max` | **mA vs mA — wymiarowo poprawne** |
| **warunek wyjścia** | `(MS.i_q_setpoint · CAL_I · MS.u_abs) >> 11 < battery_current_max · 0,9` | prąd **przewidywany z KOMENDY** |
| histereza | mnożnik 0,9 — ale na wielkości **przewidywanej**, nie mierzonej | — |
| filtr/czas | odczyt przy 16 kHz sygnału odświeżanego ≤ 4 kHz i zależnego od pętli | — |

**Jedyna część, którą FW-128B może zachować, to warunek wejścia** — porównuje to samo z tym samym.
Warunek wyjścia opiera się na `CAL_I`, o którym FW-128C0 wykazał, że **nie jest** miliamperami
na jednostkę (pochłonął czynnik 3/2 transformaty amplitudowej).

## I. Czy FW-128B potrzebuje `K`? — **NIE**

```
MS.Battery_Current [mA]  ──┐
                           ├──►  błąd [mA]  ──►  limiter ciągły  ──►  współczynnik bezwymiarowy
MP.battery_current_max [mA]┘                                              │
                                                                          ▼
                                                        Iq_allowed ← Iq_allowed × współczynnik
```

**Dlaczego `K` nie jest potrzebne:** obie strony porównania są w miliamperach **tego samego
kanału**, więc żadna nie przechodzi przez skalę prądu fazowego. Wyjściem jest **redukcja
żądania**, którą można wyrazić bezwymiarowo (ułamek bieżącego żądania). Mnożenie
`Iq_allowed × współczynnik` zachowuje jednostkę `Iq` **bez znajomości, ile to amperów**.

Nigdzie nie występuje konwersja `Ibat[A] → Iq[A]`. Konwersja ta byłaby potrzebna **tylko** dla
limitera predykcyjnego (jak stary warunek wyjścia) albo dla progu wyrażonego w amperach fazowych
— a kontrakt FW-128B wyklucza jedno i drugie.

**PHASE K REQUIRED FOR FW-128B: NO.**

## J. Kto ma być właścicielem taktu limitera — i warunek wstępny

**Właścicielem musi być NOWA PRÓBKA PRĄDU BATERII, a nie „każde wywołanie ride-control".**
Powód jest twardy i wynika z karty: jeśli limiter ma człon całkujący `I[n+1] = I[n] + Ki·błąd`,
to `Ki` jest zdefiniowane **względem rzeczywistego czasu próbkowania**. Dziś ten czas jest
zmienną losową zależną od obciążenia pętli (§G), więc **żadne `Ki` nie ma dobrze określonego
znaczenia**.

> **WARUNEK WSTĘPNY FW-128B:** akwizycja prądu baterii musi najpierw dostać deterministyczny
> takt — dokładnie tak, jak PAS w `3263c4e`. Dwie możliwe formy:
> **(a)** akumulacja IIR przeniesiona do ISR TIMER1 (kilkanaście instrukcji, wzorzec sprawdzony),
> albo **(b)** filtr taktowany deltą `control_time_ticks`, z pominiętymi próbkami traktowanymi
> jawnie. **(a) jest czystsze** — próbka i tak powstaje w tym samym takcie, w którym DMA ją
> dostarcza, więc nic nie ginie.

Bez tego FW-128B da się zbudować **tylko** jako limiter czysto proporcjonalny (bezstanowy),
którego zachowanie w stanie ustalonym nie zależy od czasu próbkowania. To jest realna, bezpieczna
opcja — patrz §K.

## K. Kontrakt limitera ciągłego — ocena trzech wariantów

Zasada z kontraktu FW-128B pozostaje w mocy i jest tu potwierdzona: **limiter nie może być flagą
trybu zmieniającą wielkość sterowaną PI_iq.** Niezmiennik końcowy:
`PI_iq.reference = Iq_ref`, `PI_iq.feedback = zmierzony Iq`, **zawsze**.

| wariant | czy B0 daje wystarczające dowody? |
|---|---|
| **A. proporcjonalna redukcja ciągła** (bezstanowa) | **TAK.** Nie ma integratora, więc nie zależy od czasu próbkowania — wystarczają dowody z §B/§I. Stan ustalony ma uchyb statyczny, ale ten uchyb jest **w stronę bezpieczną** (ogranicza wcześniej), a jego wielkość jest znana z §F. **Jedyny wariant możliwy do wdrożenia bez warunku z §J.** |
| **B. osobny PI prądu baterii** | **NIE** — dopóki nie zostanie spełniony warunek z §J. `Ki` bez określonego czasu próbkowania jest niedefiniowalne. |
| **C. hybryda predykcyjna + PI** | **NIE.** Człon predykcyjny wymagałby przeliczenia `Iq → Ibat`, czyli dokładnie tego, co ma znikać, i sięgałby po `CAL_I` — obalone w FW-128C0. **Odradzam niezależnie od §J.** |

**Prawa sterowania ani żadnej stałej nie wybieram** — karta tego zabrania i brakuje jeszcze
danych z §J.

## L. Zależności czasowe — czy dotykają FW-128B?

| licznik | dotyczy FW-128B bezpośrednio? |
|---|---|
| `torque_counter` | **NIE** (choć bramkuje reset `PI_iq.integral_part` przy zerowym żądaniu — sąsiaduje, nie wpływa na wejścia limitera) |
| `tq_fault_ticks` | NIE |
| `uint16_half_rotation_counter` | NIE |
| **`pwm_cutoff_tick`** | **NIE** — odmierza okno miękkiego zaniku stopnia mocy (`main.c:1470-1485`), nie ma związku z prądem baterii ani z limiterem. Ma tę samą wadę klasy (okno zaniku rozciąga się w czasie rzeczywistym), ale **nie w tym łańcuchu**. |
| `ui16_erps_counter` | NIE |
| `slow_loop_counter`, `t3100_counter` | NIE |
| `soc_tick_counter` | **NIE dla limitera**, ale patrz niżej |

> **NOWE USTALENIE (nie naprawiane):** `main.c:2760` liczy
> `soc_mAs_acc += MS.Battery_Current / 4000.0f`, a `main.c:2761` uznaje 4000 wykonań za jedną
> sekundę. **Oba założenia zakładają dokładnie 4000 wykonań na sekundę.** Przy koalescencji
> zliczanie kulombów **zaniża** zużycie, a „sekunda" trwa dłużej niż sekundę → **SOC dryfuje
> optymistycznie**. To bezpośredni konsument `MS.Battery_Current`, więc należy do tego audytu —
> ale nie do tej karty.

## M. PAS — zamrożone

**`3263c4e` nietknięty.** PAS host = **PASS**. PAS sprzęt = **PENDING, test skonsolidowany**.
Objaw sprzed naprawy (kadencja skacząca ok. 0 ↔ 140 rpm) użyty **wyłącznie** jako miara
koalescencji w §G. Żaden osobny przejazd PAS nie jest proszony; przyszła skonsolidowana
walidacja sprzętowa **musi obejmować obserwację kadencji**.

## N. Proponowany kontrakt FW-128B (propozycja, NIE wdrożone)

1. **Warunek wstępny (§J):** deterministyczny takt próbki prądu baterii — akumulacja IIR
   w ISR TIMER1. **Albo** świadoma decyzja: limiter **czysto proporcjonalny, bezstanowy**
   (wariant A), który tego nie wymaga.
2. **Usunąć** cały `LEGACY_BC_OVERRIDE` z `pi_iq_apply_inputs()`; `BC_limit_flag` znika z drzewa.
3. **Nowy moduł** `battery_limit.{h,c}` — czyste liczby na wejściu, ograniczony prąd na wyjściu,
   bez `main.h`, bez globali, testowalny hostowo.
4. **Miejsce:** `ride_control_update()`, **przed** `iq_chain_note_allowed()`, żeby `Iq_allowed`
   zachowało znaczenie z FW-128A i `iq_chain` nie wymagało czwartego pola.
5. **Wejście:** `MS.Battery_Current` [mA] i `MP.battery_current_max` [mA] — **bez `K`**.
6. **Wyjście:** bezwymiarowy współczynnik redukcji żądania; limiter **tylko obniża**.
7. **Przy braku wiarygodnego odczytu** zachować się jak przy prądzie **równym limitowi**.
8. **Nie ustalać** stałych, dopóki punkt 1 nie zostanie rozstrzygnięty.
9. **`CAL_BAT_I` NIE zmieniać** w FW-128B. Odchylenie 5,65 % jest udokumentowane (§F) i wymaga
   pomiaru, nie decyzji projektowej.

## O. Sprzęt

**Żaden test nie jest teraz proszony.** Domknięcie `CAL_BAT_I` wymagałoby pomiaru prądu cęgami
przy jednoczesnym logu `MS.Battery_Current` albo odczytu bocznika/wzmocnienia ze schematu —
**ta sama luka fizyczna, co `K` w FW-128C0**, i najlepiej domykana tym samym pomiarem.

## P. Kryteria PASS

| kryterium | stan |
|---|---|
| producent `MS.Battery_Current` prześledzony | ✔ |
| równanie konwersji udowodnione | ✔ |
| jednostki udowodnione | ✔ mA/zliczenie |
| pochodzenie `CAL_BAT_I` sklasyfikowane | ✔ **D** |
| offset oddzielony od wzmocnienia | ✔ |
| porównanie ze stockiem sklasyfikowane | ✔ **PARTIAL** |
| konsekwencja 37 vs 39,216 policzona | ✔ +6,0 % na limicie, kierunek niebezpieczny |
| czas filtru/aktualizacji udowodniony | ✔ |
| wpływ koalescencji sklasyfikowany | ✔ **zależny od pętli głównej** |
| ścieżka legacy udowodniona | ✔ |
| wymóg `K` rozstrzygnięty | ✔ **NIE** |
| kontrakt wejścia limitera zdefiniowany | ✔ |
| zero zmian w produkcji | ✔ |
| PAS nietknięty | ✔ |
| brak prośby o sprzęt | ✔ |
