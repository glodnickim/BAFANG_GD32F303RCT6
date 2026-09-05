# FW-137 — prędkość wirnika ma maleć, gdy wirnik staje

**Status: WDROŻONE W KODZIE, ZBUDOWANE. NIE NA ROWERZE.**
Data: 2026-09-05. Podstawa: pomiar FW-136.0 z roweru, log `log-2026-09-05-09-12-43-n0.log`.

---

## 1. Co zmierzyliśmy

Przez całą sesję, łącznie z prawdziwym zwolnieniem pedałów, **najniższa prędkość zgłoszona
podczas wybiegu wyniosła 24**. Progi, które czekały poniżej:

| Próg | Mechanizm | Po co istnieje |
|---:|---|---|
| **10** | FW-048 odcięcie prądu | „przestań podawać prąd, zanim kąt skoczy" |
| **10** | QZERO oddanie osi | „oddaj oś regulatorom, zanim kąt skoczy" |
| **3** | FW-041 preload przekładni | „wybierz luz po cichu zamiast jednym klapnięciem" |
| **0** | smooth start | „uzbrój łagodny start, gdy rower stoi" |

**Żaden nie był osiągalny.** Nie były zepsute — **nigdy nie zostały wywołane**. Dwa z nich
istnieją wyłącznie po to, żeby usunąć klik: jeden przy zatrzymaniu, drugi przy starcie. Czyli
dokładnie ten objaw, który przetrwał trzy karty poprawek.

## 2. Dlaczego odczyt zamarza

`ui16_erps` powstaje **wyłącznie w przerwaniu od czujnika Halla**, z odstępu między zboczami.

To jest dobry sposób na pytanie **„jak szybko"** i z zasady niemożliwy na pytanie **„czy stanął"**:
zatrzymanie nie generuje zbocza, brak zbocza nie generuje aktualizacji, więc ostatnia wartość
zostaje na zawsze.

Na to nakłada się **średnia z 32 zbocz**, która przy zwalnianiu zawsze pokazuje przeszłość — a
przeszłość była szybsza niż teraźniejszość.

## 3. Poprawka — to nie jest filtr, tylko fizyka

Trzy czujniki Halla przesunięte o 120° dają **sześć stanów na obrót elektryczny**, więc każde
zbocze to dokładnie **60°**. Dlatego **czas od ostatniego zbocza sam jest pomiarem**:

> jeżeli 60° nie zostało pokonane w tym czasie, wirnik nie może kręcić się szybciej niż jedno
> zbocze na ten czas.

```text
sufit_prędkości = 60° / (czas od ostatniego zbocza)
```

To jest **ograniczenie górne wynikające z tego, że zbocze NIE nadeszło** — nie oszacowanie i nie
wygładzanie.

### 3.1 Dwie własności, które czynią to bezpiecznym

- **może tylko obniżyć** odczyt, i tylko gdy zbocze się spóźnia;
- **przy stałej prędkości nie działa w ogóle** — dzięki pasowi ochronnemu 2×. Bez niego sufit
  leżałby dokładnie na granicy stanu ustalonego i podgryzałby każdy odczyt przez zwykły jitter.
  Test T2 sprawdza to dla **każdej prędkości od 3 do 400 erps**.

### 3.2 Dlaczego akumulator też jest ściągany w dół

Gdyby ograniczyć tylko wartość wyjściową, nieaktualna średnia 32-zboczowa wstawiłaby ją z
powrotem przy następnym zboczu, a testy progowe zaczęłyby drgać zamiast się zatrzasnąć.

### 3.3 Ponowne przejęcie po postoju — bez tego zrobilibyśmy nowy błąd

Gdy sufit zejdzie do zera, akumulator też. Pierwsze zbocze po ruszeniu **wmieszane** w wyzerowaną
średnią dałoby **1/32 prawdziwej prędkości** i 32 zbocza na odbudowę. Zamienilibyśmy zamarzanie
na zaniżanie przy każdym starcie.

Dlatego przy ponownym przejęciu średnia jest **zasiewana** tą próbką, a nie mieszana z niczym.
Ten sam kształt co FW-131.1: historia sprzed postoju nie jest dowodem na prędkość po nim.

## 4. Efekt liczbowy — z wartości zmierzonej na rowerze

```text
start 24 erps (dokładnie tyle, ile pokazał rower)
  ponizej 10  po  30 ms  -> FW-048 i oddanie QZERO staja sie osiagalne
  ponizej  3  po  67 ms  -> preload przekladni staje sie osiagalny
  rowne   0   po 1000 ms -> smooth start moze sie uzbroic
```

Zamiast zamarzać na 24, odczyt schodzi do zera w około sekundę po faktycznym zatrzymaniu, a przez
progi antyklikowe przechodzi w **kilkadziesiąt milisekund**.

## 5. Walk Assist — sprawdzone osobno, na pytanie właściciela

*„czy nasz Walk Assist się nie wysypie, jeśli ktoś ustawi np. 10 obr/min zębatki"*

**Nie może się wysypać, z trzech niezależnych powodów:**

1. **`walk_assist_motor.c` nie odwołuje się do `ui16_erps` ani razu.** Buduje własne oszacowanie
   z **surowego odstępu** Halla i używa **zegara wieku zbocza** do sprawdzania życia. To jest
   FW-130/130.1 — karta, którą kazałeś zrobić słowami *„jak się zatrzyma, nie może czytać
   z Halli prędkości, bo ona nie wystąpi"*. Test T6 pilnuje, żeby ta niezależność została.
2. **Zakres jest ograniczony w firmware do 20–60 obr/min** (`parser.c`, dwa miejsca). 10 obr/min
   nie da się ustawić — wartość spoza zakresu jest naprawiana do domyślnej.
3. **Sufit jest bezwymiarowy** — pas ochronny liczy się od bieżącej prędkości, nie od stałej.
   Przy 20 obr/min zbocze przychodzi co 6,3 ms, przy 60 obr/min co 2,1 ms, a sufit w obu
   przypadkach zaczyna działać dopiero po dwóch spodziewanych odstępach. Skala nie ma znaczenia.

## 6. Ryzyko — jedno, i jest realne

Ta poprawka **odmraża naraz pięć mechanizmów, których nikt nigdy nie widział w działaniu**.
Nie były testowane w ruchu, bo nie było jak.

Spodziewam się, że start i zatrzymanie będą **zauważalnie inne**. Zakładam, że lepsze — po to je
napisano — ale tego nie obiecuję. Dlatego lista obserwacji w §8 jest ułożona tak, żeby dało się
powiedzieć **który** mechanizm się odezwał.

## 7. Czego ta karta NIE robi

- **nie zmienia uśredniania z 32 na 6.** To osobna poprawka: powód uśredniania to asymetria
  rozmieszczenia czujników, która powtarza się co 6 zbocz, więc 32 jest jednocześnie pięć razy
  za wolne i nie dzieli się przez 6. Ale to dotyka odczytu **przy normalnej jeździe**, czyli
  komutacji — własna karta, własna jazda;
- **nie rusza żadnego progu.** Wszystkie zostają takie, jakie były. Zmienia się tylko to, że
  stają się osiągalne;
- **nie rusza Walk Assist** — patrz §5.

## 8. Co obserwować po wgraniu

Patrz `CHECKLISTA_0.508_PL.md`.
