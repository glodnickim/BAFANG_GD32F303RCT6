# AP-01 disturbances-001 — proponowane kryteria inhibit / recovery / timebase

**Status: PROPOZYCJA. Nic tutaj nie jest zatwierdzone i nic nie jest wymaganiem.**
Kryteria proponuje wykonawca AP-01; przyjęcie ich należy do Mastera i właściciela projektu.
Zapisane **przed** kandydatami na zmianę firmware, zgodnie z kartą.

Źródło liczb: `results/disturbances-metrics.json` z przebiegu opisanego w
`results/disturbances-results.json` (74 przypadki, każdy wykonany dwukrotnie z identycznym
SHA-256). Metoda pomiaru: `analyze.py`, prymitywy z zaakceptowanych `metrics.py` /
`event_metrics.py`.

## Kwalifikacja — czego te liczby NIE są

`controller_lab.c` nie zamyka pętli PMSM/FOC. `u_abs` i zmierzony prąd baterii są stale zerem.
Kadencja, moment i prędkość są **wymuszonymi wejściami syntetycznymi**. To są pomiary
**kompozycji ŻĄDANIA Iq** dla zadanych wejść — nie moment, nie prąd rzeczywisty, nie zachowanie
mostka ani sprzętu. Żaden próg poniżej nie jest limitem bezpieczeństwa HW i żaden nie został
potwierdzony na rowerze (`HW NOT_RUN`).

Progi dobrano z **zaobserwowanego marginesu**, a nie tak, aby obecny kod przeszedł: tam, gdzie
zachowanie produkcyjne wygląda na wątpliwe (P-3), kryterium jest napisane tak, że **obecny kod go
NIE spełnia** — i to jest celowe.

## Zmierzone zachowanie (podstawa progów)

| Klasa zakłócenia | inhibit zgłoszony | opóźnienie inhibit | Iq → 0 | opóźnienie Iq → 0 |
|---|---|---|---|---|
| reverse ciągły (2 s) | 4/4 | 1–21 ms | 4/4 | 195–219 ms |
| reverse bounce (1 × 3 ticki) | 4/4 | 0 ms | 0/4 | — (asysta utrzymana) |
| `torque_sensor_valid=false` | **0/4** | — | 4/4 | **1403–1409 ms** |
| `pas_sensor_valid=false` | **0/4** | — | 4/4 | **1403–1409 ms** |
| nielegalna sekwencja PAS | 4/4 | 0–1 ms | 0/4 | — |
| zgubiona krawędź PAS (co 3.) | 4/4 | 27–43 ms | 0/4 | — |
| spóźniona krawędź PAS (jitter) | 4/8 | 1–10 ms | 0/8 | — |
| `miss_tick` 2/4/20 | 0/12 | — | 0/12 | — |
| `fg_delay` 1/4/20 ticków | 0/15 | — | 0/15 | — |

Wartości `iq_ref` w oknach po zdarzeniu mieszczą się w zakresie przypadków kontrolnych
(`control_8s_*`, `control_6s_*`) wszędzie poza reverse ciągłym.

## Proponowane kryteria

### P-1 — Inhibit kierunku musi być szybki i musi mieć powód
**Propozycja:** dla rzeczywistego cofania korby `pas_inhibit_reason` przechodzi na wartość
niezerową w **≤ 25 ms** od pierwszej odwrotnej krawędzi, a `pas_direction_state` opuszcza
`FORWARD_SAFE` w tym samym oknie.

**Uzasadnienie:** zmierzone 1–21 ms w 4/4 przypadkach; 25 ms daje ok. 20 % marginesu nad
najgorszym pomiarem i jest znacznie poniżej 195 ms, po których i tak zanika Iq. Obecny kod to
spełnia — kryterium ma **chronić** to zachowanie przed regresją, nie wymuszać zmiany.

### P-2 — Krótkie odbicie elektryczne nie może kasować asysty
**Propozycja:** pojedyncze odbicie linii PAS trwające **≤ 3 ticki (0,75 ms)** przy utrzymanej
jeździe do przodu nie może obniżyć `iq_ref` poniżej **80 %** średniej z przypadku kontrolnego o
tej samej kadencji i prędkości, ani zmienić `session`.

**Uzasadnienie:** zmierzone — asysta utrzymana (`iq_ref` max 173–186 wobec kontroli 155–179 śr.),
`iq_ref` nigdy nie dochodzi do zera, stan końcowy identyczny z kontrolą. Obecny kod spełnia.

> **Uwaga metodologiczna (ważna).** W pierwszym przebiegu tego zadania to kryterium wyglądało na
> złamane: asysta ginęła na stałe. Przyczyną był **błąd generatora**, nie firmware —
> `reverse_bounce_at_s` uzbrajał się ponownie co 3 ticki do końca przebiegu, więc „pojedyncze
> odbicie” było w rzeczywistości zakłóceniem okresowym. Naprawione (`reverse_bounce_done`) i
> zmierzone ponownie. Zapisane tutaj, bo to dokładnie ten rodzaj artefaktu, który bez sprawdzenia
> trafiłby do raportu jako „znaleziony błąd firmware”.

### P-3 — Nieważny czujnik potrzebuje własnej, szybkiej odpowiedzi — **OBECNIE NIESPEŁNIONE**
**Propozycja:** gdy `torque_sensor_valid` albo `pas_sensor_valid` jest `false` przez
**≥ 50 ms**, tor musi zgłosić **odrębny, obserwowalny** stan (własny powód inhibit, flaga
debug albo przejście `session`) w **≤ 100 ms**, niezależnie od upływu `assist_hold_ms`.

**Zaobserwowane zachowanie:** żaden nadzorczy stan nie reaguje (`pas_inhibit_reason`,
`pas_direction_state`, `session` — 0/4 `MEASURED` w oknie do 2 s). `iq_ref` schodzi do zera
dopiero po **1403–1409 ms**, co odpowiada wygaśnięciu `assist_hold_ms = 1400` z zaakceptowanego
presetu (`ap01_runner.TUNING`) — [INFERRED] z równości liczb, nie z odczytu ścieżki decyzyjnej.
Innymi słowy: uszkodzony czujnik jest dziś nieodróżnialny od „rowerzysta przestał pedałować”,
a asysta trwa jeszcze ~1,4 s.

**To jest OBSERWACJA, nie poprawka.** Karta zabrania naprawiania firmware w tym przydziale.
Kryterium zapisano celowo tak, że obecny kod go nie spełnia — próg 100 ms **nie** został dobrany
pod istniejące zachowanie.

**Czego to NIE dowodzi:** harness ustawia wyłącznie flagi `*_sensor_valid` niesione przez tor.
Nie modeluje martwego ADC, dryfu, wartości poza zakresem ani debouncingu `Error 25`. Zakres
twierdzenia = „flaga `false` nie wywołuje odrębnej reakcji”, nic więcej.

### P-4 — Zakłócenie krawędzi PAS nie może być mylone z cofaniem — **DO ROZSTRZYGNIĘCIA**
**Propozycja:** zgubiona lub spóźniona krawędź PAS przy jeździe do przodu albo (a) nie zgłasza
inhibit w ogóle, albo (b) zgłasza go z **powodem odróżnialnym** od cofania.

**Zaobserwowane zachowanie:** zgubiona krawędź daje `pas_inhibit_reason` = 1 (*reverse*) w 4/4
przypadków po 27–43 ms, mimo że korba przez cały czas jedzie do przodu. Krok o dwie pozycje jest
z natury dwuznaczny kierunkowo, więc dekoder liczy go jako cofnięcie (`pas_sampler_reverse`
rośnie). Asysta **nie** jest kasowana (`iq_ref` nigdy nie dochodzi do zera, poziom jak w
kontroli), więc skutek jest dziś nieszkodliwy.

**Otwarte pytanie do Mastera:** czy dwuznaczność kierunku ma być raportowana jako osobna
przyczyna. Nie proponuję progu liczbowego — brak podstawy do jego wyboru.

### P-5 — Arytmetyka timebase musi przetrwać opóźnioną obsługę foreground
**Propozycja:** przy pominięciu `N` kolejnych wywołań foreground pierwsze wznowione wywołanie
przekazuje `elapsed_ticks == N + 1` do `torque_input_update_elapsed()` **i** do
`ride_control_update()`, właściciel Iq 16 kHz nie zatrzymuje się ani na jeden krok, a `iq_ref` w
oknie po wznowieniu pozostaje w zakresie przypadku kontrolnego.

**Uzasadnienie:** to jest kontrakt targetu, nie wybór — `main.c:2626` liczy
`control_delta = control_now - control_prev_processed_tick` i `main.c:2646` traktuje
`control_delta - 1` jako ticki utracone. Zmierzone dla `N` = 1, 4, 20 przy pełnej rozdzielczości
4 kHz (`sample_ticks=1`): `elapsed` = 2, 5, 21; `fast_iq_slew_ticks` rośnie o 4 w każdym
emitowanym ticku, także w czasie zastoju; brak inhibit i brak zejścia Iq do zera. Egzekwowane
bezwarunkowo przez `test_disturbances.py::test_fg_delay_mandatory`.

**Zakres:** 1, 4 i 20 ticków (0,25 / 1 / 5 ms) to **punkty ćwiczenia API**, nie limity
bezpieczne dla sprzętu. Nic tutaj nie mówi, jak długi zastój przeżywa prawdziwy sterownik.

### P-6 — Recovery musi wracać do poziomu kontrolnego bez przestrzału
**Propozycja:** po ustaniu dowolnego zakłócenia z tej serii średnia `iq_ref` w ustabilizowanym
oknie po zdarzeniu mieści się w **± 10 %** średniej przypadku kontrolnego o tej samej kadencji i
prędkości, a szczyt chwilowy nie przekracza **125 %** maksimum kontroli.

**Uzasadnienie:** zmierzone maksima po zdarzeniu 179–224 wobec maksimów kontrolnych 179–208;
najgorszy stosunek ok. 1,08. Próg 1,25 zostawia margines, ale nadal wykryłby przestrzał typu
„skok po odzyskaniu sygnału”, którego karta każe szukać.

## Czego te kryteria nie obejmują

- Rzeczywistego prądu, mostka i zachowania sprzętowego — `NOT_MEASURED`.
- Rzeczywistego uszkodzenia czujnika (patrz P-3, ograniczenie zakresu).
- Strony silnika przy cofaniu: `motor_erps` jest w harnessie wyprowadzone jako
  `cadence_gen * 4/3` i **bez znaku**, więc żadna próba reverse nie dotyka obsługi kierunku po
  stronie silnika.
- Latencji przerwań, wywłaszczania i jittera między zegarami — harness ma jedną pętlę
  (`sim/controller_lab/README.md`, „Clock map”, punkty 1–3 odstępstw).
- Progów dla `pas_edge_jitter`: inhibit zgłoszony w 4/8 przypadków (zależnie od
  `pas_edge_jitter_ticks`), co jest za słabą podstawą do progu. `UNKNOWN`, nie `REJECTED`.
