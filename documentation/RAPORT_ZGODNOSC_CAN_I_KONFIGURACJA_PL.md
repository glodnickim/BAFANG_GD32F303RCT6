# Raport — zgodność z fabryczną komunikacją CAN i dlaczego konfiguracja nie działa

Data: 2026-09-04. **Raport przed działaniem — kodu nie zmieniałem.**
Podstawa: nowy `documentation/BAFANG_CAN_STOCK_VERIFIED_REFERENCE.md` (tabele z firmware, nie
z estymacji z logu) + audyt naszego kodu.

Zlecone: co brakuje / co jest nie tak wobec fabrycznej komunikacji, dlaczego **assist i max power
nie działają**, czy **obwód i średnica koła** działają, oraz co z **akceleracją**.

---

## 1. Najważniejszy wniosek w jednym zdaniu

**Nasze ramki nie są urwane — są wysyłane w złym rytmie, część nie jest wysyłana wcale, a dwie
z nich lecą kilkanaście razy za często i najprawdopodobniej to one przepełniają kolejkę, przez
którą gubimy podtrzymanie** (patrz FW-132). Konfiguracja z kolei jest **czytana, ale w połowie
nigdzie nie trafia**.

---

## 2. Rytm nadawania — porównanie z tabelą fabryczną

Nowy dokument podaje **okresy z tablicy deskryptorów stockowego firmware**, nie z pomiaru logu.
Po raz pierwszy mamy z czym się porównać co do milisekundy.

| Ramka | Fabryka | My | Ocena |
|---|---:|---:|---|
| `0x3000` | **9900 ms** | 480 ms | ⚠ **20× za często** |
| `0x3205` | **3960 ms** | 320 ms | ⚠ **12× za często** |
| `0x320F` | 1980 ms | 480 ms | ⚠ 4× za często |
| `0x3200` | 1980 ms | 1480 ms | 34 % za często |
| `0x3201` | 247 ms | 280 ms | 13 % za rzadko |
| `0x3202` | 99 ms | 120 ms | 21 % za rzadko |
| `0x1200` | 495 ms | 480 ms | ✅ w porządku |
| `0x3203` | 3960 ms | tylko na żądanie | ⚠ brak cyklu |
| `0x3204` | **50 ms** | **nie wysyłamy** | ⚠ najszybsza ramka pulpitu |
| `0x320E`, `0x3210`, `0x3213` | 990 ms | **nie wysyłamy** | ⚠ |
| `0x1201`–`0x1204` | 495 ms | **nie wysyłamy** | ⚠ rodzina status/event |
| `0x3206`–`0x320A` | 99–197 ms | **nie wysyłamy** | ⚠ |

### To bezpośrednio łączy się z FW-132

`0x3000` i `0x3205` to razem **kilkanaście razy więcej ramek, niż wyświetlacz oczekuje**.
Kolejka nadawcza ma 16 miejsc i przy przepełnieniu **cicho wyrzuca ramki** (FW-132 §2.3).
Czyli częściowo **sami sobie zapychamy kolejkę ramkami, których nikt nie potrzebuje tak często**,
i gubimy przez to te, które są istotne.

Samo zwolnienie `0x3000` i `0x3205` do wartości fabrycznych zmniejsza ruch, który generujemy,
o **rząd wielkości** — bez dopisywania ani jednej nowej funkcji.

---

## 3. Czego wyświetlacz może od nas oczekiwać, a nie dostaje

Nie twierdzę, że każda brakująca ramka jest konieczna — dokument sam oznacza wiele z nich jako
`SEMANTIC_UNKNOWN`. Ale trzy zasługują na uwagę:

- **`0x3204`, 50 ms** — najszybsza ramka pulpitu w całej tabeli. Coś, co fabryka odświeża
  20 razy na sekundę, jest prawie na pewno wielkością „żywą" (moc/prąd/stan). Jej brak może być
  powodem, dla którego część wskazań na HMI wygląda na zamrożone albo skokowe.
- **`0x1201`–`0x1204`, 495 ms** — dokument klasyfikuje rodzinę `0x120x` jako kanał
  **status/fault/event**, przetwarzany przez HMI osobną ścieżką. Wysyłamy z niej tylko `0x1200`.
- **`0x3203`, 3960 ms** — u nas odpowiadamy tylko na zapytanie. Fabryka nadaje cyklicznie.

---

## 4. Konfiguracja — co naprawdę dociera do silnika

Prześledziłem każdą wartość od ramki CAN do miejsca, w którym wpływa na moment. Wynik jest
mieszany i **tłumaczy dokładnie to, co zgłaszasz**.

| Ustawienie | Skąd | Czy dociera | Uwaga |
|---|---|---|---|
| **Assist ratio per poziom** | `Para0` → `MP.TQO_threshold[]` | ❌ **NIE** | parsowane, odsyłane, **zero konsumentów w całym firmware** |
| **Limit prądu per poziom (%)** | `Para1[41+]` → `assist_settings[lvl][0]` | ❌ **NIE** | liczy się do `phase_current_max_scaled`, ale w gałęzi assist `ride_control.c:624` **nadpisuje** to globalnym limitem |
| **Max power (sufit fazowy)** | `Para1[9]` → `phase_current_max` | ✅ **TAK** | → `ride_core_iq_limit` → sufit wspomagania |
| **Limit prądu baterii** | `Para1[1]` → `battery_current_max` | ✅ **TAK** | → `battery_iq_cap` |
| **Obwód koła** | `Para1` → `wheel_cirumference` | ✅ **TAK** | prędkość **i** przebieg |
| **Średnica koła (kod)** | `0x3203` bajty 2–3 | ⚠ tylko metadana | świadomie (FW-076): prędkość liczy się **wyłącznie** z obwodu |
| **Limit prędkości per poziom** | `assist_settings[lvl][1]` | ✅ **TAK** | `speedlimitx100_scaled` |
| **Akceleracja / ride mode** | `Para0` → `assist_settings[lvl][2]` | ⚠ **działa, ale znaczy co innego** | ustawia `MS.TQfilter`, czyli **przesunięcie bitowe filtra momentu**, a nie rampę |

### 4.1 Dlaczego „assist nie działa"

Dwa niezależne powody, oba potwierdzone:

1. **Assist ratio z `Para0` nie ma ani jednego czytelnika.** Wartość jedzie z aplikacji, jest
   zapisywana, poprawnie odsyłana przy odczycie — i nic jej nie używa. Wygląda jak działające
   ustawienie, bo round-trip jest poprawny.
2. **Procentowy limit per poziom jest liczony i wyrzucany.** `phase_current_max_scaled` powstaje
   w `main.c:1290`, trafia jako `iq_scale`, ale w gałęzi wspomagania `ride_control.c:624` robi:
   ```c
   dynamics_iq_scale = input->ride_core_iq_limit;   // globalny sufit, nie wartość per poziom
   ```
   Poziom wpływa więc dziś **wyłącznie** przez nasz własny system banków (`assist_modes`), nie
   przez pola protokołu Bafanga.

To nie jest przypadek — tak wyszło po FW-129, gdy żądanie wspomagania przeszło na **moc
fizyczną** liczoną w `assist_modes`. Stary tor procentowy został, ale przestał być podłączony.

### 4.2 Dlaczego „max power nie działa" — najpewniej mylące nazwy pól

Max power **działa**, ale przez pole, które w aplikacji nazywa się inaczej:

```text
Para1[9]  "Max Current on Low Charge"  -> u nas SUFIT PRĄDU FAZOWEGO (max power)
Para1[1]  "Battery current limit"      -> u nas limit prądu baterii
```

Jeżeli ustawiasz „max power" tam, gdzie się go spodziewasz po nazwie, **trafiasz w inne pole**.
To jest do naprawienia przede wszystkim **w opisach Canable**, a nie w firmware.

### 4.3 Akceleracja — masz rację, jest realizowana inaczej

Pole „Acceleration" z `Para0` ustawia u nas `MS.TQfilter`, używany jako przesunięcie bitowe
w filtrze momentu (`torque_cumulated >> TQfilter`). To **wygładzanie sygnału nacisku**, a nie
narastanie mocy.

Rzeczywiste narastanie i opadanie mocy siedzi dziś w **rampach per poziom w bankach**
(`iq_rise_slow_ms` / `iq_rise_fast_ms` / `iq_fall_*`), wprowadzonych w FW-069.

Czyli fabryczne pole i nasz mechanizm to **dwie różne rzeczy o tej samej nazwie**. Podpięcie
`Para0` „Acceleration" pod nasze rampy jest możliwe, ale to **decyzja projektowa**, nie naprawa
błędu — trzeba by ustalić, jak trzy fabryczne poziomy mają się mapować na cztery czasy rampy.

---

## 5. Co proponuję — w kolejności zysku do ryzyka

| # | Zmiana | Zysk | Ryzyko |
|---|---|---|---|
| **1** | **Poprawić okresy do fabrycznych** (`0x3000` 9900, `0x3205` 3960, `0x320F` 1980, `0x3200` 1980, `0x3201` 247, `0x3202` 99) | mniej ruchu o rząd wielkości, mniejsze ryzyko przepełnienia kolejki, zgodność z fabryką | **niskie** — same stałe czasowe |
| **2** | **Naprawić opisy pól w Canable** (max power = `Para1[9]`, nie „Battery current limit") | „max power" zaczyna działać z punktu widzenia użytkownika, bez zmian w firmware | **zero** |
| **3** | **Podpiąć limit prądu per poziom** — użyć `assist_settings[lvl][0]` jako mnożnika sufitu w gałęzi assist | poziomy assist zaczynają działać przez protokół Bafanga | **średnie** — dotyka toru wspomagania, wymaga jazdy |
| **4** | **Podpiąć assist ratio** (`TQO_threshold`) do żądania mocy | pełna zgodność z fabrycznym znaczeniem poziomów | **średnie–duże** — trzeba zdecydować, jak ma się to zsumować z bankami |
| **5** | Dodać `0x3204` (50 ms) i rodzinę `0x1201`–`0x1204` | pełniejsza współpraca z HMI | **niskie**, ale najpierw trzeba wiedzieć, co w nich wysłać |
| **6** | Podpiąć „Acceleration" pod rampy per poziom | jedno znaczenie zamiast dwóch | **decyzja projektowa**, nie naprawa |

**Rekomendacja: 1 + 2 jako pierwszy krok.** Razem to zestaw stałych czasowych plus opisy w UI —
zero ryzyka dla sterowania, a rozwiązuje największy problem zgodności i realnie odciąża kolejkę.

Punkty 3 i 4 to **konflikt dwóch systemów konfiguracji** (Bafang `Para0/Para1` kontra nasze banki)
i zasługują na osobną kartę z jawną decyzją: **który system jest nadrzędny**. Nie da się mieć obu
naraz bez reguły pierwszeństwa — a dziś mamy oba i wygrywa nasz, po cichu.

---

## 6. Czego nie wiem

1. **Co dokładnie wysyłać w `0x3204`, `0x320E`, `0x3210`, `0x3213`, `0x1201`–`0x1204`.**
   Dokument oznacza je jako `SEMANTIC_UNKNOWN`. Wysłanie czegokolwiek „żeby były" może być gorsze
   niż niewysyłanie — HMI mogłoby pokazać złe dane zamiast żadnych.
2. **Czy HMI faktycznie cierpi z powodu tych braków.** Rozbieżność z fabryką jest faktem; związek
   z konkretnym objawem na wyświetlaczu jest hipotezą.
3. **Czy zmiana okresów czegoś nie zepsuje.** Wyświetlacz mógł się przyzwyczaić do naszego
   szybszego `0x3000`. Uważam to za mało prawdopodobne (wartości fabryczne są bezpieczne z definicji),
   ale to zmiana zachowania i wymaga sprawdzenia.
4. **Jak mapować trzy fabryczne poziomy „Acceleration" na cztery nasze czasy rampy.** Bez decyzji
   właściciela to zgadywanka.

---

## 7. Pytanie do decyzji, zanim ruszę punkty 3–4

**Który system konfiguracji ma być nadrzędny?**

- **A. Bafang `Para0/Para1` jako źródło prawdy** — pełna zgodność z fabrycznymi narzędziami
  i BESST, ale nasze banki (5 poziomów × tryby × rampy × Extended Boost) trzeba by podporządkować
  albo wyłączyć.
- **B. Nasze banki jako źródło prawdy** — zostaje dzisiejsza funkcjonalność, a pola Bafanga
  pozostają czytelne i odsyłane, ale jawnie oznaczone w Canable jako **nieaktywne**.
- **C. Podział** — Bafang steruje globalnymi limitami (max power, prąd baterii, prędkość, koło),
  banki sterują charakterem wspomagania per poziom.

**Moja rekomendacja: C.** Globalne limity są jednoznaczne i już dziś działają przez `Para1`;
charakter wspomagania w bankach jest bogatszy niż to, co `Para0` potrafi wyrazić, więc
podporządkowanie go trzem bajtom byłoby regresją funkcjonalną.

Przy C punkt 3 sprowadza się do jednej decyzji: czy procent per poziom z `Para1[41+]` ma
**mnożyć** sufit banku, czy go **zastępować**.

---

# ANEKS (2026-09-04) — log ON/WA/OFF z fabrycznego M510 na tym samym HMI

Właściciel dostarczył sniff CAN z fabrycznego sterownika M510 współpracującego z **tym samym
wyświetlaczem**, obejmujący włączenie → Walk Assist → wyłączenie. To pierwszy materiał, który
pozwala odpowiedzieć na pytanie o migającą ikonę **dowodem, a nie rozumowaniem**.

## A1. Oś czasu Walk Assist

Żądanie trybu (HMI → sterownik, `0x83106300`, bajt 1):

```text
869983.2 s   05 06 20 01   <- byte1 = 06 = WALK ASSIST, tryb WŁĄCZONY
869999.5 s   05 00 00 01   <- byte1 = 00, tryb WYŁĄCZONY
             czyli okno TRYBU trwa ~16,3 s
```

Licznik w ramce `0x82F83210`, bajty 4–5 (LE16):

```text
869985.6 s   F8 47   (powtarzane 9x - STAŁE)
869986.6 s   F9 47   <- START przyrostu
869987.6 s   FA 47
   ...          +1 co ~1,00 s ...
869996.7 s   04 48   <- KONIEC przyrostu
             potem znowu STAŁE do końca logu
             czyli okno PRZYROSTU trwa ~10,1 s
```

**Okno przyrostu leży WEWNĄTRZ okna trybu**: zaczyna się 3,4 s po włączeniu trybu i kończy 2,8 s
przed jego wyłączeniem.

## A2. Co to znaczy

Licznik **nie śledzi trybu — śledzi RUCH**. Przyrasta dokładnie o 1 na sekundę, więc jest to
licznik **czasu jazdy w sekundach**; stoi, gdy rower stoi, mimo trzymanego przycisku.

A to jest dokładnie opis zachowania ikony podany przez właściciela:

```text
wejście w tryb   -> ikona SIĘ POJAWIA     (HMI wie sam, sam wysłał 6300[1]=06)
rower jedzie     -> ikona ZACZYNA MIGAĆ   (pokrywa się z oknem przyrostu licznika)
```

**Hipoteza „HMI miga samo z siebie" jest tym obalona.** Istnieje sygnał od sterownika, który
zmienia się dokładnie w oknie migania — a my go **nigdy nie wysyłamy**.

## A3. M510 nadaje inny zestaw ramek niż my

W całym logu fabryczny sterownik nadaje wyłącznie:

```text
0x3000, 0x320E, 0x320F, 0x3210, 0x3213, 0x1200, 0x1203, 0x1204
```

**Ani razu `0x3200`, `0x3201`, `0x3202`, `0x3204`, `0x3205`** — czyli całą rodzinę, na której
opiera się nasza komunikacja. To „stary" harmonogram (§11 dokumentu referencyjnego); my nadajemy
„nowy" (§10).

Część wspólna z nami to **tylko `0x1200`, `0x320F`, `0x3000`**. Wszystko pozostałe, co ten
wyświetlacz dostaje od fabrycznego sterownika, od nas nie dostaje nigdy.

Ponieważ ten sam HMI działa z oboma rodzinami, wniosek jest taki, że **wyświetlacz akceptuje oba
zestawy** — ale funkcje zależne od ramek spoza części wspólnej działają tylko z tym sterownikiem,
który je nadaje.

## A4. Log potwierdza okresy z FW-133

Niezależne potwierdzenie, że poprawka okresów idzie w dobrą stronę:

| Ramka | W logu M510 | FW-133 ustawia | Ocena |
|---|---:|---:|---|
| `0x3000` | **10,01 s**, bajt0 rośnie 01 → 02 | 9,92 s, bajt0 rośnie | ✅ zgodne co do treści i okresu |
| `0x320F` | ~2,3 s | 2,00 s | ✅ |

Nasza zawartość `0x3000` (licznik sesji +1 co 10 s) okazuje się **identyczna z fabryczną**.

## A5. Rozbieżność, którą log potwierdza

Fabryczny `0x320F` w tym logu to **osiem zer**:

```text
ID:82F8320F  DLC:8  Data:00 00 00 00 00 00 00 00
```

My wysyłamy tam `0x01` w bajcie 0 („status active"). To był nasz domysł, nie fakt — log pokazuje,
że fabryka trzyma tam zera.

## A6. Najlepszy kandydat na wyzwalacz migania

```text
0x3210, DLC 8, co ~1 s
bajty 0..1  38 04     (LE16 = 1080)      - stałe
bajty 2..3  92 10     (LE16 = 4242)      - stałe
bajty 4..5  F8 47 ->  (LE16 = 18424+)    - +1 na sekundę TYLKO gdy rower jedzie
bajty 6..7  00 00                        - zera
```

18424 sekund to ~5,1 godziny — wielkość rzędu **całkowitego czasu jazdy**.

**Próba jest tania i nieinwazyjna:** nadawać `0x3210` co ~1 s z licznikiem sekund, który
przyrasta wyłącznie gdy rower jedzie. To nowa ramka broadcast, zero wpływu na sterowanie
silnikiem. Jeżeli ikona zamruga — temat zamknięty; jeżeli nie — kolejni kandydaci to brakujące
`0x1203`/`0x1204` oraz wyzerowanie naszego `0x320F`.
