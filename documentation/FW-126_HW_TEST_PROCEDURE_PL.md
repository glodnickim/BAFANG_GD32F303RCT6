# FW-126 — procedura testu sprzętowego (DIAG 0.0429)

**Data:** 2026-08-25
**Cel:** jednoznacznie rozstrzygnąć, które zbocze `TIMER0_CH3` uruchamia pomiar prądu fazowego.
Bez tej odpowiedzi FW-127 jest zablokowane (handoff §1 i §10).

---

## 0. Zanim wsiądziesz — trzy rzeczy do sprawdzenia

**a) Właściwy plik firmware.** Wgraj DIAG, nie NORMAL:

```text
.build/M820_BL820/0.0432_M820_BL820_DIAG.bin
wersja  0.0432
schema  8 (sweep CH3) + schema 2 (sonda 0x602E, zdarzenia per ADC)
SHA256  1850BDC500ADB3B81A5EB6EC4C5FF36CD66E6567AAD3F454BFF07A3C69E0E9D9
```

> **Poprzedni obraz `0.0429` (SHA256 `05D49FD5…2F4BA`) jest HISTORYCZNY i NIEPRAWIDŁOWY dla
> FW-126.2.** Zbierał jedną próbkę na wartość CH3 i nadawał ramki w schemacie 7. Logi z niego
> (17:22 i 17:44) pozostają czytelne, ale nowego pomiaru na nim wykonać się nie da.

Sprawdzenie sumy przed wgraniem (PowerShell):

```powershell
Get-FileHash .build\M820_BL820\0.0429_M820_BL820_DIAG.bin -Algorithm SHA256
```

**b) Ten obraz zużywa 94,17 % RAM (46288 B z 48 KB, wolne 2864 B).** To wersja wyłącznie do pomiaru. Po teście wracasz na
`0.0428` (NORMAL).

**c) Koło uniesione, napęd bez obciążenia.** Nie testuj momentu, nie szarp. Cały pomiar
dzieje się w pierwszej milisekundzie startu wspomagania, zanim silnik dostanie moment.

---

## 1. Co się dzieje w sterowniku (żebyś wiedział, na co czekasz)

Przy **pierwszym** starcie wspomagania po włączeniu zasilania sterownik na chwilę
przytrzymuje most w stanie neutralnym (tak robi zawsze) i w tych trzech przerwaniach
przestawia sam trigger pomiaru na `3740`, `3700`, `3660`. Nie rusza przy tym mocy silnika.
Potem przywraca wartość produkcyjną i jedzie normalnie.

Trzy rzeczy, które z tego wynikają dla Ciebie:

1. **Liczy się PIERWSZY start po włączeniu.** Pomiar uzbraja się raz na cykl zasilania.
2. Wynik siedzi potem w pamięci do wyłączenia i jest **dosyłany z każdym podsumowaniem
   sesji**, więc nie musisz go złapać „w locie".
3. Podsumowanie sesji wychodzi **dopiero po około 3 sekundach ciszy** po zakończeniu jazdy.
   Jeśli wyłączysz zasilanie od razu po pedałowaniu, w logu nie będzie nic.

---

## 2. Przebieg testu — jedno uruchomienie zbiera wszystko

1. Podłącz Canable i **uruchom zapis logu (sniffer)**.
2. Włącz zasilanie roweru. (Od tej chwili liczy się pierwszy start.)
3. **Doprowadź do tego, żeby silnik NAPRAWDĘ pociągnął.** To jedyny warunek uzbrojenia
   pomiaru — sweep startuje w chwili załączenia mostka, a mostek załącza się dopiero gdy
   `i_q_setpoint > 0` (`main.c:1493`). Samo kręcenie korbą bez oporu może nie wystarczyć:
   z kołem w powietrzu czujnik momentu prawie nic nie widzi.

   **Najpewniej: przytrzymaj WALK ASSIST.** Walk assist podaje moment tą samą drogą
   (`ride_control.c:374`), więc uzbraja sweep tak samo jak normalna jazda, a nie wymaga
   nacisku na pedały. Dwie sekundy wystarczą.

   Alternatywnie: poziom wspomagania **powyżej 0**, brak zaciśniętego hamulca i realny
   nacisk na pedał, aż silnik wyraźnie pociągnie.

   Sweep CH3 `3740 / 3700 / 3660` wykona się przy tym sam — nic nie klikasz. Po FW-126.2
   pomiar trwa dłużej (7 konwersji na każdą z trzech wartości, ~2,5 ms neutralnego wektora),
   ale nadal mieści się w jednym starcie — wystarczy jedno przytrzymanie walk assist.
4. **Przestań i odczekaj co najmniej 5 sekund w bezruchu.** To jest ten moment, w którym
   sterownik wysyła podsumowanie sesji z wynikiem pomiaru.

   **Sprawdź w panelu FW-126 TEST, czy pomiar się wykonał:**

   ```text
   STATE  : 2 DONE   points completed: 3/3     <- tak ma być
   STATE  : 0 IDLE   points completed: 0/3     <- silnik nie pociągnął, wróć do punktu 3
   ```

   Przy `IDLE` nie ma sensu jechać dalej — pomiaru nie ma i nie będzie do następnego
   włączenia zasilania. Sprawdź poziom wspomagania, hamulce i spróbuj walk assist.
5. Dla pewności powtórz punkty 3–4 jeszcze raz lub dwa. Wynik pomiaru będzie ten sam
   (jest zamrożony), ale dostaniesz kilka kopii i log przetrwa ewentualną zgubioną ramkę.
6. **Nie wyłączając zasilania**, pobierz dump kalibracji. Najprościej: w panelu
   **FW-126 TEST** (zakładka Sniffer) naciśnij **Get FW-126 Result**. Alternatywnie ręcznie,
   przez Custom Frame:

   ```text
   ID        0511602D
   Extended  YES
   DLC       0
   Data      (puste)
   ```

7. Odczekaj na pełną odpowiedź — w logu ma pojawić się 8 ramek:
   `022C602D` (lub `822C602D`), sześć `022D0000`…`022D0005` i `022E0006`.
8. Zatrzymaj zapis logu. Zapisz nazwę pliku.

**Nie wyłączaj zasilania między punktem 3 a 7.**

---

## 3a. Odczyt w aplikacji — zakładka Sniffer, panel FW-126 TEST (najprostsza droga)

W zakładce **Sniffer** jest panel **FW-126 TEST**. Działa tylko do odczytu i nie umie wysłać
niczego poza jedną ramką odczytu `0511602D`.

**Panel działa tylko przy uruchomionym snifferze** — karmi go ta sama ścieżka odbioru, więc
przy zatrzymanym snifferze nie przyjdzie nic. To dlatego krok 1 procedury (Start Sniffing) jest
obowiązkowy niezależnie od tego, czy czytasz wynik w panelu, czy z pliku.

- ramki sweepu CH3 łapie **sam, niezależnie od filtrów** — nie trzeba nic ustawiać ani włączać;
  wynik pojawia się w panelu, gdy tylko przyjdzie podsumowanie sesji;
- **Get FW-126 Result** — jeden przycisk: analizuje odebrane ramki CH3, wysyła odczyt `0x602D`
  i po kompletnej odpowiedzi pokazuje wynik końcowy. To zastępuje krok 6 procedury;
- **Read CAL 0x602D** — sam odczyt dumpu, jeśli chcesz go powtórzyć;
- **Copy FW-126 Report** — kopiuje gotowy tekst do wklejenia agentowi.

Panel jest **dodatkiem**, nie zamiennikiem loggera: zaznaczaj **Log to file**, bo plik z całą
sesją pozostaje materiałem dowodowym i to z niego liczą się poniższe polecenia offline.

## 3b. Odczyt offline — dwa polecenia z tego samego logu

Zbocze CH3:

```powershell
.\tools\decode_fw126_ch3.ps1 -Log <ścieżka_do_logu>
```

Dump kalibracji:

```powershell
.\tools\decode_fw126_cal_dump.ps1 -Log <ścieżka_do_logu>
```

Drugie polecenie kończy się linią `VERDICT: PASS` albo `FAIL` z nazwanymi kryteriami
(`last_sample_count = 128`, `valid = 1`, `fallback = 0`, `MOE-off verified = 1` i pozostałe).

Poniżej opis werdyktu pierwszego polecenia.

Skrypt sam wypisze werdykt. Interesują nas cztery linijki:

```text
LATE   : ADC0=0  ADC1=0  ADC2=0        <- wszystkie muszą być 0
ISR    : max per control tick 4        <- 4 = jedna konwersja na okres PWM; 8 = STOP
SLOPE dCNT/dCCR3 : -1, -1              <- albo +1, +1
VERDICT          : UP / DOWN / BOTH / INCONCLUSIVE
```

Co znaczy werdykt:

| Werdykt | Znaczenie | Co dalej |
|---|---|---|
| `UP` | konwersja startuje przy zliczaniu w górę | FW-126 domknięte, można otworzyć FW-127A |
| `DOWN` | konwersja startuje przy zliczaniu w dół | jw. |
| `BOTH` | dwa triggery na okres PWM | **STOP** — handoff §9, tylko rozszerzenie DIAG |
| `INCONCLUSIVE` | nie da się rozstrzygnąć | **STOP** — tak samo |
| `NO DATA` | brak ramek w logu | powtórz test, patrz sekcja 4 |

`confidence: HIGH` wymaga trzech kompletnych punktów i zera warunków STOP.

---

## 4. Jeśli w logu nic nie ma

Najczęstsza przyczyna w kolejności prawdopodobieństwa:

1. **Zabrakło ciszy po jeździe** — podsumowanie sesji nie zdążyło wyjść. Powtórz z punktem 4.
2. **Wgrany obraz NORMAL zamiast DIAG** — w `0.0428` tego pomiaru nie ma w ogóle.
   Sprawdź sumę SHA256.
   **Albo wgrany stary DIAG `0.0429`** — on też zadziała, ale odda schemat 7 (jedna próbka
   na punkt) i najpewniej znów INCONCLUSIVE. Dekoder wypisze wtedy
   `FW-126.0 neutral dwell, single sample (schema 7)` zamiast `schema 8`.
3. **To nie był pierwszy start po włączeniu** — jeśli wspomaganie ruszyło już wcześniej
   (np. przy wprowadzaniu roweru), pomiar uzbroił się wtedy i mógł zostać przerwany.
   Wyłącz zasilanie, odczekaj, zacznij od nowa.

Skrypt sam podpowiada te przyczyny przy werdykcie `NO DATA`.

---

## 5. Jeśli nie ma odpowiedzi na Custom Frame

Parser napisze `NO DATA - no 0x602D reply START`. Możliwe przyczyny:

1. **Obraz NORMAL zamiast DIAG** — `0x602D` istnieje wyłącznie w obrazie diagnostycznym.
2. **Literówka w ID** — musi być dokładnie `0511602D`, ramka **rozszerzona**, DLC 0.
   Firmware odrzuca wszystko, co nie jest odczytem od źródła 5.
3. **Log zatrzymany za wcześnie** — odpowiedź to 8 ramek, nie jedna.

Szczegóły protokołu: [FW-126.1](FW-126.1_CAL_DUMP_READBACK_PL.md).

---

## 6. Po teście

Wróć na obraz NORMAL `0.0428` do normalnego jeżdżenia (przebudowany 2026-08-25, bit w bit
identyczny z baseline — FW-126.2 nie dołożyło do NORMAL ani jednego bajtu):

```text
.build/M820_BL820/0.0428_M820_BL820.bin
SHA256 7DB8317AE1802ED2CCD2ECFEDC9F5FDAC553D30F210E0097EE9991B32CE3CE94
```
