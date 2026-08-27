# FW-127 STATUS

Plik statusowy wymagany przez `FW-126_TO_FW-127_AGENT_HANDOFF_PL.md` §20.
Aktualizowany po każdym etapie. Format wg §20 karty.

**Ostatnia aktualizacja:** 2026-08-27 — FW-127 A+B+C+D ZAIMPLEMENTOWANE (NORMAL+DIAG 0.0446), 5 testow hosta zielonych, baseline bez zmian. Czeka na JEDEN skonsolidowany test sprzetowy #2.

---

```text
MASTER:
FW-127 Phase Current Acquisition

CURRENT CARD:
FW-127 A+B+C+D gotowe, czekaja na test sprzetowy #2 (DIAG 0.0446, jedna sesja).
FW-126.7 = ZAMKNIETE.
FW-126.2 = CLOSED (zbocze CH3 zmierzone i zamrożone).
FW-126.3 = CLOSED (przy MOE off CC3 nie napędza żadnej konwersji).
FW-126.4 = CLOSED (CASE B — trigger uniewinniony, delta TRGO-SW = 0/0/1).
FW-126 overall = OPEN wyłącznie z powodu równoważności ścieżki kalibracji.

FW-126.5 FAZA 0 — WYNIK (audyt kodu, bez sprzętu):
  ~1850 (most zgaszony) i ~0 (wybieg neutralny) to OBIE surowy JDR, odczyt
  adc_inserted_data_read() SPRZED odjęcia current_cal.offset[], w bootcie
  z current_cal.valid == 0. PORÓWNANIE NIGDY NIE BYŁO MIĘDZYDOMENOWE.
  Zostaje jedno wąskie pytanie: czy surowa liczba ZMIENIA SIĘ ze stanem mostka,
  jak szybko i czy wraca -> mierzy to kampania A0/B0/B1/B2/C0.

LAST COMPLETED:
FW-126.0 implementacja + host suite PASS (build 0.0428 NORMAL / 0.0429 DIAG)

CONFIRMED (log log-2026-08-25-19-28-08-n0.log, obraz DIAG 0.0430, schema 8):
- CH3 trigger half-cycle    = DOWN-count match (połówka OPADAJĄCA)
- trigger edge semantics    = CNT_isr = CCR3 - (CONV + L), nachylenie dCNT/dCCR3 = +1
                              zmierzone: +1,05 i +0,90 (oba w tolerancji 0,30)
- CONV + L                  = 588 zliczeń = 4900 ns przy 120 MHz
                              (z tego ~408 to sama konwersja -> ~180 zliczeń = 1,5 us
                               opóźnienia wejścia w ISR; wartość fizycznie sensowna)
- inserted conversions/PWM  = 1 (zmierzone: 4 ISR na tick 4 kHz, okno 23 ISR - mierzalne)
- ADC ISR sequencing        = jednoznaczne, sekwencja rosnąca 3 < 10 < 17
- jakość pomiaru            = 7 przyjętych konwersji na każdą z 3 wartości CH3,
                              spread 1 zliczenie na każdym punkcie, CH3 readback == CH3 requested,
                              wszystkie próbki weszły na zboczu opadającym, ADC0/ADC2 late = 0.
                              ADC1_LATE=1 = jedno przerwanie ODRZUCONE przed próbkowaniem
                              (mechanizm FW-126.2 zadziałał), nie weszło do mediany.

CURRENT CODE:
branch      = diag/fw112-real-bike-ab
HEAD        = 5ffaa14  (checkpoint przed destrukcyjną czystką FW-126.5;
              poprzedni ba794be "WIP: preserve integrated state before FW-112 isolation")
worktree    = DIRTY — FW-102..FW-126 są NIEZACOMMITOWANE; HEAD nie opisuje tego,
              co jest w binarkach. Oba manifesty mają worktree_dirty: true.
NORMAL build = 0.0428  FLASH 101004 B  RAM 12064 B
               SHA256 7DB8317AE1802ED2CCD2ECFEDC9F5FDAC553D30F210E0097EE9991B32CE3CE94
               PRZEBUDOWANY 2026-08-25 po FW-126.2, przypięty do 0.0428 (wersja jest
               wkompilowana w obraz, więc tylko przy tej samej wersji test bitowy ma sens)
               -> BIT W BIT IDENTYCZNY z baseline. FW-126.2 nie dołożyło do NORMAL nic.
DIAG build   = 0.0440 (FW-126.5 kampania A0/B0/B1/B2/C0, schema 4)
               SHA256 20D6D0F18C9022BEB38CCFE2EBB8F12DF02C943F1A2025876321CF5ED5C1D814
               FLASH 150820 B (64,04 %)  RAM 46056 B (93,70 %, wolne 3096 B)
               .map: bez overflow. .bss.fw1265 = 202 B przy budżecie 260 B
               (pilnowane _Static_assert).
               Względem 0.0435 czystka adc_trigger_diag oddała 2588 B FLASH i 304 B RAM.
DIAG 0.0439  = ten sam kod przed bramką złożoności (adc_stat[1..2] bez konsumenta). NIE UŻYWAĆ.
DIAG 0.0438  = 0.0438 — przed podbiciem DIAG_SCHEMA_VERSION. NIE UŻYWAĆ.
DIAG 0.0435  = HISTORYCZNY (FW-126.4 sonda A/B, schema 3)
               SHA256 FEC3AD81D5800D60DFC4B97F195458FB25747A344B1A3A9EB9626C622658FF5A
               FLASH 153408 B  RAM 46360 B (94,32 %, wolne 2792 B)
DIAG 0.0432  = 0.0432
               SHA256 1850BDC500ADB3B81A5EB6EC4C5FF36CD66E6567AAD3F454BFF07A3C69E0E9D9
               FLASH 152808 B  RAM 46432 B (94,47 %, wolne 2720 B)
DIAG 0.0431  = 0.0431
DIAG 0.0430  = 0.0430  FLASH 151200 B  RAM 46288 B / 48 KB (94,17 %, wolne 2864 B)
               SHA256 867F2C5A1B212340BA50FCB0C6327D22E8D6343068364BD576664C17E5DB7F1D
               schema ramek = 8
               .map: bez overflow; .bss.S 324 -> 436 B (+112), pozostałe 77 symboli bss
               bez zmian; cały przyrost RAM jest w adc_trigger_diag
DIAG 0.0429  = HISTORYCZNY, schema 7, jedna próbka na punkt — nie używać do FW-126.2

HARDWARE:
CZĘŚCIOWO (oba logi na obrazie 0.0429, tj. PRZED FW-126.2) — log log-2026-08-25-17-22-24-n0.log (60 s, obraz DIAG potwierdzony)
  ścieżka odbioru  = POTWIERDZONA NA SPRZĘCIE
       żądanie 8511602D DLC 0 wyszło, odpowiedź 822C602D (len 0x37=55)
       + 822D0000..0005 + 822E0006, złożona, CRC 0x7FF9 zgodne. Dwie pełne odpowiedzi.
       Blok zbiorczy 0x10240-0x10246 też dotarł (7/7 ramek).
  sweep CH3        = log 17:22 NIE WYKONAŁ SIĘ (state IDLE, brak startu wspomagania).
                     log 17:44 WYKONAŁ SIĘ: schema 7, state=2 DONE, punkty 3/3.
       pt CH3   CNT@ISR  dCNT  ISRseq  DIR    RDY  POEN DWELL  rawA   rawB  rawC
        0 3740   2956      -     1     up     YYY  ON   YES   65526  65533   12
        1 3700   3116   +160     3     down   YYY  ON   YES   65523  65531   10
        2 3660   3072    -44     4     down   YYY  ON   YES   65522  65531   10
       WERDYKT: INCONCLUSIVE. Nachylenia -4 oraz +1,1 — nie schodzą się do -1 ani +1.
       Trzy powody, wszystkie w danych:
       a) ISR nr 2 ODRZUCONY (ADC1_LATE=1), stąd luka w sekwencji 1 -> 3 -> 4;
       b) punkt 0 to PIERWSZE przerwanie po MOE ON i jedyne z DIR=up — jest skażone
          samym załączeniem mostka;
       c) para 1->2 daje +1,1 (wskazywałoby DOWN), ale JEDNA para to nie werdykt.
       ISR/control tick = 3 przy 4 ISR łącznie NIE JEST miarą tempa konwersji: okno
       jest krótsze niż jeden tick 4 kHz. Oba dekodery pokazują teraz NOT MEASURABLE
       zamiast liczyć z tego 0,75 konwersji na okres PWM (artefakt, poprawiony).
       STOP wg §9 — wdrożono minimalne rozszerzenie DIAG (karta FW-126.2):
       odrzucenie pierwszego ISR po MOE ON, 7 przyjętych konwersji na wartość CH3
       i MEDIANA zamiast pojedynczej próbki, ADC1-late poza medianą, min/max/spread
       na drucie, odczyt zwrotny CH3, dwell 40 cykli wyłącznie w DIAG i wyłącznie na
       starcie uzbrajającym. Ramki: schema 8 (dwie na punkt, 0x10244 Data0 = 0xB1).
       Dekodery czytają schema 8 i nadal schema 7. Przy okazji naprawiono realny błąd
       dekodera: wydawał UP/DOWN z jednej pary — teraz wymaga trzech punktów.
  dump 0x602D      = ODCZYTANY I WIARYGODNY, ale kalibracja NIE PRZESZŁA (patrz niżej).

DO NOT CHANGE:
- produkcyjna geometria ADC/PWM (handoff §6) — do czasu werdyktu testu
- wszystko z listy zakazów handoff §18
- obraz 0.0429 (każda zmiana unieważnia hash w procedurze testu)

OPEN QUESTIONS:
1. UP / DOWN / BOTH dla TIMER0_CH3 — cały cel testu. NADAL OTWARTE, sweep się nie uruchomił.
4. NOWE, potwierdzone w DWÓCH logach — kalibracja prądu leci na LEGACY_FALLBACK.
   17:22 residual mean 1841/1866/1871, 17:44 residual mean 1848/1874/1879 —
   powtarzalne w granicach ~7 LSB.
   **SPRZECZNOŚĆ, która wskazuje przyczynę:** w tym samym logu 17:44 surowe JDR
   zapisane w neutral dwell (most ZAŁĄCZONY, 50 % wypełnienia) wynoszą
   -10 / -3 / +12, czyli praktycznie ZERO. Sprzętowy offset injected JEST
   zaprogramowany: 2012 / 2028 / 2020 (main.c:1868/1873/1878) i w dwellu działa.
   Czyli ~1850 nie jest stałym offsetem sprzętowym — jest właściwością warunków,
   w jakich mierzy kalibracja (most WYŁĄCZONY, trigger programowy). To falsyfikuje
   założenie FW-125, że tę samą ścieżkę da się skalibrować przy wyłączonym moście.
   status=6 LEGACY_FALLBACK, source=3 LEGACY, valid=0, attempts=3,
   ostatni powód=2 OUT_OF_RANGE, offsety 0/0/0 (ISR nie odejmuje nic).
   Pomiar sam w sobie jest CZYSTY: 128 próbek, 512 konwersji wydanych i potwierdzonych,
   MOE-off verified=1, P2P tylko 17-19 LSB, a niezależny verify daje to samo
   (mean 1842/1867/1872 vs 1841/1866/1871).
   Czyli przy WYŁĄCZONYM moście injected JDR czyta ~1850, nie ~0.
   FW-125 zakłada, że adc_inserted_data_read() jest już skorygowane offsetem
   sprzętowym i residual ma być bliski 0; okno CURRENT_CAL_RESIDUAL_MAX_DEVIATION
   = +-300 (config.h:379) odrzuca 1850 na każdej z 3 prób.
   To jest POMIAR, nie hipoteza. Wymaga osobnej karty — NIE zmieniać progu ani
   ścieżki offsetu bez niej.
2. ZAMKNIĘTE 2026-08-25. Dump 0x602D odczytujemy ręcznie: Custom Frame 0511602D,
   extended, DLC 0; odpowiedź parsuje tools/decode_fw126_cal_dump.ps1 z tego samego logu.
   Karta FW-126.1 ZAAKCEPTOWANA w zakresie zminimalizowanym: BEZ ZMIAN W FIRMWARE.
   Powstał parser logu, a następnie — na polecenie właściciela — read-only panel
   FW-126 TEST w zakładce Sniffer aplikacji CANable (odczyt na żywo tym samym
   werdyktem; jedyna ramka, jaką umie wysłać, to odczyt 0511602D DLC 0).
3. ZAMKNIĘTE 2026-08-25. Sprzeczność w dokumentacji POPRAWIONA.
   FW-126_PHASE_CURRENT_ACQUISITION_HARDENING_PL.md mówił
   „CNT malejący o około 40 oznacza UP", odwrotnie niż wyprowadzenie w
   inc/adc_trigger_diag.h (wiersze 28-33):
       UP-count match   -> CNT_isr = 2*_T - CCR3 - (CONV+L)  -> nachylenie -1
       DOWN-count match -> CNT_isr = CCR3 - (CONV+L)         -> nachylenie +1
   czyli CH3 w dół o 40 + CNT w dół o 40 = DOWN, nie UP.
   Dokument poprawiony wraz z adnotacją CORRECTION BEFORE HW VALIDATION.
   tools/decode_fw126_ch3.ps1 realizuje wersję poprawioną i NIE był dostosowywany
   do poprzedniego zdania. Sam wynik UP/DOWN/BOTH pozostaje NOT HW VERIFIED.

NEXT EXACT TASK:
Wykonać JEDEN test wg documentation/FW-126_HW_TEST_PROCEDURE_PL.md na obrazie DIAG 0.0429.
Ten sam przebieg zbiera oba pomiary: sweep CH3 dzieje się automatycznie przy pierwszym
starcie wspomagania, a dump kalibracji dociąga przycisk Get FW-126 Result w panelu
FW-126 TEST (albo ręczna ramka Custom Frame 0511602D, extended, DLC 0) przed
zatrzymaniem logu. Wynik widać od razu w panelu; kontrolnie z tego samego logu:

    .\tools\decode_fw126_ch3.ps1      -Log <log>     -> UP / DOWN / BOTH / INCONCLUSIVE
    .\tools\decode_fw126_cal_dump.ps1 -Log <log>     -> PASS / FAIL kalibracji

i na podstawie werdyktu:
  UP / DOWN            -> wypełnić raport handoff §21 i otworzyć osobną kartę FW-127A
  BOTH / INCONCLUSIVE  -> STOP wg §9, wyłącznie minimalne rozszerzenie DIAG
FW-126 nie może zostać CLOSED wg §22, dopóki oba polecenia nie dadzą wyniku z roweru.
```

---

## FW-126.7 — kalibracja wymieniona (2026-08-27)

Ciemna kalibracja usunięta z kodu; pomiar przeniesiony do istniejącego wybiegu neutralnego
FW-117, gdzie odczyt jest ważny. Rozdzielone bramki: wejście w stan neutralny wymaga tylko
żądania momentu, `current_cal_foc_allowed()` bramkuje wyłącznie aktywny FOC — to usuwa
zakleszczenie „nieskalibrowany nigdy się nie skalibruje".

Bramka ustalenia = KONIUNKCJA: okno residual ±300 NAJPIERW, stabilność DRUGA. Nasycenie było
ciche (rozrzut 10 LSB, P2P 17–19), więc sam peak-to-peak nigdy nie może decydować.

**UWAGA: NORMAL celowo się zmienił.** Baseline 0.0428 / `7DB8317A…2CE3CE94` NIE OBOWIĄZUJE.

| | NORMAL 0.0442 | DIAG 0.0442 |
|---|---|---|
| FLASH | 100 572 B (42,66 %) | 145 496 B (61,78 %) |
| RAM | 12 072 B (24,56 %) | 45 840 B (93,26 %, wolne 3 312 B) |
| SHA256 | `2D1123F7…8C55211E` | `D6EC0AC7…01024C4F` |

Oba obrazy używają TEGO SAMEGO kodu kalibracji; DIAG ma wyłącznie post-walidacyjny STOP,
żeby pierwszy test nowej kalibracji nie przeszedł od razu do momentu.

Testy hosta: **12 → 2** czerwone (zostały T14/T9 w rolling_no_assist, sprzed tej karty).
Dziesięć zniknęło razem z usuniętą architekturą, nie zostało wyciszone. Raport 0x602D to
schema 2 (66 B), layout w `protocol/fw1267_cal_schema.json`, parytet dekoderów pod testem.

Szczegóły: [FW-126.7_PRODUCTION_CURRENT_CAL_REPLACEMENT_PL.md](FW-126.7_PRODUCTION_CURRENT_CAL_REPLACEMENT_PL.md).

---

## FW-126.6 — stan ważnego pomiaru prądu (audyt, 2026-08-26)

Odtworzenie surowej konwersji (`raw = JDR + IOFF`, IOFF A/B/C = 2020/2028/2012) rozstrzyga
sprzeczność, która ciągnęła się od FW-125:

| stan | raw A/B/C | napięcie | co to jest |
|---|---|---|---|
| ciemno (stara CAL) | 3874 / 3910 / 3898 | 3,12–3,15 V | **wzmacniacz w nasyceniu przy szynie** |
| chwila po MOE ON (B0) | 4078 / 4077 / 4079 | 3,29 V | twarda szyna |
| ustalone (B1/B2) | 2004 / 2023 / 2020 | 1,615–1,630 V | **środek skali** |
| stałe IOFF w firmware | 2020 / 2028 / 2012 | 1,621–1,634 V | zgodne w granicach 16 LSB |

Wniosek: **stara ciemna kalibracja mierzyła nasycony wzmacniacz.** Okno ±300 nigdy nie było
błędem — błędem był stan. Nasycenie jest ciche (rozrzut A0 = 10 LSB, P2P dumpu 17–19), więc
sam rozrzut nie może być kryterium ważności.

Różnica eVistDrive ↔ stock, wcześniej przeoczona: eVistDrive ma `CHCTL2 = 0x1DDD`
(EN=NEN=1 **zawsze**), więc przy `IOS=1` jego piny w stanie ciemnym są **aktywnie napędzane**
na poziomy idle. Stock w CAL ma `EN=NEN=0` — timer **zwalnia** wszystkie sześć pinów. To trzy
różne stany elektryczne, nie dwa.

`0x0880/0x0808/0x0088`: we wszystkich trzech `EN=NEN=0`, więc na pinach **nie różnią się**;
po co stock rotuje NP = **OPEN**. Gate driver = **OPEN** (brak oznaczenia w całym repo).

Model: **E z przechyłem ku C** (potrzeba faktycznego przełączania) — ale 437 µs to też
realistyczna stała czasowa po skokowym enable, więc C i A nie są rozdzielone.

Stan stock-like (`EN=NEN=0`+POEN=1) jest **niezautoryzowany**: zwolnienie pinów oznacza
nieznany poziom na wejściach drivera bez schematu. Odblokowuje go schemat/PCB albo pomiar.

Szczegóły: [FW-126.6_CURRENT_SENSE_VALID_STATE_PL.md](FW-126.6_CURRENT_SENSE_VALID_STATE_PL.md).

---

## FW-126.5 — czystka architektoniczna i stan na 2026-08-26

Decyzja właściciela: **DELETE, nie DISABLE.** Zamknięta warstwa `adc_trigger_diag` została
usunięta z kodu, a nie wyłączona flagą kompatybilności.

Usunięte: `src/adc_trigger_diag.c`, `inc/adc_trigger_diag.h`, `inc/fw1264_probe.h`,
`protocol/fw1264_probe_schema.json`, trzy testy hostowe tej warstwy, blok 7 ramek zbiorczych
w `main.c`, osierocona zmienna ścieżki w `run-host-tests.ps1`. Zero referencji runtime
(`grep` po `src/`, `inc/`, `scripts/`). Żadnego `#if 0`, flagi legacy, gałęzi zgodności
ani martwych liczników. Checkpoint `5ffaa14` powstał **przed** czystką; historii nie ruszano.

Zachowane, bo są dowodem albo są potrzebne do starych logów: dokumentacja wyników FW-126.2,
rejestr dowodów, historyczne logi, dekoder `decode_fw126_ch3.ps1` dla schematów 7/8.

**`DIAG_SCHEMA_VERSION` podbity 7 → 8.** Log w wersji 7 *zawiera* ramki `0x10240..0x10246`;
log w wersji 8 nigdy ich nie zawiera. Bez tego „brak ramek 0x1024x" byłoby nieodróżnialne od
„przemiatanie się nie uzbroiło" — a to jest werdykt, nie brak. Zmiana nie dotknęła NORMAL:
przebudowa 0.0428 dała ten sam hash bit w bit.

Testy: pakiet hostowy ma **3 czerwone pakiety** — FW-119 wiring (3), FW-125 wiring (7),
rolling_no_assist (T14, T9). To **mniej** niż deklarowana baza, bo dwa pakiety FW-121 zniknęły
razem z usuniętą warstwą. **Żadnego nowego FAIL.** Nowy `fw1265_campaign_probe_host.c` (S1–S11)
przechodzi i został zweryfikowany mutacjami. Pakiet Canable zielony, lint czysty.

Szczegóły: [FW-126.5_RAW_DOMAIN_BRIDGE_STATE_PL.md](FW-126.5_RAW_DOMAIN_BRIDGE_STATE_PL.md).

---

## Narzędzia przygotowane w tym etapie

| Plik | Rola | Stan |
|---|---|---|
| `tools/decode_fw126_ch3.ps1` | dekoder ramek `0x10240–0x10246`, werdykt UP/DOWN/BOTH/INCONCLUSIVE | gotowy, 7 syntetycznych przypadków |
| `tools/decode_fw126_cal_dump.ps1` | parser odpowiedzi `0x602D` z surowego logu, werdykt PASS/FAIL | gotowy, 7 syntetycznych przypadków |
| `documentation/FW-126_HW_TEST_PROCEDURE_PL.md` | procedura jednego przebiegu, oba pomiary | gotowa |
| `documentation/FW-126.1_CAL_DUMP_READBACK_PL.md` | karta odczytu `0x602D` | ZAAKCEPTOWANA, zakres zminimalizowany |
| Canable: panel **FW-126 TEST** w zakładce Sniffer | odczyt na żywo, ten sam werdykt co skrypty, przycisk Read CAL 0x602D i Copy Report | wdrożony, 80+ testów PASS |

Dekoder CH3 weryfikowany przeciw znanym odpowiedziom: hipoteza UP (nachylenie −1),
hipoteza DOWN (+1), podwojona częstotliwość ISR (BOTH), niezerowe `ADC*_LATE` (warunek STOP),
ramki starego schematu FW-121 (odmowa dekodowania), log skompresowany markerami
„(Repeated N times)", log pusty oraz wyjście JSON. W każdym przypadku werdykt zgodny
z wartością zadaną; z punktu przecięcia poprawnie odtworzone `CONV + L`.

Parser `0x602D` weryfikowany na ładunkach zbudowanych wg `current_cal_serialize_dump()`
i framingu `can_multiframe.c`: komplet PASS, zestaw FAIL (4 nazwane kryteria), zepsuty CRC,
brak ramki END, brakujący fragment DATA, postać z prefiksem `822C602D` oraz dwie odpowiedzi
w jednym logu. Przy okazji wykryta i usunięta pułapka Windows PowerShell 5.1: literał
`0x80000000` jest tam `Int32 = -2147483648`, przez co test znaku w dekodzie `int32` zwracał
każdą liczbę konwersji jako ujemną.

**Log z 2026-08-25 17:22 — pierwsze użycie na prawdziwym rowerze.** Oba dekodery zadziałały na
prawdziwym logu, nie tylko na syntetycznym: parser `0x602D` złożył 55 B z ośmiu ramek i CRC się
zgodziło, dekoder CH3 poprawnie ROZPOZNAŁ, że blok jest w układzie legacy schema 6, i odmówił
wydania werdyktu zamiast zgadywać. Ścieżka odczytu jest potwierdzona sprzętowo.
Sam pomiar CH3 pozostaje niewykonany — patrz sekcja HARDWARE.
