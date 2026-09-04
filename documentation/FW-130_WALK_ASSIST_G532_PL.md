# FW-130 — Walk Assist w charakterze G532 (żądanie + dwa miękkie ograniczniki)

**Status: ZAAKCEPTOWANY (wariant B), WDROŻONY i ZBUDOWANY. Jeżdżony raz jako 0.0492 (zgłoszenie on-off) → poprawka FW-130.1 w 0.0494, JESZCZE NIE JEŻDŻONA. NIC NIE ZACOMMITOWANE.**

⚠ Oba buildy (0.0492 i 0.0494) to wariant **B**: `WALK_GOVERNOR_ENABLE = 1`. Wariant A nie został
zbudowany — to ustawienie źródła, a nie przełącznik w aplikacji.
Data: 2026-09-03. Autor audytu: sesja Claude. Podstawa: zgłoszenie właściciela +
załączony reverse `EVistDrive_G532_Walk_Assist_reverse_i_plan_wdrozenia.md`.

---

## 0. Decyzje właściciela i stan wdrożenia (2026-09-03)

| Pytanie | Decyzja |
|---|---|
| Wariant | **B** — charakter G532 |
| Sztywność trzymania | **pas wyśrodkowany na ustawionym RPM**, szerokość dobrana przeze mnie (patrz §7.2) |
| Siła marszu | **15 %** ≈ 105 jednostek ≈ 10 A |
| Log przed zmianą | **nie** — od razu implementacja |

Cytat wymagania, które zmieniło projekt względem pierwotnej propozycji:
> „prędkość rpm jest jako zmienna ustawiana w Canable. Są dwa banki dla walk assist i każdy może
> mieć swój rpm. Sterowanie ma oscylować wokół ustawionej prędkości rpm przez użytkownika."

Pierwotnie proponowałem pas niesymetryczny (3 ERPS poniżej celu, 8 powyżej), którego środek leżał
**powyżej** zadanej wartości. Zgodnie z powyższym pas jest teraz **symetryczny wokół celu** i
**proporcjonalny do niego**, liczony osobno dla każdego banku z jego własnego `Target chainring RPM`.

**Co jest zrobione:**
- nowe prawo sterowania (law B) w `src/walk_speed_controller.c`, obok starego (law A), przełącznik
  kompilacji `WALK_GOVERNOR_ENABLE` w `inc/config.h`;
- `Walk current` z banku steruje sufitem WA (`src/main.c` + `src/walk_assist_motor.c`), domyślnie 15 %;
- bramka prędkości koła zamieniona na zjazd + zero w miejscu + twarde zatrzymanie dopiero wyżej;
- grace watchdogu 1,5 s → 3 s;
- nowy zestaw testów hostowych `tests/host/fw130_walk_governor_host.c` (T1–T9) — **wszystkie PASS**;
- oba istniejące zestawy WA (FW-113.1, FW-113.2) **PASS w OBU prawach** (A i B);
- Canable: `Walk motor current` wraca jako działający parametr, opisy zjazdu prędkości koła.

**Czego świadomie NIE zrobiono (zmiana względem planu):**
1. **Brak podtrzymania Halla `WA_KEEPALIVE_IQ`.** Plan przewidywał podłogę 2 Iq. Historia mówi
   inaczej: FW-079 usunął podłogę 5 Iq, bo powodowała zbieganie się 30 i 50 rpm do tej samej
   prędkości, a FW-113.1 zszedł z 2 do 0 i ma na to test (`min_iq == 0`). Dokładanie podłogi z
   powrotem oznaczałoby cofnięcie dwóch przetestowanych decyzji przy okazji trzeciej. Utrata Halla
   jest nadal obsłużona istniejącą ścieżką REACQUIRE.
2. **Zero zmian w CAN.** Oba współczynniki są w pełni odtwarzalne offline: współczynnik zębatki z
   `target_erps` i `measured_erps` (ramka `0x00010205`) plus stałe pasa, a współczynnik koła z
   prędkości koła plus `Walk assist cut-off`. Nie ma powodu wydawać bajtów ani wersji schematu.
3. **Wygaszenie 110 ms przy puszczeniu przycisku** — nadal poza kartą (dotyka `immediate_cut`).

**Poprawka przy okazji (poza pierwotnym zakresem, celowo zrobiona):** w Canable minimum
`Walk chainring speed` wynosiło 18, a firmware od zawsze ma minimum 20 i po cichu naprawiał 18/19
na 20. Ustawione 20 w UI i w walidacji presetów, żeby strojenie tej właśnie funkcji nie kłamało.

Baza kodu: gałąź `diag/fw112-real-bike-ab`; przed tą kartą pliki WA były niezmienione względem HEAD
(`src/walk_assist_motor.c`, `src/walk_speed_controller.c` — ostatni commit `11dbeb6`, FW-077..082).

---

## 1. Co zgłosił właściciel

1. Mało siły przy ruszeniu.
2. Potem przestrzelenie prędkości.
3. Odcinanie w czasie działania mocy.
4. Oczekiwanie: utrzymywana ma być **stała prędkość obrotowa zębatki/silnika**, a
   **prędkość koła ma być bezpiecznikiem** — powyżej ustalonej wartości najpierw zmniejsza
   moc, a dopiero potem ją odcina.
5. Hipoteza właściciela: naprawiony, płynny FOC sam w sobie wygładzi zachowanie, bo dawna
   agresja mogła być uwypuklona przez zły ciąg generowania Iq.

**Ocena hipotezy 5 (uczciwie):** naprawiony FOC pomoże w odczuciu (mniej „szarpania" przy tym
samym prądzie), ale **nie usunie żadnej z trzech przyczyn** wypisanych w §2 — one leżą w
warstwie żądania prądu, powyżej FOC. Traktuję to jako poprawę tła, nie jako lekarstwo.

---

## 2. Prostym językiem — co dziś robi sterownik i dlaczego tak się zachowuje

### 2.1 Mało siły przy ruszeniu — ZNANE ZACHOWANIE, potwierdzone w kodzie

Walk Assist ma **na sztywno wpisany sufit siły 40 jednostek prądu**. Cały napęd przy jeździe
może dziś dać 700. Czyli marsz dostaje **5,7 % tego, czym silnik dysponuje** — około 3,8 A prądu
fazowego.

Do tego suwak **„Walk current" w Canable nic nie robi**. Bajt jedzie z aplikacji do sterownika,
sterownik go zapisuje, ale funkcja, która go czyta, **nie ma ani jednego wywołania**. Zostało to
świadomie odłączone przy FW-060 i jest opisane w kodzie Canable (komentarz CB-019), ale
**karta w aplikacji nadal wygląda jak działający regulator siły**.

Wniosek: przy ruszeniu nie ma czym dociągnąć, bo sufit jest ustawiony bardzo nisko i nie da się
go podnieść z aplikacji.

### 2.2 Przestrzelenie prędkości — ZNANE ZACHOWANIE, policzone z kodu

Start działa tak: prąd narasta do 40 jednostek w 0,43 s i **trzyma się na tym suficie tak długo,
aż silnik osiągnie 8 ERPS** — to około 6 obr/min zębatki. Cel to zwykle 27 ERPS (20 obr/min).
Czyli start kończy się przy **mniej niż jednej trzeciej celu**, z pełnym prądem startowym.

W tym momencie prąd musi zejść — i tu jest problem: **opadanie idzie 31,25 jednostki na sekundę**,
więc zejście z 40 do zera trwa **1,28 s**. Przez ten czas rower już przyspiesza z prawie pełną siłą
startową. To jest przestrzelenie.

Dla porównania, z załączonego reverse G532:

| | narastanie pełnej skali | opadanie pełnej skali | stosunek |
|---|---:|---:|---:|
| G532 (stock) | 550 ms | 110 ms | opada **5× szybciej** niż narasta |
| EBICS dziś | 427 ms | 1280 ms | opada **3× wolniej** niż narasta |

**Nasza rampa jest asymetryczna w złą stronę.** Narasta szybciej niż fabryczna, a opada
około **12× wolniej**. To jedna liczba i tłumaczy objaw wprost.

Drugi, mniejszy współwinny: regulator ma całkę (I) i przy przekroczeniu celu odwija ją tylko
6,25 jednostki na sekundę. Nawet gdy rower jedzie za szybko, regulator zmniejsza prąd powoli.

### 2.3 Odcinanie w czasie działania mocy — HIPOTEZA GŁÓWNA, wymaga potwierdzenia logiem

Limit prędkości koła (domyślnie **7,0 km/h**) nie jest dziś „bezpiecznikiem zmniejszającym moc".
Jest **przełącznikiem**:

```
prędkość koła >= 7,00 km/h   ->  prąd 0 w tym samym takcie (twarde zero)
prędkość koła <  6,50 km/h   ->  wspomaganie wraca
```

a powrót **nie zaczyna od miejsca, w którym przerwał** — startuje praktycznie od zera i narasta
31,25 jednostki na sekundę, czyli potrzebuje ponad sekundy, żeby wrócić do 40. Efekt na rowerze:
**pchnięcie — cisza — słaby powrót — pchnięcie**.

Kiedy to się włącza? Wtedy, gdy przy zadanych obrotach zębatki koło jedzie szybciej niż 7 km/h,
czyli **na lżejszych biegach**. Przykład (koło 29″, zębatka 38T):

| bieg z tyłu | prędkość koła przy 20 obr/min zębatki | limit 7,0 km/h |
|---:|---:|:--|
| 18T | ok. 5,8 km/h | nie rusza |
| 16T | ok. 6,6 km/h | nie rusza |
| **14T** | ok. **7,5 km/h** | **tnie** |
| 12T | ok. 8,7 km/h | tnie mocno |

Przy celu 30 obr/min zębatki limit tnie praktycznie na każdym biegu. **To jest dokładnie ten sam
objaw, który zgłosił właściciel** — i jest to konflikt dwóch regulatorów: my regulujemy obroty
zębatki, a osobna bramka pilnuje prędkości koła i wygrywa twardym zerem.

Drugi możliwy mechanizm odcięcia (mniej prawdopodobny, ale realny): watchdog zakleszczenia.
Jeżeli przy prądzie ≥ 24 jednostek silnik przez 200–300 ms nie osiąga 15 % celu, sterownik
wchodzi w `LIMIT` (15 jednostek), a po kolejnych 400 ms **zatrzaskuje `STALL` z prądem 0 aż do
całkowitego puszczenia przycisku**. Przy suficie 40 jednostek na podjeździe z obciążonym rowerem
to jest realne: silnik nie ma czym ruszyć, więc watchdog uznaje to za zakleszczenie.

**WYMAGANY TEST NA ROWERZE / LOG:** ramka diagnostyczna `0x00010228` (build z `-CanDiagnostics`)
mówi jednoznacznie, który mechanizm zadziałał — bit `SPEED_GATE` (limit koła) kontra `JAM`/`STALL`
(watchdog). To rozstrzyga §2.3 bez zgadywania.

---

## 3. Audyt techniczny (etap 0 i 1 z załączonego dokumentu)

### 3.1 Ownership toru prądu — WYNIK: CZYSTO, nic do naprawy

Dokument G532 słusznie każe najpierw sprawdzić, czy nie ma kilku nakładających się ramp
(§31, §37). Sprawdzone:

```
walk_assist_iq_request()            src/main.c:6257
   -> walk_motor_update()           src/walk_assist_motor.c:218      (Hall + bezpieczeństwo)
      -> walk_speed_controller_update()  src/walk_speed_controller.c:80   (JEDYNA rampa)
   -> assist_limits_apply()         (podnapięcie + temperatura sterownika)
   -> ride_control_update()         src/ride_control.c:463  — gałąź walk pomija CAŁY tor jazdy
   -> fast_iq_slew: FIS_MODE_BYPASS  src/ride_control.c:1215 — bez końcowego slew 16 kHz
```

- Rampy jazdy (`ramp_up_*`/`ramp_down_*`) są **celowo zerowe** na ścieżce WA (`ride_control.c:459`).
- Końcowy slew 16 kHz jest **pominięty** (`FIS_MODE_BYPASS` = przepisanie wartości 1:1).
- QZERO nie uzbraja się na WA (`immediate_cut` wygrywa przed gałęzią `profile_release`,
  `ride_control.c:1211`).

**Czyli mamy dokładnie jedną rampę i jednego właściciela.** Punkt §37 dokumentu G532 jest już
spełniony — nie ma „potrójnego wygładzania". Zmiana kształtu rampy trafia więc wprost w odczucie.

### 3.2 Co reguluje WA dzisiaj — już obroty zębatki, nie koło

To też jest już zgodne z życzeniem właściciela na poziomie architektury (FW-042/044/060):

```
Hall silnika -> ERPS (średnia z 12 okresów) -> błąd względem celu -> PI -> Iq
cel: bankowy "Target chainring RPM" 20..60, przeliczany erps = rpm x 4/3   (M820: 80 obr. el./obr. korby)
```

**Nowego kierunku regulacji nie trzeba budować — trzeba naprawić sposób regulacji i bramkę koła.**

### 3.3 Liczby, którymi dziś steruje WA

| Wielkość | Wartość w kodzie | W amperach fazowych (1 j. ≈ 95 mA) |
|---|---:|---:|
| Sufit startu `WA_MOTOR_START_MAX_IQ` | 40 | ok. 3,8 A |
| Sufit RUN `WA_MOTOR_RUN_MAX_IQ` | 40 | ok. 3,8 A |
| Sufit absolutny `WA_MOTOR_IQ_ABS_MAX` | 157 | ok. 14,9 A |
| Podłoga RUN `WA_MOTOR_RUN_MIN_IQ` | **0** | 0 |
| Sufit jazdy `PH_CURRENT_MAX` | 700 | ok. 66,5 A |
| Narastanie START | 6/256 na takt 4 kHz = 93,75 j./s | — |
| Narastanie RUN | 2/256 = 31,25 j./s | — |
| Opadanie (zawsze) | 2/256 = 31,25 j./s | — |
| Deadband PI | ±2 ERPS (≈ ±1,5 obr/min) | — |
| Ograniczenie błędu | ±12 ERPS | — |
| Kp | 1 jednostka na ERPS | — |

Uwaga do dokumentacji: `documentation/WALK_ASSIST_DZIALANIE.md` opisuje podłogę RUN „2 Iq".
**W kodzie jest 0** — dokument jest w tym miejscu nieaktualny (do poprawienia niezależnie od tej karty).

Drugi szczegół: START oddaje sterowanie przy **8 ERPS**, a próg `RIDE_COAST_RELEASE_ERPS`
(strefa, w której kąt komutacji przeskakuje między formułą interpolowaną a sześciostopniową,
FW-048/QZERO) wynosi **10 ERPS**. Czyli **przekazanie START → PI dzieje się jeszcze wewnątrz
niepewnej strefy komutacji.** To może dokładać do odczucia „szarpnięcia" przy ruszeniu.
HIPOTEZA — do potwierdzenia logiem.

### 3.4 Bramki bezpieczeństwa (dziś)

| Bramka | Gdzie | Zachowanie |
|---|---|---|
| Przycisk + żądanie CAN | `main.c:2890` | debounce 20 próbek — **OK, zgodne z §39 dokumentu G532** |
| Hamulec / błąd sterownika | `main.c:2966` | natychmiastowe zero — zostaje bez zmian |
| Prędkość koła | `main.c:2898` + `walk_assist_motor.c:231` | **twardy przełącznik z histerezą 0,5 km/h** |
| Watchdog JAM/LIMIT/STALL | `walk_assist_motor.c:272` | `STALL` zatrzaskuje do puszczenia przycisku |
| Podnapięcie / temperatura | `assist_limits_apply()` | wspólne z jazdą, bez zmian |

### 3.5 Diagnostyka — etap 1 dokumentu G532 jest w większości gotowy

Ramki (tylko build `-CanDiagnostics`):
`0x00010205` stan/flagi/cel ERPS/zmierzone ERPS · `0x00010206` błąd/całka/prąd startowy ·
`0x00010228` stan + wszystkie bity powodu (`SPEED_GATE`, `BRAKE`, `ERROR`, `HALL`, `JAM`,
`STALL`, `LIMIT`, `NO_CAN_REQUEST`, `BUTTON_RELEASED`) + surowe bramki.

**Brakuje tylko** wartości sufitu (`iq_cap`) i — po zmianie — współczynników obu ograniczników.
To 2 bajty w istniejącej ramce, bez zmiany schematu.

### 3.6 Twarde ograniczenie: bank konfiguracji jest PEŁNY

```
BANK_BLOB_HEADER_LEN 13 + 5 poziomów x 48 B + 2 B CRC = 255 B = ASSIST_BANK_BLOB_LEN (maksimum)
```

**Nie ma miejsca na nowy parametr banku bez wersji v9 i przebudowy transportu.**
Schemat `protocol/evistdrive_config_schema.yaml` przewiduje pola (`walk_base_iq_pct`,
`walk_target_erps`…), ale ma `status: draft` i `wire_id: null` — ten transport jeszcze nie istnieje.

**Konsekwencja dla planu:** wszystkie warianty poniżej mieszczą się w **już przesyłanych** bajtach
banku (`Walk current %` bajt 8 — dziś ignorowany, `Target chainring RPM` bajt 9,
`Walk assist cut-off` bajt 7). **Zero zmian protokołu CAN.** Reszta zostaje stałymi kompilacji
do czasu, aż powstanie nowy blok konfiguracyjny.

---

## 4. Porównania

### 4.1 G532 (z załączonego reverse — cudze ustalenia, nie moje)

Przyjmuję z dokumentu jako CONFIRMED: jeden stan `5` przez cały marsz (nie ma drugiej fazy),
własne żądanie marszu ograniczane przez `min(żądanie, dostępny sufit)`, brak klasycznego PI
prędkości, wyjście przez wspólne limitery.
Jako STRONG: rampa 550 ms w górę / 110 ms w dół pełnej skali, próg wejścia ok. 6,00 km/h.
**NIE kopiuję liczby `5500` jako amperów** (§24 dokumentu) — kopiuję kształt i czasy.

### 4.2 TSDZ2 / OpenSource (emmebrusa) — sprawdzone w źródle, nie z pamięci

`apply_walk_assist()` reguluje **wypełnienie PWM (napięcie), nie prąd**:
- start: `ui8_duty_cycle_target = WALK_ASSIST_DUTY_CYCLE_STARTUP` przy stojącym silniku;
- cel: uczy się `ui16_walk_assist_erps_target` z pierwszego ruchu koła
  (`motor_speed_erps × zadana_prędkość / zmierzona_prędkość`);
- trzymanie: **martwa strefa** `target ± WALK_ASSIST_ERPS_THRESHOLD`, w niej wypełnienie się nie
  zmienia; poza nią **powolne korekty**;
- osobno: **twardy limit prądu** `WALK_ASSIST_ADC_BATTERY_CURRENT_MAX`;
- rampa wypełnienia: `WALK_ASSIST_DUTY_CYCLE_RAMP_UP_INVERSE_STEP`.

### 4.3 Wniosek z porównania

**Ani G532, ani TSDZ2 nie regulują prędkości marszu pętlą PI na prądzie.**
- TSDZ2 reguluje napięcie — a napięcie samo z siebie ustala prędkość (siła
  elektromotoryczna), więc przestrzelenie jest z natury ograniczone; prąd jest tylko ograniczany.
- G532 zadaje prąd, ale prędkość ogranicza **sufitem zależnym od prędkości**, a nie regulatorem.

My sterujemy prądem (Iq) i nie mamy toru napięciowego. Zostaje więc droga G532:
**żądanie + miękki ogranicznik zależny od obrotów**. To potwierdza rekomendację z §54 załączonego
dokumentu i jest zgodne z życzeniem właściciela („prędkość koła = bezpiecznik zmniejszający moc").

Przejście na sterowanie wypełnieniem jak w TSDZ2 **odrzucam**: wymagałoby własnego toru u_q obok
FOC, czyli złamania ownership prądu, którego nie chcemy ruszać zaraz po naprawie FOC/QZERO.

---

## 5. Warianty

### Wariant A — minimalny („trzy chirurgiczne cięcia", bez zmiany architektury)

1. **Podłączyć suwak „Walk current"** do sufitów startu i RUN. Koniec z martwym polem w aplikacji.
2. **Naprawić asymetrię rampy** — opadanie z 31,25 j./s na charakter G532 (pełna skala w 110 ms).
3. **Zamienić przełącznik prędkości koła na miękki ogranicznik** (zjazd mocy przed limitem,
   twarde cięcie dopiero wyżej) i **nie zerować sesji** przy zjeździe mocy.

Zostaje PI z całką, zostaje watchdog bez zmian.
Ryzyko: małe. Nie usuwa windupu, tylko go maskuje szybszym opadaniem.
Nakład: 1 plik firmware + tabela w instrukcji. Bez zmian CAN i bez zmian Canable (poza opisem).

### Wariant B — ZALECANY („charakter G532")

Wariant A **plus** wymiana prawa sterowania w `walk_speed_controller.c`:

- **żądanie znormalizowane 0…1** z rampą o stałej szybkości (550 ms w górę / 110 ms w dół
  pełnej skali) — jak G532;
- **ogranicznik obrotów zębatki (główny)**: ciągły zjazd sufitu prądu wokół celu — bez całki,
  bez windupu, bez przestrzelenia;
- **ogranicznik prędkości koła (bezpiecznik)**: drugi, niezależny zjazd mocy przed limitem,
  twarde odcięcie dopiero powyżej;
- **prąd = sufit_marszu × min(oba współczynniki)**, potem istniejące limity (podnapięcie,
  temperatura, prąd baterii);
- **całka usunięta** w wersji 1 (zgodnie z §36.1 i §52.13 dokumentu);
- watchdog JAM/STALL przestrojony tak, by liczył dopiero po dojściu rampy do celu.

Ryzyko: średnie — to nowe prawo sterowania, wymaga własnych testów hostowych i jazdy A/B.
Zysk: usuwa **wszystkie trzy** zgłoszone objawy u źródła, a nie tłumi je.

### Wariant C — rozbudowany (na później, częściowo ZABLOKOWANY)

B **plus**: czasy rampy i szerokości zjazdów wystawione do Canable (⚠ **zablokowane** przez
pełny bank 255 B — wymaga wersji v9 albo nowego bloku konfiguracyjnego), wygaszenie 110 ms przy
puszczeniu przycisku zamiast dzisiejszego twardego zera, dodatkowe pola w ramce diagnostycznej.

**Rekomendacja: B.** C rozbić na osobne karty, gdy powstanie nowy transport konfiguracji.

---

## 6. Wariant B — projekt szczegółowy

Podział plików bez zmian: `walk_assist_motor.c` (Hall + bezpieczeństwo + stany) zostaje,
wymieniana jest **treść** `walk_speed_controller.c` przy zachowaniu jego API.

Co takt 4 kHz:

```
1. ŻĄDANIE
   demand = aktywny ? 1.0 : 0.0                       (stałoprzecinkowo)

2. SUFIT SIŁY
   WALK_IQ_MAX = min( PH_CURRENT_MAX * walk_current_pct / 100 , WA_IQ_ABS_MAX )

3. OGRANICZNIK OBROTÓW ZĘBATKI  (główny, zgodny z życzeniem właściciela)
   erps_full = cel - WA_GOV_BAND_LO      -> poniżej: współczynnik 1,00
   erps_zero = cel + WA_GOV_BAND_HI      -> powyżej: współczynnik 0,00
   pomiędzy: liniowo

4. OGRANICZNIK PRĘDKOŚCI KOŁA  (bezpiecznik: najpierw mniej mocy, potem cięcie)
   v_full = cut_off - WA_WHEEL_TAPER     -> poniżej: 1,00
   v_zero = cut_off                      -> powyżej: 0,00
   twarde zatrzymanie sesji dopiero przy cut_off + WA_WHEEL_HARD_MARGIN

5. CEL PRĄDU
   iq_target = WALK_IQ_MAX * min(wsp_zębatki, wsp_koła)

6. RAMPA (charakter G532, szybkość skalowana do WALK_IQ_MAX)
   w górę: pełna skala w WA_RISE_FULL_SCALE_MS (550)
   w dół : pełna skala w WA_FALL_FULL_SCALE_MS (110)

7. PODTRZYMANIE HALLA
   jeżeli żądanie trwa, Hall żyje i erps >= RIDE_COAST_RELEASE_ERPS, podłoga WA_KEEPALIVE_IQ

8. WYJŚCIE
   istniejące assist_limits_apply() -> ride_control -> FIS_MODE_BYPASS -> FOC   (bez zmian)
```

**Czego tu nie ma i celowo nie będzie:** całki, drugiej rampy, regulacji na prędkości koła jako
sprzężeniu zwrotnym, bezpośredniego zapisu do PI/FOC.

**Co to zmienia w odczuciu (przewidywanie do potwierdzenia jazdą):**
- ruszanie: pełny dostępny sufit marszu narasta w 550 ms — mocno, ale bez strzału;
- dojście do prędkości: prąd zjeżdża sam, zanim rower dojdzie do celu, bo współczynnik maleje
  jeszcze poniżej celu — nie ma czym przestrzelić;
- lżejszy bieg: przy tych samych obrotach zębatki koło jedzie szybciej, więc **wcześniej włącza
  się drugi ogranicznik i płynnie ujmuje mocy** zamiast dzisiejszego wyłącznika;
- puszczenie przycisku: jak dziś, natychmiastowe zero (świadomie niezmienione — patrz §12).

**Świadomy koszt (trzeba go zaakceptować):** bez całki prędkość marszu **będzie się nieco zmieniać
z obciążeniem** — pod górę wolniej, po płaskim szybciej, w granicach pasa zjazdu. To jest wprost
zamiana „przestrzelenie + odcinanie" na „lekkie pływanie prędkości". Tak zachowuje się G532 i tak
radzi §22 załączonego dokumentu.

### 6.1 Jak to trzyma obroty w praktyce (pytanie właściciela, 2026-09-03)

**Pytanie:** czy stałe obroty są trzymane od startu, czy dopiero powyżej jakiejś prędkości?

**Odpowiedź:** regulacja działa **od pierwszej chwili**, ale **nic nie jest ujmowane, dopóki jest
wolno**. To jeden ciągły sufit mocy zależny od obrotów zębatki, a nie regulator włączający się przy
jakimś progu. Przy celu 20 obr/min i pasie 3/8 ERPS:

| Obroty zębatki | Co robi sterownik |
|---|---|
| 0 → 18 obr/min | pełna siła marszu, sufit nietknięty |
| 18 → 26 obr/min | prąd zjeżdża liniowo z pełnego do zera |
| powyżej 26 obr/min | prąd zero (sesja trwa, przycisk trzyma) |

Przebieg jednego naciśnięcia:

```
0 ms      przycisk -> cel prądu = pełny sufit (obroty 0 -> współczynnik 1,00)
0-550 ms  prąd narasta liniowo od 0 do sufitu; rower rusza gdzieś po drodze
~1 s      obroty rosną, poniżej 18 obr/min nic nie jest ujmowane -> pełne ciągnięcie
~1,5 s    18 obr/min: sufit zaczyna schodzić proporcjonalnie do obrotów
ustalone  równowaga tam, gdzie moc zrówna się z oporem
```

**To trzyma PAS, nie jedną liczbę.** Punkt równowagi zależy od obciążenia:

| Sytuacja | Gdzie się ustali (cel 20 obr/min) |
|---|---|
| płasko, lekki rower | ok. 24–26 obr/min (górny koniec pasa) |
| płasko, obciążony | ok. 21–23 obr/min |
| podjazd | ok. 18–20 obr/min (dolny koniec pasa) |

Trzymanie dokładnie jednej liczby niezależnie od obciążenia wymaga integratora — czyli dokładnie
tego elementu, który dziś daje przestrzelenie i odcinanie. Kompromis jest realny i nieusuwalny:
**wąski pas = sztywniej, bliżej szarpania; szeroki pas = spokojnie, ale prędkość pływa.**
Zaczynamy szeroko (§34/§43 dokumentu G532), zwężamy po logach; ograniczona korekta bez
integratora (§35) zostaje otwarta na później.

**Który ogranicznik rządzi, zależy od biegu:**
- cięższy bieg → koło jedzie wolno, bezpiecznik śpi, prędkość marszu trzyma zębatka;
- lżejszy bieg → koło przekracza limit, przejmuje bezpiecznik koła i ustala prędkość na limicie.

W obu przypadkach jest **płynne ujęcie mocy, nie odcięcie** — to jest naprawa objawu nr 3.

---

## 7. Parametry

### 7.1 Konfigurowalne z Canable (bez zmiany protokołu — bajty już jadą)

| Nazwa w aplikacji | Bajt banku | Dziś | Po zmianie | Zakres | Krok | Jednostka | Ryzyko |
|---|---|---|---|---|---|---|---|
| Walk current | 8 | **ignorowany** | sufit siły marszu, % `PH_CURRENT_MAX`, obcięty do `WA_IQ_ABS_MAX` | 1–100 | 1 | % | ⚠ **wymaga decyzji o wartości domyślnej — patrz §13** |
| Target chainring RPM | 9 | działa | bez zmian (środek pasa ogranicznika) | 20–60 | 1 | obr/min | niskie |
| Walk assist cut-off | 7 | twardy wyłącznik | koniec zjazdu mocy; twarde cięcie wyżej | 1,0–25,5 | 0,1 | km/h | niskie |

### 7.2 Stałe kompilacji (na razie nie do aplikacji — bank pełny)

Wartości WDROŻONE (w `inc/config.h`, jeżeli nie zaznaczono inaczej):

| Stała | Wartość | Znaczenie | Ryzyko |
|---|---:|---|---|
| `WA_MOTOR_IQ_ABS_MAX` (`walk_assist_motor.c`) | 157 (bez zmian) | absolutny sufit marszu ≈ 14,9 A | zostawione celowo |
| `WA_RISE_FULL_SCALE_TICKS` | 2200 (550 ms) | charakter startu G532 | niskie |
| `WA_FALL_FULL_SCALE_TICKS` | 440 (110 ms) | charakter wygaszenia G532 | niskie |
| `WA_GOV_BAND_PCT` | 15 | połowa szerokości pasa, w % celu | strojenie po logach |
| `WA_GOV_BAND_MIN_ERPS` | 4 | podłoga pasa dla niskich celów | wąsko = ryzyko pompowania |
| `WA_WHEEL_TAPER_X100` | 150 | 1,50 km/h zjazdu przed limitem koła | niskie |
| `WA_WHEEL_HARD_MARGIN_X100` | 100 | twarde zatrzymanie 1,00 km/h nad limitem | niskie |
| `WA_MOTOR_JAM_GRACE_TICKS` (`walk_assist_motor.c`) | 12000 (3 s, było 6000) | grace watchdogu pokrywa dłuższą rampę | niskie |
| `WALK_GOVERNOR_ENABLE` | 1 | 1 = prawo B (FW-130), 0 = prawo A (FW-060..082) | — |

**Pas jest symetryczny i proporcjonalny — środek = dokładnie zadany RPM.** Przy zadanym RPM
współczynnik wynosi równo 128/256, czyli połowa sufitu (test T2/T3 pilnuje tej liczby):

| Ustawiony cel | Cel w ERPS | Pas | Pełny sufit do | Zero od | Zakres pracy zębatki |
|---:|---:|---:|---:|---:|---|
| 20 obr/min | 27 | ±4 | 23 ERPS | 31 ERPS | ok. **17–23 obr/min** |
| 30 obr/min | 40 | ±6 | 34 ERPS | 46 ERPS | ok. **25–34 obr/min** |
| 60 obr/min | 80 | ±12 | 68 ERPS | 92 ERPS | ok. **51–69 obr/min** |

Zaczynamy **szeroko** (rada §34/§43 dokumentu G532: wąski pas bez integratora = wysokie
wzmocnienie = szarpanie). Zwężenie = obniżenie `WA_GOV_BAND_PCT`, dopiero po jeździe.

---

## 8. Zakres zmian

**Firmware**
- `src/walk_speed_controller.c` — wymiana prawa sterowania (rdzeń zmiany).
- `inc/walk_speed_controller.h` — nowe pola wejścia (sufit z banku, oba współczynniki na wyjściu).
- `src/walk_assist_motor.c` — usunięcie twardego resetu na bramce koła, przekazanie sufitu
  z banku, przestrojenie watchdogu JAM/STALL, publikacja współczynników do diagnostyki.
- `src/main.c` — bramka prędkości koła: zjazd mocy zamiast pauzy sesji; przekazanie
  `assist_modes_get_wa_current_pct()` do modułu (funkcja istnieje, brak wywołań).
- `inc/config.h` — nowe stałe z §7.2.

**Custom CAN / protokół:** **bez zmian.** Żaden bajt nie zmienia znaczenia ani pozycji;
`Walk current` zaczyna być czytany, co jest zgodne z jego oryginalnym przeznaczeniem.
Ramka `0x00010205` dostaje dwa dotąd niewykorzystane bajty (współczynniki) — bez zmiany
`DIAG_SCHEMA_VERSION`, do potwierdzenia przy implementacji.

**Canable (eVistDrive):** `ui/js/evistdrive/walk.js` — usunąć/zmienić komentarz CB-019 i
**pokazać `Walk current` jako działający parametr**; opis karty Walk w `ui/index.html`.
Zgodnie z [[feedback-firmware-change-check-canable]] sprawdzić też opisy sekcji.
**Nie dodajemy nic do starych kart Bafanga.**

**Instrukcja / dokumentacja**
- `documentation/WALK_ASSIST_DZIALANIE.md` — przepisać (jest nieaktualny już dziś: opisuje podłogę
  2 Iq, w kodzie jest 0);
- `documentation/FAQ_PYTANIA_UZYTKOWNIKA_PL.md` — wpis „dlaczego suwak Walk current nic nie robił
  i co poprawiliśmy w UI";
- `CHANGELOG.md`.

**Testy hostowe**
- nowy `tests/host/fw130_walk_governor_host.c`;
- aktualizacja `tests/host/walk_assist_diag_host.c`, `walk_assist_run_min_host.c`,
  `tests/fw060_walk_speed_controller.js` (część przypadków opisuje usuwaną całkę).

---

## 9. Plan testów (7 poziomów, zgodnie z protokołem)

1. **Kod** — kompilacja NORMAL i DIAG; kontrola `.map` (RAM DIAG jest ciasny, ok. 512 B zapasu).
2. **Testy hostowe** — nowy zestaw: rampa 550/110, oba ograniczniki, brak windupu, watchdog,
   podtrzymanie Halla, twarde cięcie koła; plus cały istniejący zestaw bez nowych awarii.
3. **CAN** — odczyt/zapis banku bez zmiany długości; `Walk current` zapisany i odczytany.
4. **Canable** — karta Walk pokazuje działający parametr; zapis banku potwierdzony
   (uwaga: odstęp 600 ms między zapisami, [[project-can-one-multiframe-channel]]).
5. **Koło w górze** — rampa, obroty, zjazd współczynnika, brak pompowania; cel 20 / 30 / 50 obr/min
   musi dawać wyraźnie różne obroty zębatki.
6. **Bezpieczny teren** — testy A–G z §48 dokumentu G532: start płasko lekki bieg, ciężki bieg,
   podjazd, **zmiana biegu w trakcie marszu** (to jest test przewagi obrotów zębatki nad
   prędkością koła), szybkie puszczenie, hamulec w trakcie.
7. **Regresja jazdy** — normalne wspomaganie i wygaszenie bez zmian względem obecnego buildu.

Formularz A/B (start / trzymanie / wygaszenie, oceny 1–5) wymagany — nie pytanie „czy lepiej".

---

## 10. Kryteria zaliczenia

- **Start:** wyraźnie mocniejszy niż dziś, bez kliknięcia i bez skoku; pełne narastanie ≈ 0,55 s.
- **Trzymanie:** brak przestrzelenia; obroty zębatki wchodzą w pas i tam zostają; brak cyklicznego
  pompowania i oscylacji.
- **Brak odcinania:** na lekkim biegu moc **maleje płynnie** zamiast znikać; w logu `0x10228`
  **żadnego** wejścia w `STALL` w normalnym marszu.
- **Bieg:** zmiana biegu zmienia prędkość roweru, a nie charakter wspomagania.
- **Bezpieczeństwo:** hamulec, błąd i twarde cięcie koła mają pierwszeństwo nad rampą; limity
  prądu/temperatury nie są omijane.

## 11. Plan powrotu

Zmiana jest zamknięta w dwóch plikach WA plus trzech stałych. Powrót = przywrócenie
`walk_speed_controller.c`/`walk_assist_motor.c` z commita `11dbeb6` i cofnięcie bramki koła
w `main.c`. **Build A/B:** proponuję przełącznik kompilacji `WALK_GOVERNOR_ENABLE` (0 = dzisiejsze
PI, 1 = nowe prawo), żeby jazda porównawcza była dwoma plikami .bin, a nie dwoma gałęziami —
tak jak przy QZERO.

## 12. Czego to NIE poprawi + skutki uboczne

**Nie poprawi:**
- odczucia przy jeździe ze wspomaganiem — to inny tor, całkowicie nietknięty;
- kliknięcia na końcu wybiegu (strefa `RIDE_COAST_RELEASE_ERPS`, temat QZERO/FW-048);
- braku wygaszenia przy puszczeniu przycisku — **zostaje twarde zero** jak dziś (G532 ma tam
  110 ms; zmiana tego dotyka `immediate_cut`, świadomie zostawiona poza kartą);
- niczego w torze momentu/nacisku.

**Skutki uboczne, które trzeba przyjąć:**
- prędkość marszu **pływa z obciążeniem** (§6) — świadoma zamiana za brak przestrzelenia;
- podniesienie sufitu marszu to **więcej ciepła i większy prąd przy zakleszczeniu**; chroni
  watchdog i limity fazowe/termiczne, ale to realna zmiana obciążenia napędu;
- domyślne zachowanie WA **zmieni się dla każdego banku** — to nie jest zmiana neutralna.

---

## 13. Pytania do właściciela — ODPOWIEDZIANE 2026-09-03, patrz §0

**P1 — jak mocny ma być marsz?** `Walk current` zacznie działać. Dziś sufit to sztywne 40 jednostek
(3,8 A). Proponuję **domyślnie 15 %** (= 105 jednostek ≈ 10 A, czyli **ok. 2,6× dzisiejszej siły**),
z sufitem absolutnym 157 (14,9 A) przy 100 %. Alternatywy: 10 % (2× dzisiaj, ostrożniej) albo
zostawić 30 % z banku, co po obcięciu daje od razu maksimum 157 (≈ 3,9× dzisiaj — moim zdaniem za
dużo na pierwszą jazdę).

**P2 — wariant A czy B?** Rekomendacja: **B**, bo A tylko tłumi objawy. A jest sensowne, jeżeli
chcesz najpierw jednym małym buildem potwierdzić, że limit koła faktycznie jest sprawcą odcinania.

**P3 — czy najpierw log?** Mogę przygotować build DIAG **bez żadnej zmiany zachowania**, żeby
ramka `0x10228` z jednej jazdy rozstrzygnęła, czy odcina limit koła (`SPEED_GATE`) czy watchdog
(`JAM`/`STALL`). To jedna jazda i zdejmuje jedyną hipotezę z tej karty.

---

---

## 14. Wynik testów hostowych (2026-09-03)

Nowy zestaw `tests/host/fw130_walk_governor_host.c` — linkuje prawdziwe
`walk_assist_motor.c` + `walk_speed_controller.c`, bez zaślepek:

| Test | Co dowodzi | Wynik |
|---|---|---|
| T1 | narastanie pełnej skali ~550 ms, opadanie ~110 ms, **opada szybciej niż narasta** | PASS |
| T2 | przy zmierzonych = cel współczynnik = równo 128/256 → pas **wyśrodkowany** | PASS |
| T3 | to samo przy 60 obr/min → pas **proporcjonalny**, 20 i 60 zachowują się tak samo | PASS |
| T4 | 5 s poniżej celu (to, co nakręcało całkę w prawie A) i zejście do zera nadal w jednej rampie | PASS |
| T5 | zjazd współczynnika koła PRZED limitem, bez fałszywego `SPEED_GATE` | PASS |
| T6 | na limicie zero w tym samym takcie, **sesja żyje**, powrót = jedna rampa 550 ms | PASS |
| T7 | powyżej limit + margines to prawdziwe zatrzymanie z `SPEED_GATE` | PASS |
| T8 | `Walk current` dociera do silnika; 0 = stare 40; absurdalna wartość obcięta do 157 | PASS |
| T9 | 3 s grace nie nazywa wolnego startu zakleszczeniem; prawdziwy stall nadal zatrzaskuje | PASS |

Zestaw sam raportuje **SKIPPED** przy `WALK_GOVERNOR_ENABLE=0`, żeby build prawa A nie wyglądał na
zielony w zachowaniu, którego nie implementuje.

Regresja: `walk_assist_run_min_host.c` (FW-113.1) i `walk_assist_diag_host.c` (FW-113.2) —
**PASS w obu prawach, A i B.** Pełny przebieg `tests/host/run-host-tests.ps1`: jeden padający
zestaw, wyłącznie znana wcześniejsza rodzina diag schema-3 (514 linii `FAIL`, dwa komunikaty:
`T14 schema-3 metadata` i `T9 frozen wire bytes`) — **żadnej nowej awarii**.

Trzeba było poszerzyć jedno okno source-guarda o stałym rozmiarze w
`tests/host/stopclick_c1_pi_integral_host.c` (3700 → 4300 znaków), bo `walk_assist_iq_request()`
urosło o przekazanie `walk_iq_max`. To ten sam zabieg co przy QZERO; treść guarda bez zmian.

## 15. Czego jeszcze NIE zrobiono

- **Build firmware NIE uruchomiony** — zgodnie z zasadą „nie budować bez polecenia".
  Przed jazdą: `.\build_firmware.ps1`, kontrola `.map` (RAM w DIAG jest ciasny).
- **Nic nie zacommitowane**, ani w EBICS, ani w Canable.
- **Nie było jazdy.** Wszystkie liczby o odczuciu w tym dokumencie to przewidywania z modelu i
  testów hostowych — **WYMAGANY TEST NA ROWERZE** wg planu z §9.
- Jazda A/B: build z `WALK_GOVERNOR_ENABLE=1` (B) kontra `0` (A) — dwa pliki .bin.

## 16. Uwaga o teście modelowym JS

`tests/fw060_walk_speed_controller.js` czyta źródło jako TEKST i modeluje **prawo A**. Wszystkie
stałe `WA_SPEED_*` zostały dlatego w pliku zachowane (nawet nieużywane przez prawo B), żeby ten
model dalej działał. Świadome ograniczenie: **ten test nie mówi nic o prawie B** — pokrycie prawa B
daje wyłącznie `fw130_walk_governor_host.c`, który kompiluje i URUCHAMIA prawdziwy kod.

---

# FW-130.1 — poprawka po pierwszej jeździe (0.0492 → 0.0494)

**Zgłoszenie:** „rusza płynnie i po chwili jest on-off, i tak na przemian", przy `Walk current 15 %`
i prędkości koła maks. 3 km/h (czyli **bezpiecznik koła w ogóle nie działał** — zjazd zaczyna się
dopiero przy 5,5 km/h).

## 1. Co sprawdziłem, zanim cokolwiek zmieniłem

Zamknięta pętla z **prawdziwymi modułami** (`walk_assist_motor.c` + `walk_speed_controller.c`)
i modelem napędu z wolnobiegiem: `scratchpad/wa_plant_loop.c`. Sweep po trzech napędach
(ciężki/średni/lekki), po suficie 35…157 i po obciążeniu 30…1,5 jednostki prądu.

**Wynik: regulator siada spokojnie w każdym z tych przypadków.** Prąd ustala się na ok. 26,
obroty na 29 ERPS, zero cykli. To **oczyszcza hipotezę „za duże wzmocnienie"** jako samodzielną
przyczynę — sam pas i sama siła tego nie robią.

## 2. Co jest naprawione

### 2.1 Brak sygnału ≠ prędkość zero (wymaganie właściciela)

> „zabezpiecz sterowanie WA na wypadek zaniku sygnału z Halla, bo wtedy algorytm może nie wiedzieć
> jakiej prędkości albo przybrać złą. Na pewno jak się zatrzyma, nie może czytać z Halla prędkości,
> bo ona nie wystąpi." — właściciel, 2026-09-03

W v1 brakujący albo jeszcze niepoliczony odczyt dawał `gear_factor = 256`, czyli **pełny sufit**.
To zamyka pętlę bez żadnego udziału roweru: pełna moc → przestrzelenie → zero → wirnik staje →
brak sygnału → „nie jedziemy, dawaj pełną moc".

Wprowadzone jawne pojęcie **`speed_known`**: prawda tylko wtedy, gdy estymator faktycznie zmierzył
okres. `hall_valid` NIE wystarcza — po przerwie pierwszy impuls czyni sygnał „ważnym", podczas gdy
policzona prędkość to nadal 0. Trzy przypadki są teraz rozdzielone:

| Sytuacja | Odpowiedź sterownika |
|---|---|
| odzyskiwanie po utracie Halla | ograniczony sufit modułu (24), jak dotąd |
| przed zakończeniem startu | pełny sufit — wirnik STOI i o tym wiemy, to jest definicja startu |
| w ruchu, brak odczytu | **trzyma ostatni werdykt** — nic nie zmierzono, więc nic nie zmienia zdania |

### 2.2 Nie doprowadzać do zatrzymania wirnika

Sama reguła 2.1 nie wystarcza: po odzyskaniu wirnika zmierzona prędkość jest **naprawdę** niska,
więc regulator zgodnie z prawem daje pełną moc — i cykl wraca. Dlatego dochodzi podłoga
`WA_MOTOR_KEEPALIVE_IQ = 2` (prawo B), aktywna **tylko** gdy sesja biegnie i istnieje realny
odczyt prędkości. Każde zero bezpieczeństwa pozostaje prawdziwym zerem: limit koła, hamulec,
puszczenie, błąd, `LIMIT` i `STALL` nadal podają 0.

### 2.3 Łagodniejsze opadanie: 110 → 250 ms

Fabryczne 110 ms dotyczy G532, który ma **za sobą osobny slew**; my go nie mamy (WA omija slew
16 kHz), więc to jest opadanie samego prądu silnika. Przy 110 ms każda chwilowa przerwa stawała
się pełnym zejściem do zera, a odbudowa trwała 550 ms — czyli on-off. 250 ms jest nadal **5×
szybsze** niż 1280 ms w prawie A, więc własność antyprzestrzeleniowa zostaje.

## 3. Co z wymaganiami FW-113 (pytanie właściciela)

**FW-113.2 (brak timeoutu trzymania + bity powodu): w pełni aktualne, nietknięte.**

**FW-113.1 („RUN minimum Iq 2 → 0", brak dodatniej podłogi): aktualne dla prawa A, ZASTĄPIONE dla
prawa B.** Powód, dla którego wtedy wybrano 0, przestał obowiązywać razem z prawem, dla którego go
wybrano:

- FW-079 usunął podłogę **5 Iq**, bo ona **sama napędzała rower** — 30 i 50 obr/min zbiegały się do
  tej samej prędkości. To był argument o wielkości podłogi, nie o jej istnieniu.
- FW-113.1 zszedł z 2 na 0, bo w prawie A **nic nie potrzebowało utrzymywania wirnika**: powyżej
  celu prąd opadał 1280 ms, więc wirnik praktycznie nigdy nie stawał.
- W prawie B prąd potrafi zejść do zera w 250 ms i tam zostać, więc wirnik **staje** — a wtedy
  znika sygnał, którego cały regulator potrzebuje.

Dlatego wartość to **2, nie 3**: 2 to ostatnia liczba, którą ktoś świadomie wybrał do tego samego
celu (FW-082), i mniej niż połowa szkodliwych 5 z FW-079. Nie wymyślam nowej stałej tam, gdzie
istnieje sprawdzona. Dodatkowo, inaczej niż podłoga z FW-079, ta jest **bramkowana realnym
odczytem prędkości**, więc nie może być tym, co trzyma rower powyżej celu.

Testy `walk_assist_run_min_host.c` i `walk_assist_diag_host.c` mają teraz próg zależny od prawa
(`RUN_FLOOR_IQ`): **0 dla prawa A, 2 dla prawa B**. Wymaganie FW-113.1 jest więc nadal egzekwowane
tam, gdzie obowiązuje.

## 4. Testy

- Nowy `T10` w `fw130_walk_governor_host.c`: **utrata sygnału prędkości nigdy nie PODNOSI prądu** —
  szczyt nie przekracza ograniczonego odzyskiwania (24) i jest daleko od sufitu marszu.
- `T1`/`T2`/`T4` przestrojone na 250 ms i na podłogę zamiast twardego zera.
- FW-113.1 i FW-113.2 **zielone w obu prawach**.
- Pełny przebieg hostowy: **jeden padający zestaw** — wyłącznie znana wcześniejsza rodzina diag
  schema-3. Bez nowych awarii.

## 5. Buildy

```text
0.0494  NORMAL  108 112 B  RAM 25,93 %  SHA-256 FC10FBF2C5AEF49FE551D0980077028080216156C442D0E9CA1811D4661789DA
0.0495  DIAG    156 992 B  RAM 99,01 %  SHA-256 1DEA210C63C3C152A11BB0FDF456037C527875BF86E7D54751C553E7CB91F46F
```

⚠ RAM w DIAG: **99,01 %**, wolne 488 B. Ciasno; kolejna funkcja w buildzie DIAG wymaga zwolnienia
miejsca.

**Nadal HIPOTEZA:** nie odtworzyłem on-off offline, więc nie mam dowodu, że to były te przyczyny.
Jeśli po 0.0494 pulsowanie wróci, następny krok to log z 0.0495 (ramka `0x10205`: czy w każdym
cyklu znika flaga Halla).

---

# ZAMKNIĘCIE KARTY (2026-09-03)

**Status: Walk Assist ZAMKNIĘTY jako działający.** Decyzja właściciela po serii testów.

## Ustawienia potwierdzone jazdą

```text
Walk motor current   = 25 %   (sufit 157 Iq ~ 15 A - twarde maksimum tego firmware)
Target chainring RPM = 30
```

Przy tych wartościach: **rusza płynnie, jedzie bez szarpania.** Objawy, od których zaczęła się
karta — mało siły przy ruszeniu, przestrzelenie prędkości, odcinanie w czasie działania — **nie
wystąpiły**. FW-130 i FW-130.1 potwierdzone na rowerze.

## Co zmierzono z ramek `0x10205` (trzy migawki z jazdy 22:30–22:33)

| Cel | Zmierzone | Prąd | Stan |
|---|---|---|---|
| 40 ERPS (30 obr/min) | 37 ERPS (27,8 obr/min) | 84 | REGULATE, nasycony |
| 27 ERPS (20 obr/min) | 18 ERPS (13,5 obr/min) | 104 | REGULATE, nasycony |
| 27 ERPS (20 obr/min) | 19 ERPS (14,3 obr/min) | 91 | REGULATE, nasycony |

**Wniosek:** przy celu 20 silnik siedzi na 18–19 ERPS z pełnym prądem — czyli **poniżej dolnej
krawędzi pasa zjazdu (23 ERPS)**, więc regulator nic nie moduluje, tylko oddaje wszystko, co ma.
To po prostu **brak momentu przy suficie 105**, a nie wada sterowania. Przy celu 30 pas leży wyżej,
silnik go dosięga i pracuje w obszarze regulacji — stąd płynność.

Sufit 105 w tych logach dowodzi, że sterownik miał wtedy `Walk current` = **15 %**, a nie 25 %.

## Co ZOSTAJE OTWARTE (świadomie, poza tą kartą)

1. **Szarpanie przy celu 20 i niskim prądzie** — nie odtworzone na buildzie DIAG, nie zmierzone
   przebiegiem. Trzy migawki są daleko zarówno od pasma przełączania kąta (5,6–8,3 ERPS), jak i od
   pasa zjazdu mocy — więc **żadna z dwóch hipotez nie została potwierdzona ani obalona**.
   Praktycznie obchodzone ustawieniem 25 % / 30 rpm.
2. **Klik na końcu wybiegu i przy ruszaniu** — osobny, realny defekt. Karta **FW-131**.
3. **Nieregularne odczyty prądu i kadencji na HMI** zgłoszone przy buildzie DIAG — niesprawdzone.
   Miernikiem jest `missed_control_ticks`.

## Uwaga architektoniczna odkryta przy okazji (ważna na przyszłość)

**Ramki `0x10203`–`0x10208` i `0x10228` NIE lecą już na żywo.** Od FW-106 są budowane jako
**jedna migawka na zarejestrowaną sesję** (`diag_aggregate_frame` jest źródłem rekordu dla
`diag_session`, nie strumieniem). Dlatego w logu z jazdy jest po jednej sztuce każdej z nich.
Dla porównania log z czerwca 2026 ma **71 573** ramek `0x10204` — wtedy strumień leciał ciągle.

Kto będzie planował diagnostykę WA: **nie zakładać strumienia**. Przebieg czasowy wymaga albo
nowego źródła rekordu, albo świadomego dodania żywej emisji w buildzie DIAG.

---

# PASS OSTATECZNY — 2026-09-04

**Decyzja właściciela: Walk Assist zamknięty jako PASS.** Cytat: *„ma siłę i utrzymuje prędkość"*.

Potwierdzone na buildzie **0.0496** (FW-130 + FW-130.1 + FW-131), ustawienia
`Walk motor current 25 %` + `Target chainring RPM 30`.

Trzy objawy, od których zaczęła się karta, są zamknięte:

| Zgłoszenie z 2026-09-03 | Stan |
|---|---|
| mało siły przy ruszeniu | **naprawione** — suwak `Walk current` w ogóle nie miał czytelnika w firmware |
| przestrzelenie prędkości | **naprawione** — rampa opadała 12× wolniej niż fabryczna, teraz trzyma prędkość |
| odcinanie w czasie działania mocy | **naprawione** — limit koła był wyłącznikiem z resetem sesji, jest bezpiecznikiem ze zjazdem |

**Sekcja B pytań kontrolnych** (`PYTANIA_POTWIERDZAJACE_2026-09-03.md`) — zamknięta tą decyzją.
Wracamy do tematu tylko, jeżeli pojawią się nowe uwagi z jazdy.

**Otwarte pozostaje wyłącznie to, co nie dotyczy Walk Assist:** klik na końcu zatrzymania
(QZERO-3, build 0.500 czeka na test) oraz commit całej serii.
