# Raport z audytu — CAN / heartbeat HMI / on-off / restart kontrolera

Data: 2026-09-04. Zakres zlecony przez właściciela: sprawdzić kartę
`EVistDrive_CAN_HMI_UPDATE_SHUTDOWN_WYTYCZNE_DLA_AGENTA.md`, zbadać **urwane podtrzymanie
heartbeat**, sprawdzić **czy wysyłanie CAN jest urwane albo czy częstotliwość została zaburzona**,
i przygotować **raport przed planem naprawy**.

**Nic nie zmieniałem w kodzie.**

---

## 1. Odpowiedź na pytanie wprost

**Wysyłanie CAN nie jest urwane — ale jego CZĘSTOTLIWOŚĆ JEST zaburzona, i to strukturalnie.**
Znalazłem dwa niezależne mechanizmy, przez które ramki podtrzymujące mogą się spóźniać albo
znikać, oba potwierdzone w kodzie, oraz trzeci, który tłumaczy reset kontrolera przy aktualizacji
wyświetlacza.

---

## 2. Fakty potwierdzone w kodzie

### 2.1 Ramki podtrzymujące mają zgubny licznik okresu — POTWIERDZONE

Wszystkie ramki do HMI są wysyłane z jednej gałęzi „wolnej pętli":

```c
if (slow_loop_counter > 160){        //main.c:1321  — 160/4000 Hz = 40 ms
    ...
    if(++hb_tick    >=12){hb_tick=0;    sendCAN_status_broadcast(&MS);}  //480 ms
    if(++speed_tick >= 7){speed_tick=0; sendCAN_Poll(&MP,&MS,0x3201);}   //280 ms
    if(++cad_tick   >=37){cad_tick=0;   sendCAN_Poll(&MP,&MS,0x3200);}   //1480 ms
    if(++misc_tick  >= 8){misc_tick=0;  sendCAN_Poll(&MP,&MS,0x3205);}   //320 ms
    if(++s202_tick >= 3){s202_tick=0;   sendCAN_3202();}                 //120 ms
    ...
    slow_loop_counter = 0;           //main.c:1389
}
```

`slow_loop_counter` narasta w takcie sterowania 4 kHz, więc podstawa czasu jest dokładna.
**Ale warunek jest sprawdzany w pętli głównej, a licznik jest ZEROWANY, a nie pomniejszany
o okres.**

Konsekwencja: jeżeli pętla główna spóźni się o 100 ms, licznik dojdzie do 400, gałąź wykona się
**raz**, i zostanie skasowana do zera. **Dwa okresy przepadają bezpowrotnie.** Nadmiar nie jest
odrabiany, więc rzeczywisty rytm to „40 ms **albo więcej**, nigdy mniej", a błąd się nie kompensuje.

Poprawny wzorzec to `slow_loop_counter -= 160;` — wtedy spóźnienie jest odrabiane, a średni okres
pozostaje 40 ms.

**To jest dokładnie „zaburzona częstotliwość", o którą pytasz.** Nie sporadyczny błąd, tylko
własność konstrukcji: każde spowolnienie pętli głównej trwale przesuwa cały rytm podtrzymania.

### 2.2 Kolejka nadawcza cicho gubi ramki — POTWIERDZONE

`sendCAN_status_broadcast()` nie nadaje bezpośrednio, tylko wkłada do kolejki
(`can_tx_queue_enqueue`, `CAN_Display.c:847`). Kolejka ma **16 miejsc**
(`CANQ_CAPACITY`, `can_tx_queue.c:6`) i przy przepełnieniu:

```c
if (q_count >= CANQ_CAPACITY) {
    dropped_ctr++;
    return false;                     //can_tx_queue.c:84-88
}
```

Ramka **przepada po cichu**. Licznik zgubionych istnieje (`can_tx_queue_dropped_count()`), ale:

**nikt go nie czyta.** Jedyne trafienie poza samą kolejką to komentarz w `can_multiframe.c`.
Nie ma go w żadnej ramce diagnostycznej, w żadnym logu, w niczym.

Czyli **heartbeat może znikać i nie zostawia po sobie żadnego śladu.** To najpoważniejszy problem
obserwowalności w tym torze: nie da się dziś odróżnić „HMI nie dostało ramki" od „HMI ją
zignorowało".

Kandydat na przepełnienie: odczyt banku profilu przez Canable to **255 bajtów**, czyli około
**37 ramek** przy pojemności 16.

### 2.3 Watchdog „HMI żyje" nie sprawdza, czy to HMI — POTWIERDZONE

```c
if(Ext_ID_Rx.target==2){              //CAN_Display.c:293
    comm_lost_ticks=0;
    comm_seen=1;
```

Warunek mówi „ramka skierowana do kontrolera", a nie „ramka **od HMI**". Pole `source` jest
odczytane linię wyżej i **nie jest użyte** w tym warunku.

Skutek praktyczny, ważny dla nas na co dzień: **podłączony Canable podtrzymuje watchdog.**
Możemy stracić HMI i nigdy się o tym nie dowiedzieć, dopóki aplikacja jest podpięta — czyli
dokładnie w sytuacji, w której diagnozujemy.

Karta nazywa to błędem logicznym i zgadzam się.

### 2.4 Aktualizacja wyświetlacza resetuje nasz kontroler — POTWIERDZONE LICZBOWO

```c
}                                     //CAN_Display.c:649  <- koniec bloku target==2
...
if(Ext_ID_Rx.command==0x3005){        //CAN_Display.c:652  <- POZA tym blokiem
    NVIC_SystemReset();
}
```

Sprawdziłem wcięcia i klamry: handler `0x3005` stoi **poza** bramką `target==2`.

Ramka rozgłoszeniowa updatera HMI to `0x85FF3005`. Rozkładając ją regułą z kodu:

```text
command   = 0x3005
target    = (0x85FF3005 >> 19) & 0x1F = 31   (broadcast, NIE 2)
source    = (0x85FF3005 >> 24) & 0x1F = 5
```

Gdyby handler był wewnątrz bramki, target 31 nigdy by go nie dosięgnął. **Jest na zewnątrz, więc
broadcast do wszystkich węzłów restartuje nasz kontroler w środku aktualizacji wyświetlacza.**

To potwierdza główną hipotezę karty (rozdział 4) — i to nie z rozumowania, tylko z klamer w pliku.

### 2.5 Przycisk on/off jest próbkowany w tej samej, pływającej gałęzi — POTWIERDZONE

```c
if(adc_value[5]<2800)shutoffcounter++;      //main.c:1392, wewnątrz gałęzi 40 ms
else shutoffcounter=0;
if(shutoffcounter>62){ power_off_controller(); }   //62 x 40 ms = 2,48 s
```

Czyli **czas przytrzymania przycisku jest liczony w tych samych, pływających okresach**. Przy
obciążonej pętli głównej wyłączenie wymaga dłuższego trzymania, i to niedeterministycznie.
Nie jest to „zawiesza się", ale jest to „raz reaguje szybciej, raz wolniej" — czyli dokładnie ten
rodzaj zachowania, który użytkownik odbiera jako niesprawny przycisk.

### 2.6 `power_off_controller()` nie jest stanem terminalnym — POTWIERDZONE

```c
void power_off_controller(void){      //main.c:5876
    ...zapis SOC, stop PWM, lifecycle IDLE...
    GPIO_BC(GPIOB) = GPIO_PIN_4;      //DC/DC off
    GPIO_BC(GPIOB) = GPIO_PIN_5;      //Display off
}                                     // <- i wraca do pętli głównej
```

Funkcja **wraca**. Jeżeli zewnętrzny zatrzask zasilania nie odetnie naprawdę (bo trzymasz przycisk,
bo ładowarka trzyma szynę, bo cokolwiek), wykonanie leci dalej: PWM wyłączony, ale pętla główna,
CAN i cała reszta pracują — i **nic nie próbuje ani dokończyć wyłączenia, ani wrócić do sprawnego
stanu**. To jest niezdefiniowana strefa pośrednia.

---

## 3. Jak to składa się w objaw „urwane podtrzymanie heartbeat"

Trzy niezależne mechanizmy, które dają ten sam objaw i **nakładają się**:

```text
pętla główna się spóźnia
   -> okres 40 ms przepada w całości (2.1)
   -> heartbeat wychodzi z opóźnieniem, rytm się przesuwa

kolejka 16 ramek się zapełnia (np. odczyt banku = 37 ramek)
   -> heartbeat zostaje CICHO wyrzucony (2.2)
   -> brak jakiegokolwiek śladu (licznik nieczytany)

updater HMI rozgłasza 0x3005
   -> nasz kontroler się resetuje (2.4)
   -> sesja CAN pada w środku, HMI wraca w błędzie komunikacji
```

A ponieważ **watchdog HMI reaguje na dowolną ramkę do kontrolera (2.3)**, to podczas pracy
z Canable nie zobaczymy, że HMI zamilkło — aplikacja sama podtrzymuje watchdog.

---

## 4. Czego NIE potwierdziłem — otwarte

Piszę jawnie, żeby nie trafiło do planu jako fakt:

1. **Nie zmierzyłem, jak bardzo spóźnia się pętla główna.** Wiem, że opóźnienie jest gubione;
   nie wiem, ile go realnie jest. Bez tego nie wiem, czy 2.1 jest przyczyną główną, czy tylko
   dokłada się do 2.2.
2. **Nie potwierdziłem przepełnienia kolejki na sprzęcie.** Wiem, że jest możliwe i że byłoby
   niewidoczne. To jest hipoteza z dobrym uzasadnieniem, nie pomiar.
3. **Nie wiem, jak długo stock G532 toleruje brak HMI** — karta też oznacza to jako niepewne.
4. **Nie sprawdzałem zachowania bootloadera** po zakończeniu flashowania (rozdział 19 karty
   nazywa to brakującymi danymi).
5. **Nie wiem, czy zapis do flash** (SOC, bank) blokuje pętlę główną na tyle, żeby zgubić okresy.
   Erase strony to zwykle dziesiątki milisekund — jeżeli tak, byłby to czwarty mechanizm, ale tego
   **nie zmierzyłem**.

---

## 5. Co możemy dodać — obserwowalność, zanim cokolwiek naprawimy

Rekomendacja mocna: **najpierw zobaczyć, potem naprawiać.** Wszystkie trzy są tanie i niczego nie
zmieniają w zachowaniu:

| Co dodać | Po co | Koszt |
|---|---|---|
| **licznik zgubionych ramek do ramki diagnostycznej** | dziś zgubiony heartbeat nie zostawia śladu; ten licznik JUŻ ISTNIEJE, tylko nikt go nie czyta | 2 bajty |
| **maksymalne opóźnienie gałęzi 40 ms** (najwyższa wartość `slow_loop_counter` przy wejściu) | zamienia 2.1 z „możliwe" w liczbę: ile okresów naprawdę gubimy i kiedy | kilka bajtów RAM |
| **licznik ramek 0x3005 i ich `target`** | powie, czy broadcast updatera faktycznie do nas dociera i ile razy | 2 bajty |

Bez pierwszego punktu każda naprawa heartbeatu będzie oceniana „na ucho".

---

## 6. Proponowana kolejność naprawy

Zgodna z priorytetami karty, ale przestawiona według **stosunku ryzyka do zysku**, bo dwie rzeczy
są tanie i pewne, a reszta wymaga decyzji.

| # | Zmiana | Dlaczego tu | Ryzyko |
|---|---|---|---|
| **1** | **`0x3005` tylko dla `target == 2`** (P0 karty) | jedna linia, usuwa reset kontrolera przy aktualizacji wyświetlacza; nie ma żadnego powodu, by broadcast nas resetował | **minimalne** |
| **2** | **`slow_loop_counter -= 160` zamiast `= 0`** | jedna linia, przywraca stały średni rytm podtrzymania i deterministyczny czas przycisku on/off | minimalne, ale **dotyka też czasu przycisku** — trzeba sprawdzić na rowerze |
| **3** | **obserwowalność z §5** | żeby ocenić 1 i 2 danymi, a nie wrażeniem | zero |
| **4** | **watchdog na `source == HMI`, nie `target == 2`** (P1 karty) | poprawność logiczna; ale **zmienia zachowanie przy podpiętym Canable** — dziś aplikacja podtrzymuje watchdog, po zmianie przestanie | **średnie** — może zacząć wyłączać rower podczas pracy z aplikacją, jeśli HMI jest odpięte |
| **5** | rozdzielenie stanów HMI lost / HMI update / controller update (P3–P6 karty) | porządek architektoniczny | duże, osobna karta |
| **6** | terminalny shutdown (P5 karty) | domyka 2.6 | średnie, wymaga decyzji co ma się stać, gdy zatrzask nie zadziała |

**Rekomendacja: 1 + 2 + 3 jako jedna mała karta.** Trzy zmiany, z czego dwie to po jednej linii,
wszystkie w torze CAN/power, żadna nie dotyka sterowania silnikiem — czyli spełniają założenie
z rozdziału 1 karty.

Punkt 4 celowo **osobno**, bo jako jedyny z tej trójki może zacząć wyłączać rower w sytuacji,
w której dziś tego nie robi.

---

## 7. Testy bez oscyloskopu

- **T1 — aktualizacja wyświetlacza.** Przed poprawką 1 kontroler się restartuje; po niej ma
  przejść aktualizację bez resetu. To jedyny test, który potrzebuje realnej aktualizacji HMI.
- **T2 — rytm podtrzymania.** Log CAN z jazdy: odstępy między kolejnymi `0x02F83000` powinny
  trzymać się 480 ms. Rozrzut i „przeskoki" pokażą, ile gubimy dziś, i będą miarą poprawki 2.
- **T3 — odczyt banku przez Canable podczas jazdy.** Jeżeli licznik zgubionych ramek (z §5)
  rośnie w tym momencie, hipoteza 2.2 jest potwierdzona.
- **T4 — czas on/off.** Zmierzyć stoperem, ile trzeba trzymać przycisk. Ma być stabilne ~2,5 s
  przed i po poprawce 2; jeżeli dziś pływa, to jest bezpośredni dowód na 2.1.
- **T5 — utrata HMI.** Odpiąć HMI podczas jazdy **bez podpiętego Canable**: wspomaganie ma zniknąć
  po ~3 s, wyłączenie po ~10 s na postoju. To sprawdza dzisiejsze zachowanie **przed** zmianą 4.

---

## 8. Podsumowanie jednym akapitem

Wysyłanie CAN działa, ale **rytm podtrzymania jest zbudowany tak, że gubi okresy przy każdym
spowolnieniu pętli głównej**, a kolejka nadawcza **cicho wyrzuca ramki, gdy się zapełni, i nikt
tego nie liczy**. Do tego watchdog „HMI żyje" w rzeczywistości sprawdza „ktokolwiek do nas mówi",
więc podczas pracy z aplikacją jest ślepy, a handler `0x3005` stoi poza bramką adresata, przez co
**rozgłoszenie z aktualizacji wyświetlacza restartuje nasz kontroler**. Trzy pierwsze poprawki to
łącznie dwie linie kodu i jedna ramka diagnostyczna.
