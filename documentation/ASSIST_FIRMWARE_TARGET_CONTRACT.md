# Docelowy tor wspomagania w firmware silnika

Status: projekt roboczy, 2026-09-08. Kierunek ustalony przez właściciela: **najpierw spójne działanie silnika, później CANable dopasowane do tego działania**. Dokument rozwija [master plan](ASSIST_PIPELINE_MASTER_PLAN.md). Nie opisuje jeszcze wdrożonego kodu.

## 1. Zachowanie, które projektujemy

Podczas ciągłego pedałowania silnik ma odpowiadać na wysiłek rowerzysty z ograniczoną pulsacją między naciśnięciami. Ma rozpoznawać świadome zwiększenie i zmniejszenie wysiłku oraz szybko reagować na zakończenie pedałowania. Start, restart po toczeniu, zwykłe doliny siły i zatrzymanie są odrębnymi zdarzeniami.

Płynność nie może wynikać wyłącznie z podniesienia średniej pomocy, przycięcia szczytów limitem lub przedłużenia wspomagania po zatrzymaniu nóg. Te wielkości mierzymy oddzielnie.

Obecne funkcje mogą zostać połączone, zastąpione lub usunięte. Nie zakładamy obowiązku utrzymania hold, floor, okna stopni, czterech ramp ani nazw trybów w obecnym kształcie. Każdy zachowany mechanizm musi mieć samodzielne uzasadnienie w zachowaniu napędu.

## 2. Kolejność obliczeń — proponowany kontrakt

| Krok | Wejście i wynik | Właściciel / zakaz mieszania odpowiedzialności |
|---|---|---|
| 1. Fakty z czujników | ADC, PAS, wheel, Hall, prądy, napięcie, temperatura, timestamp → spójny snapshot | Właściciele czujników; nie wyprowadzają zgody na wspomaganie z filtrowanego Iq |
| 2. Walidacja i kalibracja | ADC → fizyczna siła na pedale + status wiarygodności; PAS → kierunek, ruch, wiek zbocza | Sensor conditioning. Surowa informacja o braku nacisku pozostaje dostępna obok filtrowanej siły |
| 3. Szybki sygnał i wysiłek roboczy | Kalibrowana siła + historia ruchu → szybka siła, oszacowany wysiłek, wiek i jakość | Jeden estimator; jeden writer każdego wyjścia, bez nadpisywania kątowego wyniku innym filtrem |
| 4. Stan wspomagania | Fakty ruchu/siły/bezpieczeństwa → stan, zdarzenie start/stop/restart, dozwolone źródła | Jeden lifecycle; nie potrzebuje dodatniego wyniku trybu, aby rozpoznać pedałowanie |
| 5. Wybór wejścia trybu | Szybka/robocza estymata + lifecycle → jawne wejście startowe lub robocze | Polityka przekazania w estimatorze/lifecycle; tryb nie wybiera sam konkurencyjnego FAST/RUN |
| 6. Charakterystyka wspomagania | Wysiłek + kadencja + profil → żądanie momentu lub mocy, z oznaczoną jednostką | Funkcja trybu; bez pamięci podtrzymania, zgody na ruch i rampy końcowej |
| 7. Funkcje dodatkowe | Żądanie + zdarzenia lifecycle → żądanie po start boost / przedłużeniu, jeśli funkcja zostaje | Dodatki mają jawny punkt działania i warunki zakończenia; żadnego ukrytego ponownego boostu w dolinie |
| 8. Normalizacja i ograniczenie źródła | Żądanie fizyczne + stan napędu → Iq żądane i dozwolone dla źródła | Jedna konwersja jednostek; wszystkie dodatki podlegają limitowi źródła/profilu |
| 9. Wybór źródła | Pedal / throttle / Walk / service → jedno wybrane żądanie i reason | Arbitraż; jawne priorytety i uprawnienia. Brak przypadkowego sumowania |
| 10. Ograniczenia globalne | Żądanie + bateria/termika/speed → target Iq, cap, klasy reakcji | Jeden wynik ograniczeń dla napędu. Limity zależne od źródła uwzględniają jego oznaczenie |
| 11. Polecenie wykonawcze | Target + lifecycle + ograniczenia → kompletne polecenie do szybkiej pętli | 4 kHz publikuje atomowo spójny zestaw; nie zapisuje bezpośrednio końcowego Iq |
| 12. Trajektoria i obwiednia | Polecenie + aktualny stan trajektorii → final Iq reference | Jeden writer w 16 kHz; start/run/release i ewentualny hard clamp należą do tego właściciela |
| 13. FOC i mostek | Iq reference + zmierzone prądy/kąt → napięcie/PWM i stan mostka | Regulator prądu oraz właściciel mostka; zakończenie momentu oceniane także po prądzie rzeczywistym |

To kolejność logiczna, nie nakaz utworzenia 13 nowych plików. Obliczenia ochronne mogą działać wcześniej i w ISR; ich priorytet nie zależy od dojścia do kroku 10. Mostek może wymusić wyłączenie niezależnie od toru komfortu.

## 3. Jednostki i granice konwersji

- ADC/native pozostaje w warstwie czujnika. Do sterowania przekazujemy skalibrowaną siłę z jawnym przelicznikiem; nazwa `torque` nie może oznaczać raz native, raz siły, a raz momentu.
- Moment na korbie wynika z siły i długości korby. Moc mechaniczna człowieka wynika z momentu i prędkości kątowej. Przy braku obrotu moc wynosi zero mimo dodatniego momentu.
- Dlatego startu nie rozwiązujemy przez podstawianie fikcyjnej kadencji do całego toru. Proponowanym kontraktem jest osobna startowa charakterystyka momentu, z kontrolowanym przejściem do charakterystyki roboczej.
- Tryb może naturalnie opisywać wsparcie momentem albo mocą, ale musi jawnie zadeklarować wielkość. Wspólny adapter normalizuje wynik do phase Iq. Nie wymuszamy pozornej równoważności „torque assist” i battery-current-based power.
- Docelowa konwersja momentu mechanicznego na Iq wymaga zweryfikowanego przelicznika silnika/przekładni; nie wpisujemy go na podstawie samej nazwy silnika. Dla mocy trzeba określić, czy limit oznacza moc elektryczną pobieraną, czy mechaniczną oddawaną. To otwarta decyzja techniczna przed implementacją adaptera.
- W pierwszym porównaniu estimatorów zachowujemy obecny adapter mocy/Iq jako kontrolowane tło. Zmiana estymacji i zmiana fizycznej konwersji naraz uniemożliwiłyby przypisanie poprawy do właściwego etapu.

Obliczenia produkcyjne pozostają fixed-point; dokładną skalę i szerokości liczb dobieramy po analizie zakresu, błędu i czasu wykonania. Proponowane nazwy w tym dokumencie nie są nowym formatem CAN.

## 4. Dwie niezależne informacje: czy wolno pomagać i ile pomagać

Lifecycle rozpoznaje stan ruchu, estimator ocenia wysiłek. Dodatnia pamięć estymatora nie może otworzyć permission. Z kolei chwilowo zerowa próbka siły przy ważnym PAS nie może automatycznie zamknąć całej sesji.

Proponowane stany pedałowania:

| Stan | Znaczenie i zachowanie wyjścia | Przejście / reset |
|---|---|---|
| IDLE | Brak aktywnego wspomagania pedałowego | Start tylko ze świeżych faktów zgodnych z polityką startu |
| STARTING | Nacisk i dowody rozpoczęcia ruchu potwierdzone; estymator roboczy jeszcze się wypełnia | Przejście do RUNNING przez zdefiniowane przekazanie estymaty, nie skok Iq |
| RUNNING | Pedałowanie trwa; doliny siły są elementem cyklu | Spadek siły zmienia demand; potwierdzony stop rozpoczyna STOPPING |
| STOPPING | Zwykłe zakończenie pedałowania; nowe żądanie jest zerowe albo wynika z jawnej funkcji przedłużenia | Jedna kontrolowana trajektoria do zera; po zakończeniu COASTING lub IDLE |
| COASTING | Koło nadal jedzie, pedały stoją, pomoc pedałowa zakończona | Zachowuje kontekst do szybkiego restartu, lecz nie dodatnie stare żądanie |
| INHIBITED | Hamulec, błąd lub direction inhibit | Reakcja według przyczyny; restart wymaga świeżych warunków, nie samego usunięcia inhibit |

Walk i service są trybami własności źródła, a nie dodatkowymi stanami RUNNING pedałowania. Nie mnożymy jednej wielkiej maszyny stanów przez wszystkie tryby jazdy. Detektor kierunku PAS pozostaje właścicielem faktu direction inhibit; lifecycle jest jego konsumentem, nie drugim dekoderem zboczy.

W przypadku krótkiego zatrzymania i restartu przed zakończeniem STOPPING nowa trajektoria zaczyna się od aktualnej referencji. Nie zerujemy arbitralnie żywego akumulatora, nie odtwarzamy zapamiętanego pre-stop Iq i nie uruchamiamy ponownie release co tick.

Funkcja startu bez obrotu jest osobną decyzją produktową. Jeśli zostanie przyjęta, wymaga kompletnej gałęzi lifecycle z warunkami wejścia, czasu trwania i zakończenia; nie może ograniczać się do wyjątku wewnątrz wzoru trybu.

## 5. Właściciele pamięci i resetów

| Pamięć | Jedyny właściciel | Kiedy zachować / kiedy unieważnić |
|---|---|---|
| Kalibracja i zero czujnika | Sensor conditioning | Zmiana kalibracji unieważnia estymatę wysiłku; zwykła zmiana profilu nie |
| Historia FAST i wysiłku RUN | Effort estimator | Dolina: zachować; stop: jawnie oznaczyć starzenie; nie przenosić dodatniego wysiłku bezwarunkowo na restart |
| Kierunek i wiek PAS | Dekoder PAS/liveness | Reset tylko z zasad dekodera; downstream nie nadpisuje ich dla łatwiejszego startu |
| Stan jazdy i powód przejścia | Assist lifecycle | Zmiana źródła/awaria mają jawne zdarzenia; brak dodatkowej bramki z własnym konkurencyjnym latch |
| Startup boost | Polityka boost | Nowy start może uzbroić; dolina nacisku i chwilowy limiter nie uzbrajają |
| Przedłużenie pomocy | Polityka przedłużenia, jeżeli zachowana | Jedno zdarzenie aktywacji i skończony termin; hamulec/inhibit mają pierwszeństwo |
| Histereza ograniczenia baterii | Limiter baterii | Zachować jako ochronę pomiarową; nie włączać do filtra komfortu |
| Żywa trajektoria Iq | Właściciel 16 kHz | Zmiana targetu zachowuje ciągłość; reset tylko boot/bridge-off przygotowanie albo zdefiniowana reakcja twarda |
| PI i wyciszenie mostka | FOC/bridge lifecycle | Własne warunki zero/quiet, zsynchronizowane z powodem wyłączenia, nie każdym target=0 |

## 6. Co zastępujemy, zamiast dokładać

1. Obecny RUN ring + nadpisujący filtr czasowy zastępuje **jeden estimator** wybrany na podstawie prób. Jeśli ma kilka składowych, ich połączenie jest jawne i ma jedno wyjście oraz jednego właściciela.
2. Rozpoznawanie dolin wysiłku przenosimy do estymatora i lifecycle. **Hold/floor nie są domyślnym sposobem naprawy pulsacji.** Ich pozostawienie wymaga osobnej potrzeby użytkowej; w wariancie bazowym docelowego toru nie zakładamy takiej podłogi.
3. Smooth start jako mnożnik przed kolejną rampą zastępuje polityka rozpoczęcia jednej trajektorii. Szybkość reakcji estymatora nadal jest osobną, mierzoną cechą.
4. Release ma jednego właściciela i rozpoczyna się od faktycznego stanu Iq reference. Nie dodajemy milcząco czasów kilku liczników. Jeśli pozostaje zamierzone przedłużenie, raportujemy osobno jego czas i właściwy release.
5. Nieaktywne power filters nie wracają do toru tylko dlatego, że są zapisane w starym banku.
6. Cztery obecne rampy nie są wymaganiem. Docelowa polityka może potrzebować mniej lub innych parametrów. Sam wzrost prędkości koła nie musi automatycznie wybierać najszybszego opadania pomiędzy naciśnięciami.

## 7. Limity i jeden końcowy writer

Dla każdego źródła obowiązuje: **wszystkie jego bonusy i ewentualne minima → cap źródła → arbitraż → ograniczenia globalne**. Procenty zawsze mają nazwaną podstawę. Test graniczny z audytu (profil 1%, floor 25%) ma w przyszłym torze wykazać brak obejścia profilu, niezależnie od tego, czy floor zostanie usunięty czy zachowany.

Rozdzielamy:

- `target_iq`: cel, do którego zwykła trajektoria może dochodzić;
- `hard_iq_ceiling`: obwiednia, której referencja nie może przekroczyć po przyjęciu nowego polecenia;
- `stop_policy` i `stop_reason`: sposób oraz przyczyna zakończenia napędu.

To propozycja kontraktu wykonawczego, nie gotowa zmiana mailboxa. Obecny battery cap jest upstream targetem; nie przekształcamy go automatycznie w hard clamp. Każdemu ograniczeniu przypiszemy klasę reakcji na podstawie wymagań elektrycznych i testu zamkniętej pętli. Przerwanie nadprądowe/wyłączenie mostka pozostaje niezależną ochroną i nie czeka na mailbox.

Nawet przy hard clamp końcowy zapis referencji należy do tego samego właściciela 16 kHz. Walk może dostarczać własną trajektorię przy jawnym przekazaniu sterowania; wszystkie drogi nadal podlegają właściwym ograniczeniom. Zmiana źródła musi określać ciągłość referencji, stan akumulatora i zachowanie PI.

## 8. Punkty pomiarowe potrzebne do wyboru rozwiązania

Minimalny ślad hosta: timestamp, raw/kalibrowana siła, FAST, effort, wiek/jakość effort, PAS i jego wiek, surowa/sterująca kadencja, lifecycle i reason, wybrane wejście trybu, wynik trybu z jednostką, wynik po dodatkach, source target/cap, wybrane źródło, global target/cap, policy, final Iq reference.

W zamkniętej pętli dodatkowo: Iq actual, battery current, napięcie, utilization/duty, prędkość silnika, stan mostka i powód ograniczenia. Dopiero porównanie tych etapów pokaże, czy fala powstaje w wysiłku, mapowaniu, konwersji, limiterze czy wykonaniu.

To wymaganie obserwowalności firmware/hosta. Nie wymaga teraz nowego ekranu CANable ani wysyłania wszystkich kanałów jednocześnie po CAN. Dobór kanałów, pasma i wersji telemetrycznej jest późniejszą pracą integracyjną.

## 9. Następna praca techniczna

Przydział agentów i bramki odbioru określa [instrukcja wykonawcza](../../integration/tasks/ASSIST_PIPELINE_AGENT_WORK.md). Ten kontrakt pozostaje projektem do niezależnego sprawdzenia i próby; żaden wykonawca nie może potraktować go jako samodzielnego zezwolenia na przebudowę całego toru.

1. Ustalić scenariusze porównujące estimator A/B: doliny, nierówne nogi, skok/odpuszczenie siły, start, stop, toczenie, restart i nieregularny PAS. Testować również niską kadencję, gdzie kompromis jest najtrudniejszy.
2. Najpierw porównać same estymaty w domenie siły przy identycznym wejściu. Mierzyć tłumienie pulsacji, bias średniej, opóźnienie i starzenie po stopie. Nie wybierać na podstawie samej amplitudy Iq przy saturacji.
3. Kandydatów podłączyć do tego samego produkcyjnego dalszego toru i jawnych parametrów hosta, a następnie do symulacji z obciążeniem. Obecny serializer może odtworzyć bazę historyczną; nie definiuje parametrów kandydatów.
4. Wybrać wariant oraz politykę start/restart/stop na podstawie wyników, zapisać decyzję i dopiero wtedy zastępować obecnych właścicieli. Osobno sprawdzić koszt RAM/CPU na target.

Gotowość do przygotowania CANable następuje po ustaleniu znaczenia funkcji i parametrów firmware. Wcześniej nie odtwarzamy starych suwaków ani nie stabilizujemy nowego wire schema. Obsługa starych danych może oznaczać migrację lub jawne odrzucenie; nigdy ciche przypisanie starego pola do nowej funkcji.
