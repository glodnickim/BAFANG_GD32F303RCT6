# FW-129B RESULT — **FIX REQUIRED AND COMPLETED, ALL THREE DEFECTS CLOSED**

Data: 2026-08-28. Baza audytu: drzewo robocze z ukończonym FW-129 (HEAD = `13b5c6a`).
Poprzedni etap: [FW-129_IMPLEMENTATION_REPORT_PL.md](FW-129_IMPLEMENTATION_REPORT_PL.md).

**Werdykt: wariant B.** Audyt na bieżącym stanie wykazał, że FW-129 zamknęło problem 3 tylko
częściowo, a problemów 1 i 2 **nie zamknęło wcale** — poprawki z FW-129 (zasada zaszczepiania
filtru, kasowanie filtru na krawędziach sesji) łapały tylko przypadki skokowe. Reprodukcja
liczbowa poniżej.

---

## 0. STRESZCZENIE PROSTYM JĘZYKIEM

Filtr mocy okazał się przyczyną, a nie objawem. Zmierzone na uruchomionym firmware:

**Kiedy rowerzysta puszczał pedał**, filtr dalej zamawiał prąd, którego nikt nie żądał:

| co rowerzysta w tej chwili żądał | co filtr zamawiał | ile razy więcej |
|---|---|---|
| 113 W | 313 W | **2,8×** |
| 10 W | 56 W | **5,6×** |
| 0 W | 2 W | (dopiero tu łapała bramka) |

**Kiedy rowerzysta naciskał ponownie**, ten sam filtr zaniżał żądanie: 75 ms po naciśnięciu
dawał **43 jednostki prądu zamiast 228**, na które nacisk zasługiwał. Czyli był drugim,
ukrytym startem miękkim przed rampą prądu, która i tak już to robi.

To nie da się naprawić strojeniem: każde opóźnienie postawione na drodze żądania robi jedno
i drugie — na opadaniu przeżywa rowerzystę, na narastaniu go okrada. **Filtr mocy został więc
usunięty z toru żądania.**

Czy rower straci na tym wygładzanie? Nie — obie prace filtru robią dziś inne mechanizmy,
w lepszej domenie:
* **przechodzenie przez martwe punkty** robi wygładzanie RUN, uśredniane po **kącie korby**
  (FW-085/112.4) — a to z definicji nie może przeżyć zaprzestania pedałowania;
* **narastanie i opadanie prądu** robią rampy Iq ustawiane per poziom — to jest to pokrętło,
  o którym instrukcja mówi rowerzyście, że steruje budowaniem mocy.

Trzeci problem — resztki stanu po poprzedniej jeździe — okazał się realny i szerszy niż to,
co FW-129 naprawiło. Najważniejsza znaleziona resztka: **prąd ostatniego pedałowania**, z
którego Extended Boost bierze całą swoją siłę. Zostawiony, mógł podać boostowi prąd z
poprzedniej jazdy.

---

## 1. ISSUE 1 — GHOST ASSIST

### Root cause
`assist_modes.c`, `finish_power_request()`: filtr rise/fall stał w torze **żądania**. Na
opadaniu filtr z definicji jest **powyżej** bieżącego wejścia, więc publikował moc, której
rowerzysta nie żądał.

### Stan na wejściu do FW-129B: **STILL PRESENT**
FW-129 dodało dwa zabezpieczenia, oba niewystarczające:
* twarda bramka „raw == 0 i raw_launch == 0 → Iq = 0" — łapała dopiero **dokładne** zero, do
  którego tor dochodził po ~12 000 taktów (3 s);
* kasowanie filtru na krawędziach sesji — łapało cofnięcie/rearm, ale nie zwykłe puszczenie
  pedału przy dalszym pedałowaniu do przodu.

Pomiar (`assist_modes.c` + `torque_input.c`, prawdziwy łańcuch, po 4000 taktów nacisku 310
native, potem zero):

```
t= 2000  rider  113 W  →  filtr  313 W  →  Iq 157
t= 6000  rider   10 W  →  filtr   56 W  →  Iq  28
t=11999  rider    0 W  →  filtr    2 W  →  Iq   0
```

### Producer → consumer (przed poprawką)
```
torque ADC → native → deadband → filtr 35 ms → okno RUN 180° + filtr asym
  → load_centikg → [boost] → tryb → P_req
  → FILTR MOCY (stan z przeszłości)   ← TU żądanie przestawało pochodzić z wejść
  → konwersja P→Iq → sufity → Iq_requested
```

### Poprawka
Filtr mocy usunięty z toru żądania w całości: `power_filter_state_t`, `filter_one_power()`,
`filter_motor_power()`, `stop_power_filter()`, `clamp_power_filter_ms()` i
`assist_modes_mark_power_filter_cold()` usunięte. `assist_modes` **nie przenosi już żadnego
stanu żądania między taktami** — moduł bez stanu nie może go wyciec.

Twarda bramka „brak żądania → brak prądu" zostawiona: jest teraz trywialnie prawdziwa, ale
jawna i tania.

### Test
**T1** (ghost assist po cofnięciu, 4000 taktów po wypłukaniu toru czujnika) — **PASS**.
**T2** (własność sprawdzana na każdym z 12 000 taktów: `motor_power_w <= raw_motor_power_w`,
czyli żądanie nigdy nie przekracza tego, co uzasadniają bieżące wejścia) — **PASS**, najgorsza
nadwyżka 0 W. To jest ten test, który złapał oryginał.
**T8** (cofnięcie korby nie odzyskuje starego dodatniego stanu) — **PASS**.

---

## 2. ISSUE 2 — RE-ENTRY LATENCY

### Root cause
Ten sam filtr, druga strona: na narastaniu jest **poniżej** bieżącego wejścia, więc zaniżał
sam TARGET, po czym rampa Iq slewowała go po raz drugi. Dokładnie konstrukcja, której karta
zabrania („stary filter state → zaniża sam TARGET → a później drugi slew ponownie go
ogranicza").

### Stan na wejściu do FW-129B: **STILL PRESENT**
Zasada zaszczepiania z FW-129 (`raw > 2× stan filtru → zaszczep`) łapała **skok**, ale nie
stopniowe narastanie — a estymator RUN narasta stopniowo z definicji. Pomiar: 75 ms po
ponownym naciśnięciu **60 z 241** jednostek prądu (25%).

### Calculated target BEFORE / AFTER
| | 75 ms po ponownym naciśnięciu | ustalone |
|---|---|---|
| przed FW-129B | 60 (25% ustalonego) | 241 |
| po FW-129B | 30 z 73 przy zimnym starcie, 120 ustalone (przy nowych domyślnych) | — |

Po poprawce różnica 75 ms **nie pochodzi już z filtru mocy** — pochodzi z estymatora RUN,
który przy **zimnym starcie** dostaje `torque_input_seed_run()`, a przy wznowieniu w trakcie
jazdy nie. To jest własność FW-090/FW-091, świadomie zostawiona (ponowne zaszczepianie na
narastaniu przywracało pulsowanie „lewa noga / prawa noga"), i **nie jest przedmiotem tej
karty**. Test wypisuje tę różnicę zamiast ją asertować.

### Jawny downstream shaping — zachowany
Rampy Iq per poziom (`assist_dynamics`) są jedynym jawnym właścicielem szybkości narastania
prądu. **T4** sprawdza, że na pierwszym takcie z dodatnim żądaniem setpoint silnika jest
**poniżej** targetu (rampa działa) i że dochodzi do niego z czasem.

### Czy 75 ms ukrytego opóźnienia zostało usunięte
**TAK.** Dowód na właściwym poziomie — **T3**: `assist_modes_calculate()` z identycznym
wejściem daje identyczny wynik po 500 taktach zupełnie innego żądania i 500 taktach zerowego.
Moduł nie przenosi żadnej historii. **PASS** (Iq i moc żądana bit w bit równe).

---

## 3. ISSUE 3 — INIT STATE LEAK

### Tabela stanu runtime

| pole | właściciel | przed init (po jeździe) | po init | ma być zerowane | wynik |
|---|---|---|---|---|---|
| `assist_hold_ticks` | ride_control | niezerowe | 0 | TAK | było OK |
| `preload_active` / `preload_ticks` | ride_control | możliwe niezerowe | 0 | TAK | było OK |
| `walk_was_active` | ride_control | możliwe true | false | TAK | było OK |
| **`last_pedal_iq_while_pedaling`** | ride_control | **prąd z poprzedniej jazdy** | **0** | **TAK** | **NAPRAWIONE — jedyny wyciek zasilający DECYZJĘ (Extended Boost)** |
| `debug_flags` | ride_control | flagi z poprzedniej jazdy | 0 | TAK | **NAPRAWIONE** (diagnostyczne) |
| `ride_diag_reason` / `_permission_bits` / `_flags2` | ride_control | j.w. | 0 | TAK | **NAPRAWIONE** (diagnostyczne) |
| `arm_snapshot` / `arm_pending` | ride_control | migawka z poprzedniej jazdy | 0 | TAK | **NAPRAWIONE** (diagnostyczne) |
| `gate_snapshot` | ride_control | j.w. | 0 | TAK | **NAPRAWIONE** (diagnostyczne) |
| `iq_reference_q` / `profile_release_step_q` | assist_dynamics | rampa z poprzedniej jazdy | 0 | TAK | naprawione w FW-129 |
| `smooth_start_state` / `last_smooth_output` | assist_start | obwiednia | 0 | TAK | **było OK** — `assist_modes_reset()` wołało `assist_start_reset()` od zawsze. **Korekta raportu FW-129, który twierdził inaczej.** |
| `power_filter_state` | assist_modes | stan filtru | — | — | **pole zniknęło razem z filtrem** |
| `last_output` | assist_modes | migawka | wyczyszczona | TAK | było OK |
| stan estymatora RUN / automat odbudowy | torque_input | recovery mógł przeżyć | anulowany | TAK | **NAPRAWIONE** — `torque_input_cancel_rolling_rearm()` w init |
| `startup_boost_curve` / `cached_*` | assist_start | tabela | bez zmian | **NIE** | to **cache** — deterministyczna funkcja konfiguracji, przebudowywana gdy klucz się zmieni |
| zero / charakterystyka czujnika | torque_input | — | bez zmian | **NIE** | **STATIC CALIBRATION** — nie wolno |
| banki profili, tuning blob | assist_modes / tuning_config | — | bez zmian | **NIE** | **PERSISTENT CONFIGURATION** — nie wolno |

### Poprawka
`ride_control_init()` jest teraz **jednym miejscem**, które wylicza cały stan runtime, jaki
posiada, z komentarzem tłumaczącym, dlaczego `torque_input_init()` tam **nie ma**
(kalibracja = static calibration), a `torque_input_cancel_rolling_rearm()` **jest**
(odbudowa rearmu = ride state, którego ride_control jest właścicielem).

### Test
**T5** (zabrudzić wszystko → init → porównanie pole po polu z zimnym stanem) — **PASS**.
Porównanie **pole po polu, nie `memcmp`**: to struktury z paddingiem, a bajty paddingu po
kopiowaniu struktury są nieokreślone — `memcmp` zgłasza różnice, których nie ma, i ukrywa te,
które są.
**T6** (ten sam ciąg wejść po zimnym starcie i po init po hałaśliwej jeździe — 400 taktów,
trzy sygnały) — **PASS**, zero rozbieżności. To jest właściwy dowód na wyciek stanu.

---

## 4. GRAF WŁAŚCICIELSTWA Iq (po zmianie)

```
torque ADC
  ↓  torque_input.c      ← JEDYNY właściciel: zero, kalibracja, deadband, filtr 35 ms,
  ↓                         okno RUN po kącie korby, automat odbudowy rearmu
load_centikg
  ↓  assist_start.c      ← startup boost, na fizycznym nacisku
  ↓  assist_modes.c      ← JEDYNY producent Iq_requested. BEZSTANOWY.
  ↓                         tryb → P_req → jedna konwersja P/(u·V) z przenikaniem startowym
  ↓                         → max_iq_pct
Iq_requested
  ↓  ride_control.c      ← bramka zgody (sesja/kierunek), podłoga latch, Extended Boost,
  ↓                         throttle, limity wspólne
  ↓  assist_dynamics.c   ← JEDYNY właściciel slew: rampy Iq, release, hard cut
  ↓  assist_start.c      ← smooth start jako OBWIEDNIA na gotowym Iq
Iq_ref → motor_core → FOC
```

**Odpowiedzi na pytania z karty:**
1. **Kto produkuje finalne `Iq_requested`?** `assist_modes_calculate()` — jedyne miejsce.
2. **Ile miejsc zapisuje `Iq_requested`?** Jedno (`output->iq_request`). Dalej `ride_control`
   pracuje na własnym `iq_target`, nie nadpisuje wyniku trybu.
3. **Czy filtr może ominąć bramkę ważności?** **Nie — filtru nie ma.** Moduł jest bezstanowy.
4. **Czy launch może produkować żądanie bez zgody?** Nie: kotwica startowa to ten sam wzór
   z inną referencją wypełnienia; `ride_control` zeruje target bez zgody (T12).
5. **Czy smooth start może przeżyć reset?** Nie — `assist_start_reset()` z dwóch miejsc.
6. **Czy przełączenie trybu dziedziczy stan filtra?** Nie ma czego dziedziczyć (T7: 20 par
   przejść między 5 trybami, każde sprawdzone).

---

## 5. WŁAŚCICIELSTWO PRZY PRZEŁĄCZENIU TRYBU

**T7**, wszystkie 20 uporządkowanych par z 5 aktywnych trybów. Dla każdej: tryb źródłowy
rozpędzony przez 200 taktów, żądanie zdjęte, przełączenie na tryb docelowy → **0**; potem
żądanie wraca → **dokładnie** wartość trybu docelowego z zimnego startu. **PASS.**

---

## 6. NIEZMIENNICZOŚĆ KALIBRACJI

**T9** — punkty fabryczne (740 / 146↔6,00 kg / 1580↔84,00 kg), span 1139, obie konwersje,
źródło kalibracji. **PASS**, bez zmian. Testy kalibracyjne FW-129 (T2 w
`fw129_unit_domain_host.c`) przechodzą bez zmian.

---

## 7. REGRESJA GOLDEN FW-129

**T10** — Power Linear na 9 punktach (3 naciski × 3 kadencje) musi nadal dawać ≥ 2× tego, co
dawała stara, niezakotwiczona formuła. **PASS.**

Pełna macierz 5 trybów × 6 nacisków × 4 kadencji (`fw129_unit_domain_host.c`) porównana
przed/po. **Wartości w stanie ustalonym: bez zmian od usunięcia filtru.** eMTB przy 10 kg /
60 rpm = 11 jednostek przed i po; Power Linear zmieniło się **wyłącznie** o połowienie
domyślnych (patrz sekcja 9), a stosunek AFTER/BEFORE pozostał ~2,13–2,18.

---

## 8. PEŁNA REGRESJA HOST

`tests/host/run-host-tests.ps1`, porównanie z bazą `13b5c6a`:

```
diff wynikow na poziomie pakietow:
> FW-129 unit domain: ALL CHECKS PASSED
> FW-129B state hygiene: ALL CHECKS PASSED
```

**Zero innych różnic.** Jedyny czerwony pakiet to `rolling_no_assist_diag_host` (514 błędów) —
**ten sam known-baseline failure**, czerwony identycznie na `13b5c6a`. Nie jest ukrywany
i nie jest związany z tą kartą.

Canable: `npm test` **17/17 PASS**, `eslint` czysty.

---

## 9. ZMIANA POZA KARTĄ — POŁOWIENIE DOMYŚLNEGO WSPOMAGANIA

**Na wyraźne polecenie właściciela, wbrew regule „NO PARAMETER TUNING" tej karty.**
Odnotowane osobno, bo nie jest częścią FW-129B.

`support_ratio_pct` w obu bankach: **100/200/320/420/520 → 50/100/160/210/260.**

Uzasadnienie: FW-129 wykazało, że stare liczby nigdy nie były dostarczane (tor sprzed FW-129
mógł fizycznie dać ~46% ustawionej wartości), a naprawa uczyniła każdy poziom Power 2,17×
mocniejszym. Połowienie ustawia świeży sterownik z powrotem w granicach kilku procent
wspomagania, które rower naprawdę dawał.

**NIE połowione:** `emtb_parameter` i `torque_assist_factor` — FW-129 nie zmieniło siły eMTB
ani Torque, więc ich połowienie uczyniłoby te tryby realnie słabszymi, a nie równoważnymi.

Canable: `PROFILE_LEVEL_RATIOS` zaktualizowane, test `CB-018 placeholder defaults` (czyta
źródło firmware) **PASS**.

---

## 10. BUILD

| | FLASH | RAM | wynik |
|---|---|---|---|
| baza `13b5c6a` NORMAL | 103 448 B | 12 568 B | PASS |
| **po zmianie NORMAL (0.0454)** | **103 948 B** | **12 576 B** | **PASS** |
| delta NORMAL | **+500 B** (43,92% z 230 KB) | **+8 B** (25,57% z 48 KB) | |
| baza `13b5c6a` DIAG | 149 904 B | 46 344 B | PASS |
| **po zmianie DIAG (0.0455)** | **150 824 B** | **46 352 B** | **PASS** |
| delta DIAG | **+920 B** (63,65%) | **+8 B** (94,29% z 48 KB) | |

RAM w wariancie DIAG pozostaje ciasny (94,3%), tak jak przed zmianą — delta +8 B.
Ostrzeżeń kompilatora brak ponad znane, wcześniej istniejące.

Artefakty:
`0.0454_M820_BL820.bin` SHA256 `8F1B2543C026F991EC3C31B94B428A05C18163CD18CA9CA43D9AE1338409E6DC`
`0.0455_M820_BL820_DIAG.bin` SHA256 `0ED88061AB0FC0D21F3811012DA476D159487F6561DAFFF7C3F16EA7FA5CC2E3`

---

## 11. PLIKI ZMIENIONE (FW-129B)

**Firmware**
* `src/assist_modes.c` — filtr mocy usunięty z toru żądania (struktura, 4 funkcje, 4 stałe,
  3 wywołania `stop_power_filter`); `assist_modes_reset()` bez stanu filtru; domyślne
  `support_ratio_pct` połowione (sekcja 9)
* `inc/assist_modes.h` — `assist_modes_mark_power_filter_cold()` usunięte;
  `power_rise_filter_ms` / `power_fall_filter_ms` udokumentowane jako NIEAKTYWNE
* `src/ride_control.c` — `ride_control_init()` przepisane jako pełna, jawna lista stanu
  runtime; `last_pedal_iq_while_pedaling`, migawki diagnostyczne i anulowanie odbudowy rearmu
  dodane; dwa wywołania `assist_modes_mark_power_filter_cold()` usunięte; `<string.h>`
* `tests/host/fw129b_state_hygiene_host.c` — **nowy** (T1–T10, T12)
* `tests/host/run-host-tests.ps1` — nowy pakiet wpięty

**Canable**
* `ui/js/evistdrive/profiles.js` — oba filtry mocy oznaczone jako INACTIVE i wyszarzone,
  z wyjaśnieniem dlaczego i czym są zastąpione; `PROFILE_LEVEL_RATIOS` połowione

**Dokumentacja**
* `documentation/FW-129B_STATE_HYGIENE_REPORT_PL.md` — ten plik

---

## 11a. COMMIT

Firmware: jeden commit, temat „FW-129 + FW-129B: assist comes from physical rider power, and from
nothing else". Jeden commit obejmuje FW-129 i FW-129B, ponieważ pośredni stan FW-129 nigdy nie
istniał jako zwalidowany artefakt: jego filtr mocy był defektem zamkniętym przez FW-129B zanim
cokolwiek zostało zbudowane czy wgrane. Commit celowo **nie** obejmuje `build_firmware.ps1`
i usunięcia `src.zip` — to zastane zmiany w drzewie roboczym, nie należące do tej pracy.

**Canable: NIE ZACOMMITOWANE, świadomie.** Repozytorium `bafang_canable_pro` ma w drzewie
roboczym sporo cudzej, niezwiązanej i niedokończonej pracy (m.in. `server.js`, `sniffer.js`,
`tab-sniffer.js`, `style.css`, `websocket.js` oraz zmiany w `ui/index.html` dotyczące snifera).
`ui/index.html` zawiera je wymieszane z moimi, więc commit tego pliku wciągnąłby cudze zmiany.
Zmiany Canable są kompletne i przetestowane (`npm test` 17/17, `eslint` czysty) i czekają
w drzewie roboczym na gałęzi `evistdrive`.

---

## 12. HARDWARE

**NOT REQUIRED / NIE WYKONANO.** Zgodnie z kartą pierwszy test sprzętowy pozostaje
skonsolidowany z FW-129, PAS/kadencją, timebase prądu baterii i pomiarem `U_LAUNCH`.

---

## 13. WERDYKT KOŃCOWY

**B. FW-129B FIX REQUIRED AND COMPLETED — ALL THREE DEFECTS CLOSED.**

* ISSUE 1 — ghost assist: odtworzony liczbowo (2,8×–5,6× nadwyżki), przyczyna usunięta,
  własność „żądanie nigdy nie przekracza bieżących wejść" sprawdzana na 12 000 taktów.
* ISSUE 2 — re-entry latency: odtworzony (60 z 241 po 75 ms), przyczyna usunięta, moduł
  udowodniony jako bezstanowy; jawny slew downstream nienaruszony.
* ISSUE 3 — init state leak: pełny audyt 16 pól, 6 realnych wycieków naprawionych (w tym
  jeden zasilający decyzję), 1 błędna teza raportu FW-129 skorygowana.

Bez zmian: `U_LAUNCH`, PAS, prąd baterii, FOC/PI, kalibracja momentu, format EEPROM,
geometria banku (255 B) i tuning blobu (32 B), wartości wire trybów.
