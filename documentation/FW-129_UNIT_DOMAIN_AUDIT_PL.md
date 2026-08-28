# FW-129 — AUDYT DOMEN JEDNOSTEK (etap 1 karty, przed jakąkolwiek zmianą produkcyjną)

Status: **AUDYT ZAKOŃCZONY — ZERO ZMIAN W KODZIE.**
Data: 2026-08-28. Baza: gałąź `master`, ostatni commit `13b5c6a` (FW-128B1).

Karta FW-129 §3 nakazuje wykonać audyt domen jednostek zanim powstanie jakakolwiek zmiana
produkcyjna. Ten dokument jest tym audytem: część A (BEFORE) i część B (FOUND DEFECTS) raportu
z §40, plus projekt części C (AFTER) i D (Canable) do akceptacji.

---

## 0. STRESZCZENIE PROSTYM JĘZYKIEM

Rower ma dziś dwa niezależne „mózgi" liczące, ile pomocy dać, i one się ze sobą nie zgadzają.

1. **Jeden mózg liczy poprawnie moc.** Bierze nacisk na pedał w kilogramach, mnoży przez tempo
   pedałowania i wie, ile watów wkłada rowerzysta. Potem mnoży to przez ustawione „wspomaganie
   320%" i wie, ile watów ma dołożyć silnik. To jest liczone dobrze.

2. **Drugi mózg wyznacza rzeczywisty prąd silnika** — i on ignoruje wynik pierwszego. Ma własną,
   wymyśloną regułę: „60 kg nacisku przy 500% wspomagania = pełny prąd". Ta reguła nie wynika
   z niczego fizycznego. Nikt jej nie zmierzył na tym silniku.

Wynik pierwszego mózgu służy tylko jako **sufit** („nie więcej niż tyle watów"), a nie jako
polecenie. W praktyce polecenie wydaje zawsze ten drugi, wymyślony.

**Skutek, policzony arytmetycznie (sekcja 3, wada D1):** przy ustawionym wspomaganiu 320%
rower fizycznie nie jest w stanie dać 320%. Najlepsze, co może dać przy 48 V, to około
**połowy** tej liczby, a im wolniej kręci się silnik, tym mniej. Żeby dostać naprawdę 320%,
sterownik musiałby użyć **więcej niż 100% dostępnego napięcia** — czyli to nie jest kwestia
strojenia, tylko błędu w równaniu.

**Tryb eMTB jest jeszcze słabszy.** Na tym samym poziomie 3 daje około **37%** wspomagania tam,
gdzie Power Linear daje ~167%. Czyli eMTB jest około **4,5 raza słabszy** niż Power Linear przy
tym samym numerze poziomu — i podnoszenie „eMTB sensitivity" tego nie naprawia (sekcja 3, wada
D5). To zgadza się z odczuciem z jazdy: bank 2 (eMTB) zawsze wydawał się wyraźnie słabszy.

**Kalibracja czujnika psuje charakterystykę.** Dziś kalibracja ciężarkiem zamienia zmierzoną,
łamaną charakterystykę czujnika na zwykłą prostą. Policzone: jeżeli czujnik jest **idealny**
(dokładnie taki jak fabryczna charakterystyka) i skalibruje się go ciężarkiem 20 kg, to od tej
chwili ten sam nacisk pokazuje **+21% przy lekkim nacisku** i **−6% przy mocnym** (sekcja 3,
wada D8). Kalibracja, która nie powinna zmienić nic, zmienia kształt wspomagania.

**Czego ten audyt NIE potwierdza:** nie mam pomiaru z roweru mówiącego, ile wynosi napięcie
silnika przy danej kadencji (`u_abs`/rpm). Ta jedna liczba decyduje o dokładnej wartości
współczynnika „ile naprawdę dostaję zamiast 320%". Wszystkie wnioski jakościowe (za słabo,
eMTB słabszy od Power Linear, kalibracja zmienia kształt) są **niezależne** od tej liczby —
wynikają z samej arytmetyki kodu. Oznaczenie **HIPOTEZA — WYMAGANY TEST NA ROWERZE** dotyczy
wyłącznie dokładnej wartości liczbowej „~52%".

---

## 1. TABELA DOMEN JEDNOSTEK (§3)

| # | Sygnał | Jednostka | Zakres | Producent (plik:linia) | Konsumenci | Miejsce konwersji |
|---|--------|-----------|--------|------------------------|------------|-------------------|
| 1 | ADC raw | native (≈ mV) | 0..4095 | `main.c` ADC | `torque_input_correct()` | — |
| 2 | `raw_native` | native | 0..4095 | `torque_input.c:766` | snapshot, telemetria | — |
| 3 | `zero_effective_native` | native | ~740 | `torque_input.c:768` | diagnostyka | auto-zero (coast) |
| 4 | `delta_native` | native ponad zero | 0..2600 | `torque_input.c:770` | `load_centikg` | `native_delta_to_centikg` |
| 5 | `assist_delta_native` | native po deadbandzie | 0..`span_native` | `torque_input.c:774` | filtr 35 ms | `−TORQUE_ASSIST_DEADBAND_NATIVE` (10) |
| 6 | `assist_delta_filtered_native` | native | 0..span | `torque_input.c:777` | start, safety, gate | filtr 35 ms Q8 |
| 7 | `assist_delta_run_native` | native | 0..span | `torque_input.c:762` | **wszystkie tryby wspomagania**, boost | okno 180° + filtr asymetryczny |
| 8 | `load_centikg` | **0,01 kg (fizyczne)** | 0..12000 | `torque_input.c:767` | Human Power, progi kg, Ext. Boost, 0x6025 | `native_delta_to_centikg()` |
| 9 | `torque_for_assist_mv` | **native, po booście** | 0..~3× span | `assist_modes.c:686` | eMTB, Torque | `assist_start_apply_boost()` |
| 10 | `assist_load_centikg` | 0,01 kg | 0..12000 | `assist_modes.c:687` | Power L/P/C, telemetria | `native_delta_to_centikg()` |
| 11 | `human_load_centikg` | 0,01 kg | 0..12000 | `assist_modes.c:684` | Human Power (telemetria) | kopia wiersza 8 |
| 12 | `cadence_rpm` | rpm korby | 0..255 | `pas_cadence.c` | Human Power, mianownik eMTB, kompensacja | — |
| 13 | `human_power_mw` | mW (mechaniczne) | 0..~2·10⁶ | `assist_modes.c:547` | telemetria, baza wsparcia | `1,694 mW/(ckg·rpm)` — **stała dla korby 165 mm** |
| 14 | `support_ratio_pct` | % | 0..1000 | `assist_modes.c:528` | `motor_power_mw`, `load_iq` | — |
| 15 | `motor_power_mw` | mW **elektryczne (bateria)** | 0..1,5·10⁶ | `assist_modes.c:920` / `:1013` / `:1096` | limit mocy, filtr, sufit P/U | — |
| 16 | `requested_battery_current_ma` | mA baterii | 0..~40000 | `assist_modes.c:813` | sufit P/U, 0x6029 | `P/V` |
| 17 | `motor_voltage_utilization` (`u_abs`) | 0..2048 = 0..100% wypełnienia | 0..1920 (`_U_MAX`) | `main.c:2783` | sufit P/U | — |
| 18 | `emtb_target_x160` (`target_x160_q`) | **NIEJEDNOZNACZNE** — patrz D4 | 0..160 (Q8) | `assist_modes.c:1004` / `:1076` | prąd baterii **oraz** % Iq | dwie sprzeczne konwersje |
| 19 | `phase_iq_request` | **licznik Iq**, 1 = `CAL_I` = 95 mA fazowo | 0..700 (`PH_CURRENT_MAX`) | `assist_modes.c:924` / `:1018` / `:1101` | limity, rampy, FOC | — |
| 20 | `iq_limit` (`ride_core_iq_limit`) | licznik Iq | 0..`MP.phase_current_max` | `main.c:990` | wszystkie tryby | — |
| 21 | prąd fazowy fizyczny | mA | Iq × 95 | — | FOC | `CAL_I` |

**Wniosek z tabeli.** Do wiersza 8 (`load_centikg`) tor jest czysty i fizyczny. Od wiersza 9
w dół istnieją trzy odrębne domeny, które nigdzie nie są jawnie skonwertowane jedna w drugą:
**native/mV** (wiersz 9, wejście eMTB/Torque), **x160** (wiersz 18, o dwóch sprzecznych
znaczeniach) i **liczniki Iq** (wiersz 19). Wiersz 8 — jedyna wielkość naprawdę fizyczna —
w torze sterowania jest używany tylko przez Power L/P/C oraz przez progi startu.

---

## 2. BEFORE — RÓWNANIA PER TRYB (§40A)

Oznaczenia: `L` = `assist_load_centikg` [0,01 kg], `n` = kadencja [rpm], `V` = napięcie pakietu
[mV], `u` = `u_abs` [0..2048], `IQL` = `iq_limit` [liczniki].

### POWER_LINEAR (1)

```
INPUT DOMAIN     L (0,01 kg) — fizyczny, poprawny
MODE EQUATION    ratio   = support_ratio_pct                       (stała)
                 P_human = L · n · 1,694 mW
                 P_motor = P_human · ratio / 100
DEMAND DOMAIN    DWA RÓWNOLEGŁE ŻĄDANIA:
                 (a) P_motor [mW elektryczne]                     assist_modes.c:920
                 (b) Iq = IQL · (L·ratio/3000)/1000               assist_modes.c:562, :924
IQ CONVERSION    Iq = (b);  (a) TYLKO jako sufit: Iq ≤ P_motor·2048/(u·V·CAL_I)
LIMITS           max_motor_power_w → filtr mocy → max_iq_pct → sufit P/U → limity wspólne
```

### POWER_PROGRESSIVE (2) / POWER_CURVE (6)

Identycznie jak wyżej; różnica wyłącznie w sposobie powstania `ratio` (okno
`support_min..support_max` kształtowane odpowiednio wielomianem `progression_pct` albo dwiema
gammami). **Krzywa kształtuje wyłącznie `ratio`; żądanie Iq nadal pochodzi z (b).**
Tu karta §8/§9 ma rację: równoległa krzywa `load → Iq` faktycznie istnieje i to ona decyduje.

### EMTB (3)

```
INPUT DOMAIN     torque_for_assist_mv (NATIVE, po booście) — NIE kg
MODE EQUATION    x   = mv · 160 / torque_input_span_native()      assist_modes.c:979
                 den = 510 − 2·parameter [− n gdy based_on_power] + 10
                 target_x160 = x² / den
DEMAND DOMAIN    DWA SPRZECZNE ZNACZENIA tej samej liczby:
                 (a) prąd baterii: I = target_x160 · 160 mA       assist_modes.c:1011
                     P_motor = I · emtb_reference_voltage_mv
                 (b) procent limitu fazowego: Iq = target_x160/160 · IQL   :590, :1018
IQ CONVERSION    Iq = (b), następnie ścięte sufitem P/U liczonym z (a)
LIMITS           jak wyżej
```

### TORQUE (5)

```
INPUT DOMAIN     torque_for_assist_mv (NATIVE, po booście)
MODE EQUATION    x = mv · 160 / span_native();  target_x160 = x · factor / 120
DEMAND DOMAIN    identyczna podwójna semantyka (a)/(b) jak w eMTB
IQ CONVERSION    Iq = (b), ścięte sufitem P/U z (a)
LIMITS           jak wyżej
```

---

## 3. FOUND DEFECTS (§40B)

### D1 — Power L/P/C: żądanie Iq nie pochodzi z policzonej mocy (KRYTYCZNA)

* **Miejsce:** `src/assist_modes.c:562` `calculate_load_iq_request()`, wywołanie `:924`.
* **Stare równanie:** `Iq = IQL · min(1000, (L·ratio + 1500)/3000) / 1000`.
  Zakotwiczenie: „60 kg przy 500% = pełny Iq" (komentarz w linii 578).
* **Dlaczego jednostki są złe:** to odwzorowanie *nacisku* na *prąd fazowy* przez stałą, która
  nie ma źródła fizycznego. Nie zawiera ani długości korby, ani przełożenia, ani stałej momentu
  silnika, ani napięcia pakietu. Policzona moc (`motor_power_mw`) — jedyna wielkość w tym torze
  wyrażona w fizycznych watach — jest użyta wyłącznie jako sufit (`:823-833`).
* **Skutek w jeździe (czysta arytmetyka, bez żadnych założeń o silniku):**

  ```
  wspomaganie dostarczone / wspomaganie nominalne = u · V / (1,5651·10⁶ · n)
  ```

  Wyprowadzenie: `P_motor = Iq·CAL_I·(u/2048)·V`, `Iq = IQL·L·ratio/3·10⁶`,
  `P_human = L·n·1,694 mW`, `IQL = 700`, `CAL_I = 95`.

  Przy 48 V i 60 rpm równość (dostarczone = nominalne) wymagałaby `u = 1957`, czyli **więcej
  niż `_U_MAX` = 1920**. Przy 36 V wymagałaby `u = 2610`. **Ustawione wspomaganie nie jest
  osiągalne na żadnym punkcie pracy.** Im niższe `u` (start, wolne kręcenie, podjazd na lekkim
  biegu), tym proporcjonalnie mniej.

  *HIPOTEZA — WYMAGANY TEST NA ROWERZE:* przyjmując `u ≈ 1024` przy 60 rpm i 48 V, ustawione
  320% daje realnie **~167%** (współczynnik 0,52). Dokładna wartość zależy od `u_abs`/rpm tego
  silnika — wielkości, której firmware nigdzie nie zna i nigdzie nie mierzy.
* **Poprawka:** żądaniem RUN ma być `P_motor → prąd baterii → prąd fazowy` przez **zmierzone**
  `u` (§7). Ten tor sam się kalibruje: dzieli przez zmierzone wypełnienie, więc nie potrzebuje
  żadnej stałej silnika.

### D2 — sufit mocy pełni rolę charakterystyki wspomagania

* **Miejsce:** `src/assist_modes.c:823-833` (sufit P/U) kontra `:924` (żądanie).
* **Dlaczego źle:** karta §22 wymaga rozdziału REQUEST od LIMIT. Dziś przy wysokim `u`
  o wyniku decyduje sufit, przy niskim `u` — wymyślone żądanie z D1. Charakterystyka
  wspomagania jest więc *zszyta z dwóch limitów* i przełącza się między nimi zależnie od
  wypełnienia PWM, czyli od biegu i prędkości.
* **Skutek w jeździe:** inny charakter wspomagania na tym samym poziomie zależnie od
  przełożenia; „na płasko w miarę, pod górę nic".
* **Poprawka:** jedno żądanie (D1), sufity osobno.

### D3 — eMTB/Torque normalizują wejście przez `torque_input_span_native()`

* **Miejsce:** `src/assist_modes.c:979` (eMTB), `:1071` (Torque); dodatkowo obcięcie wejścia do
  `span_native` w `:640` oraz w `src/assist_start.c:79-83`.
* **Stare równanie:** `x = torque_for_assist_mv · 160 / span_native()`.
* **Dlaczego jednostki są złe:** `span_native` jest liczbą **kalibracji czujnika**, a nie
  parametrem odczucia jazdy. Zmiana kalibracji przesuwa całą oś 0..160, czyli zmienia kształt
  krzywej eMTB dla tego samego fizycznego nacisku. To dokładnie to pomieszanie dwóch warstw,
  którego zakazuje „GŁÓWNA ZASADA FW-129".
* **Skutek w jeździe:** po kalibracji ciężarkiem eMTB reaguje inaczej, mimo że kilogramy na
  ekranie się zgadzają.
* **Poprawka:** wejściem eMTB/Torque ma być `assist_load_centikg` znormalizowany przez nowy
  `ASSIST TORQUE FULL SCALE` (§6, §11, §14).

### D4 — `target_x160` ma jednocześnie dwa sprzeczne znaczenia (KRYTYCZNA)

* **Miejsce:** `src/assist_modes.c:1011` i `:1094` (znaczenie A) kontra `:590`
  `calculate_target_x160_iq_request()` wywoływane z `:1018` i `:1101` (znaczenie B).
* **A:** `I_bat[mA] = target_x160 · EMTB_UNIT_CURRENT_MA(160)`. Przy `target_x160 = 160` daje
  **25,6 A prądu baterii**.
* **B:** `Iq = target_x160/160 · IQL`. Przy `target_x160 = 160` daje **700 liczników = 66,5 A
  prądu fazowego**.
* **Dlaczego jednostki są złe:** te dwie liczby opisują różne wielkości fizyczne w różnych
  punktach obwodu i różnią się o czynnik `1/wypełnienie`. Zbieżność „obie kończą się na ~160"
  jest przypadkiem zapisu, nie fizyką.
* **Semantyka źródłowa (ROZSTRZYGNIĘTA):** w TSDZ2 (emmebrusa / OpenSourceEBike, do którego
  odsyła `protocol/evistdrive_config_schema.yaml:204`) wynikiem `apply_emtb_assist()` jest
  `ui8_adc_battery_current_target` — **kroki ADC prądu BATERII**, ok. 0,16 A na krok — a wejściem
  `ui16_adc_pedal_torque_delta`, czyli kroki ADC momentu. Znaczenie A jest zatem oryginalne,
  a `EMTB_UNIT_CURRENT_MA = 160` odtwarza je poprawnie. **Znaczenie B nie ma źródła.**
  → Warunek STOP §39.4 **NIE zachodzi**: jednostka wyjścia eMTB jest jednoznaczna.
* **Poprawka:** usunąć `calculate_target_x160_iq_request()`; eMTB/Torque produkują żądanie
  prądu → mocy i dalej idą wspólnym torem (§12).

### D5 — eMTB dostarcza ~4,5× mniej niż Power Linear na tym samym poziomie

* **Wyliczenie** (poziom 3: `emtb_parameter = 140`, `based_on_power = true`, 15 kg, 60 rpm,
  kalibracja domyślna, `reference_voltage = 36 V`, `u = 1024`, `V = 48 V`):
  * `x = 42,3`; `den = 510 − 280 − 60 + 10 = 180`; `target_x160 = 42,3² / 180 = 9,93`
  * A: `I = 9,93 · 160 mA = 1,59 A`; `P = 1,59 A · 36 V = 57,2 W`
  * B: `Iq = 9,93/160 · 700 = 43 liczniki`
  * sufit P/U z A: `1191 mA · 2048 / (1024 · 95) = 25 liczników` → **sufit wygrywa, wynik 25**
  * dostarczone `P = 25 · 95 mA · 0,5 · 48 V = 57 W`; rowerzysta `152,5 W` → **wspomaganie 37%**
  * ten sam punkt w Power Linear na poziomie 3: **~167%**
* **Sufit strukturalny:** przy `den = 180` krzywa nigdy nie osiąga 160 — maksimum to
  `160²/180 = 142`, czyli `142 · 160 mA · 36 V = 818 W`. Przy 60 kg rowerzysta produkuje 610 W,
  więc **eMTB ma twardy sufit wspomagania ~134%, niezależnie od poziomu.**
* **Skutek w jeździe:** bank eMTB jest odczuwalnie słabszy niż Power Linear, a podnoszenie
  „eMTB sensitivity" tego nie naprawia (podnosi sufit, nie przesuwa punktu pracy).

### D6 — `emtb_reference_voltage_mv` steruje siłą wspomagania, a opis twierdzi, że nie steruje

* **Miejsce:** `src/assist_modes.c:1013` / `:1096`;
  `bafang_canable_pro/ui/js/evistdrive/profiles.js:96-99`.
* **Dlaczego źle:** ponieważ sufit P/U prawie zawsze wiąże (D5), `reference_voltage` jest
  **efektywnym wzmocnieniem eMTB i Torque**, a nie tylko przelicznikiem wyświetlanych watów.
  Tekst pomocy w Canable („does NOT change how hard the motor actually pushes") jest dziś
  **nieprawdziwy**.
* **Rola po naprawie (§13):** przelicznik krzywej TSDZ odniesionej do 36 V na **stałą moc** przy
  innym napięciu pakietu — jednoznaczny, opisany, do zachowania. Nie wolno mu dalej maskować
  błędu z D4.

### D7 — Startup Boost pracuje w native/mV, więc zależy od kalibracji czujnika

* **Miejsce:** `src/assist_start.c:79-84` (obcięcie do `span_native`), `:143-148`
  (`boosted = mv + mv·extra/100`), `:10` `STARTUP_BOOST_AUTO_TORQUE_MV = 20`.
* **Dlaczego źle:** boost mnoży sygnał *przed* przeliczeniem na kilogramy, a to przeliczenie
  jest nieliniowe (łamana). Ten sam „+27%" daje inny przyrost kg zależnie od punktu na łamanej
  i od kalibracji. Próg AUTO `20 mV` jest liczbą bez fizycznego znaczenia.
* **Poprawka:** boost na `load_centikg`; próg AUTO przeliczyć na kilogramy, zachowując dzisiejsze
  domyślne zachowanie (§16).

### D8 — Kalibracja użytkownika zastępuje charakterystykę czujnika prostą (KRYTYCZNA)

* **Miejsce:** `src/torque_input.c:128-137` (gałąź `TORQUE_CAL_SOURCE_USER`) oraz `:1007-1008`
  (wyliczenie `span` z ciężarka).
* **Stare równanie:** przy kalibracji `span = delta_ref · 6000 / ref_ckg`; w biegu
  `load = delta · 6000 / span` — czyli **prosta przez punkt kalibracji**, zamiast fabrycznej
  łamanej (146 native ↔ 6,00 kg; 1580 native ↔ 84,00 kg).
* **Dlaczego źle:** kalibracja ciężarkiem ma korygować **wzmocnienie** czujnika, a nie jego
  kształt. Dziś koryguje jedno i drugie, przy czym kształt zawsze zamienia na liniowy.
* **Skutek liczbowy — czujnik IDEALNY, kalibracja ciężarkiem 20 kg:**

  | Rzeczywisty nacisk | Przed kalibracją | Po kalibracji | Błąd |
  |---|---|---|---|
  | 2,05 kg | 2,05 kg | 2,48 kg | **+21%** |
  | 6,00 kg | 6,00 kg | 7,24 kg | **+21%** |
  | 20,00 kg | 20,00 kg | 20,00 kg | 0% (punkt kalibracji) |
  | 60,00 kg | 60,00 kg | 56,53 kg | **−5,8%** |

  Kalibracja, która na idealnym czujniku nie powinna zmienić nic, zmienia wskazanie o +21% przy
  lekkim nacisku. To przechodzi wprost na kształt wspomagania we wszystkich trybach.
* **Poprawka (§5) — z jedną poprawką względem treści karty:** karta proponuje
  `corrected = measured · SPAN_DEFAULT / span_cal`, `load = piecewise(corrected)`. To właściwy
  kierunek, ale **samo to nie wystarczy** — trzeba równocześnie zmienić sposób *wyliczania*
  `span` przy kalibracji, inaczej punkt kalibracji przestaje się zgadzać. Wzór poprawny:

  ```
  przy kalibracji:  span = TORQUE_DEFAULT_SPAN_NATIVE · delta_ref
                           / default_centikg_to_native_delta(ref_ckg)
  w biegu:          corrected = measured · TORQUE_DEFAULT_SPAN_NATIVE / span
                    load      = default_native_delta_to_centikg(corrected)
  ```

  Sprawdzenie: idealny czujnik + ciężarek 20 kg → `delta_ref = 403`,
  `default_centikg_to_native_delta(2000) = 403` → `span = 1139` = wartość domyślna → zero
  zmiany. Dokładnie to, czego wymaga test §30.
* **Zgodność wstecz:** `span_native` zachowuje dotychczasowe znaczenie („native delta przy
  60,00 kg"), więc format zapisu i długość rekordu się nie zmieniają. Ale **istniejąca
  kalibracja użytkownika po tej zmianie da inne kilogramy** (w punkcie 20 kg ok. −6%), bo stara
  procedura zapisywała inną wielkość. → **decyzja właściciela, punkt 7.2.**

### D9 — Długość korby jest stałą wkompilowaną

* **Miejsce:** `src/assist_modes.c:547-560`, stała `1,694 mW/(0,01 kg · rpm)`
  = `9,81 · 0,165 m · 2π/60`.
* **Dlaczego źle:** skoro `load_centikg` to fizyczna siła na pedale, długość korby jest częścią
  równania mocy. Zmiana korby przesuwa moc rowerzysty (a więc i wspomaganie) o `L/165`.
* **Poprawka:** globalny `crank_length_mm`, domyślnie 165, zakres **150..190 mm**
  (uzasadnienie w 4.3), stałoprzecinkowo.

### D10 — Kompensacja kadencji działa na dwóch równoległych wielkościach

* **Miejsce:** `src/assist_modes.c:782-791`.
* **Stan:** mnożnik stosowany osobno do `motor_power_mw` i do `phase_iq_request`. Dziś to **nie
  jest** podwójne zastosowanie (to dwa niezależne tory), ale po unifikacji z D1 zostanie jeden
  tor i mnożnik musi zostać zastosowany dokładnie raz.
* **Poprawka:** `bazowe żądanie → kompensacja → jedna konwersja do Iq` (§18) plus test
  równoważności.

### D11 — `assist_basis_power` liczone z sygnału filtrowanego, `human_power` z niefiltrowanego

* **Miejsce:** `src/assist_modes.c:684` (`human_load_centikg = input->torque_load_centikg`,
  surowa próbka z `torque_input.c:767`) kontra `:687` (`assist_load_centikg`, po oknie RUN
  i po booście).
* **Skutek:** telemetria „Human Power" (0x6029, Canable) skacze z każdym naciskiem nogi,
  podczas gdy wspomaganie idzie za sygnałem wygładzonym. Wada wyłącznie diagnostyczna — na tor
  sterowania nie wpływa. Do udokumentowania, nie do naprawy w tej karcie.

### D12 — Schema protokołu rozjechana z firmware

* **Miejsce:** `protocol/evistdrive_config_schema.yaml:82-90`, `:129`.
* **Stan:** enum kończy się na `emtb_custom = 4`, `max: 4`. Firmware ma `torque = 5`
  i `power_curve = 6`. Brakuje pól `torque_assist_factor`, `curve_exponent_x10`,
  `curve_exponent_high_x10`.
* **Uwaga:** `ASSIST_MODE_EMTB_CUSTOM = 4` **nie ma dispatchera** w `assist_modes_calculate()`
  i jest odrzucany przez `bank_mode_valid()` (`assist_modes.c:248`). Canable poprawnie go nie
  oferuje (`ui/js/evistdrive/common.js:43-49`). Do oznaczenia w schemie jako reserved/inactive,
  zgodnie z §27, bez zmiany wartości wire.

---

## 4. AFTER — PROJEKT DOCELOWY (§40C)

```
ADC
 → native (auto-zero, deadband, filtr 35 ms, okno RUN 180°)
 → KALIBRACJA:      corrected = native · SPAN_DEFAULT / span_cal
 → CHARAKTERYSTYKA: load_centikg = default_piecewise(corrected)   [KONIEC WARSTWY CZUJNIKA]
 → STARTUP BOOST na load_centikg                                  [POCZĄTEK WARSTWY ODCZUCIA]
 → TRYB:
     Power L/P/C:  P_human = load · max(n, N_LAUNCH_RPM) · k(crank_length_mm)
                   ratio   = stała | progresja | krzywa
                   P_req   = P_human · ratio
     eMTB/Torque:  x160    = min(160, load · 160 / assist_torque_full_scale_centikg)
                   target  = x160² / den   |   x160 · factor / 120
                   I_req   = target · 160 mA          (semantyka TSDZ, D4/A)
                   P_req   = I_req · emtb_reference_voltage_mv
 → KOMPENSACJA KADENCJI (dokładnie raz)
 → SUFIT max_motor_power_w
 → FILTR mocy (rise/fall)
 → KONWERSJA:  I_bat = P_req / V
               Iq    = I_bat · 2048 / (max(u_abs, U_LAUNCH) · CAL_I)
 → SUFIT max_iq_pct
 → LIMITY WSPÓLNE (undervoltage, temperatura, prędkość/legal, SOC, prąd baterii, hamulec,
                   cofanie, awaria czujnika) — BEZ ZMIAN
 → DYNAMIKA (rampy Iq, release, smooth start jako obwiednia, Extended Boost)
 → FOC
```

### 4.1 Przekazanie sterowania przy niskim wypełnieniu (§15)

Problem: przy `u_abs → 0` dzielenie `P/(u·V)` jest źle uwarunkowane, a przy kadencji → 0 sama
`P_human` dąży do zera, więc żądanie mocy znika dokładnie tam, gdzie potrzeba jest największa.

Rozwiązanie: **dwie podłogi zakotwiczone w tym samym nominalnym punkcie startowym**, bez
jakiejkolwiek gałęzi `if` i bez skoku:

```
n_eff = max(cadence, N_LAUNCH_RPM)      // N_LAUNCH_RPM = START_PHASE_CURVE_RPM = 60
u_eff = max(u_abs,  U_LAUNCH)           // U_LAUNCH = u_abs odpowiadające N_LAUNCH_RPM
Iq    = P_req(n_eff) · 2048 / (u_eff · V · CAL_I)
```

Dlaczego to jest ciągłe i fizyczne: obie podłogi wiążą **jednocześnie i w tym samym punkcie**,
bo w napędzie środkowym `u_abs ∝ kadencja` (sztywne przełożenie korba → silnik). Powyżej 60 rpm
żadna nie działa i obowiązuje czysta fizyka. Poniżej — żądanie zamraża się na wartości „ile
dostałbyś przy 60 rpm", czyli dokładnie tej, do której tor normalny dochodzi od góry. Brak
progu, brak skoku, brak nowego pokrętła dla użytkownika.

Pojawia się przy tym ważna własność: `P_req ∝ n` oraz `u ∝ n`, więc **Iq nie zależy od
kadencji** — i tak być powinno, bo silnik i korba kręcą się w stałym stosunku, więc stały
stosunek mocy = stały stosunek momentu = stały prąd.

**To rozstrzyga pozorną sprzeczność w §31 karty.** Przy 10 kg i 40 rpm moc rowerzysty jest
połową tej z 80 rpm i żądana moc silnika też jest połową — ale końcowe Iq wychodzi **takie
samo**, ponieważ `u` również spadło o połowę. Tor reaguje poprawnie „zgodnie z fizycznym power
demand"; poprawnym wynikiem jest tu **stałe Iq, nie zmniejszone**. Test §31 należy zapisać
w tej postaci, inaczej sam siebie unieważnia.

**`U_LAUNCH` — jedyna nowa stała wewnętrzna.** Nie jest to pokrętło użytkownika ani wzmocnienie:
to jawnie zadeklarowany zakres pracy trybu startowego, którego wymaga §15. Firmware nie zna dziś
stałej napięciowej silnika (`FLUX_LINKAGE` w `inc/config.h:43` jest **martwa** — nigdzie nie
używana), więc `U_LAUNCH` trzeba zmierzyć.
**HIPOTEZA — WYMAGANY TEST NA ROWERZE:** wartość startowa `U_LAUNCH = 1024` (50% wypełnienia
przy 60 rpm korby). Próba wyciągnięcia jej z istniejących logów jazdy
(`bafang_canable_pro/logs`, ramka `0x00010204` = `u_abs`, `0x00010203` = kadencja) **nie
powiodła się**: logi z realną jazdą pod obciążeniem mają za mało próbek w oknie 30–110 rpm przy
`i_q > 60`. Propozycja: dołożyć `u_abs/kadencja` do diagnostyki 0x6029 i ustalić stałą z jednego
przejazdu.

### 4.2 `ASSIST TORQUE FULL SCALE` (§6)

```c
assist_torque_full_scale_centikg   /* domyślnie 6000 = 60,0 kg; zakres 2000..12000 */
torque_x160 = min(160, load_centikg * 160 / assist_torque_full_scale_centikg);
```

Nie dotyka: kilogramów na ekranie, zera, kalibracji span, telemetrii 0x6025 ani Human Power
w trybach Power. Dotyka wyłącznie osi znormalizowanej eMTB i Torque.

### 4.3 `CRANK LENGTH` (§10)

```c
crank_length_mm    /* domyślnie 165; zakres 150..190 */
P_human_mw = load_centikg * rpm * 1694 * crank_length_mm / (1000 * 165);
```

Zakres 150..190 mm zamiast proponowanego w karcie 140..190: 140 mm nie występuje w rowerach
elektrycznych z korbą standardową (najkrótsze seryjne to 150 mm), a każdy milimetr poniżej
zwiększa błąd zaokrąglenia stałoprzecinkowego. **Do potwierdzenia przez właściciela.**

---

## 5. CANABLE I PROTOKÓŁ (§40D)

### 5.1 Tuning blob v8 (0x6023 read / 0x6024 write / 0x6022 save)

Długość **bez zmian: 32 B**. Liczba ramek bez zmian.

| Offset | v7 | v8 |
|---|---|---|
| 0..23 | bez zmian | bez zmian |
| 24..25 | reserved = 0 | `assist_torque_full_scale_centikg` LE |
| 26..27 | reserved = 0 | `crank_length_mm` LE |
| 28..29 | reserved = 0 | reserved = 0 |
| 30..31 | CRC16 | CRC16 (bez zmiany położenia) |

`TUNING_VERSION 7 → 8`. Akceptowane nadal v2..v8.

### 5.2 Migracja (§24)

* v < 8 → `assist_torque_full_scale_centikg = 6000`, `crank_length_mm = 165`.
* **Ochrona przed starym Canable:** stary tool negocjuje wersję w dół (`canbus.js:1024`) i wyśle
  v7, więc zera z pól reserved nigdy nie trafią do nowych pól. Mimo to firmware musi traktować
  `0` w bajtach 24..27 blobu **v8** jako „nieustawione" → wartość domyślna, nigdy jako
  „0 kg / 0 mm". Sanity obowiązkowe.
* Nowy Canable + stare firmware: negocjacja w dół do v7, nowe pola nie mają dokąd trafić — UI ma
  je wtedy pokazać jako niedostępne (mechanizm `minBankSchema` już istnieje).

### 5.3 UI

* **Ride Feel / Torque:** `ASSIST TORQUE FULL SCALE`, kg, domyślnie 60,0, krok 0,5, zakres
  20,0..120,0. Tooltip wg §25.
* **Bike / Power Model:** `CRANK LENGTH`, mm, domyślnie 165, krok 2,5, zakres 150..190.
* **Poprawka tekstu (D6):** pomoc dla `Reference voltage` w `ui/js/evistdrive/profiles.js:98`
  musi przestać twierdzić, że parametr nie wpływa na siłę.
* **Live Diagnostics (§28):** `Pedal Load: xx.x kg`, `Assist Torque: xxx / 160`,
  `Assist Torque Full Scale: xx.x kg` — liczone lokalnie z 0x6025, bez powiększania ramki.
* **Audyt §26:** UI **już dziś** poprawnie obsługuje wszystkie pięć aktywnych trybów i nie
  oferuje ani `reserved_0`, ani `emtb_custom` (`common.js:43-49`, `profiles.js:52-112`). Zmiany
  wymaga tylko tekst z D6 oraz dodanie dwóch nowych pól globalnych.

### 5.4 Schema

Naprawić enum (dodać `torque: 5`, `power_curve: 6`, `max: 6`), oznaczyć `emtb_custom: 4` jako
reserved/inactive, dodać `torque_assist_factor`, `curve_exponent_x10`,
`curve_exponent_high_x10`, `assist_torque_full_scale_kg`, `crank_length_mm`.
Wartości wire 1/2/3/5/6 **bez zmian**.

---

## 6. KONTROLA WARUNKÓW STOP (§39)

| # | Warunek | Stan |
|---|---|---|
| 1 | `ASSIST_BANK_BLOB_LEN > 255` | **NIE** — nowe pola idą do tuning blob 24..27, bank bez zmian |
| 2 | zmiana wartości wire trybu | **NIE** |
| 3 | zmiana `sizeof(MotorParams_t)` bez migracji | **NIE** — tuning blob ma stałe 32 B |
| 4 | niejednoznaczna jednostka wyjścia eMTB/TSDZ | **NIE** — rozstrzygnięte w D4 (prąd baterii, 160 mA/krok) |
| 5 | dzielenie przez `u_abs = 0` bez jawnego fallbacku | **NIE** — podłogi `U_LAUNCH` / `N_LAUNCH_RPM` (4.1) |
| 6 | ominięcie limitera bezpieczeństwa | **NIE** — `assist_limits_apply()` i limiter prądu baterii są za miejscem zmiany |
| 7 | zapis nowego pola do zajętego offsetu | **NIE** — bajty 24..29 są dziś zerowane (`tuning_config.c:127-129`) |

**Żaden warunek STOP nie zachodzi.** Kartę da się wykonać w całości w zaproponowanym kształcie.

---

## 7. DECYZJE WYMAGAJĄCE AKCEPTACJI WŁAŚCICIELA

### 7.1 Skala zmiany odczucia jazdy

Po naprawie **ten sam numer poziomu będzie znacznie mocniejszy** (Power Linear ok. ×1,9 przy
średnim wypełnieniu i więcej przy starcie; eMTB kilkukrotnie). Dotychczasowe ustawienia banków
przestaną odpowiadać dotychczasowemu odczuciu. Warianty:

* **A** — zostawić `support_ratio_pct` bez zmian; poziomy stają się mocniejsze zgodnie z tym, co
  deklarują. Wymaga ponownego dobrania poziomów przez właściciela.
* **B (zalecany)** — przy migracji banku przeliczyć zapisane `support_ratio_pct` tak, by pierwszy
  przejazd po zmianie był zbliżony do dzisiejszego, i dopiero potem podnosić. Bezpieczniejszy
  start, ale wprowadza jednorazowy współczynnik migracyjny.
* **C** — wdrożyć zmianę wyłącznie na banku 2 (eMTB) i zostawić bank 1 na starym torze do
  porównania A/B. Odrzucam: oznacza utrzymywanie dwóch torów obliczeniowych, czyli dokładnie
  tego, co karta likwiduje.

### 7.2 Istniejąca kalibracja czujnika

Po naprawie D8 zapisana kalibracja użytkownika da inne kilogramy (w punkcie 20 kg ok. −6%).

* **A (zalecany)** — zachować zapis, a przy pierwszym uruchomieniu po aktualizacji pokazać
  w Canable informację „kalibrację należy powtórzyć".
* **B** — automatycznie skasować kalibrację użytkownika i wrócić do charakterystyki fabrycznej.

### 7.3 `U_LAUNCH`

Potrzebny jeden przejazd z logowaniem `u_abs` i kadencji pod obciążeniem, żeby ustalić
wypełnienie przy 60 rpm. Do czasu pomiaru proponuję `U_LAUNCH = 1024` oraz dołożenie
`u_abs/kadencja` do 0x6029.

### 7.4 Zakres `crank_length_mm`

Karta proponuje 140..190; proponuję 150..190 (uzasadnienie w 4.3).

---

## 8. PLAN TESTÓW (§29–37) — do wykonania w implementacji

Wszystko na istniejącej infrastrukturze host (`tests/host/run-host-tests.ps1` kompiluje
i **uruchamia** prawdziwe moduły).

| Test | Zakres | Kryterium |
|---|---|---|
| T1 (§29) | normalizacja torque | full scale 60 kg: 0/6/15/30/45/60/84 kg → 0/16/40/80/120/160/160; full scale 40 kg: 10/20/30/40/60 → 40/80/120/160/160 |
| T2 (§30) | **niezmienniczość kalibracji** | dwa modele czujnika (default span / user span), ten sam fizyczny nacisk → `load_centikg` i żądanie każdego trybu równe z dokładnością do zaokrągleń |
| T3 (§31) | Power Linear | 10 kg @ 80 rpm, korba 165 → 135,5 W; 100/200/320% → 135,5/271/434 W; 10 kg @ 40 rpm → połowa mocy, **to samo Iq** (patrz 4.1) |
| T4 (§32) | Progressive / Curve | seria 25..400 W: monotoniczność, zgodność z min/max, brak skoków, niezależność od native span |
| T5 (§33) | eMTB | macierz load × cadence × parameter × full_scale, log 9 wielkości, kontrola położenia „kolana" |
| T6 (§34) | Torque | macierz load × full_scale × factor × cadence |
| T7 (§35) | handover niskiego wypełnienia | `u_abs` = 0/32/64/128/256/512/1024/1536/2048 przy stałym nacisku → brak skoku i zapadnięcia |
| T8 (§36) | macierz modyfikatorów | 5 trybów × {boost, without-rotation, kompensacja kadencji, smooth start, extended boost, filtry mocy, rampy Iq, release} |
| T9 (§37) | bezpieczeństwo | poziom 0, hamulec, cofanie, awarie czujników, przegrzanie, undervoltage, limiter baterii, prędkość/legal, throttle, Walk, FW-112/117/128 |
| T10 (§21) | Walk i throttle | bit-identyczne z bazą |
| T11 (§23/24) | tuning blob v8 | round-trip v2..v8, migracja, sanity zer, długość 32 B, bank 255 B |

---

## 9. CO TEN AUDYT ZMIENIŁ W KODZIE

**Nic.** Zero zmian produkcyjnych, zero zmian w Canable, zero zmian w schemie protokołu.
