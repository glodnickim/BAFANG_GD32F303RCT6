# QZERO — Quiet Zero: kontrolowane wygaszanie całek PI po dojściu Iq_ref do 0

Status: **wdrożone w źródle, zbudowane, NIE przetestowane na rowerze.**
Buildy: **A = 0.0491** (baseline, `QUIET_ZERO_ENABLE 0`), **B = 0.0490** (QZERO, `QUIET_ZERO_ENABLE 1`).
Poprzednia para 0.0489/0.0488 jest **wycofana** - nie miała bramy prędkościowej z sekcji 3.4
ani domyślnego `TORQUE_RUN_ASYM_FALL_MS = 250` (sekcja 3.5). Nie flashować jej.

---

## 1. Prostym językiem

Po usunięciu kliku (karta STOP-CLICK-C1) pojawił się nowy objaw: silnik po puszczeniu pedałów
kręci się i cichnie zbyt długo — „przeciąga”.

Dlaczego? Wcześniej, przy zerowym momencie, firmware co chwilę zerował pamięć regulatora prądu.
Efektem ubocznym było to, że mostek przestawał podtrzymywać napięcie i **hamował** wirnik.
To hamowanie kończyło bieg silnika szybko — ale skok napięcia był właśnie tym słyszalnym klikiem.

STOP-CLICK-C1 słusznie usunął to zerowanie (bo dwa różne miejsca programu pisały po tej samej
pamięci jednocześnie). Klik zniknął. Razem z nim zniknęło jednak hamowanie: regulator, który
pracuje bez przerwy, ustawia napięcie mostka **dokładnie równe napięciu generowanemu przez
kręcący się silnik**. Prąd nie płynie, nic nie hamuje, silnik zwalnia sam — czyli długo.

Quiet Zero robi to samo co dawny reset, ale **łagodnie i tylko wtedy, kiedy trzeba**: po
normalnym zakończeniu pedałowania albo po cofnięciu/awaryjnym odcięciu pamięć regulatora jest
sprowadzana do zera **liniowo w ciągu 10 ms** i tam przytrzymana. Nie ma skoku (czyli nie ma
kliku), a hamowanie wraca. Mostek, FOC, PWM i część proporcjonalna regulatora pracują cały czas
— nic się nie wyłącza, nic nie startuje od zera przy ponownym naciśnięciu pedałów.

Czego Quiet Zero **nie** robi: nie zadaje ujemnego prądu, nie włącza rekuperacji, nie wyłącza
mostka, nie robi „zimnego startu” stopnia mocy i nie zmienia żadnego czasu ramp (Ramp Up/Down,
release_ms, filtry momentu — bez zmian).

UWAGA: ten build wiezie **drugą, osobną poprawkę** — domyślne wygładzanie opadania RUN wraca z 0
na 250 ms (sekcja 3.5). To nie jest część QZERO i dotyczy innego objawu (moc zjeżdżająca w martwym
punkcie korby), ale jedzie w tym samym obrazie, więc trzeba o niej wiedzieć przy ocenie jazdy.

---

## 2. Root cause — potwierdzony w źródle

Zadanie wymagało potwierdzenia sześciu rzeczy przed wdrożeniem. Wszystkie sześć potwierdzone:

| # | Twierdzenie | Dowód w źródle |
|---|---|---|
| 1 | QS-3D poprawnie doprowadza `MS.i_q_setpoint` do 0 | `src/fast_iq_slew.c` gałąź `FIS_MODE_RELEASE/SAFETY`: `if (fis.accumulator_q10 <= 0 \|\| fis_to_iq(...) == 0) { accumulator = 0; rate = 0; iq_ref = 0; }` — publikowane zero i stan frakcyjny stają się dokładnym zerem w tym samym ticku. Test `qs3d_16khz_slew_host.c` pinuje to od dawna; nowy `qzero_quiet_zero_host.c` T10 przejeżdża prawdziwy release 650 ms przez prawdziwy owner. |
| 2 | normal release ≈ 650 ms | `src/ride_control.c:648` `profile_release_ms = level->release_ms` → `ride_final_iq_slew_compute()` → `*out_release_ticks_16k = release_ms * 16`. 650 ms to skonfigurowana wartość profilu (patrz komentarz FW-072 w `assist_dynamics.c`: „A configured 650 ms behaved like ~1250 ms” — od FW-072 to jedna liniowa rampa). **Uwaga na kwantyzację:** `fis_live_release_rate()` liczy `ceil(accumulator/ticks)`, więc release ląduje zawsze nieco **wcześniej**, nigdy później — dla 300 counts i 10400 ticków realnie 10223 ticki = 639 ms (98,3 %). To zachowanie QS-3D, sprzed tej karty, i **nie jest** tu zmieniane. |
| 3 | reverse/safety ≈ 200 ms | `src/ride_control.c:228` `#define RIDE_HARD_CUT_RAMP_MS 200` + `_Static_assert(... <= 250)`; przy `hard_cut` linia 849 ustawia `profile_release_ms = RIDE_HARD_CUT_RAMP_MS` i `profile_pedaling_active = false`, więc ta sama gałąź release wydaje `FIS_MODE_SAFETY`. |
| 4 | po Iq_ref=0 mostek, FOC, SVPWM i PI nadal działają | `BRIDGE_LIFECYCLE_RUN && MS.i_q_setpoint == 0 → BRIDGE_LIFECYCLE_ARMED_ZERO` (`main.c` ~1533) i nic więcej: żadnego MOE off, dwell, resetu ani przejęcia CCR (komentarz w miejscu). `POWER_STAGE_STOP_TICKS` jest jeszcze zdefiniowane w `config.h`, ale **nie ma już ani jednego użycia w `src/`** — dawnego soft-cutoffu nie ma. ISR: `if(ui_8_PWM_ON_Flag){ ... } else` → `FOC_calculation()` co tick, a w nim `runPIcontrol()` → `PI_control()` → `svpwm()`. |
| 5 | po C1 integratory PI nie są resetowane przy zwykłym zerze | `main.c:3601` i `:3654` — dwa dawne miejsca resetu usunięte, z komentarzem STOP-CLICK-C1 w miejscu. Test `stopclick_c1_pi_integral_host.c` T1/T10 liczy wystąpienia `PI_iq.integral_part=` w spanie `reg_ADC_processing()` i wymaga zera. |
| 6 | **to ciągły PI po Iq_ref=0 powoduje przeciąganie** | Wniosek z 4+5, potwierdzony liczbowo na prawdziwej matematyce regulatora: `qzero_quiet_zero_host.c` **T11-A** trzyma `setpoint=0` przez 500 ms na replice `PI_control()` z FOC.c i mierzy, że całka osiada na wartości BEMF (±40 z 700) a prąd uzwojenia jest ~0. To jedyny punkt równowagi: przy `Iq_ref=0` regulator zbiega tam, gdzie `Iq_measured=0`, czyli `u_q ≈ BEMF`. Mostek aktywnie **dopasowuje się** do napięcia generowanego przez wirnik → zero prądu → zero momentu hamującego → długi wybieg. **T11-B** pokazuje odwrotność: po wygaszeniu całki prąd hamujący wraca (>20 counts, wielokrotność wybiegu bazowego) i pozostaje w granicy abort. |

---

## 3. Co dokładnie zostało zrobione

### 3.1 Nowy moduł `src/quiet_zero.c` + `inc/quiet_zero.h`

Automat stanów w domenie 16 kHz, bez globali (cały stan po stronie wołającego), bez dzielenia
(mnożenie przez stałą odwrotność `QZERO_BLEND_RECIP`), testowalny na hoście:

```
QZERO_INACTIVE → QZERO_BLEND → QZERO_HOLD
```

* **Wejście** wymaga trzech rzeczy jednocześnie: poprzedni tick miał `Iq_ref > 0`, ten ma
  dokładnie `0`, a producent oznaczył to zero jako `FIS_ZERO_POLICY_QUIET`.
  Przy wejściu zapamiętywane są `PI_iq.integral_part` i `PI_id.integral_part`.
* **BLEND** — obie całki liniowo do dokładnego zera przez `QZERO_BLEND_TICKS = 160` ticków
  (10,0 ms @16 kHz). Wartość liczona jako `wejście × (N−k)/N` z zapamiętanej wartości, nie przez
  odejmowanie — trajektoria jest dokładnie liniowa, a jej ostatni punkt to dokładnie `0.0f`.
* **HOLD** — `integral_part = 0` trzymane tak długo, jak `Iq_ref == 0`.
* **Wyjście** — `Iq_ref > 0` kończy QZERO w tym samym ticku. Nic nie jest wtedy narzucane:
  regulatory dostają swoją całkę z powrotem, FOC-AW1 wraca, bez MOE toggle, bez cold PREPARE,
  bez neutral dwell.
* **Abort** — `|Iq| ≥ QZERO_ABORT_CURRENT` **lub** `|Id| ≥ QZERO_ABORT_CURRENT` w trakcie
  BLEND/HOLD kończy tryb P-only i oddaje osie pełnemu PI zerowego prądu (całka znów wolna — to
  właśnie stan, który daje zero prądu). Abort **nie uzbraja się ponownie**: dopiero nowa
  krawędź release może wejść w QZERO. `QZERO_ABORT_CURRENT = PH_CURRENT_MAX >> 1` = 350 counts.
* `PI.out` **nigdy** nie jest dotykane, referencja pozostaje dokładnie 0, część P pracuje.

### 3.2 Jawna polityka zera w skrzynce QS-3D (`zero_policy`)

Sam tryb nie odpowiadał na pytanie „dlaczego to zero”: `FIS_MODE_SAFETY` obejmuje cofnięcie,
hamulec, przegrzanie **oraz kalibrację nacisku pedałów**, a zero od limitera prędkości/baterii
przychodzi jako zwykły `FIS_MODE_FALL` do 0. Dlatego skrzynka dostała **siódme słowo**
`zero_policy` (seqlock i `_Static_assert` zaktualizowane na 28 B / siedem słów; pole uczestniczy
w tej samej generacji — test J1/J2/J3 i K26 sprawdzają to jawnie), a `fast_iq_slew_publish()`
— jawny parametr. Consumer czyta `fast_iq_slew_current_zero_policy()`, czyli politykę **tej samej
zweryfikowanej generacji**, która wyprodukowała ten `Iq_ref`.

`ride_control.c` jest default-deny: `*out_zero_policy = FIS_ZERO_POLICY_NONE` na wejściu do
`ride_final_iq_slew_compute()`, a `QUIET` przyznawane jest w **jednym** miejscu — w gałęzi
`profile_release_active`, z wyłączeniem kalibracji:

```c
*out_zero_policy = input->service_cut ? FIS_ZERO_POLICY_NONE : FIS_ZERO_POLICY_QUIET;
```

`service_cut` to nowe pole (`ride_control_input_t.service_cut_active`, wypełniane w `main.c`
z `torque_input_calibration_active()`). **Nie zmienia żadnej decyzji odcięcia** — kalibracja
nadal hard-cutuje przez `safety_cut_non_direction`; pole służy wyłącznie polityce.

Skutek: QZERO **nie** uruchamia się dla limitera prędkości, limitera baterii/napięcia,
zmiany poziomu, Walk Assist, kalibracji pozycji, kalibracji nacisku, FW-048 coast ani FW-112
force-zero.

### 3.3 Wpięcie w ISR (`runPIcontrol()`, `main.c`)

Decyzja podejmowana jest **po** `pi_iq_apply_inputs()` (czyli po wyprodukowaniu `Iq_ref` przez
`fast_iq_slew_tick()`) i **przed** oboma regulatorami. Zadana całka jest zapisywana **przed** i
**po** wywołaniach `PI_control()`: `PI_control()` dodaje własny jednotickowy przyrost
`gain_i·error`, a nadpisanie po wywołaniu sprawia, że trajektoria fade (i dokładne zero w HOLD)
jest tym, co regulator faktycznie trzyma i co widzi trace — i że ten przyrost nie może się
skumulować w nowe napięcie kompensujące BEMF. `PI.out` przyrost zachowuje (część P ma żyć).

FOC-AW1 w trakcie QZERO nie może ruszać całki: `PI_iq.aw_sat_error`/`PI_id.aw_sat_error`
(i `MS.u_q_sat_err`/`u_d_sat_err`) są zerowane w ISR na każdym ticku QZERO oraz **raz** na
krawędzi wejścia/wyjścia/abortu — dzięki temu po ponownym ruszeniu nie ma skoku z korekty
liczonej względem trzymanego wektora. To **nie jest** `foc_aw_tracking_reset()` (ta funkcja jest
hookiem foreground przy mostku off i czyści też wektor żądany oraz licznik saturacji).

Reset stanu QZERO tylko w trzech miejscach — dokładnie tych, które `foc_aw_tracking_reset()`
już nazywa (cold PREPARE przed MOE ON, failsafe dwell-timeout, ścieżka kalibracji Halla). Test
T13 wymusza, żeby **każde** z nich sąsiadowało z `foc_aw_tracking_reset()`, i żeby w
`reg_ADC_processing()` (4 kHz foreground) nie było ani jednego dotknięcia tego stanu — to
byłaby ta sama wada, którą usunął C1, tylko przez inne pole.

### 3.4 Brama prędkościowa - hamowanie kończy się PRZED przeskokiem kąta

Dodane po uwadze właściciela, że historyczny klik występował **na końcu wybiegu, przy ~0 obr/min,
gdy układ się rozłączał** - a nie w chwili puszczenia pedałów. To zmienia mechanizm, nie tylko
moment, i odsłoniło realną lukę w pierwszej wersji tej karty.

Co siedzi na końcu wybiegu (liczby z kodu: TIMER2 500 kHz, 6 zdarzeń Halla na obrót elektryczny,
`SIXSTEPTHRESHOLD = 10000`):

* **przeskok formuły kąta**: w dół na sześciostopniowy przy **5,56 erps ≈ 4,2 obr/min zębatki**,
  w górę na interpolację przy 8,33 erps (histereza). Obie metody dają inny kąt, więc kąt
  przeskakuje. Komentarz FW-048 mówi to wprost: "any current still flowing then jumps with it,
  which is the clunk heard exactly at standstill (and why stretching the release ramp never
  helped: it is a step, not a slope)";
* **rozłączenie mostka** ~1 s po zatrzymaniu wirnika (`POWER_STAGE_STOP_TICKS = 4000` @4 kHz) -
  mechanizm, który badała karta FW-117; persistent ARMED_ZERO go usunął (stała nadal jest w
  `config.h`, ale **nie ma ani jednego użycia w `src/`**).

Spójne wyjaśnienie obu faktów - obserwacji właściciela i starego kodu: przeskok kąta jest słyszalny
tylko wtedy, gdy w tej chwili płynie prąd. Stary reset całek utrzymywał prąd hamujący aż do
zatrzymania, czyli **dowoził prąd do strefy, w której klik mieszka**. Reset nie był klikiem - był
jego warunkiem. To tłumaczy, dlaczego usunięcie resetu klik zabrało, mimo że oba zdarzenia
dzieliło kilkaset ms, i dlaczego wydłużanie rampy nigdy nie pomagało.

**Luka, którą to odsłoniło:** FW-048 chroni tę strefę, zerując **zadany** prąd poniżej 10 erps.
QZERO wytwarza prąd **niezależnie od zadanej wartości** - bierze się on z braku napięcia
dorównującego BEMF, nie z żądania. Zerowa referencja przestała więc wystarczać, a pierwsza wersja
tej karty mogła przywrócić klik na końcu wybiegu (w mniejszej postaci: przy 4,2 obr/min BEMF to
~6 % wartości przelotowej).

**Poprawka:** `quiet_zero.c` oddaje osie pełnemu regulatorowi zerowego prądu poniżej
`RIDE_COAST_RELEASE_ERPS`, i **odmawia wejścia**, jeśli release kończy się już w tej strefie.
Próg przeniesiony z `ride_control.c` do `inc/config.h` - dwóch konsumentów, jedna liczba, bo
dwie kopie progu, którego całym zadaniem jest margines bezpieczeństwa, to gotowy rozjazd.

Nic nie tracimy: energia rośnie z kwadratem prędkości, więc przy 4-7 obr/min zębatki układ ma
**pod 1 %** energii przelotowej. Test T15d pinuje to liczbowo - hamowanie pokrywa ~89 % zjazdu
prędkości, czyli praktycznie całą energię.

### 3.5 Domyślne `TORQUE_RUN_ASYM_FALL_MS` = 250 (było 0)

Osobna, ale powiązana poprawka: właściciel zdjął filtr opadania RUN do zera, żeby usunąć ogon po
odpuszczeniu nacisku. Skutkiem ubocznym było przepuszczanie tętna nogi 1:1 do napędu - moc
zjeżdżała w każdym martwym punkcie korby. **Firmware sam to raportował**: test S5 w
`torque_run_asym_host.c` ma granicę 100 native i przy 0 pokazywał 142/126/106/89 (20/40/60/80 rpm),
a `fw112_run_rearm_recovery_host.c` sypał 24 błędami "warm RUN estimate positive".

Wartość **nie jest wybrana gustem, tylko przez testy**: 250 ms to najszybszy zjazd, przy którym
wszystkie istniejące testy zachowania są zielone (175 -> 2 błędy, 225 -> 1, 250 -> 0). Pełna
zmierzona tabela kompromisu jest w komentarzu przy stałej w `inc/torque_input.h`. Skutek na pełnym
zestawie: **-27 błędów, 0 nowych** (543 -> 516 linii FAIL, resztą jest niezależna od tej karty
diagnostyka schema-3).

Ten filtr **nie wydłuża zatrzymania** - gdy pedałowanie ustaje, żądanie jest zerowane w tym samym
ticku inną drogą (`pedaling_active`), więc stała kształtuje wyłącznie ZMNIEJSZENIE nacisku przy
dalszym pedałowaniu.

### 3.6 Przełącznik A/B


`inc/config.h`: `QUIET_ZERO_ENABLE` (domyślnie **1**). Bramkuje **wyłącznie** consumer w
`runPIcontrol()` — `zero_policy` jest publikowane w obu obrazach, więc A i B różnią się tylko
tym, czy ISR stosuje akcję QZERO. Różnica flash: 732 B.

---

## 4. Czego ta karta NIE zmienia

Filtry momentu, min-Iq hold (`Current floor`, `Sustain`), `release_ms`, hard-cut 200 ms,
Ramp Up/Down, `Kp/Ki/Kaw`, limiter prędkości, limiter baterii, Hall/theta, próbkowanie prądu,
EEPROM, protokół CAN, UI Canable (brak nowego parametru i brak zmiany protokołu — nie ma czego
synchronizować).

**Wyjątek, świadomy i osobny:** domyślne `TORQUE_RUN_ASYM_FALL_MS` (wygładzanie opadania RUN)
zmienia się z 0 na 250 — patrz sekcja 3.5. To stała kompilacji, nie ustawienie z aplikacji, więc
nie da się jej zmienić bez wgrywki; nie wpływa na czas zatrzymania po zaprzestaniu pedałowania.

---

## 5. Testy

`tests/host/qzero_quiet_zero_host.c` — nowy suite, linkuje **prawdziwy** `quiet_zero.c` i
**prawdziwy** `fast_iq_slew.c`:

* **T1–T9** automat: krawędź wejścia, brak wejścia dla polityki NONE (5000 ticków), brak wejścia
  bez krawędzi, liniowość fade na każdym ze 160 ticków + dokładne `0.0f`, HOLD przez 10 s,
  natychmiastowe wyjście i jednorazowość czyszczenia AW, abort w obu osiach i obu znakach + brak
  ponownego uzbrojenia + brak fałszywego abortu jeden count pod progiem, ciągłość HOLD przy
  zmianie trybu na zerze, reset.
* **T10** integracja z prawdziwym ownerem 16 kHz: wejście dokładnie na ticku pierwszego zera
  prawdziwego release 650 ms (jedno wejście, nie jedno na tick), release nadal trwa swój czas,
  prawdziwy FALL do zera z polityką NONE nie dotyka PI, oraz krótki release + ponowne naciśnięcie
  w trakcie fade.
* **T11/T12** efekt na prawdziwej matematyce `PI_control()` (replika pinowana source-guardem):
  A parkuje na BEMF przy zerowym prądzie, B wygasza w 160 tickach i przywraca prąd hamujący w
  granicy abort, referencja nigdy nie schodzi poniżej zera, wyjście rusza `PI.out` o najwyżej
  `max_step` (bez skoku), a oś wraca na referencję po ponownym naciśnięciu.
* **T15** brama prędkościowa: przekazanie osi poniżej progu w trakcie HOLD, brak fałszywego
  abortu, brak ponownego uzbrojenia, granica progu bez błędu o jeden, odmowa wejścia dla release
  kończącego się w strefie przeskoku kąta, oraz pełny zjazd 93 -> 0 erps z pomiarem pokrycia
  hamowania (~89 % rampy prędkości).
* **T13/T14** wiring guards na `main.c`/`ride_control.c` (jedno wywołanie tick w
  `runPIcontrol()` przed regulatorami, dwa miejsca aplikacji, zero `foc_aw_tracking_reset()` i
  zero resetu w ścieżce periodycznej, brak MOE toggle / resetu `PI.out` / zapisu neutral CCR,
  C1 nadal naprawiony, trzy sparowane miejsca resetu, jedno miejsce przyznania QUIET).

Pełny `tests/host/run-host-tests.ps1`: nowy suite **PASS**, i **zero nowych awarii względem
baseline**. Punkt wyjścia miał 4 padające suite'y / 543 linie FAIL (zweryfikowane na kopii drzewa
z odwróconymi zmianami QZERO — zbiory identyczne). Po tej karcie zostaje **1 suite / 516 linii**,
wyłącznie `rolling_no_assist_diag` (schemat v3 diagnostyki, niezależny od tej pracy). 27 błędów
zniknęło dzięki sekcji 3.5: cały FW-112 v2 (24), test tętna S5 (1) i warm-demand (2).
`comm` na posortowanych zbiorach potwierdza, że pozostałe awarie są **podzbiorem** baseline.

Dwa guardy o stałym rozmiarze okna trzeba było poszerzyć, bo QZERO dodał linię resetu do tych
samych bloków: `cold_prepare + 3800 → 4200` (`focaw1`, `stopclick`; kotwica MOE_ON jest teraz na
3817) i `hall_cal + 900 → 1200` (`stopclick`; kotwica DISABLE na 934). Powód zapisany przy
liczbach.

---

## 6. Test A/B na rowerze

**A = 0.0491** (`QUIET_ZERO_ENABLE 0`) — baseline C1 + obie poprawki spoza QZERO.
**B = 0.0490** (`QUIET_ZERO_ENABLE 1`) — to samo + Quiet Zero.

Oba mają już `FALL_MS = 250` i bramę prędkościową, więc jedyną różnicą A vs B jest samo QZERO.

Oba NORMAL, `debug`, M820_BL820. Jedna zmienna, jeden przejazd na obraz, ten sam odcinek i ten
sam poziom wspomagania.

| # | Scenariusz | Co robić | Na co patrzeć (A vs B) |
|---|---|---|---|
| 1 | **normalne puszczenie pedałów** | rozpędzić się na poziomie 3, mocno pedałować, puścić pedały i **nie ruszać nimi** | jak długo silnik jeszcze słyszalnie kręci się po zaniku wspomagania. Oczekiwane: B wyraźnie krócej. Klik: **nie może wrócić** ani w A, ani w B |
| 2 | **reverse pedals** | jadąc, cofnąć pedały o pół obrotu | odcięcie nadal natychmiastowe (200 ms rampa), wybieg silnika krótszy w B, brak stuku |
| 3 | **krótkie odpuszczenie i ponowne naciśnięcie** | puścić pedały na ~0,3–1 s i mocno nacisnąć ponownie | czy powrót wspomagania jest tak samo szybki jak w A i czy nie ma szarpnięcia. To najbardziej wrażliwy przypadek: w B silnik był krótko hamowany, więc musi „dogonić” korbę przez wolnobieg |

Dodatkowo w obu obrazach: start z miejsca (czy nic się nie zmieniło), hamulec podczas jazdy,
oraz jedno zatrzymanie do zera prędkości.

Jeśli B daje krótszy wybieg **ale** pojawia się jakikolwiek dźwięk przy puszczaniu pedałów —
pierwsze pokrętło to `QZERO_BLEND_TICKS` (dłuższy fade = łagodniej). Jeśli B **nie** daje
krótszego wybiegu — pierwszy podejrzany to abort guard; `QZERO_ABORT_CURRENT` jest wtedy zbyt
niskie i licznik `aborts` rośnie (dziś widoczny tylko z debuggera — patrz KNOWN ISSUES).

---

## 7. KNOWN ISSUES / świadome granice

0. **Klik na końcu wybiegu.** Brama prędkościowa (3.4) usuwa znany mechanizm - prąd w strefie
   przeskoku kąta. Nie da się jednak wykluczyć, że przy 4-7 obr/min zębatki zostaje jeszcze inne
   źródło hałasu (luz przekładni przy odwróceniu kierunku momentu). To jest do usłyszenia na
   rowerze, nie do rozstrzygnięcia przy biurku; scenariusz 1 testu A/B właśnie tego słucha.
1. **Zero od limitera, a potem release.** Wejście wymaga krawędzi „niezerowe → dokładne 0”. Jeśli
   prąd zszedł do zera **wcześniej** z innego powodu (limiter prędkości przy 25 km/h, limiter
   baterii) i rowerzysta puszcza pedały **później**, nowej krawędzi nie ma, więc QZERO nie
   wejdzie i wybieg będzie jak w A. Tak literalnie brzmi zadany kontrakt („nie uruchamiaj go
   automatycznie dla każdego Iq_ref=0”), więc zostało to zaimplementowane dokładnie tak, a nie
   szerzej. Rozszerzenie (krawędź polityki NONE→QUIET przy zerowym `Iq_ref`) jest jednolinijkowe
   i czeka na decyzję.
2. **Brak obserwowalności na rowerze.** `quiet_zero_t` liczy `entries`/`aborts`/`hold_ticks`, ale
   nic ich jeszcze nie wystawia na CAN ani do FW-117 trace. Świadomie: karta nie miała rozszerzać
   diagnostyki, a budżet RAM DIAG jest ciasny. Jeśli test A/B wyjdzie niejednoznacznie, dodanie
   jednego pola do trace jest następnym krokiem.
3. **Kwantyzacja release** (639 ms z 650 ms) jest zachowaniem QS-3D sprzed tej karty i nie została
   ruszona, bo `release_ms` jest na liście „nie zmieniać”.
4. **Prąd hamujący jest prawdziwym prądem.** Nie jest zadawany (referencja to dokładnie 0) i nie
   ma aktywnej rekuperacji, ale przy P-only i kręcącym się wirniku fizycznie płynie prąd
   generatorowy, ograniczony wyłącznie częścią proporcjonalną i guardem abort. Energia jest mała
   (wirnik odsprzęglony wolnobiegiem), ale to jest właśnie mechanizm, który skraca wybieg — i to
   jest powód, dla którego guard istnieje.
