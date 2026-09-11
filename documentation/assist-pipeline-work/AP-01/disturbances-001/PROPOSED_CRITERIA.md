# AP-01 disturbances-001 — proponowane kryteria inhibit / recovery / timebase

**Status: PROPOZYCJA. Nic tutaj nie jest zatwierdzone i nic nie jest wymaganiem.**
Kryteria proponuje wykonawca AP-01; przyjęcie ich należy do Mastera i właściciela projektu.
Zapisane **przed** kandydatami na zmianę firmware, zgodnie z kartą.

**Wersja 3 (po REVIEW-EVD-AP-01-012).** P-1 z wersji 2 był mierzony na zdecymowanym zapisie i
podawał „ten sam tick, 0,00 ms" — sprostowane niżej. Wersje poprzednie zachowane w
`archive/iteration-011/` i `archive/iteration-012/`.

| | v2 (iteracja 012) | v3 (ta) |
|---|---|---|
| krawędź | dwa niezgodne detektory: spadek `pas_transition_index` w P-1, `pas_ab != pas_normal_ab` w opisie zdarzenia | **jeden** detektor kroku wstecz na rzeczywistej linii (`dm.line_edge`), używany przez oba |
| rozdzielczość | pomijana — zdecymowany zapis dawał „dokładne" chwile | każda chwila niesie `resolution_s` i przedział `(t_prev, t_obs]` |
| P-1 na `sample_ms=1` | PASS 12, latencja 0,00 ms | **NOT_EVALUATED** — zapis nie rozstrzyga |
| P-1 na `sample_ticks=1` | brak takich przypadków | **PASS 8**, latencja ≤ 1 tick, przedział podany |

**Wersja 2 (po REVIEW-EVD-AP-01-011).** Wersja z iteracji 011 zawierała twierdzenia sprzeczne z
danymi — zachowana w `archive/iteration-011/PROPOSED_CRITERIA.md`. Co się zmieniło:

| | v1 (iteracja 011) | v2 (ta) |
|---|---|---|
| P-1 | opóźnienie liczone od **czasu komendy** ze scenariusza | liczone od **krawędzi elektrycznej**; komenda / krawędź elektryczna / krawędź przyjęta / odpowiedź rozdzielone |
| P-2 | „obecny kod spełnia” — **błąd** | oceniane programowo → **FAIL 8/8** |
| P-6 | uzasadnione z ręcznie odczytanych maksimów | oceniane programowo wobec dopasowanej kontroli |
| ocena | opisowa, w prozie | `analyze.py::evaluate_criteria()`, werdykt w `results/disturbances-metrics.json` |

Werdykty poniżej pochodzą **z kodu, nie z lektury wykresu**: `results/disturbances-metrics.json`
→ `criteria_summary` i `cases[*].criteria`. Progi **nie zostały poruszone**, żeby uzyskać PASS.

## Kwalifikacja — czego te liczby NIE są

`controller_lab.c` nie zamyka pętli PMSM/FOC. `u_abs` i zmierzony prąd baterii są stale zerem.
Kadencja, moment i prędkość są **wymuszonymi wejściami syntetycznymi**. To są pomiary
**kompozycji ŻĄDANIA Iq** dla zadanych wejść — nie moment, nie prąd rzeczywisty, nie zachowanie
mostka ani sprzętu. Żaden próg poniżej nie jest limitem bezpieczeństwa HW i żaden nie został
potwierdzony na rowerze (`HW NOT_RUN`).

## Cztery różne chwile, których nie wolno mylić (D011-02, D012-01)

```text
KOMENDA            czas z karty scenariusza — kiedy generator DOSTAŁ polecenie
KRAWĘDŹ LINII      pierwszy krok WSTECZ na rzeczywistej linii pokazywanej ISR (pas_ab)
PRZYJĘCIE          pierwszy tick, w którym licznik samplera produkcji drgnął
INHIBIT            pierwszy tick, w którym pas_inhibit_reason != 0
```

Wykrywanie krawędzi: krok wstecz na pierścieniu `FWD_AB = [0,2,3,1]` liczony z `pas_ab`.
**Nie** `pas_ab != pas_normal_ab` — to diagnostyka override, a czyste cofanie nie ma override w
ogóle (`pas_normal_ab` podąża za cofającą się korbą), więc dla dokładnie tego przypadku dawało
`NOT_OBSERVED`. Ten sam detektor obsługuje odbicie (linia wymuszona o krok wstecz) i cofanie.

**Rozdzielczość obserwacji.** Chwila odczytana ze zdecymowanego zapisu leży w przedziale
`(t_poprzedniej_próbki, t_obserwacji]`. Latencja między dwiema takimi chwilami ma więc przedział
`(b_prev − a_obs, b_obs − a_prev)`. Ujemna dolna granica oznacza, że zapis **nie ustala nawet
kolejności** zdarzeń.

### Dlaczego `sample_ms=1` nie wystarcza — zmierzone

Przy 4 kHz `sample_ms=1` daje jeden wiersz na 4 ticki. Zmierzone na
`reverse_clean_cad72_speed18` (te same argumenty, dwie rozdzielczości):

| | `sample_ticks=1` (0,25 ms) | `sample_ms=1` (1 ms) |
|---|---|---|
| pierwsza odwrotna krawędź linii | **3,00000 s** | 3,00900 s |
| przyjęcie (`pas_sampler_reverse` 0→1) | 3,00075 s | 3,00100 s |
| inhibit | **3,00075 s** | 3,00100 s |
| latencja krawędź → inhibit | **0,75 ms (3 ticki)** | **−8 ms** |

Mechanizm: w chwili zmiany kierunku kąt korby przekracza granicę kwadratury i natychmiast wraca —
powstaje **jednotickowe** odbicie linii (2,99975 s: `ab=0`, 3,00000 s: `ab=1`). Zdecymowany zapis
próbkuje 2,99900 i 3,00000, w obu `ab=1` — całe zdarzenie wypada **między** wierszami. Pierwszą
widoczną zmianą linii jest dopiero następny krok kadencyjny (3,00900 s), czyli **po** widocznym
już inhibicie (3,00100 s). Stąd ujemna latencja i stąd „0,00 ms" wersji 2: kod szukał inhibitu
*od* błędnie późnej krawędzi i znajdował go natychmiast.

Dlatego P-1 jest oceniane **wyłącznie** na zapisach `sample_ticks=1`; zapis rzadszy daje
`NOT_EVALUATED` z podaną rozdzielczością, nigdy PASS.

## Proponowane kryteria i ich zmierzony werdykt

### P-1 — Inhibit kierunku musi być szybki i musi mieć powód → **PASS 8 / FAIL 0 / N.E. 59**

**Propozycja:** dla odwrotnej krawędzi PAS `pas_inhibit_reason` staje się niezerowy w
**≤ 25 ms od KRAWĘDZI LINII** (nie od czasu komendy). Werdykt bierze się z **górnej granicy**
przedziału latencji, więc obowiązuje niezależnie od nierozstrzygniętej kolejności wewnątrz ticka.

**Zmierzone na zapisach pełnej rozdzielczości (0,25 ms/wiersz):**

| Przypadek | komenda → krawędź | krawędź → inhibit | przedział | kolejność w ticku |
|---|---|---|---|---|
| `hires_reverse_cad72_speed0/18` | **5,00 ms** | 0,00 ms | [−0,25; +0,25] ms | nierozstrzygnięta |
| `hires_reverse_cad30_speed0/18` | **20,75 ms** | 0,00 ms | [−0,25; +0,25] ms | nierozstrzygnięta |
| `hires_bounce_cad72_speed0/18` | 0,00 ms | 0,00 ms | [−0,25; +0,25] ms | nierozstrzygnięta |
| `hires_bounce_cad30_speed0/18` | 0,00 ms | 0,00 ms | [−0,25; +0,25] ms | nierozstrzygnięta |

Odczyt: krawędź linii, przyjęcie przez sampler i inhibit padają **w tym samym ticku 4 kHz**.
Latencja jest więc **ograniczona jednym tickiem (≤ 0,25 ms)** — podana jako **granica**, nie jako
dokładne 0,00 ms. Kolejność wewnątrz jednego ticka nie ma tu obserwowalnej odpowiedzi: 4 kHz to
timebase, na którym produkcyjny foreground sam pracuje, więc nie istnieje drobniejsza obserwacja
w tym harnessie. Górna granica 0,25 ms leży daleko poniżej progu 25 ms, więc werdykt jest
rozstrzygnięty mimo tej nieoznaczoności.

**Sprostowanie wobec wersji 2.** Podawane tam „inhibit 1–21 ms" (v1) i „0,00 ms, ten sam tick,
12/12" (v2) były obie błędne, z różnych powodów:

* liczby v1 były w istocie odstępem **komenda → krawędź** (5,00 ms przy 72 rpm, 20,75 ms przy
  30 rpm) — czyli czasem, jakiego korba potrzebuje na obrócenie się wstecz przez granicę
  kwadratury. To **fizyka generatora, nie reakcja firmware**;
* „0,00 ms" v2 pochodziło z zapisu zdecymowanego, który tej krawędzi w ogóle nie widział.

`NOT_EVALUATED = 59`: 51 przypadków bez odwrotnej krawędzi (kryterium ich nie dotyczy) oraz
**8 przypadków `reverse_clean`/`reverse_bounce` zapisanych z `sample_ms=1`**, dla których zapis
nie rozstrzyga latencji. Te 8 nie jest zaliczone — mimo że te same scenariusze przechodzą w
wariancie `hires_*`.

**Próg pozostaje 25 ms.** Nie został zmieniony ani w górę, ani w dół.

### P-2 — Krótkie odbicie elektryczne nie może kasować asysty → **FAIL 8 / PASS 0 / N.E. 59**

**Propozycja:** pojedyncze odbicie linii PAS trwające **3 ticki (0,75 ms)** przy niezmienionym
ruchu korby do przodu nie zmienia `session` i nie obniża `iq_ref` poniżej **80 %** średniej
przypadku kontrolnego o tej samej kadencji i prędkości, we wspólnym oknie 1 s od zdarzenia.

**Zmierzone — kryterium NIE jest spełnione, w 8/8 przypadków:**

| Przypadek | session | `iq_ref` min | średnia kontroli | stosunek | próg |
|---|---|---|---|---|---|
| `hires_bounce_cad72_speed18` | 1 → **2** → 1 | 75 | 154,4 | **0,486** | 0,80 |
| `hires_bounce_cad72_speed0` | 1 → **2** → 1 | 75 | 154,5 | **0,485** | 0,80 |
| `hires_bounce_cad30_speed18` | 1 → **2** → 1 | 88 | 156,0 | **0,564** | 0,80 |
| `hires_bounce_cad30_speed0` | 1 → **2** → 1 | 88 | 156,2 | **0,564** | 0,80 |
| `reverse_bounce_cad72_speed18` | 1 → **2** → 1 | 99 | 158,0 | **0,627** | 0,80 |
| `reverse_bounce_cad72_speed0` | 1 → **2** → 1 | 99 | 158,0 | **0,626** | 0,80 |
| `reverse_bounce_cad30_speed18` | 1 → **2** → 1 | 79 | 155,1 | **0,509** | 0,80 |
| `reverse_bounce_cad30_speed0` | 1 → **2** → 1 | 79 | 155,2 | **0,509** | 0,80 |

Czas trwania zapadu (poniżej 80 % kontroli), zmierzony na przebiegach 4 kHz:

| | `session` w stanie 2 | `iq_ref` < 80 % kontroli | minimum |
|---|---|---|---|
| 72 rpm | 3,0 ms | **159 ms** | 48 % kontroli |
| 30 rpm | 8,25 ms | **336 ms** | 56 % kontroli |

**Sprostowanie wobec iteracji 011.** Wersja 1 stwierdzała „asysta utrzymana, obecny kod spełnia”.
To był błąd wnioskowania: patrzyłem na maksimum i na to, że `iq_ref` nie osiągnęło zera. **Brak
zera nie jest dowodem braku zapadu.** Dane zawsze pokazywały zarówno przejście `session`, jak i
zapad Iq; wersja 1 ich nie odczytała.

Osobno: część zapadu zmierzonego w iteracji 011 była artefaktem generatora (odbicie cofało korbę,
D011-01). Po naprawie **efekt pozostaje** i jest zachowaniem produkcji, nie harnessu — przebieg z
odbiciem jest bajt w bajt identyczny z kontrolą we wszystkich kanałach generatora
(`crank_angle_deg`, `torque_gen_nm`, `cadence_gen_rpm`, `crank_direction`, `pas_normal_ab`) na
wszystkich 8000 wierszach; różni się wyłącznie linia PAS w 3 tickach.

**Progu NIE obniżam do wartości, którą obecny kod przechodzi.** 0,80 jest propozycją do oceny
Mastera; jeżeli zapad do ~50 % na 160–340 ms po 0,75 ms zakłócenia elektrycznego jest akceptowalny,
to **próg** ma zostać świadomie zmieniony decyzją, a nie dopasowany po fakcie do wyniku.

**Zakres:** to jest zachowanie żądania Iq dla wymuszonych wejść. Czy 0,75 ms odbicie w ogóle
występuje na tym rowerze — `UNKNOWN`, `HW NOT_RUN`.

### P-3 — Nieważny czujnik potrzebuje własnej, szybkiej odpowiedzi → **NOT_EVALUATED (propozycja)**

**Propozycja:** gdy `torque_sensor_valid` albo `pas_sensor_valid` jest `false` przez ≥ 50 ms, tor
zgłasza **odrębny, obserwowalny** stan (własny powód inhibit, flaga debug albo przejście
`session`) w ≤ 100 ms, niezależnie od upływu `assist_hold_ms`.

**Zaobserwowane:** żaden nadzorczy stan nie reaguje (`pas_inhibit_reason`, `pas_direction_state`,
`session` — 0/4 w oknie do 2 s, potwierdzone niezależnie przez Mastera w review 011). `iq_ref`
osiąga tolerancję zera dopiero ~1,40 s po wejściu. Równość z `assist_hold_ms = 1400` z przyjętego
presetu pozostaje **[INFERRED]** — nie prześledziłem ścieżki decyzyjnej.

Nie oceniam tego programowo: kryterium mówi o „odrębnym obserwowalnym stanie”, a wybór, który
kanał miałby nim być, należy do projektu architektury, nie do wykonawcy testów. Zostaje
`NOT_EVALUATED` — świadomie, nie z braku danych.

**Ograniczenie zakresu (za review 011):** harness ustawia wyłącznie **badaną flagę** niesioną przez
tor. To **nie** jest dowód reakcji całego targetu na fizyczną awarię czujnika. Sformułowania
„uszkodzony czujnik nieodróżnialny od zaprzestania pedałowania” wolno używać **wyłącznie** o tej
fladze w tym harnessie. Martwe ADC, dryf, wartość poza zakresem i debouncing `Error 25` nie są
odwzorowane. Obserwacja zachowana dla projektu architektury; produkcji nie naprawiam.

### P-4 — Zakłócenie krawędzi PAS nie może być mylone z cofaniem → **DO ROZSTRZYGNIĘCIA**

Zgubiona krawędź daje `pas_inhibit_reason` = 1 (*reverse*) mimo jazdy do przodu — krok o dwie
pozycje jest z natury dwuznaczny kierunkowo. Asysta nie jest kasowana. Nie proponuję progu
liczbowego: brak podstawy do jego wyboru. Pytanie do Mastera: czy dwuznaczność kierunku ma być
raportowana jako osobna przyczyna.

### P-5 — Arytmetyka timebase musi przetrwać opóźnioną obsługę foreground → **egzekwowane testem**

Przy pominięciu `N` kolejnych wywołań foreground pierwsze wznowione wywołanie przekazuje
`elapsed_ticks == N + 1` do `torque_input_update_elapsed()` **i** do `ride_control_update()`,
właściciel Iq 16 kHz nie zatrzymuje się ani na krok, a `iq_ref` po wznowieniu pozostaje w zakresie
kontroli.

To kontrakt targetu, nie wybór: `main.c:2626` liczy `control_delta = control_now -
control_prev_processed_tick`, `main.c:2646` traktuje `control_delta - 1` jako ticki utracone.
Zmierzone dla `N` = 1, 4, 20 przy pełnej rozdzielczości 4 kHz: `elapsed` = 2, 5, 21;
`fast_iq_slew_ticks` rośnie o 4 w każdym ticku, także w czasie zastoju. Egzekwowane bezwarunkowo
przez `test_disturbances.py::test_fg_delay_mandatory`.

**Zakres:** 1, 4 i 20 ticków (0,25 / 1 / 5 ms) to **punkty ćwiczenia API**, nie limity bezpieczne
dla sprzętu.

### P-6 — Recovery musi wracać do poziomu kontrolnego bez przestrzału → **PASS 56 / FAIL 0 / N.E. 11**

**Propozycja:** po ustaniu zakłócenia średnia `iq_ref` w ustabilizowanym oknie (od `koniec + 1 s`
do końca przebiegu) mieści się w **± 10 %** średniej dopasowanej kontroli, a szczyt nie przekracza
**125 %** maksimum kontroli.

**Zmierzone na 56 ocenionych przypadkach:** `mean_ratio` **0,990–1,000**, `peak_ratio`
**1,000–1,010**. Margines do obu progów jest duży. Obejmuje to również oba warianty odbicia:
mimo zapadu z P-2 asysta **w pełni wraca** — zapad jest przejściowy, nie trwały.

`NOT_EVALUATED` w 11 przypadkach: puste okno po recovery (zdarzenie zbyt blisko końca krótkiego
przebiegu — `fg_delay_hires`, cztery `hires_bounce`) oraz brak dopasowanej kontroli (m.in. cztery
nowe `hires_reverse`, które trwają 2,5 s i nie mają kontroli o tej długości). To są luki pokrycia,
**nie** ciche zaliczenia.

**Ograniczenie zakresu przyjęte za REVIEW-EVD-AP-01-012:** P-6 porównuje wyłącznie **okno
ustabilizowane od `koniec zakłócenia + 1 s`**. PASS 56 **nie dowodzi**, że w samym przejściu nie
wystąpił chwilowy przestrzał — okno przejściowe nie jest tym kryterium badane. Cztery
`hires_bounce` mają puste okno i **nie** są wśród potwierdzonych.

## Czego te kryteria nie obejmują

- Rzeczywistego prądu, mostka i zachowania sprzętowego — `NOT_MEASURED`, `HW NOT_RUN`.
- Fizycznej awarii czujnika (P-3, ograniczenie zakresu).
- Strony silnika przy cofaniu: `motor_erps` jest wyprowadzone jako `cadence_gen * 4/3` i **bez
  znaku**, więc żadna próba reverse nie dotyka obsługi kierunku po stronie silnika.
- Latencji przerwań, wywłaszczania i jittera między zegarami — harness ma jedną pętlę
  (`sim/controller_lab/README.md`, „Clock map”, odstępstwa 1–3).
- Progów dla `pas_edge_jitter`: inhibit zgłoszony w 4/8 przypadków, za słaba podstawa.
  `UNKNOWN`, nie `REJECTED`.
- Kombinacji kilku zakłóceń naraz — każda próba wstrzykuje jedną klasę.
