# Build 0.508 — co obserwować po wgraniu

Data: 2026-09-05.

```text
0.508  NORMAL  110 012 B  RAM 26,0 %  SHA-256 DEC75BAEC23DFD48BFEF64BBA39E1E98CCD8C2B12DB77EE55860319CF4B8DB78
0.509  DIAG    159 376 B  RAM 99,1 %  SHA-256 5501255DA334E6540641202F91F36EB6119B317DE0AA09122E7F1DF35027AE5B
```

**Wgrywasz 0.508.** Na rowerze masz 0.506.

---

## To nie jest zwykła poprawka — przeczytaj to jedno zdanie

Zmieniliśmy jedną liczbę: **prędkość wirnika teraz maleje, gdy wirnik staje**, zamiast zamarzać
na ostatnim odczycie. Ale od tej liczby zależy **pięć mechanizmów, które przez cały czas istnienia
tego firmware nie zadziałały ani razu**, bo ich warunek był nieosiągalny.

Wszystkie pięć budzi się naraz. **Nikt nigdy nie widział ich w ruchu.**

| Mechanizm | Co ma robić | Gdzie to poczujesz |
|---|---|---|
| **FW-041** preload przekładni | wybrać luz po cichu przed przyłożeniem momentu | **start** |
| **smooth start** | złagodzić rozruch z postoju | **start** |
| **FW-048** odcięcie prądu | przestać podawać prąd, zanim kąt komutacji skoczy | **zatrzymanie** |
| **QZERO** oddanie osi | oddać oś regulatorom przed tym samym skokiem | **zatrzymanie** |
| licznik ruchu | przestać tykać po zatrzymaniu | ikona WA na HMI |

---

## KROK 1 — koło w górze, PRZED jazdą

Poprawka dotyka sygnału, którego używa komutacja. **To jest obowiązkowe.**

1. koło w górze, sterownik zasilony;
2. powoli obracaj koło ręką **w obie strony**;
3. ma być gładko, bez zacięć i trzasków;
4. krótkie naciśnięcie Walk Assist — ma ruszyć i utrzymać obroty jak dotąd.

**Cokolwiek zgrzyta albo silnik nie rusza — wróć na 0.506 i przerwij.**

---

## KROK 2 — sprawdź liczby, zanim ruszysz

Zakładka Debug: `CAN ID` = `05116032`, `Data` puste, **Send Custom**. Odpowiedź `822A6032`
w Snifferze przy filtrze `ALL TRAFFIC`.

Zakręć kołem ręką i puść, żeby wirnik wybiegł do zera. Potem odczytaj:

```text
bajty 6..7  (najnizsze erps na wybiegu)
   przed poprawka:  24 00   czyli 24  <- nigdy nie schodzilo nizej
   po poprawce:     ma byc BLISKO ZERA
```

**To jest jedyny test, który potwierdza, że poprawka w ogóle działa.** Jeśli tam nadal będzie 24
albo więcej — reszta listy nie ma sensu, dawaj znać.

---

## KROK 3 — START. Tu budzą się dwa mechanizmy

Pięć ruszeń z pełnego postoju, za każdym razem odczekaj, aż silnik ucichnie.

- **czy klik przy starcie osłabł albo zniknął?** To robota preloadu przekładni — jego opis w
  kodzie to dosłownie „wybierz luz po cichu zamiast wyrwać go jednym klapnięciem";
- **czy start nie stał się zbyt miękki albo ospały?** Dwa mechanizmy łagodzące start włączyły się
  jednocześnie. Jeśli rower zaczął ruszać leniwie — powiedz, bo to znaczy, że nakładają się
  i trzeba jeden przystopować;
- **czy start jest powtarzalny?** Pięć razy podobnie, czy losowo.

Zapis w stylu `klik 1/5, start trochę wolniejszy` w zupełności wystarczy.

---

## KROK 4 — ZATRZYMANIE. Tu budzą się kolejne dwa

- **czy klik na końcu zatrzymania osłabł albo zniknął?** To jest cel całej serii od FW-131;
- **czy wybieg się zmienił?** FW-048 odcina teraz prąd przed strefą skoków kąta, więc ostatni
  kawałek zatrzymania może być dłuższy i cichszy;
- **czy nie pojawił się nowy dźwięk w momencie odcięcia?** Odcięcie następuje ~30 ms po
  zatrzymaniu — jeśli usłyszysz coś nowego w tym miejscu, to jest właśnie ono.

---

## KROK 5 — Walk Assist

Sprawdziłem osobno i **nie powinien się zmienić w ogóle**: Walk Assist ma własny pomiar prędkości
i nie korzysta z tej liczby. Ale skoro sprawdzamy, to potwierdź:

- siła i utrzymywanie obrotów jak dotąd;
- brak wypadania trybu.

Gdyby cokolwiek się zmieniło — to by znaczyło, że pomyliłem się co do niezależności i chcę
o tym wiedzieć od razu.

---

## KROK 6 — zwykła jazda

Wspomaganie, narastanie mocy, wygaszanie. **Poza startem i zatrzymaniem nic nie ma prawa się
zmienić** — sufit prędkości z założenia nie działa, dopóki zbocza Halla przychodzą na czas.

Jeśli poczujesz różnicę w środku jazdy, przy stałej prędkości — to sygnał, że pas ochronny 2×
jest za wąski i chcę o tym usłyszeć.

---

## KROK 7 — po jeździe, przed wyłączeniem

Jeszcze raz `05116032`. Teraz interesują dwie rzeczy:

```text
bajty 0..1   ile bylo zwolnien pedalow    (mianownik)
bajty 2..3   szczyt pradu w wybiegu       (bylo 76 = 7,2 A na 0.506)
```

Jeśli szczyt prądu **wyraźnie spadł**, to znaczy, że FW-048 faktycznie zaczął odcinać prąd przed
strefą kliku — i wtedy klik i ten prąd są powiązane. Jeśli prąd spadł, a klik **został**, to
mechanika, i zamykamy temat w firmware.

---

## Minimum, jeśli masz mało czasu

**KROK 1** (koło w górze), **KROK 2** (czy najniższe erps zeszło blisko zera — bez tego nic
innego nie znaczy nic), **KROK 3 i 4** (klik na starcie i przy zatrzymaniu).
