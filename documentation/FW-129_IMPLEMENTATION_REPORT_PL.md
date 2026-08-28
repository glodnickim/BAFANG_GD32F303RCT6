# FW-129 — RAPORT WDROŻENIA (części C, D, E karty §40)

Status: **WDROŻONE, ZBUDOWANE, PRZETESTOWANE NA HOŚCIE — NIE ZAINSTALOWANE NA ROWERZE.**
Data: 2026-08-28. Baza: `master` @ `13b5c6a`. Audyt (części A i B): [FW-129_UNIT_DOMAIN_AUDIT_PL.md](FW-129_UNIT_DOMAIN_AUDIT_PL.md).

---

## 0. STRESZCZENIE PROSTYM JĘZYKIEM

**Co się zmieniło najbardziej:** tryby Power (Linear, Progressive, Curve) dostały **2,17 raza
więcej siły** przy tym samym ustawieniu. Nie dlatego, że coś podkręciłem — dlatego, że
wcześniej rower fizycznie nie umiał dać tego, co miał ustawione. Teraz ustawione 320% naprawdę
znaczy 320%.

**Co się nie zmieniło:** eMTB i Torque dają praktycznie tyle samo co przedtem (różnice
kilkuprocentowe). Ich jednostki są teraz poprawne i nie zależą już od kalibracji czujnika, ale
sama siła została taka sama — zgodnie z Twoim punktem 6 („nie stroimy jeszcze wartości").

**Uwaga ważna dla jazdy:** po tej zmianie **eMTB jest wyraźnie słabszy od Power Linear na tym
samym poziomie** — przy 20 kg i 60 rpm Power Linear prosi o 325 jednostek prądu, a eMTB o 45.
To nie jest już błąd jednostek (te są poprawne), tylko rzeczywista różnica skali między
parametrem eMTB a procentem wspomagania. Do wyrównania w osobnej karcie strojenia.

**Druga ważna różnica, teraz widoczna i zamierzona:** Power Linear/Progressive/Curve trzymają
**stały stosunek momentu** — kręcisz szybciej, prąd zostaje taki sam, a moc silnika rośnie
razem z Twoją. eMTB i Torque trzymają **stałą moc** — kręcisz szybciej, prąd maleje. To wynika
z algorytmu TSDZ, z którego pochodzi eMTB, i jest teraz zapisane w teście, żeby nikt tego
przypadkiem nie „naprawił".

**Czego NIE potwierdzam:** nic z tego nie było jeszcze na rowerze. Wszystkie liczby pochodzą
z testów uruchamiających prawdziwe moduły firmware na PC.

---

## 1. AFTER — TOR DOCELOWY, JAK ZOSTAŁ ZBUDOWANY (§40C)

```
ADC
 → native (auto-zero, deadband, filtr 35 ms, okno RUN 180°, filtr asymetryczny)
 → KALIBRACJA (torque_input.c):  corrected = native · SPAN_DEFAULT / span_cal
 → CHARAKTERYSTYKA:              load_centikg = default_piecewise(corrected)
                                                        ── KONIEC WARSTWY CZUJNIKA ──
 → STARTUP BOOST na load_centikg (assist_start.c)        ── POCZĄTEK WARSTWY ODCZUCIA ──
 → TRYB (assist_modes.c):
     Power L/P/C:  P_human = load · kadencja · k(crank_length_mm)
                   ratio   = stała | progresja | krzywa
                   P_req   = P_human · ratio
     eMTB/Torque:  x160    = min(160, load · 160 / assist_torque_full_scale_centikg)
                   target  = x160²/den   |   x160 · factor/120
                   I_req   = target · 160 mA          (jednostka TSDZ)
                   P_req   = I_req · emtb_reference_voltage_mv
 → KOMPENSACJA KADENCJI (raz, na obu kotwicach)
 → SUFIT max_motor_power_w (na obu kotwicach)
 → FILTR mocy rise/fall (dwa stany, po jednym na kotwicę)
 → JEDNA KONWERSJA + PRZENIKANIE:
       Iq_normal = P_req / (u_abs · V · CAL_I) · 2048
       Iq_launch = P_req(60 rpm) / (U_LAUNCH · V · CAL_I) · 2048
       Iq        = przenikanie(Iq_launch, Iq_normal) wg u_abs w oknie 256..1024
 → SUFIT max_iq_pct
 → LIMITY WSPÓLNE (napięcie, temperatura, prędkość/legal, prąd baterii, hamulec,
                   cofanie, awaria czujnika)  — BEZ ZMIAN
 → DYNAMIKA (rampy Iq, release, smooth start jako obwiednia, Extended Boost)
 → FOC
```

### 1.1 Przenikanie startowe — dowód, że jest ciągłe

W tabeli w sekcji 4 kolumny `launchIq` i `normIq` są **identyczne w całym normalnym zakresie
jazdy**. To nie przypadek — to konstrukcja: na napędzie środkowym wypełnienie jest wprost
proporcjonalne do kadencji, więc obie kotwice liczą tę samą wielkość, tylko zaczepioną w innym
punkcie. Przenikanie jest w normalnej jeździe **niewidoczne** i zaczyna cokolwiek robić dopiero
wtedy, gdy `u_abs` odrywa się od kadencji: przy starcie, przy zdławieniu silnika, przy
ślizgu. Brak progu, brak gałęzi `if`, brak skoku.

Test T7 przemiata `u_abs` przez 0/32/64/128/256/512/1024/1536/2048 dla trzech trybów
i sprawdza, że żaden krok nie zmienia prądu o więcej niż połowę wartości i że prąd nigdy nie
zapada do zera pod realnym naciskiem 20 kg.

### 1.2 `U_LAUNCH` — jedyna nowa stała, wciąż HIPOTEZA

`ASSIST_LAUNCH_REFERENCE_U_ABS = 1024` (50% wypełnienia przy 60 rpm korby).
**HIPOTEZA — WYMAGANY TEST NA ROWERZE.** Nie jest pokrętłem użytkownika. Jej błąd skaluje
wyłącznie człon startowy i wyłącznie poniżej 60 rpm; nie może wpłynąć na normalną jazdę, gdzie
używane jest zmierzone wypełnienie. Diagnostyka 0x6029 v6 podaje teraz `u_abs` i kadencję
obok siebie, więc jeden przejazd wystarczy do ustalenia właściwej wartości.

---

## 2. CO ZOSTAŁO ZMIENIONE W KODZIE

### Firmware

| Plik | Zmiana |
|---|---|
| `src/torque_input.c` / `inc/torque_input.h` | D8: kalibracja koryguje **wzmocnienie** nad fabryczną łamaną (a nie zastępuje jej prostą); nowy wzór na `span` przy przechwycie; wersja rekordu 1→2, rekord v1 odrzucany z flagą „przekalibruj"; sufit ścieżki assist w kg zamiast w `span_native`; bit zdolności `TORQUE_CAP_GAIN_CALIBRATION_V3` i flaga w bajcie 33 telemetrii |
| `src/tuning_config.c` / `inc/tuning_config.h` | blob v8 (32 B bez zmian): `assist_torque_full_scale_centikg` @24, `crank_length_mm` @26; akceptacja v2..v8; migracja do wartości domyślnych; naprawa bramki wersji dla okna RUN (v7, nie `TUNING_VERSION`) |
| `src/assist_start.c` / `inc/assist_start.h` | D7: boost startowy pracuje na `load_centikg`; próg AUTO 20 mV → 0,80 kg; brak sufitu `span_native` |
| `src/assist_modes.c` / `inc/assist_modes.h` | D1–D6, D9, D10: usunięte `calculate_load_iq_request()` i `calculate_target_x160_iq_request()`; nowe `assist_torque_x160_q()`, `power_to_phase_iq()`, `launch_blend_permille()`, `emtb_denominator()`, `emtb_target_to_power_mw()`; długość korby w równaniu mocy; dwa stany filtru mocy; zasada zaszczepiania filtru; `assist_modes_mark_power_filter_cold()`; nowe pola diagnostyczne |
| `src/ride_control.c` | filtr mocy oznaczany jako zimny na tych samych krawędziach sesji, na których kasowana/otwierana jest odbudowa estymatora RUN; `ride_control_init()` zeruje rampę prądu i obwiednię smooth start |
| `src/assist_dynamics.c` / `inc/assist_dynamics.h` | nowe `assist_dynamics_reset()` (naprawa wycieku stanu) |
| `src/CAN_Display.c` | diagnostyka 0x6029 v5 → **v6**, 55 B → 71 B, blok jednostkowy |
| `src/fw112_diag.c` | komentarz: `FW112_FLAG2_PU_CLAMPED` nazywa teraz sufit `max_iq_pct`, nie sufit P/U |
| `inc/assist_modes.h` | D11: jawny opis różnicy między `human_power_w` (surowa próbka, skacze z każdym naciskiem nogi) a `assist_basis_power_w` (to, z czego naprawdę liczone jest wspomaganie) — bez zmiany zachowania, żeby log dało się czytać |
| `protocol/evistdrive_config_schema.yaml` | D12: `torque: 5`, `power_curve: 6`, `max: 6`; `emtb_custom` oznaczony jako nieaktywny; dodane `torque_assist_factor`, `curve_exponent_x10`, `curve_exponent_high_x10`, `assist_torque_full_scale_kg`, `crank_length_mm`; poprawiony opis kalibracji |

### Canable (`bafang_canable_pro`, gałąź `evistdrive`)

| Plik | Zmiana |
|---|---|
| `bafang-parser.js` | tuning blob v8 (odczyt @24/@26 z zabezpieczeniem przed zerami); diagnostyka 0x6029 v6 (8 nowych pól); telemetria 0x6025: `gain_calibration`, `legacy_calibration_dropped` |
| `canbus.js` | serializacja blobu v8 z negocjacją wersji w dół i zaciskaniem zakresów |
| `ui/js/evistdrive/dynamics.js` | dwa nowe pola globalne + bramka `minTuningSchema: 8` + wartości domyślne |
| `ui/js/evistdrive/profiles.js` | podgląd używa konfigurowalnej pełnej skali i długości korby (był na sztywno 60 kg / 165 mm); poprawiony **nieprawdziwy** opis Reference voltage |
| `ui/js/evistdrive/system.js` | 8 nowych odczytów w diagnostyce (łańcuch wspomagania) |
| `ui/js/evistdrive/torque.js` | komunikat o odrzuconej starej kalibracji / o kalibracji wzmocnienia |
| `ui/index.html` | nowa karta „Torque axis and crank", blok „assist chain (live)", miejsce na komunikat kalibracji |
| `tests/fw129_tuning_v8_roundtrip.js` | nowy test (wpięty w `npm test`) |

---

## 3. CANABLE — POLA, OFFSETY, WERSJE, MIGRACJA (§40D)

### Tuning blob (0x6023 read / 0x6024 write / 0x6022 save)

| Offset | v7 | v8 | Zakres | Domyślnie |
|---|---|---|---|---|
| 0..23 | bez zmian | bez zmian | — | — |
| 24..25 | reserved = 0 | `assist_torque_full_scale_centikg` LE | 2000..12000 (20,0–120,0 kg) | 6000 |
| 26..27 | reserved = 0 | `crank_length_mm` LE | 150..190 | 165 |
| 28..29 | reserved = 0 | reserved = 0 | — | — |
| 30..31 | CRC16 | CRC16 | — | — |

**Długość 32 B bez zmian. Bank nadal dokładnie 255 B.** Liczba ramek bez zmian.

**Migracja:** blob v2..v7 → obie nowe wartości domyślne (czyli dokładnie to, jak firmware
zachowywało się przed kartą — migracja nie zmienia odczucia jazdy). Zero w bajtach 24..27
blobu v8 też jest traktowane jako „nieustawione" → wartość domyślna, po obu stronach
(firmware i Canable), bo „0 kg pełnej skali" i „0 mm korby" nie są ustawieniami.

**Stary Canable + nowe firmware:** stary tool negocjuje wersję w dół do v7 i nigdy nie dotyka
tych bajtów. **Nowy Canable + stare firmware:** negocjacja w dół, oba pola pokazane jako
niedostępne (`minTuningSchema: 8`).

### Diagnostyka 0x6029: v5 (55 B) → v6 (71 B)

Nowe pola, wszystkie **na żywo** (nie peak-hold): `assist_load_kg`, `assist_torque_x160`,
`iq_launch_request`, `iq_normal_request`, `launch_blend_permille`, `iq_pre_limit`,
`requested_motor_power_w`, `u_abs_live`. Starsze wersje nadal parsowane bez zmian.

### Widoczność per tryb w UI

Bez zmian — audyt wykazał, że UI **już** obsługiwało poprawnie wszystkie pięć aktywnych trybów
i nie oferowało ani `reserved_0`, ani `emtb_custom`. Doszły tylko dwa pola globalne (w karcie
Dynamics, nie per poziom) i poprawka tekstu Reference voltage.

---

## 4. TABELA BEFORE / AFTER (punkt 8 polecenia właściciela)

Poziom 3, ustawienia domyślne, pakiet 42 V, wypełnienie proporcjonalne do kadencji, **boost
startowy wyłączony** (ma własną regresję; przy włączonym zaciemniałby porównanie).
BEFORE = stare równania odtworzone dosłownie w `old_reference_iq()` w harnessie, razem ze
starym sufitem P/U — więc porównanie jest z tym, co firmware naprawdę robiło.

### Power Linear (Progressive i Curve dają w tych punktach to samo)

| kg | rpm | u_abs | rowerzysta [W] | silnik [W] | AFTER [Iq] | BEFORE [Iq] | AFTER/BEFORE |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 5 | 40 | 682 | 33 | 108 | 81 | 38 | **2,13×** |
| 5 | 100 | 1706 | 84 | 271 | 81 | 38 | 2,13× |
| 10 | 40 | 682 | 67 | 217 | 163 | 75 | **2,17×** |
| 10 | 60 | 1024 | 101 | 326 | 163 | 75 | 2,17× |
| 10 | 80 | 1365 | 135 | 434 | 163 | 75 | 2,17× |
| 10 | 100 | 1706 | 169 | 543 | 163 | 75 | 2,17× |
| 20 | 60 | 1024 | 203 | 649 | 325 | 150 | **2,17×** |
| 30 | 60 | 1024 | 304 | 975 | 488 | 224 | 2,18× |
| 40 | 60 | 1024 | 406 | 1300 | 652 | 299 | 2,18× |
| 60 | 40 | 682 | 406 | 1301 | 700 | 448 | 1,56× (limit fazowy) |

Dwie rzeczy do zauważenia:

1. **Iq nie zależy od kadencji** (163 przy 40, 60, 80 i 100 rpm dla 10 kg). Moc rowerzysty
   rośnie 4×, żądana moc silnika rośnie 4×, a prąd zostaje ten sam — bo wypełnienie też
   wzrosło 4×. To jest poprawka §31 z audytu, potwierdzona liczbowo przez działający kod.
2. **`launchIq` = `normIq` w każdym wierszu** — przenikanie startowe jest w normalnej jeździe
   przezroczyste.
3. Od 30 kg / 100 rpm zaczyna działać twardy sufit 1500 W i `AFTER` spada poniżej proporcji —
   to limiter, nie charakterystyka.

### eMTB (poziom 3: parameter 140, based_on_power, ref. 36 V)

| kg | rpm | rowerzysta [W] | silnik [W] | wspomaganie | AFTER [Iq] | BEFORE [Iq] |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 60 | 101 | 22 | 22% | 11 | 15 |
| 20 | 60 | 203 | 90 | 44% | 45 | 51 |
| 30 | 60 | 304 | 204 | 67% | 102 | 109 |
| 40 | 60 | 406 | 364 | 90% | 182 | 188 |
| 60 | 60 | 609 | 819 | 134% | 410 | 410 |

### Torque (poziom 3: factor 120 = 1,0×)

| kg | rpm | rowerzysta [W] | silnik [W] | AFTER [Iq] | BEFORE [Iq] |
|---:|---:|---:|---:|---:|---:|
| 10 | 60 | 101 | 154 | 77 | 89 |
| 20 | 60 | 203 | 306 | 153 | 163 |
| 40 | 60 | 406 | 614 | 307 | 312 |
| 60 | 60 | 609 | 921 | 461 | 461 |

**Wniosek dla eMTB/Torque:** siła praktycznie bez zmian (−3…−10% przy niskich naciskach, zero
różnicy przy wysokich). Stary kod był w tych punktach i tak zdominowany przez sufit P/U, który
liczył tę samą fizykę. Zmieniło się to, że **wejście nie zależy już od kalibracji czujnika**
i że zniknęła błędna droga `x160 → % limitu fazowego`, która wiązała tylko przy niskim
wypełnieniu (start) — czyli dokładnie tam, gdzie było najtrudniej to zauważyć.

**Pełna tabela** (5 trybów × 6 nacisków × 4 kadencje, z kolumnami launch/normal/blend):
`tests/host/fw129_unit_domain_host.c` uruchomiony bez `--quiet`.

---

## 5. WYNIKI TESTÓW (§40E)

### Host, firmware (`tests/host/run-host-tests.ps1`)

| Test karty | Zakres | Wynik |
|---|---|---|
| T1 (§29) | normalizacja torque przy 60,0 i 40,0 kg pełnej skali | **PASS** |
| T2 (§30) | **niezmienniczość kalibracji** — 2 czujniki × 4 siły × 5 trybów | **PASS** |
| T3 (§31, poprawione) | Power Linear: 135,5 W przy 10 kg/80 rpm; 100/200/320% → 135/271/434 W; przy 40 rpm połowa mocy i **to samo Iq** | **PASS** |
| T4 (§32) | Progressive/Curve: monotoniczność, okno min/max, niezależność od span | **PASS** |
| T5 (§33) | eMTB: 4 skale × 4 parametry × 5 kadencji × 7 nacisków; przesunięcie kolana | **PASS** |
| T6 (§34) | Torque: 4 współczynniki × 6 nacisków; oś torque wolna od kadencji; stała moc | **PASS** |
| T7 (§35) | przenikanie przy niskim wypełnieniu: 9 punktów `u_abs` × 3 tryby | **PASS** |
| T8 | długość korby proporcjonalnie w mocy rowerzysty | **PASS** |
| T9 | oba nowe ustawienia nie ruszają odczytu kg ani telemetrii | **PASS** |
| T10 (§36) | **macierz modyfikatorów × wszystkie 5 trybów**: boost OFF/ON i jego wygasanie, assist without rotation (włącza się powyżej progu kg, milczy poniżej), filtr mocy przezroczysty przy pierwszym żądaniu, `max_iq_pct` i `max_motor_power_w` nadal tną, poziom 0 = brak wspomagania, kompensacja kadencji = jedność na banku domyślnym | **PASS** |
| T11 (§23/§24) | tuning blob v8 od strony sterownika: 32 B, bank nadal 255 B, round trip, zaciskanie zakresów, zera → wartości domyślne, v2..v7 nadal akceptowane i migrowane, v7 nie gubi okna RUN przy skoku wersji, nieznana wersja odrzucona | **PASS** |

### Regresja pełna

Cały pakiet host uruchomiony przed i po zmianie. **Wyniki na poziomie pakietów są
IDENTYCZNE** (`diff` czysty). Jedyny pakiet czerwony to `rolling_no_assist_diag_host`
(514 błędów) — czerwony tak samo w bazie `13b5c6a`, sprawa niezwiązana z tą kartą.

Obejmuje to regresję: Walk Assist, throttle, hamulec, cofanie, FW-109 (kierunek), FW-112
(rearm/sesja/estymator RUN), FW-117, FW-119, FW-125..FW-128 (tor prądowy).

### Canable (`npm test`)

16/16 **PASS**, w tym nowy `fw129_tuning_v8_roundtrip.js`. `eslint` czysty.

### Kompilacja

Wszystkie zmienione pliki przechodzą `arm-none-eabi-gcc -fsyntax-only` z produkcyjnymi
flagami (Cortex-M4, `-DGD32F30X_HD -DCAN_DIAGNOSTICS_ENABLE=1`). **Firmware NIE zostało
zlinkowane ani zbudowane** — zgodnie z zasadą „nie budować bez polecenia".

---

## 6. TRZY BŁĘDY ZNALEZIONE PODCZAS WDROŻENIA (nie były w audycie)

Warto je zapisać, bo wszystkie trzy pojawiły się dopiero wtedy, gdy moc stała się
**żądaniem**, a nie sufitem.

1. **Filtr mocy jako generator żądania.** Opadający filtr utrzymywał pełne żądanie przez cały
   czas opadania po tym, jak nacisk zniknął — zmierzone 127 jednostek prądu przy 0,04 kg
   nacisku, po cofnięciu. Naprawa: filtr kasowany na tych samych krawędziach sesji, na których
   kasowana jest odbudowa estymatora RUN, plus twarda zasada „zero nacisku = zero prądu".
2. **Filtr mocy jako opóźniacz startu wspomagania.** Narastanie od zera dokładało pełne
   `power_rise_filter_ms` przed każdym ponownym złapaniem wspomagania — 46 jednostek zamiast
   211, 75 ms po tym, jak rowerzysta znów nacisnął. Naprawa: żądanie większe niż 2× stan
   filtru **zaszczepia** filtr zamiast do niego narastać. Filtr wygładza zmiany w trakcie
   wspomagania, nie stoi przed jego pojawieniem się.
3. **Wyciek stanu w `ride_control_init()`.** Rampa prądu (`assist_dynamics`) to statyczne
   zmienne modułu, których nic nie zerowało — funkcja, której zadaniem jest przywrócenie toru
   jazdy do znanego stanu, zostawiała za sobą referencję prądu. W produkcji utajone (init leci
   raz przy starcie), w testach natychmiast widoczne. Naprawa: `assist_dynamics_reset()`.

   **KOREKTA (FW-129B).** Ten punkt twierdził też, że wyciekała obwiednia smooth start. To
   nieprawda: `assist_modes_reset()` wołało `assist_start_reset()` od zawsze, więc smooth
   start był czyszczony i przed tą kartą. Pełny audyt wszystkich 16 pól stanu runtime — z
   sześcioma realnymi wyciekami, w tym jednym zasilającym decyzję Extended Boost — jest w
   [FW-129B_STATE_HYGIENE_REPORT_PL.md](FW-129B_STATE_HYGIENE_REPORT_PL.md) sekcja 3.

   **KOREKTA (FW-129B) do punktów 1 i 2 powyżej.** Obie poprawki opisane wyżej (zasada
   zaszczepiania filtru, kasowanie filtru na krawędziach sesji) okazały się niewystarczające:
   łapały tylko przypadki skokowe. Filtr mocy został ostatecznie **usunięty z toru żądania** —
   szczegóły i pomiary w raporcie FW-129B.

---

## 7. CO ZOSTAŁO OTWARTE

1. **`U_LAUNCH = 1024` to hipoteza.** Jeden przejazd z odczytem 0x6029 v6 (`u_abs` i kadencja
   są teraz obok siebie) ustali właściwą wartość. Do czasu pomiaru błąd tej stałej skaluje
   wyłącznie start poniżej 60 rpm.
2. **eMTB jest ~7× słabszy od Power Linear na tym samym numerze poziomu** (45 vs 325 jednostek
   przy 20 kg/60 rpm). Jednostki są poprawne; to różnica skali parametrów. Materiał na osobną
   kartę strojenia — zgodnie z punktem 6 polecenia nie ruszam tego tutaj.
3. **Stara kalibracja czujnika jest odrzucana** przy pierwszym starcie po aktualizacji
   (rekord v1 nie da się uczciwie przeliczyć). Canable pokazuje komunikat „przekalibruj".
   Do czasu ponownej kalibracji obowiązuje charakterystyka fabryczna.
4. **Migracja `support_ratio_pct` (wariant B z audytu 7.1) NIE została wdrożona** — nie było
   jej w Twojej odpowiedzi, a punkt 6 mówi „nie stroimy jeszcze wartości". Skutek: **pierwszy
   przejazd po wgraniu będzie w trybach Power ponad dwa razy mocniejszy.** Zacznij od poziomu 1
   w bezpiecznym terenie.
5. **Nic nie zostało zacommitowane** i firmware nie zostało zbudowane.
