# FW-131 — jeden kanoniczny kąt wirnika (naprawa kliku u źródła)

**Status: ZAAKCEPTOWANY („ruszaj z FW-131"), WDROŻONY W KODZIE, testy zielone. NIE ZBUDOWANY, NIE NA ROWERZE.** Szczegóły wdrożenia na końcu dokumentu.
Data: 2026-09-03. Podstawa: `EVistDrive_G532_FakeTaxi_ROTOR_ANGLE_IMPLEMENTATION_GUIDE_20260903.md`
(§24 architektura, §25 automat, §26 klik, §27 kolejność) + audyt naszego kodu z tej sesji.

---

## 1. Co przewodnik rozstrzygnął

**G532 odpada jako wzorzec do przepisania** — ma inny sprzęt: enkoder A/B + absolutna referencja
PWM, 4096 kroków na obrót mechaniczny. Przewodnik mówi wprost: **nie udawać programowo
pseudo-enkodera z Halli** (§5). Z G532 przenosimy tylko układ warstw, nie liczby.

**Wzorcem algorytmicznym jest FAKE TAXI**, bo ma dokładnie ten sam surowy sygnał co my — trzy
Halle — i rozwiązuje ten sam problem: znacznik sprzętowy każdego zbocza, historia 6 sektorów,
kotwica sektora + korekta per sektor, ciągła interpolacja, ograniczenie ekstrapolacji, timeout.

---

## 2. Nasz defekt — zmierzony, nie oszacowany

Mamy **dwa różne zera kąta**. To jest dokładnie przypadek z §26 przewodnika.

```c
// src/main.c, wybór formuły w przerwaniu sterowania
if(!ui8_6step_flag)
    kąt = hall + MP.angle_correction + interpolacja(0°…60° w sektorze);
else
    kąt = hall - MP.reverse * 30°;
```

- gałąź szybka kotwiczy na **początku sektora** i dolicza interpolację przez 60°,
- gałąź wolna siedzi **w środku sektora** i **nie dodaje `MP.angle_correction`**.

`MP.angle_correction` = 71 582 790 w skali Q31, czyli **6°** (wynik kalibracji z FW-022).

**Rozmiar uskoku przy przełączeniu** (dla `MP.reverse = -1`, czyli wartości domyślnej):

| Gdzie w sektorze nastąpi przełączenie | Skok kąta |
|---|---|
| dokładnie na zboczu Halla | **24°** elektrycznych |
| w środku sektora | **−6°** |
| tuż przed następnym zboczem | **−36°** |

Czyli **od 24° do 36°** co do wartości bezwzględnej, zależnie od chwili. Przy płynącym prądzie
wektor prądu przeskakuje o tyle samo. To jest ten trzask — i to tłumaczy, dlaczego jest słyszalny,
a nie subtelny.

**Nawet na zboczu Halla obie formuły się nie zgadzają**, bo jedna kotwiczy na początku sektora,
a druga w środku. Nie ma więc żadnego momentu, w którym przełączenie byłoby bezbolesne.

Progi przełączania (histereza, `SIXSTEPTHRESHOLD` = 10000):

```text
zwalnianie → formuła wolna   poniżej ~5,6 obr. elektr./s   (~4,2 obr/min zębatki)
przyspieszanie → interpolacja powyżej ~8,3 obr. elektr./s  (~6,25 obr/min zębatki)
```

To zgadza się z obserwacją z jazdy: „zębatka wizualnie już stoi", a silnik jeszcze się kręci.

**Dlaczego klik jest też na starcie:** ochrona FW-048 (koniec prądu przed strefą) działa
**wyłącznie przy zwalnianiu** — w kodzie stoi „a start needs current down here". Każdy start od
postoju przejeżdża przez to przełączenie pod prądem. Wcześniej nie było tego słychać, bo silnik
po długim wybiegu często jeszcze się kręcił i start zaczynał się powyżej strefy; odkąd QZERO
faktycznie zatrzymuje silnik, każdy start zaczyna od zera.

---

## 3. Co proponuję — i czego NIE proponuję

### NIE: łagodzić prąd wokół przełączenia

Przewodnik §26 mówi to wprost: nie naprawiać wolniejszym przełączaniem prądu. To leczy objaw,
nie usuwa uskoku, i **nie da się tak naprawić startu**, bo start musi mieć prąd w tej strefie.

### TAK: jeden kanoniczny kąt bazowy (ETAP 4 przewodnika)

```text
base_theta = kotwica_sektora + korekta_sektora + wspólny_offset
```

i **wszystkie** gałęzie — wolna, szybka, timeout — używają tego samego `base_theta`.
Interpolacja przestaje być **alternatywnym zerem**, a staje się **składnikiem dodawanym**:

```text
wolno / brak zaufania :  theta = base_theta
z zaufaniem          :  theta = base_theta + ograniczona_interpolacja
```

Wtedy przełączenie nie jest zmianą układu odniesienia, tylko dodaniem albo odjęciem składnika,
który **w chwili zbocza Halla wynosi zero**. Nie ma czym trzasnąć — niezależnie od kierunku
przejazdu i od tego, czy płynie prąd.

To naprawia **oba** trzaski (koniec wybiegu i start) **jednym mechanizmem** i jest niezależne
od QZERO — QZERO może wtedy spokojnie hamować.

---

## 4. Kolejność, którą rekomenduję

Przewodnik daje ETAP 0–8. Dla nas najlepszy stosunek zysku do ryzyka:

| Krok | Co | Po co | Ryzyko |
|---|---|---|---|
| **0** | dokończyć audyt toru theta: wszyscy właściciele `q31_rotorposition_absolute`, gdzie jest switch, czy oba tory mają ten sam offset | częściowo zrobione w tej sesji (§2) | zero — bez zmian |
| **1** | **zmierzyć uskok** w buildzie DIAG: theta tuż przed i tuż po przełączeniu, plus prąd w tej chwili | zamienia moje 24–36° z kartki na liczbę z roweru i daje dowód przed/po | zero — tylko log |
| **4** | **jeden `base_theta`**, interpolacja jako składnik | TO JEST NAPRAWA KLIKU | **wysokie** — komutacja, przerwanie 16 kHz |
| 5 | zachowanie klasy Fake Taxi przy starcie: postój = sam anchor, pierwsze zbocze = re-anchor, interpolacja dopiero po historii | porządkuje ruszanie | średnie |
| 3 | historia 6 sektorów, prędkość z pełnego cyklu | mniej modulacji od nierównej geometrii Halli | średnie |
| 7 | korekty per sektor | dokładność, mniej tętnienia momentu | niskie po 4 |
| 8 | phase advance zależny od prędkości | sprawność przy wysokich obrotach | odłożyć |

**Rekomendacja: 0 → 1 → 4, i zatrzymać się.** Kroki 5/3/7 to jakość i sprawność, nie klik.
Krok 8 w ogóle nie teraz.

**Dlaczego krok 1 przed 4:** zmiana kąta komutacji to najbardziej wrażliwe miejsce w firmware.
Mieliśmy już build, który po zmianie w tej okolicy **wcale nie kręcił silnikiem** (0.0298/0.0299,
wycofane). Log daje liczbę, do której można się przyrównać po zmianie — bez niego jedziemy na
wyczucie w najgorszym możliwym miejscu.

---

## 5. Pytanie sprzętowe — ZAMKNIĘTE, nie jest warunkiem wstępnym

**Rozstrzygnięcie właściciela, 2026-09-03:** skoro reverse fabrycznego firmware dla tej klasy
sprzętu (Fake Taxi) pokazuje, że producent **czyta wyłącznie 3 bity Halla**, to najprawdopodobniej
płytka po prostu nie ma doprowadzonego absolutnego wyjścia enkodera.

Przewodnik popiera to jako ustalenie **CONFIRMED**, a nie brak danych: Fake Taxi czyta te same
piny co my — `PC6` / `PC7` / `PC8` — i składa z nich sam 3-bitowy stan (funkcja ok. `0x08004E78`).
Producent potrafi zrobić inaczej, bo robi to w G532; gdyby absolutne wyjście tu było, użyłby go.

**Konsekwencja: idziemy ścieżką Fake Taxi i nie czekamy na oględziny płytki.** Gdyby kiedyś
okazało się, że absolutne wyjście jednak istnieje, będzie to ulepszenie do osobnej oceny (§5
przewodnika), a nie powód do zmiany tego planu.

Poniższe zostaje wyłącznie jako zapis tego, co rozważaliśmy i dlaczego to odpadło.

### Rozważana (odrzucona) skrótowa droga

Nasza baza reverse M820 mówi, że trzy bity wirnika to najprawdopodobniej **enkoder MT6816
w trybie UVW** — czyli czujnik **absolutny**, z którego bierzemy tylko 3 bity, czyli 6 pozycji
na obrót elektryczny.

MT6816 potrafi oddać **pełny kąt absolutny** przez SPI, ABZ albo PWM. Jeżeli którekolwiek z tych
wyjść jest fizycznie doprowadzone do mikrokontrolera, to możemy mieć klasę G532 — **poprawny kąt
także przy zerowych obrotach, bez żadnej formuły zastępczej**, i cały ten problem znika, zamiast
być obchodzony.

To jest **oględziny płytki plus nota katalogowa**, nie programowanie. Tanie, a może zmienić plan.
Przewodnik przewiduje ten scenariusz w §5: jeżeli hardware to udostępnia, wracamy do oceny
architektury G532.

**Proponuję sprawdzić to zanim ruszę komutację.**

---

## 6. Plan powrotu i A/B

Zmiana w kroku 4 dostaje własny przełącznik kompilacji (jak `WALK_GOVERNOR_ENABLE` i
`QUIET_ZERO_ENABLE`), żeby jazda porównawcza była dwoma plikami .bin. Powrót = jedna stała.

Test na koźle przed jazdą: koło w górze, powolne obroty ręką w obie strony przez strefę
przełączenia, obserwacja `theta_delta_per_cycle` — po poprawce nie może być skoku.

---

## 7. Czego ta karta NIE dotyka

Walk Assist, PAS, moment, bateria, QZERO. Wyłącznie **kąt wirnika i jego ciągłość**.
QZERO zostaje jak jest — po naprawie kąta jego hamowanie przestaje być problemem.

---

# WDROŻENIE (2026-09-03) — kod gotowy, NIE zbudowany

**Status: ZAAKCEPTOWANY („ruszaj z FW-131"), WDROŻONY, testy zielone. NIE zbudowany, NIE na rowerze.**

## 1. Uskok zmierzony, nie oszacowany

Nowy zestaw testów modeluje **stare** prawo obok nowego, więc poprawa jest liczbą, a nie deklaracją:

```text
L0 legacy step: worst 36.0 deg (na końcu sektora),
                24.0 deg nawet DOKŁADNIE na zboczu Halla
```

Moje wyliczenie z kartki (24–36°) potwierdzone co do dziesiątej. **Nie istniał żaden moment,
w którym przełączenie byłoby bezbolesne** — nawet na zboczu Halla obie formuły dawały inny kąt.

## 2. Co powstało

`src/rotor_angle.c` + `inc/rotor_angle.h` — czysta funkcja bez globali, więc **w całości
uruchamialna na PC**. Wpięta w `main.c` za `CANONICAL_ANGLE_ENABLE` (1 = nowe, 0 = stare
gałęzie bajt w bajt). Stan zerowany w tych samych trzech miejscach co regulatory prądu.

```text
theta = (hall + angle_correction) + offset
offset = interpolowany  albo  trzymany
```

## 3. Prawo przekazania — testy poprawiły mój projekt

Pierwotnie proponowałem przełączanie w środku sektora. Test wykazał, że to **bezuderzeniowe tylko
w jedną stronę** (30° skoku przy wychodzeniu). Poprawione prawo jest **niesymetryczne**, bo
niesymetryczny jest problem:

| Kierunek | Reguła | Dlaczego |
|---|---|---|
| **w stan trzymany** | natychmiast, z zatrzaśnięciem bieżącej wartości | wyjście się nie rusza, więc nie ma na co czekać |
| **ze stanu trzymanego** | dopiero gdy interpolacja dogoni trzymaną wartość | to jedyny punkt, w którym obie formuły dają to samo |

Wynik pomiaru: **0,000°** przy zwalnianiu i **0,077°** przy przyspieszaniu (0,077° to zwykły ruch
w jednym takcie testu, nie skok).

## 4. Trzy rzeczy znalezione przy okazji

1. **Trzymanie ostatniej wartości jest DOKŁADNIEJSZE niż środek sektora.** Stare prawo skakało na
   środek, co jest najlepszym zgadywaniem, gdy nic nie wiadomo — a coś było wiadomo: wirnik
   zatrzymał się tam, gdzie mówiła interpolacja.
2. **Stara gałąź wolna brała znak z konfiguracji (`MP.reverse`), szybka z pomiaru
   (`i8_recent_rotor_direction`).** Druga niespójność, niezależna od brakującej korekty. Teraz
   trzymana wartość dziedziczy znak z interpolacji, więc żadna opinia o `MP.reverse` nie jest
   potrzebna.
3. **Komentarz w `main.c` kłamał:** „10923<<16 is 715827883". Faktycznie 715 849 728 — różnica
   0,0018°, fizycznie bez znaczenia, ale moduł używa liczby, którą arytmetyka naprawdę produkuje.

Dodatkowo **ograniczona ekstrapolacja**: przesunięcie nigdy nie wychodzi poza własny sektor.
Stare prawo ekstrapolowało bez ograniczenia, więc kąt uciekał przed wirnikiem dokładnie wtedy,
gdy estymata była najmniej wiarygodna.

## 5. Testy

`tests/host/fw131_rotor_angle_host.c` — L0 (odniesienie do starego prawa), T1 jedno zero,
T2/T3 bezuderzeniowość w obie strony, T4 ograniczona ekstrapolacja, T5 zatrzymany wirnik,
T6 przekazanie ograniczone jednym sektorem, T7 znak z pomiaru. **Wszystkie przechodzą.**

Pełny przebieg hostowy: **jeden padający zestaw** — wyłącznie znana wcześniejsza rodzina
diag schema-3. Trzeba było poszerzyć jedno okno source-guarda w
`stopclick_c1_pi_integral_host.c` (1300 → 1600), bo `rotor_angle_reset()` dołączył do tej samej
domeny resetu co regulatory; treść guarda bez zmian.

## 6. Zanim to pojedzie — obowiązkowy test na koźle

Zmiana dotyczy komutacji. Mieliśmy build, który po zmianie w tej okolicy **wcale nie kręcił
silnikiem** (0.0298/0.0299, wycofane). Dlatego **przed jazdą**:

1. koło w górze, sterownik zasilony,
2. **powoli obracać koło ręką w obie strony** przez strefę przełączania (kilka obr/min zębatki),
3. silnik ma się kręcić gładko w obu kierunkach, bez zacięć i bez trzasków;
4. dopiero potem krótka jazda.

Jeżeli silnik ruszy szarpiąc albo nie ruszy — **natychmiast wrócić na build z
`CANONICAL_ANGLE_ENABLE = 0`**. Pierwszym podejrzanym jest wtedy znak kierunku: stare prawo brało
go z `MP.reverse`, nowe z pomiaru, i jeśli te dwa nie zgadzały się na tym silniku, to nowe jest
poprawne, a stare było przesunięte — ale objaw wyjdzie na koźle, nie na drodze.

---

# WYNIK JAZDY — 0.0496, 2026-09-03

**POTWIERDZONE NA ROWERZE.** Zgłoszenie właściciela:

- rower **rusza, nie szarpie**;
- **Walk Assist działa**, ruszanie z korby też, krótka jazda w porządku;
- **pozostał delikatny klik na samym końcu zatrzymania.**

Test na koźle przeszedł — silnik kręci się gładko w obu kierunkach przez strefę przełączania.

## Co to znaczy

Główny cel karty osiągnięty: **uskok kąta 24–36° zniknął**, a razem z nim mocny trzask i szarpanie.
Zostało coś **jakościowo innego** — cichy klik przy prawie zatrzymanym wirniku.

## Dlaczego coś jeszcze zostało — i czego to NIE jest

To **nie jest** resztka naprawionego defektu. Przy trzech bitach Halla kąt na samym dole
**z natury postępuje skokami po 60°** — jeden skok na zbocze Halla. Tak działa komutacja
sześciostopniowa i tak działała zawsze; FW-131 usunął uskok przy *zmianie formuły*, a nie
samą ziarnistość czujnika, bo ta jest własnością sprzętu.

Skok kąta o 60° jest **słyszalny tylko wtedy, gdy w tej chwili płynie prąd.**

## Co zostało do zrobienia — i to NIE wymaga enkodera

Nadal otwarta jest rzecz zidentyfikowana przy QZERO-2, ale wtedy **nieruszona**: QZERO oddaje
sterowanie przy `RIDE_COAST_RELEASE_ERPS` **zostawiając całkę regulatora na dokładnym zerze**.
Regulator musi ją odbudować, a dopóki jej nie odbuduje, napięcie nie pasuje do siły
elektromotorycznej i **prąd hamujący płynie dalej w dół** — czyli dokładnie w strefę skoków 60°.

Czyli dźwignia nie leży już w kącie, tylko w **prądzie**: jeżeli w ostatnim odcinku wybiegu prądu
nie ma, skoki kąta są nieme.

**Propozycja (osobna, mała, nie dotyka komutacji):** oddawać sterowanie z **podaną wartością
całki** zamiast z zerem — siła elektromotoryczna jest proporcjonalna do prędkości, więc wartość
zerująca prąd przy prędkości oddania to w przybliżeniu `całka_zapamiętana × (obroty_teraz /
obroty_przy_wejściu)`. Alternatywnie: oddawać wyżej i rampą, żeby prąd zdążył zejść do zera
z zapasem nad strefą skoków.

## Czy enkoder absolutny by to usunął

Tak — i to jest uczciwa odpowiedź. Sterownik z absolutnym kątem (G532) nie ma ani skoków 60°, ani
formuły zastępczej. Ale ustaliliśmy, że producent na naszej klasie sprzętu czyta wyłącznie 3 bity,
więc płytka najpewniej nie ma tego wyjścia doprowadzonego.

**Bez enkodera „delikatny klik przy prawie stojącym wirniku" jest blisko podłogi możliwości —
ale prąd w tej chwili nie jest nieunikniony i to jest jeszcze do wygrania.**

---

# WYNIK 0.500 (QZERO-3) — NIE POMOGŁO

Jazda 2026-09-04, build **0.500** (= 0.498 + zasilana całka przy oddaniu sterowania):

- **klik jest na starcie I przy zatrzymaniu**,
- czas wybiegu **się nie wydłużył**,
- innych negatywnych zmian nie zaobserwowano.

## Co to znaczy

Poprawka QZERO-3 **nie usunęła kliku**. Co więcej, na 0.0496 (bez niej) właściciel odnotował, że
start „się wyczyścił", a teraz klik jest na obu końcach. Możliwe są dwa odczyty i **nie da się ich
rozróżnić bez jazdy**:

1. **QZERO-3 spowodowało regresję startu.** Mechanizm jest realny: po oddaniu sterowania z zasianą
   całką regulator musi ją odkręcić, a robi to wyłącznie przez błąd prądu. Jeżeli wirnik zatrzyma
   się szybciej, niż całka zdąży zejść, to **przy zerowym błędzie całka zostaje zamrożona tam,
   gdzie jest** (tak działa PI: `Delta == 0` nie zmienia całki). Następny start zaczyna się wtedy
   z naładowanym integratorem → skok napięcia → klik.
2. **Klik na starcie nigdy nie zniknął**, a wcześniejsze „wydaje mi się, że się wyczyścił" było
   wrażeniem z jednej próby, nie świadomym testem (pytanie A1 nadal bez odpowiedzi).

## Test, który to rozstrzyga bez żadnej zmiany w kodzie

**Wgrać 0.498** (jest już zbudowane, to ten sam kod bez QZERO-3) i **świadomie zrobić kilka startów
z pełnego postoju pod rząd**.

| Wynik | Wniosek |
|---|---|
| starty czyste na 0.498, brudne na 0.500 | QZERO-3 zaszkodziło → wycofać |
| brudne na obu | start nigdy nie był czysty, QZERO-3 jest neutralne |

## Co proponuję zamiast QZERO-3

Zasiewanie całki było próbą postawienia regulatora od razu w stanie bez prądu. Prostsza droga,
bez dodatkowego stanu i bez ryzyka zamrożonej całki: **oddawać sterowanie WYŻEJ**.

Dziś oddanie następuje przy `RIDE_COAST_RELEASE_ERPS` = 10, czyli tuż nad strefą skoków
(5,6–8,3 ERPS). Regulator nie ma kiedy odbudować dopasowania. Gdyby oddanie następowało np. przy
25 ERPS, miałby cały odcinek 25 → 8 ERPS na spokojne zejście prądu do zera, **zanim** zacznie się
strefa skoków.

Wymaga to **osobnego progu** (`QZERO_HANDBACK_ERPS`), a nie podniesienia
`RIDE_COAST_RELEASE_ERPS` — ten drugi jest współdzielony z FW-048 i steruje też odcięciem
wspomagania przy zwykłym puszczeniu pedałów.

Koszt: oddajemy hamowanie poniżej 25 ERPS. Obserwacja z tej jazdy sugeruje, że jest tani —
**czas wybiegu nie wydłużył się** mimo że QZERO-3 już oddało ostatni odcinek.
