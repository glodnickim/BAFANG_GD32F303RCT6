
# Pytania potwierdzające — co musimy wiedzieć, żeby domknąć zmiany i wybrać dalszy kierunek

Data: 2026-09-03. Dotyczy zmian: **FW-130/130.1** (Walk Assist), **QZERO-2/3** (cichy stop),
**FW-131** (kanoniczny kąt wirnika).

Każde pytanie ma dopisane, **co zmienia odpowiedź**. Jeżeli odpowiedź niczego nie zmienia,
pytania tu nie ma.

---

## A. Potwierdzenie zmian, które już pojechały

### A1. Czy klik przy ruszaniu naprawdę zniknął po 0.0496?

Napisałeś „wydaje mi się, że start się wyczyścił". Potrzebuję świadomego sprawdzenia: **kilka
startów z pełnego postoju pod rząd**, najlepiej z różną siłą nacisku i na różnych biegach.

**Co zmienia odpowiedź:** jeżeli tak — potwierdza, że klik startowy był uskokiem kąta, i FW-131
jest zamknięty. Jeżeli czasem wraca — uskok nie był jedynym mechanizmem i wracamy do analizy
z konkretnym przypadkiem, w którym wraca.

### A2. Czy po 0.500 klik na końcu zatrzymania zniknął, osłabł, czy jest bez zmian?

To jest jedyny test poprawki QZERO-3. **Stroną A jest 0.498** — ten sam kod bez tej zmiany.

**Co zmienia odpowiedź:**
- zniknął → prąd w strefie skoków był przyczyną, temat zamknięty;
- osłabł → kierunek dobry, zostaje resztka do dострojenia (próg oddania wyżej);
- bez zmian → prąd NIE był przyczyną i trzeba szukać w samych skokach 60° (kierunek D1).

### A3. Czy po 0.500 wybieg silnika po puszczeniu pedałów się wydłużył?

QZERO-3 oddaje ostatni odcinek hamowania. Wyliczyłem, że kosztuje to poniżej 1 % energii, ale
liczenie to nie jest to samo co odczucie.

**Co zmienia odpowiedź:** jeżeli wybieg wyraźnie się wydłużył, poprawka kupuje ciszę za cenę,
której nie chcemy — wtedy zamiast oddawać całkę trzeba oddawać ją **rampą**, żeby hamowanie
trwało dłużej, a prąd i tak zszedł do zera przed strefą skoków.

### A4. Czy przy zwykłej jeździe nic się nie zmieniło na gorsze?

FW-131 dotyka komutacji, czyli wszystkiego. Interesuje mnie **regresja**: czy przy stałej
prędkości nie pojawił się nowy dźwięk, wibracja, szarpanie przy zmianie biegu, słabsze
wspomaganie, albo cokolwiek, co wcześniej było gładkie.

**Co zmienia odpowiedź:** jakikolwiek objaw = natychmiastowy powrót na **0.0494** (stary kąt)
i analiza. To jest najwrażliwsza zmiana w całej serii.

### A5. Czy silnik zachowuje się poprawnie przy cofaniu / kręceniu do tyłu?

FW-131 zmienił źródło znaku kierunku przy niskich obrotach: dawniej brał go z **konfiguracji**
(`MP.reverse`), teraz z **pomiaru**. Przy jeździe do przodu to bez znaczenia, ale przy ruchu
wstecz może się różnić.

**Test:** koło w górze, obrót ręką **do tyłu** przez strefę niskich obrotów; potem ewentualnie
cofnięcie roweru z włączonym sterownikiem.

**Co zmienia odpowiedź:** zacinanie lub trzask tylko przy ruchu wstecz = znak kierunku wymaga
osobnego potraktowania. To jedyne miejsce, gdzie FW-131 mógł coś pogorszyć.

---

## B. Walk Assist — domknięcie

### B1. Czy przy `25 % / 30 rpm` marsz wystarcza **pod górę** i z obciążonym rowerem?

Testowaliśmy głównie po płaskim. Sufit 157 jednostek (~15 A) to twarde maksimum tego firmware.

**Co zmienia odpowiedź:** jeżeli pod górę brakuje — trzeba podnieść stałą `WA_MOTOR_IQ_ABS_MAX`,
co jest osobną decyzją o bezpieczeństwie (marsz z prądem 25–30 A potrafi wyrwać rower z ręki).
Jeżeli wystarcza — Walk Assist zostaje zamknięty bez dalszych zmian.

### B2. Czy na **lekkim biegu** bezpiecznik prędkości koła ujmuje moc płynnie, czy nadal urywa?

Cel: przy szybszym kole moc ma **maleć**, a nie znikać. Najłatwiej wywołać na najlżejszym biegu.

**Co zmienia odpowiedź:** jeżeli nadal urywa, zjazd 1,5 km/h przed limitem jest za wąski i trzeba
go poszerzyć. Jeżeli płynnie — punkt 3 pierwotnego zgłoszenia jest potwierdzony jako naprawiony.

### B3. Czy podczas Walk Assist **korba się kręci** razem z zębatką?

Pytanie techniczne, nie o odczucie. Od tego zależy, czy odczyt kadencji na HMI w ogóle mierzy
zębatkę.

**Co zmienia odpowiedź:** jeżeli pedały stoją, wszystkie wnioski wyciągnięte z kadencji podczas
marszu trzeba unieważnić i powtórzyć inaczej.

---

## C. Rzeczy niesprawdzone, które wiszą

### C1. Czy nieregularne odczyty prądu i kadencji na HMI występują na buildzie NORMAL, czy tylko na DIAG?

Zgłosiłeś to przy buildzie diagnostycznym, który dokłada ruchu na magistrali.

**Co zmienia odpowiedź:** jeżeli tylko na DIAG — to koszt diagnostyki, nieszkodliwy. Jeżeli także
na NORMAL — coś nie wyrabia się z czasem, a wtedy **wszystkie rampy liczą się nierówno**, bo są
oparte na takcie. Miernikiem jest licznik `missed_control_ticks`.

### C2. Czy po dłuższej jeździe silnik albo sterownik grzeje się bardziej niż wcześniej?

Podnieśliśmy prąd marszu prawie czterokrotnie, a QZERO hamuje prądem.

**Co zmienia odpowiedź:** wzrost temperatury = trzeba przyjrzeć się, ile czasu marsz spędza przy
suficie, i ewentualnie ograniczyć czas trzymania pełnego prądu.

---

## D. Kierunki dalej — do wyboru, nie do zrobienia naraz

### D1. Skoki kąta o 60° przy prawie zatrzymanym wirniku

To jest **własność sprzętu**, nie wada: trzy bity Halla dają sześć pozycji na obrót elektryczny.
Przewodnik Fake Taxi opisuje, jak producent to łagodzi — historia sześciu sektorów (ETAP 3),
zachowanie startowe z re-anchorem (ETAP 5) i indywidualne korekty sektorów (ETAP 7).

**Kiedy warto:** tylko jeżeli A2 wyjdzie „bez zmian", czyli prąd nie był przyczyną. Inaczej to
strojenie jakości, nie naprawa.

### D2. Wygaszenie prądu przy puszczeniu przycisku Walk Assist

Dziś puszczenie to natychmiastowe zero. Fabryczny G532 ma tam ~110 ms wygaszania.

**Kiedy warto:** jeżeli puszczanie przycisku odczuwasz jako szarpnięcie. Jeżeli nie — zostawić.

### D3. Strojenie pasa regulacji marszu

Dziś ±15 % celu. Wiemy, że przy celu 20 silnik trafiał w sam obszar zjazdu mocy.

**Kiedy warto:** jeżeli chcesz marszu wolniejszego niż 30 obr/min i wtedy wraca nierówna praca.

### D4. Commit

Nic z tej serii nie jest zacommitowane: FW-130, FW-130.1, QZERO-2, QZERO-3, FW-131, zmiana
numeracji wersji, dokumentacja, Canable.

**Decyzja:** kiedy commitować. Im dłużej czekamy, tym trudniej będzie odtworzyć, który build
zawierał co.

---

## Minimalny zestaw, jeżeli masz czas tylko na jedną jazdę

**A2** (klik na końcu — jedyny test poprawki 0.500), **A3** (czy wybieg się nie wydłużył),
**A4** (czy nic się nie zepsuło przy zwykłej jeździe). Te trzy zamykają albo otwierają wszystko
inne.
