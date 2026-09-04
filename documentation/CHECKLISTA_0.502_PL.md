# Build 0.502 — co się zmieniło i co sprawdzić po wgraniu

Data: 2026-09-04.

```text
0.502  NORMAL  109 284 B  RAM 25,99 %  SHA-256 6B4903671918509B7533D41E0B71B3AD396F9B4B77FAF904CC8F2520B16BFC79
0.503  DIAG    158 540 B  RAM 99,07 %  SHA-256 AD6991DCFF0C55A1EA58648032E9FCE01345DE57577EBE58A218CFB35115746C
```

**Na rowerze masz teraz 0.500.** Poniżej wyłącznie to, co się względem niego zmieniło.

⚠ RAM w DIAG: **99,07 %**, wolne **456 B**. Bardzo ciasno — kolejna funkcja w buildzie DIAG będzie
wymagała zwolnienia miejsca.

---

## Cztery zmiany w jednym buildzie

| Karta | Czego dotyczy | Co może pójść źle |
|---|---|---|
| **FW-131.1** | kąt wirnika: środek sektora zamiast brzegu, interpolacja dopiero po 2 zboczach | silnik: szarpanie, brak startu, nowy hałas |
| **FW-132** | `0x3005` broadcast ignorowany, rytm 40 ms odzyskuje poślizgi, nowa ramka diag `0x1022B` | aktualizacja **kontrolera**, czas przycisku on/off |
| **FW-133** | okresy CAN do HMI zgodne z fabryką | wypadanie Walk Assist, zamrożone wskazania na HMI |
| **FW-134** | nowa ramka `0x3210` z licznikiem ruchu | nic — sam dodatek; albo zadziała, albo nie |

Zmieniły się też **wartości domyślne Walk Assist** (25 % / 30 obr/min), ale to dotyczy wyłącznie
świeżego banku — Twój sterownik zachowa swoje ustawienia.

---

## KROK 1 — koło w górze, PRZED jazdą

**To jest obowiązkowe.** FW-131.1 dotyka komutacji, a mieliśmy już build, który po zmianie w tej
okolicy wcale nie kręcił silnikiem.

1. koło w górze, sterownik zasilony;
2. **powoli obracaj koło ręką w obie strony** — szczególnie bardzo wolno, w tempie jednego obrotu
   zębatki na 10–15 sekund;
3. silnik ma się kręcić **gładko w obu kierunkach**, bez zacięć i trzasków;
4. krótkie naciśnięcie Walk Assist na uniesionym kole.

**Jeśli cokolwiek zgrzyta, szarpie albo silnik nie rusza — wróć na 0.500 i przerwij testy.**
Winowajcą jest wtedy FW-131.1.

---

## KROK 2 — klik przy starcie i zatrzymaniu (to jest główny cel FW-131.1)

Poprzednio na 0.500 klik był **na obu końcach**.

1. **Pięć startów z pełnego postoju.** Za każdym razem odczekaj, aż silnik ucichnie całkowicie,
   dopiero potem ruszaj. Policz, ile z pięciu kliknęło.
2. **Jeden start po wyłączeniu i włączeniu sterownika.** To jest test kluczowy: na 0.500 moduł
   startował z brzegu sektora i od razu ufał staremu okresowi, więc ten start powinien być tam
   jednym z brzydszych. Po FW-131.1 **ma przestać się wyróżniać**.
3. **Klik przy zatrzymaniu** — jest, osłabł, zniknął?

Zapis w formie `starty 2/5, po restarcie zasilania czysto, stop cichy` w zupełności wystarczy.

---

## KROK 3 — Walk Assist i migająca ikona (FW-133 + FW-134)

**To jest test o największej szansie na coś nowego.** Ważny jest wzorzec, nie samo „miga":

1. wejdź w Walk Assist i **postój kilka sekund bez ruchu** → ikona ma się **pojawić, ale nie migać**;
2. **ruszaj** → ma **zamigać**;
3. **stań, nadal trzymając przycisk** → ma **przestać migać**.

Taki wzorzec odtwarzałby zachowanie fabryczne co do sekundy.

⚠ **Osobno sprawdź, czy Walk Assist nie wypada.** `0x320F` idzie teraz **4× rzadziej** (2 s zamiast
0,48 s), a to ramka, od której zależy utrzymanie trybu marszu na wyświetlaczu. Jeżeli WA zacznie
się urywać po kilku sekundach — to pierwszy podejrzany i wracamy dla niej do krótszego okresu.

---

## KROK 4 — wyświetlacz w normalnej jeździe (FW-133)

Zmieniliśmy okresy pięciu ramek, więc:

- prędkość — płynna, nie skacze i nie zamarza;
- kadencja;
- poziom wspomagania;
- stan baterii;
- ikona Walk Assist;
- **czy nie pojawia się błąd komunikacji.**

Cokolwiek zniknie albo zamarznie → FW-133, i wiemy, którą ramkę zwolniliśmy za bardzo.

---

## KROK 5 — przycisk on/off (FW-132)

Zmierz stoperem, **ile trzeba trzymać przycisk do wyłączenia**. Ma być stabilne **~2,5 s** — i,
co ważniejsze, **za każdym razem tyle samo**. Wcześniej pływało razem z pętlą główną.

---

## KROK 6 — regresja jazdy

Zwykła jazda: **nic w zachowaniu silnika nie ma się zmienić** względem 0.500. Ani siła, ani
narastanie, ani wygaszanie. Trzy z czterech kart nie dotykają sterowania w ogóle, a czwarta
(FW-131.1) tylko kąta — więc każda zmiana odczucia jest sygnałem, że coś przeoczyłem.

---

## KROK 7 — jeżeli będziesz aktualizował firmware

⚠ **Sprawdź, że aktualizacja KONTROLERA nadal działa.** FW-132 przestał reagować na `0x3005`
wysłane jako broadcast. Jeżeli nasz updater ogłasza się właśnie broadcastem, aktualizacja
przestanie działać — wtedy wróć na 0.500 i powiedz, a ramka diagnostyczna `0x1022B` poda adresata
do poprawnego zawężenia.

**Aktualizacja WYŚWIETLACZA** powinna od teraz przechodzić **bez restartu kontrolera** — to była
główna poprawka FW-132.

---

## Co daje build DIAG 0.503

Tylko jeśli będziesz zbierał log. Nowa ramka **`0x1022B`** zawiera liczby narastające od startu:

```text
Data1 = ramki ZGUBIONE przez kolejkę      (powinno rosnąć wolniej niż wcześniej albo wcale)
Data2 = najgorszy poślizg gałęzi 40 ms    (160 = na czas, 320 = cały okres spóźnienia)
Data3 = liczba ramek 0x3005
Data4 hi = adresat ostatniej 0x3005 (31 = broadcast updatera wyświetlacza)
Data4 lo = stan watchdoga komunikacji
```

Po jeździe na 0.503 wystarczy jedna migawka tej ramki, żeby powiedzieć, czy rytm i kolejka są już
zdrowe.

---

## Minimum, jeśli masz mało czasu

**KROK 1** (koło w górze — nieobowiązkowe tylko wtedy, gdy nie zależy Ci na rowerze),
**KROK 2 punkt 2** (start po restarcie zasilania — rozstrzyga FW-131.1),
**KROK 3** (Walk Assist: czy nie wypada i czy ikona zamigała).
