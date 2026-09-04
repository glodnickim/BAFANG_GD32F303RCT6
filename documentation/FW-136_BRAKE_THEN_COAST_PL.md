# FW-136 — najpierw hamowanie, potem wolny wybieg

**Status: KARTA DO AKCEPTACJI. Kodu nie zmieniałem.**
Data: 2026-09-04. Pomysł właściciela: *„pierwsza część jest hamowana, a potem puszczamy jako
wybieg — powiedzmy 50 %, nie w punkcie przed samym zatrzymaniem"*.

Podstawa: `QZERO_QUIET_ZERO_PL.md`, `inc/quiet_zero.h`.

---

## 1. Dobra wiadomość — to nie jest nowy mechanizm

Moduł QZERO **już oddaje** oś regulatorom przy niskich obrotach. Robi to w jednym miejscu
(`src/quiet_zero.c:150`):

```c
if (in->rotor_erps < in->min_brake_erps) {   /* koniec hamowania, oddaj wybieg */
```

`min_brake_erps` to **pole wejściowe**, nie stała w module. Dziś dostaje `RIDE_COAST_RELEASE_ERPS`
= **10 erps**, czyli **7,5 obr/min zębatki**. To jest tempo wolniejsze niż marsz — czyli
hamujemy praktycznie do samego końca, a „wybieg" dostajemy przez ostatni ułamek obrotu.

Dokładnie to, o co pytasz, to **podniesienie tego progu**. Żadnej nowej maszyny stanów.

## 2. Dlaczego progiem ma być PRĘDKOŚĆ, a nie czas

Pytałeś, czy liczyć czasem, prędkością, czy rampą. **Prędkość** — z dwóch powodów:

- klik mieszka w **strefie prędkości** (okolica przełączania kąta komutacji), a nie w oknie
  czasowym. Próg prędkości trafia w niego wprost;
- **stały czas nie działa od każdej prędkości.** 300 ms hamowania z 25 km/h zabiera niewiele,
  a z 5 km/h zabiera całe zatrzymanie. Byłby to parametr, który znaczy co innego przy każdym
  hamowaniu.

## 3. Próg WZGLĘDNY, nie stały

Stała wartość (np. „oddaj poniżej 60 erps") ma tę samą wadę co czas: przy szybkiej jeździe
hamowanie objęłoby ułamek zjazdu, a przy wolnej nie byłoby go wcale.

Dlatego próg liczony **od prędkości, przy której puściłeś pedały**. Moduł **już zapamiętuje**
tę prędkość — pole `erps_entry`, dodane przy QZERO-3:

```c
qz->erps_entry = in->rotor_erps;   /* prędkość w chwili wejścia */
```

Nowy próg:

```text
próg oddania = MAX( RIDE_COAST_RELEASE_ERPS , QZERO_HANDBACK_PCT % z erps_entry )
```

Stary próg 10 erps zostaje jako **twarda podłoga** — nie da się przypadkiem oddać wybiegu
poniżej niego, a FW-048 działa bez zmian.

### Przykład przy 60 obr/min zębatki

```text
puszczasz pedały     60 obr/min zebatki  = 80 erps
prog 50 %            30 obr/min zebatki  = 40 erps
  80 -> 40 erps      HAMOWANIE elektryka
  40 ->  0 erps      WOLNY WYBIEG, bez pradu
```

## 4. Ile naprawdę znaczy „50 %" — to jest ważniejsze, niż wygląda

Energia rośnie z **kwadratem** prędkości. Więc oddanie wybiegu przy połowie prędkości oznacza,
że hamowanie zdążyło już zabrać:

```text
1 - (0,50)^2 = 75 % energii
```

Czyli **„50 % prędkości" to 75 % hamowania**, nie połowa. Tabela do wyboru progu:

| Próg (% prędkości startowej) | Ile energii zabiera hamowanie | Jak długi wybieg |
|---:|---:|---|
| 30 % | 91 % | krótki |
| **50 %** | **75 %** | średni |
| **70 %** | **51 %** — realna „połowa hamowania" | długi |
| 85 % | 28 % | bardzo długi |

Jeżeli chodziło Ci o **połowę hamowania**, właściwą liczbą jest **70 %**, nie 50 %.
Jeżeli o „większość wyhamowaną elektryką, ale ciche zakończenie" — 50 % jest w sam raz.

## 5. Efekt uboczny: QZERO-3 wreszcie zacznie coś robić

QZERO-3 oddaje całkę regulatora przeskalowaną stosunkiem `erps_now / erps_entry`, żeby
regulator nie musiał odbudowywać napięcia od zera. Na rowerze nie dał nic — i teraz wiadomo
dlaczego: **przy 10 erps nie ma czego oddawać**, całka jest tam mikroskopijna.

Przy oddaniu w połowie prędkości ten zasiew robi całą robotę: napięcie odwzorowujące SEM jest
w tym punkcie duże, a siła elektromotoryczna rośnie liniowo z prędkością, więc skalowanie
liniowe jest **dokładnie właściwym** wzorem. QZERO-3 był napisany pod ten przypadek, zanim ten
przypadek istniał.

## 6. Ryzyko — jedno, konkretne

**Możemy zamienić klik na końcu na klik w środku wybiegu.** Moment oddania osi to jedyne nowe
zdarzenie, jakie ta karta wprowadza. Jeśli zasiew z §5 nie trafi, prąd na chwilę skoczy i będzie
to słychać — tyle że tym razem **w połowie zjazdu, a nie przy zatrzymaniu**.

To jest dokładnie to, czego trzeba słuchać w teście. I jest to informacja sama w sobie: klik
w punkcie oddania oznacza, że problemem jest **przejście**, a nie strefa niskich obrotów.

Poza tym: nie dotyka sterowania przy jeździe, nie dotyka konfiguracji, nie dotyka CAN.

## 7. Czego ta karta NIE robi

- nie zmienia `RIDE_COAST_RELEASE_ERPS` — ta stała ma **drugiego** konsumenta (FW-048, wymuszenie
  zerowego żądania) i ruszanie jej zmieniłoby zachowanie w miejscu niezwiązanym z tą kartą;
- nie dodaje żadnego nowego stanu ani nowej ścieżki — tylko liczy inaczej jeden próg;
- nie rusza wygaszania 10 ms ani polityki QUIET.

## 8. Kolejność, którą proponuję

1. **Najpierw wariant A** (`QUIET_ZERO_ENABLE 0`) — jeden build, potwierdza, że diagnoza jest
   trafna: brak prądu = brak kliku. Bez tego potwierdzenia strojenie progu jest zgadywaniem.
2. **Potem FW-136** z progiem wybranym z tabeli §4.

Można też zrobić to jedną jazdą, budując od razu **parę A + FW-136**, i porównać na miejscu.

## 9. Testy

- **T1 — czy klik zniknął z zatrzymania.** To jest cel karty.
- **T2 — czy nie pojawił się klik W ŚRODKU zjazdu**, w punkcie oddania osi. Ryzyko z §6.
- **T3 — czy hamowanie jest jeszcze wyczuwalne** po puszczeniu pedałów. Jeśli nie, próg jest za
  wysoki i schodzimy w tabeli §4.
- **T4 — czy wybieg nie jest za długi** przy zatrzymywaniu się na światłach.
- **T5 — regresja jazdy.** Nic w ciągnięciu ani w narastaniu mocy nie może się zmienić —
  ta karta dotyka wyłącznie tego, co dzieje się po zejściu żądania do zera.

---

## 10. Skąd sterownik wie, że próg wypadł — i dlaczego to jest sedno sprawy

### 10.1 Czym jest ta prędkość

`ui16_erps` — obroty elektryczne wirnika na sekundę. Powstaje **wyłącznie w przerwaniu od
czujnika Halla** (`src/main.c:2291`):

```c
ui32_erps_cumulated -= ui32_erps_cumulated>>5;      // srednia wykladnicza,
ui32_erps_cumulated += 500000/(ui16_timertics*6);   // pamiec ~32 zbocza
ui16_erps = ui32_erps_cumulated>>5;
```

Czyli: przy każdym zboczu Halla liczymy prędkość z odstępu do poprzedniego zbocza i wpuszczamy
ją do średniej o pamięci **32 zboczy**.

### 10.2 Dwie konsekwencje, obie niekorzystne przy dzisiejszym progu 10 erps

**Aktualizuje się tylko na zboczach.** Im wolniej, tym rzadziej. Przy 10 erps zbocze przychodzi
co ~17 ms, a średnia potrzebuje 32 zboczy, żeby nadążyć — czyli **ponad pół sekundy**
opóźnienia dokładnie tam, gdzie podejmujemy decyzję.

**Przy zatrzymaniu nigdy nie dochodzi do zera.** Sprawdziłem: `ui16_erps` **nie jest nigdzie
zerowane** przy zatrzymaniu wirnika — brak zboczy oznacza brak aktualizacji, więc wartość
**zamarza na ostatnim odczycie**. Wiek ostatniego zbocza jest liczony osobno
(`ui16_erps_counter`), ale QZERO go nie czyta.

### 10.3 Co z tego wynika — hipoteza, która tłumaczy porażkę QZERO-3

Skoro odczyt zostaje w tyle i zamarza, to przy zatrzymywaniu się realna prędkość idzie do zera,
a **raportowana zatrzymuje się powyżej progu**. Ostatnie zbocze może wypaść przy faktycznych
~3 erps, gdy średnia pokazuje jeszcze ~12 — i wtedy warunek `rotor_erps < 10` **nigdy się nie
spełnia**.

Jeżeli tak jest, to QZERO trzyma całki na zerze **aż do samego zatrzymania**, czyli hamuje
przez całą strefę skoków kąta — i **oddanie osi w ogóle nie następuje**. To by tłumaczyło,
dlaczego QZERO-3 nic nie dał: ścieżka, którą poprawiał, prawdopodobnie się nie wykonuje.

**To jest hipoteza wyprowadzona z kodu, nie pomiar.** Ale jest sprawdzalna i tania — patrz §10.5.

### 10.4 Dlaczego wyższy próg naprawia to sam z siebie

Próg z §3 wypada przy połowie prędkości startowej — tam, gdzie zbocza Halla lecą gęsto, średnia
jest świeża, a decyzja zapada **na długo przed** zamarznięciem odczytu. To nie jest tylko
„wcześniejsze oddanie wybiegu" — to **przeniesienie decyzji do zakresu, w którym pomiar prędkości
jest w ogóle wiarygodny**.

To ta sama lekcja co w FW-130.1 i FW-131.1: *brak sygnału Halla nie znaczy prędkość zero*. Tu
występuje trzeci raz, w kolejnym module.

### 10.5 Jak sprawdzić, gdzie ten próg wypada

**Na rowerze, bez narzędzi.** Próg jest ułamkiem **kadencji, przy której puściłeś pedały**, bo
obroty zębatki = erps × 3/4. Puszczasz przy 60 obr/min → przy progu 50 % oddanie następuje przy
30 obr/min zębatki. Ma to być wyczuwalne **w połowie zjazdu**: hamowanie wyraźnie ustaje i
zaczyna się swobodny wybieg. Jeżeli tego nie czuć, próg jest za nisko.

**W buildzie DIAG.** Warto dołożyć do agregatu dwie liczby, których dziś nie ma:

```text
ile razy oddanie osi NASTAPILO   (jesli ~0 przy wielu zatrzymaniach -> hipoteza z 10.3 potwierdzona)
erps w chwili oddania            (gdzie realnie wypada prog)
```

To rozstrzyga §10.3 jednym logiem i jest **warunkiem wstępnym** do strojenia progu — bez tego
dobieralibyśmy liczbę do mechanizmu, o którym nie wiemy, czy w ogóle się uruchamia.
