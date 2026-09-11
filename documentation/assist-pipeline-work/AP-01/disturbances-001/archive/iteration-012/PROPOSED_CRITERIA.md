# AP-01 disturbances-001 — proponowane kryteria inhibit / recovery / timebase

**Status: PROPOZYCJA. Nic tutaj nie jest zatwierdzone i nic nie jest wymaganiem.**
Kryteria proponuje wykonawca AP-01; przyjęcie ich należy do Mastera i właściciela projektu.
Zapisane **przed** kandydatami na zmianę firmware, zgodnie z kartą.

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

## Cztery różne chwile, których nie wolno mylić (D011-02)

```text
KOMENDA            czas z karty scenariusza — kiedy generator DOSTAŁ polecenie
KRAWĘDŹ ELEKTR.    pierwszy tick, w którym linia PAS faktycznie się zmieniła
KRAWĘDŹ PRZYJĘTA   pierwszy tick, w którym licznik samplera produkcji drgnął
ODPOWIEDŹ          pierwszy tick, w którym stan nadzorczy się zmienił
```

Są to cztery różne chwile i różnice między nimi bywają duże. Zmierzone dla `reverse_clean`:

| Przypadek | komenda | krawędź elektryczna | opóźnienie generatora |
|---|---|---|---|
| `reverse_clean_cad30_*` | 3,00000 s | 3,02100 s | **21,0 ms** |
| `reverse_clean_cad72_*` | 3,00000 s | 3,00900 s | **9,0 ms** |

Przy 30 rpm korba potrzebuje 21 ms, żeby w ogóle wygenerować pierwszą odwrotną krawędź kwadratury
po zmianie kierunku. **To jest opóźnienie GENERATORA, nie reakcja firmware.** Wersja 1 tego
dokumentu podawała „inhibit 1–21 ms” — te liczby były w całości tym opóźnieniem. Mierzone od
krawędzi elektrycznej inhibit pojawia się w **tym samym ticku (0,00 ms)** we wszystkich 12
przypadkach. `analyze.py::pas_edge_timing()` raportuje te chwile osobno dla każdego zdarzenia.

## Proponowane kryteria i ich zmierzony werdykt

### P-1 — Inhibit kierunku musi być szybki i musi mieć powód → **PASS 12 / FAIL 0 / N.E. 51**

**Propozycja:** dla odwrotnej krawędzi PAS `pas_inhibit_reason` staje się niezerowy w
**≤ 25 ms od KRAWĘDZI ELEKTRYCZNEJ** (nie od czasu komendy).

**Zmierzone:** 12/12 przypadków z odwrotną krawędzią (`reverse_clean`, `reverse_bounce`,
`reverse_bounce_hires`) — latencja **0,00 ms**, czyli ten sam tick. Margines do progu jest pełny.
`NOT_EVALUATED` dla 51 przypadków, w których żadna odwrotna krawędź nie występuje (invalid,
timebase, PAS edge, kontrole) — kryterium ich nie dotyczy, więc nie są liczone jako PASS.

Obecny kod spełnia. Kryterium ma **chronić** to zachowanie przed regresją.

### P-2 — Krótkie odbicie elektryczne nie może kasować asysty → **FAIL 8 / PASS 0 / N.E. 55**

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

### P-6 — Recovery musi wracać do poziomu kontrolnego bez przestrzału → **PASS 56 / FAIL 0 / N.E. 7**

**Propozycja:** po ustaniu zakłócenia średnia `iq_ref` w ustabilizowanym oknie (od `koniec + 1 s`
do końca przebiegu) mieści się w **± 10 %** średniej dopasowanej kontroli, a szczyt nie przekracza
**125 %** maksimum kontroli.

**Zmierzone na 56 ocenionych przypadkach:** `mean_ratio` **0,990–1,000**, `peak_ratio`
**1,000–1,010**. Margines do obu progów jest duży. Obejmuje to również oba warianty odbicia:
mimo zapadu z P-2 asysta **w pełni wraca** — zapad jest przejściowy, nie trwały.

`NOT_EVALUATED` w 7 przypadkach: 4 × okno po recovery jest puste (zdarzenie zbyt blisko końca
przebiegu — przypadki `fg_delay_hires` i `miss_tick` o krótkim ogonie), 3 × brak dopasowanej
kontroli. To są luki pokrycia, **nie** ciche zaliczenia.

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
