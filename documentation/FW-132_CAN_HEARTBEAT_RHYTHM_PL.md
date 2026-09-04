# FW-132 — rytm podtrzymania HMI, ownership 0x3005 i widoczność zgubionych ramek

**Status: WDROŻONE W KODZIE, NIE ZBUDOWANE (build kanoniczny), NIE NA ROWERZE.**
Data: 2026-09-04.
Podstawa: `EVistDrive_CAN_HMI_UPDATE_SHUTDOWN_WYTYCZNE_DLA_AGENTA.md` (P0, P1) +
`RAPORT_CAN_HMI_HEARTBEAT_AUDYT_PL.md` (audyt z tej sesji).

**Zakres:** wyłącznie tor CAN / HMI / power. **Nie tknięto** sterowania silnikiem — ani momentu,
ani Assist, ani ramp, ani PI, ani FOC, ani kąta, ani Walk Assist. To warunek z rozdziału 1 karty
wytycznych i jest spełniony.

---

## 1. Problem odczuwany

1. Aktualizacja wyświetlacza kończy się błędem komunikacji na HMI.
2. Podtrzymanie („heartbeat") do HMI bywa urywane.
3. Przycisk on/off reaguje raz szybciej, raz wolniej.

---

## 2. Co znalazłem — trzy przyczyny, wszystkie potwierdzone w kodzie

### 2.1 Aktualizacja wyświetlacza restartowała nasz kontroler

Handler `0x3005` („skok do bootloadera") stał **poza** bramką adresata `target == 2`. Updater
DP-C245 ogłasza sesję ramką `0x85FF3005`, a rozkładając to id regułą z naszego własnego pliku:

```text
command = 0x3005
target  = (0x85FF3005 >> 19) & 0x1F = 31   -> BROADCAST
```

Czyli rozgłoszenie przeznaczone dla wszystkich węzłów **restartowało nasz sterownik w środku
transferu**. To wprost tłumaczy objaw 1.

### 2.2 Rytm podtrzymania gubił całe okresy

Wszystkie ramki do HMI wychodzą z jednej gałęzi „wolnej pętli" co 40 ms. Podstawa czasu jest
dokładna — licznik idzie w takcie 4 kHz — ale warunek sprawdza **pętla główna**, a licznik był
**zerowany**:

```c
if (slow_loop_counter > 160) { ...wyślij... slow_loop_counter = 0; }
```

Spóźnienie pętli o 100 ms → licznik dochodzi do 400 → gałąź wykonuje się **raz** → zero.
**Dwa okresy przepadają i nigdy nie są odrabiane.** Rytm przesuwa się trwale, błąd się nie
kompensuje.

### 2.3 Zgubione ramki nie zostawiały śladu

Ramki podtrzymania nie idą prosto na magistralę, tylko do kolejki na **16 miejsc**. Przy
przepełnieniu ramka **przepada po cichu**. Licznik zgubionych istniał w `can_tx_queue.c`
od zawsze — i **nikt go nie czytał**. Ani jedna ramka diagnostyczna, ani jeden log.

Czyli nie dało się odróżnić „HMI nie dostało ramki" od „HMI ją zignorowało".

Kandydat na przepełnienie: odczyt banku profilu przez Canable to ~37 ramek przy pojemności 16.

### 2.4 Skutek uboczny 2.2 — przycisk on/off

Przycisk jest próbkowany w **tej samej** gałęzi 40 ms, a wyłączenie wymaga 62 kolejnych próbek
(≈2,48 s). Gdy gałąź pływa, pływa też czas przytrzymania. To tłumaczy objaw 3, bez żadnego
osobnego błędu w obsłudze przycisku.

---

## 3. Co zmieniono

### 3.1 `0x3005` — ignorowany tylko jako broadcast

```c
if(Ext_ID_Rx.command==0x3005){
    ...policz i zapamiętaj adresata...
    if(Ext_ID_Rx.target!=31){ NVIC_SystemReset(); }
}
```

**Świadomie NAJWĘŻSZA możliwa poprawka.** Karta wytycznych chce docelowo `target == 2`, ale sama
nakazuje *„najpierw potwierdzić z logów, która ramka rozróżnia controller update od HMI update.
Nie zgadywać."*

Gdyby updater **kontrolera** też ogłaszał się broadcastem, wymaganie `target == 2` **zepsułoby
naszą własną aktualizację firmware**. Blokada samego broadcastu usuwa szkodę i nie rusza żadnej
adresowanej ścieżki. Licznik z §3.3 zbierze dowody, żeby zacieśnić to później na podstawie logu,
a nie przypuszczenia.

### 3.2 Rytm — odzyskiwanie małych poślizgów, odrzucanie dużych

```c
if(slow_loop_counter < 2U*SLOW_LOOP_TICKS) slow_loop_counter -= SLOW_LOOP_TICKS;
else slow_loop_counter = 0;
```

- **poślizg do jednego okresu** → odejmujemy okres, następne przejście przychodzi wcześniej
  o tyle, o ile to się spóźniło; **średnia częstotliwość wraca do nominalnej**;
- **poślizg większy** → **odrzucamy świadomie**. Nadrabianie czterech zgubionych okresów czterema
  seriami ramek zalałoby 16-miejscową kolejkę i wyrzuciło dokładnie te ramki, które miało uratować.
  Duże poślizgi trafiają do licznika szczytowego zamiast być zamiatane pod dywan.

Próg jest tak dobrany, że **gałąź nie może zapalić się dwa razy pod rząd**: po odjęciu okresu
licznik zawsze ląduje poniżej progu.

Przy okazji `160` dostało nazwę `SLOW_LOOP_TICKS` — dwa miejsca zależą teraz od tej samej liczby.

### 3.3 Widoczność — nowa ramka diagnostyczna `0x0001022B`

Wszystkie wartości są **narastające**, i to jest celowe: agregat diagnostyczny to **jedna migawka
na sesję, nie strumień** (pułapka opisana w `WALK_ASSIST_DZIALANIE.md`). Suma narastająca
odpowiada na pytanie z jednej migawki; wartość chwilowa by nie odpowiedziała.

| Pole | Znaczenie |
|---|---|
| Data1 | ramki **zgubione** przez kolejkę od startu |
| Data2 | **najgorszy poślizg** gałęzi 40 ms (w taktach 4 kHz; 160 = na czas, 320 = cały okres spóźnienia) |
| Data3 | liczba ramek `0x3005` |
| Data4 hi | adresat **ostatniej** `0x3005` (31 = broadcast updatera, który teraz ignorujemy) |
| Data4 lo | stan watchdoga: bit0 uzbrojony, bit1 po cięciu wspomagania, bit2 po progu wyłączenia |

Liczba ramek agregatu podniesiona 16 → 17 (`_Static_assert` pilnuje, pojemność to 21).

---

## 4. Czego ŚWIADOMIE nie zmieniono

**Watchdog `target == 2` zostaje jak jest — na razie.**

Warunek „HMI żyje" sprawdza dziś *„ktoś mówi do kontrolera"*, a nie *„mówi HMI"*; pole `source`
jest odczytywane i nieużywane. Karta słusznie nazywa to błędem logicznym (P1).

Ale to jedyna zmiana z tej trójki, która **zaczęłaby wyłączać rower w sytuacji, w której dziś tego
nie robi**: przy odpiętym HMI i podpiętej aplikacji Canable watchdog dziś jest podtrzymywany przez
aplikację, po zmianie przestanie. To jest realna zmiana zachowania w warsztacie i zasługuje na
własną kartę i własną jazdę, a nie na doklejenie do poprawki rytmu.

Nie ruszono też: stanów HMI lost / HMI update / controller update (P3–P6), terminalnego shutdownu
(P5, `power_off_controller()` nadal wraca zamiast kończyć), ani domyślnego poziomu assist.

---

## 5. Ryzyko

| Zmiana | Ryzyko | Dlaczego |
|---|---|---|
| 3.1 `0x3005` | **niskie** | tylko broadcast; każda adresowana ścieżka bez zmian |
| 3.2 rytm | **niskie–średnie** | zmienia też czas przytrzymania on/off — powinien się **ustabilizować**, ale to zmiana odczuwalna i trzeba ją sprawdzić |
| 3.3 diagnostyka | **zero** | tylko build DIAG, tylko odczyt istniejących liczników |

## 6. Stan budowy

Kompilacja sprawdzona w trybie deweloperskim dla **obu** wariantów — `RESULT: PASS`,
numer wersji **nie zużyty**, katalog tymczasowy usunięty:

```text
NORMAL   FLASH 109 048 B   RAM 25,98 %
DIAG     FLASH 158 296 B   RAM 99,04 %   (472 B wolne - ciasno)
```

Pełny przebieg hostowy: **bez nowych awarii**, zostaje wyłącznie znana rodzina diag schema-3.

**Build kanoniczny nie został wykonany** — czeka na polecenie.

---

## 7. Testy

- **T1 — aktualizacja wyświetlacza.** Przed: kontroler się restartuje. Po: ma przejść bez resetu.
  Jedyny test wymagający realnej aktualizacji HMI.
- **T2 — rytm.** Log CAN: odstępy między `0x02F83000` mają trzymać 480 ms. Porównać rozrzut przed
  i po. W buildzie DIAG dodatkowo `0x1022B` Data2 pokaże najgorszy poślizg liczbą.
- **T3 — zgubione ramki.** Odczyt banku przez Canable podczas jazdy; jeśli `0x1022B` Data1 rośnie,
  hipoteza z §2.3 potwierdzona i wiemy, gdzie szukać dalej.
- **T4 — on/off.** Stoperem: ile trzeba trzymać przycisk. Ma być stabilne ~2,5 s.
- **T5 — regresja.** Zwykła jazda: nic w zachowaniu silnika nie ma się zmienić. Ta karta nie tyka
  sterowania, więc każda zmiana odczucia jest sygnałem, że coś przeoczyłem.
- **T6 — aktualizacja kontrolera.** ⚠ **Sprawdzić, że nadal działa.** Jeżeli przestała, to znaczy,
  że nasz updater ogłasza się broadcastem — wtedy `0x1022B` Data4 hi poda adresata i będziemy
  wiedzieli, jak to zawęzić poprawnie.
