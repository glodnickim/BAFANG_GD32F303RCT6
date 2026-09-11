# Tor wspomagania — audyt, diagnoza i architektura docelowa (V2)

```text
STATUS:      PROPOSAL — do niezależnego review i decyzji właściciela. Nie jest zatwierdzony.
AUTHORED_AT: 2026-09-10T22:07:24+02:00
AUTHOR:      Master sesji (Claude Opus 5)
BASELINE:    motor-controller-firmware HEAD 9b41070 + zastany WIP
             (M: main.c, rotor_angle.c/.h, fw131_rotor_angle_host.c, build_firmware.py,
              decode_canable_ride_log.py, M820_STOCK_REVERSE_KNOWLEDGE_BASE.md;
              ??: documentation/, sim/controller_lab/, TASK_HISTORY.md, RUN_CONTROLLER_LAB_WINDOWS.bat)
EVIDENCE:    CODE (odczyt drzewa) + zewnętrzny reverse stocku EVD-AP-RE-01. HW NOT_MEASURED.
REVIEW:      NOT_RUN — autor nie zatwierdza własnej architektury (RULE 1).
```

**Relacja do istniejących dokumentów.** `ASSIST_PIPELINE_MASTER_PLAN.md` (audyt etapu 1) i
`ASSIST_FIRMWARE_TARGET_CONTRACT.md` (roboczy kontrakt) pozostają w mocy jako **materiał
źródłowy i historia**. Ten dokument **zastępuje ich część TARGET STATE**: tamten kontrakt
projektował lepszy estimator wejściowy; poniższa diagnoza mówi, że problem nie leży
w estymatorze wejściowym. Rozdziały 1–9 master planu (mapa toru, macierz opcji, historia)
nie są tu powtarzane i nadal obowiązują.

---

# CZĘŚĆ I — DIAGNOZA

## 1. Wniosek w jednym zdaniu

**eVistDrive liczy wspomaganie jako bezpamięciową funkcję przefiltrowanego wejścia; stock
liczy je jako stan całkującego wyjścia korygowany regulatorem.** To jest różnica
architektoniczna, nie różnica nastaw — i to ona, a nie zbyt krótka rampa, produkuje
falowanie między naciśnięciami nóg.

Konsekwencja praktyczna: w naszej architekturze tętnienie da się zmniejszyć **wyłącznie**
mocniejszą filtracją wejścia, a każde takie wzmocnienie kupuje się opóźnieniem reakcji.
To jest ten sam kompromis, o który rozbiło się dziesięć kolejnych kart. Nie ma w nim
dobrego punktu — jest tylko wybór, co zaboleć bardziej.

## 2. Dlaczego dotychczasowe naprawy nie mogły zadziałać

Historia toru momentu to sekwencja filtrów wejściowych, z których każdy miał naprawić
skutek uboczny poprzedniego:

| Karta | Co dodała | Co się z tym stało |
|---|---|---|
| FW-033 | wolny estimator RUN (okno czasowe ms) | okno w ms = różny ułamek obrotu przy różnej kadencji |
| FW-085 | okno **kątowe** (bufor pierścieniowy, krok 3,75°) | wynik nadpisywany, patrz niżej |
| FW-090/091 | „fast attack" — reseed okna przy narastaniu | **wyłączone w dostawie** (`TORQUE_RUN_ATTACK_STEPS 0`) |
| FW-112 v2 | automat recovery (3 stany) na rearm | trzeci właściciel tej samej zmiennej |
| FW-112.4 | filtr czasowy niesymetryczny 120/250 ms | **nadpisuje bufor kątowy** |
| FW-141 | przeliczanie kaskady po elapsed ticks | naprawa taktowania, nie tętnienia |

**Bufor kątowy FW-085 jest martwy dla wyjścia.** `torque_input.c:815-823` — w każdym
z trzech stanów recovery `run_value_native` zostaje nadpisane przed publikacją: filtrem
czasowym (IDLE), sygnałem FAST (TRACK_FAST) albo reseedem per krok (WAIT_FRESH_LOAD).
Bufor liczy się dalej co krok korby i nie wpływa na nic. Suwak
`assist_torque_run_window_deg` działa wyłącznie jako przełącznik 0 / nie-0 — co potwierdziła
próba różnicowa 30° / 180° / 360° dająca identyczne przebiegi.

Osad tej metody jest mierzalny: **110 różnych kart FW-xxx**, 1 563 wystąpienia w `src/`+`inc/`,
`ride_control.c` w **50 % złożony z komentarza** będącego w większości archeologią
tłumaczącą łatkę przez odwołanie do poprzedniej łatki. **20 % `src/` (4 888 z 24 500 linii)**
to rusztowanie diagnostyczne zbudowane, żeby zrozumieć własne zachowanie firmware.

## 3. Co robi stock — dowód zewnętrzny

Źródło: `EVD-AP-RE-01`, dwa niezależne wykonania na dwóch obrazach.
Zapis: `M820_STOCK_REVERSE_KNOWLEDGE_BASE_EVISTDRIVE.md` §22 (FT, CONFIRMED),
`G532_ACCEL_REVERSE_ANALYSIS_PL.md` + `WORKING-20260910-…-G532-ACCEL.md` (G532).

**Oba obrazy zgodnie: dziedzina czasu, zero okien kątowych w torze momentu.** To rozstrzyga
pytanie, nad którym firmware pracował od FW-033: **stock nigdy nie uśredniał po kącie korby.**

Łańcuch FT (`§22.2`–`§22.6`), z adresami:

```text
ADC0/ADC1 dual + DMA        0x0800B520
   -> rozpakowanie, raw x8   0x08009EB0     -> 0x200003FE
   -> minus baseline 16 pr.  0x0800B860     -> 0x200004F4
   -> IIR czasowy (3y+x)/4   0x08009FA0     -> 0x20000676
   -> prawo PI: e = torque_filt - threshold 0x0800934C -> addend 0x20000674
   -> rampa niesymetryczna z hold          0x0800EB3C -> stan 0x2000036E
   -> FINAL = 0x2000036E + 0x20000674      0x0800EE66 -> 0x200004DE
```

Kluczowe jest ostatnie równanie. Wyjście stocku to **suma dwóch pamięci wyjściowych**:
wolno pełzającej rampy i całki regulatora PI. Nie jest to funkcja bieżącej próbki momentu.
Filtr IIR `(3y+x)/4` jest tylko odszumianiem wejścia — sam z siebie nie tłumi doliny
międzyskokowej i nie po to tam jest.

## 4. Arytmetyka różnicy — dlaczego nasza rampa nie ogranicza tętnienia

`ride_control.c:1236` (`ride_final_iq_slew_compute`):

```c
step_4k = iq_scale / ticks          // ticks = ramp_ms * 4
```

Krok rampy jest **ułamkiem pełnej skali na jednostkę czasu**. Przy `iq_scale = 700`
i `ramp_down = 500 ms`: krok = 700 / 2000 = 0,35 counts/tick = 1,4 counts/ms.
Przejście doliny o głębokości 50 counts zajmuje **~36 ms**. Dolina martwego punktu korby
trwa 110–150 ms. **Rampa przepuszcza ją niemal w całości** — nie dlatego, że jest źle
nastrojona, tylko dlatego, że jej krok skaluje się z pełną skalą, a nie z wielkością
zaburzenia.

Stock parametryzuje opadanie **krokami absolutnymi na wywołanie** (−1/−2/−3/−4, warunkowo,
w zależności od pasm prądu i porównań momentu; `§22.6`). Krok nie zależy od wielkości
wychylenia, więc małe wychylenie jest ograniczane tą samą bezwzględną szybkością co duże.

To jest cała różnica. Nasze cztery rampy są sprawne — po prostu wyrażone w jednostce,
w której nie mogą wykonać tego zadania.

> **Granica dowodu:** nie podaję czasów bezwzględnych rampy stocku. Kadencja wywołań
> warstwy `0x0800BE70` pozostaje `OPEN` (`§22.9`), więc kroki na wywołanie nie przeliczają
> się na sekundy. Twierdzenie strukturalne (krok absolutny vs krok proporcjonalny do pełnej
> skali) tej kadencji nie potrzebuje.

## 5. Dwóch „jedynych właścicieli" tej samej decyzji

Cytaty z nagłówków, dosłowne:

- `ride_session.h:121` — `bool latched; /* THE answer: may assist current flow this tick */`
- `pedal_assist_gate.h:3` — *„Single-purpose module: the SOLE owner of pedal permission."*

Oba moduły deklarują wyłączność na tę samą decyzję. `ride_control.c:646` łączy je przez
`bool latched = gate_open;` — nazwa `latched` zostaje użyta powtórnie dla **innego**
znaczenia niż w `ride_session`. Do tego dochodzą cztery dalsze pamięci mogące niezależnie
zmienić wynik: `assist_hold_ticks` (grace + podłoga prądu), `recovery_wait` (wymuszenie
zera), redundantny final gate (`:751`) i osobny blok `hard_cut` (`:848`).

Sześć miejsc odpowiada na jedno pytanie. Żadne nie jest zbędne z osobna — każde powstało,
bo poprzednie nie pokrywało jakiegoś przypadku.

## 6. Podłoga omija limit profilu

`ride_control.c:719-731`: `min_iq` liczone z `input->ride_core_iq_limit` (limit **globalny**)
i stosowane **po** `assist_modes_calculate()`, które nałożyło już cap **profilu**.
Zmierzone w audycie: limit trybu 7 counts, podłoga 25 % → wynik 175. Profil obiecuje
limit, którego tor nie dotrzymuje.

## 7. Drugi tor momentu w tym samym firmware

`main.c:2796` liczy legacy EMA `torque_cumulated >> MS.TQfilter` → `MS.torque_filtered`,
wstawiane do `rider_input_t` (`main.c:3056`). **Grep nie wykazał ani jednego konsumenta**
tego pola poza definicjami struktur. Obok: `MS.p_human` z mnożeniem `float` w torze
sterowania (`main.c:2847`) i pole `sample_tick`, którego własny komentarz mówi wprost
*„no consumer reads it"*.

To jest dokładnie ten drugi tor, którego w jednym firmware być nie powinno.

## 8. Tor testowy wykonywał inną gałąź kodu niż jazda — NAPRAWIONE (AP-0a)

Harness **nie ustawiał wcale** pola `rider_input_t.motor_voltage_utilization`; struktura jest
zero-inicjalizowana, więc do `assist_modes` szło zawsze 0. Wtedy:
- `launch_blend_permille(0)` (`assist_modes.c:626`) zwraca 0,
- `finish_power_request()` wydaje **czysty launch anchor** na stałej
  `ASSIST_LAUNCH_REFERENCE_U_ABS = 1024`,
- gałąź `iq_normal` (`power_to_phase_iq` z mierzonym `u_abs`) **nie wykonuje się nigdy**.

> **Korekta:** pierwsza wersja tego rozdziału wskazywała `replay_fw.c:126` (`ci.u_abs = 0`,
> pole `ride_control_input_t`). Błędnie — to pole zasila wyłącznie ogranicznik prądu baterii.
> Sprawdzone empirycznie: patch samego `ci.u_abs` nie zmienił wyniku ani o bit.

**Naprawione:** knob `REPLAY_U_ABS` / `--u-abs` ustawia oba pola; domyślna 0 zachowana, żeby
nie unieważnić `accepted_output_sha256` zarejestrowanych regresji (wynik domyślny
bajt-identyczny, `cmp` PASS).

**Wynik naprawy jest dobrą wiadomością.** Sweep `u_abs` 0→1920 zmienia **poziom** wspomagania
ponad dwukrotnie (F04: 162 → 333), ale **tętnienie względne pozostaje 195–203 %** w całym
zakresie. Usterka psuła wartości bezwzględne i **nie podważa wniosku o tętnieniu**.

Dowody i pełne tabele: [w1-replay-002-ap0a](../../integration/evidence/w1-replay-002-ap0a/README.md).

### 8b. Skala czujnika — sprawdzona i NIEISTOTNA dla tej pracy

> **KOREKTA 2026-09-10:** wcześniejsza wersja tego rozdziału żądała wczytania do harnessu
> kalibracji z roweru i stawiała to jako warunek etapu 0. **Zbędne.** Właściciel projektu
> nigdy czujnika nie kalibrował i używa krzywej domyślnej — a `replay_fw.c:78` startuje
> dokładnie na tej samej krzywej domyślnej (`span_native = 1139`, zero 740). **Obie strony
> już się zgadzają.** Etap 0b odwołany.

Pozostaje pytanie ogólniejsze, postawione przez właściciela: skoro przebudowujemy cały tor
tworzenia mocy, po co w ogóle zajmować się skalą czujnika.

**Odpowiedź: nie ma po co.** Dopóki ta sama krzywa działa po obu stronach — w harnessie
i na rowerze — „kilogram" jest jednostką **spójną**, choćby nie odpowiadał kilogramowi
fizycznemu. Nastawy dobrane na replay przenoszą się na rower bez poprawki. Rozbieżność
wobec prawdziwej masy jest wtedy **kwestią etykiety na ekranie, nie zachowania napędu**.

Jedyne, co nie byłoby kosmetyczne, to **saturacja**: gdyby oś zawyżała, rowerzysta biłby
w clamp pełnej skali (`TUNING_ASSIST_TORQUE_FULL_SCALE_CENTIKG_DEFAULT = 6000` ckg,
próg `delta = 1139` native) i szczyty byłyby ścinane, a doliny nie — co zmieniłoby
mierzony kształt tętnienia.

Sprawdzone na W1, 19 993 próbki: **0,43 % próbek przekracza próg saturacji** (86 z 19 993),
`p99 = 1710 mV` czyli delta 970 — poniżej 1139. Saturacja jest w tym nagraniu pomijalna.

**Skala czujnika zamknięta. Nic z roweru nie jest potrzebne.**

## 8c. Ile naprawdę wynosi sygnał wejściowy — pomiar na W1

Najmocniejszy dowód dla całej diagnozy leży w nagraniu, które już mamy. Rozkład momentu
z **rzeczywistej jazdy**, próbki z kadencją > 20 rpm (14 010 z 19 993), krzywa domyślna
— czyli dokładnie ta, którą jeździ ten rower:

| Percentyl | mV | delta native |
|---|---:|---:|
| p5 | 744 | 4 |
| p25 | 744 | 4 |
| p50 | 835 | 95 |
| p75 | 1058 | 318 |
| p90 | 1285 | 545 |
| p99 | 1776 | 1036 |

**Przez 35,9 % czasu pedałowania sygnał momentu leży poniżej martwej strefy**
(`TORQUE_ASSIST_DEADBAND_NATIVE = 10`), czyli `assist_delta` wynosi dokładnie **zero**.

To jest sedno problemu wyrażone liczbą. Architektura, która liczy
`Iq = f(bieżący przefiltrowany moment)`, próbuje wyprodukować równe wspomaganie z sygnału,
który **ponad jedną trzecią czasu nie istnieje**. Filtr wejściowy musi te dziury przykryć —
a im głębsze i dłuższe, tym mocniejszy musi być filtr i tym większe opóźnienie kupujemy.
Nie ma nastawy, która to rozwiązuje.

Dlatego `base` musi **trzymać przez te dziury na podstawie stanu jazdy**, a nie próbować
odfiltrować sygnał, którego w tych momentach po prostu nie ma.

> **Granica dowodu:** log ma 20 Hz, więc przy 90 rpm to ok. 7 próbek na cykl nogi —
> głębokość dolin jest wiarygodna, szczyty mogą być ścięte. Filtr `kadencja > 20 rpm`
> nie wyklucza w pełni toczenia z obracającą się korbą. Rząd wielkości (jedna trzecia
> czasu przy zerze) tych zastrzeżeń nie odwraca.

### 8d. Jak to wychodzi na wyjściu — baseline po naprawie harnessu

Żądanie Iq przez naprawiony tor (`u_abs = 1024`, bez pierwszych 20 % próbek rozruchu):

| Fragment | średnia | p5 | p50 | p95 | (p95−p5)/średnia |
|---|---:|---:|---:|---:|---:|
| F02 67 rpm | 209,4 | 117 | 200 | 338 | **106 %** |
| F04 90 rpm | 314,8 | 175 | 309 | 503 | **104 %** |

**Korekta wcześniejszego dowodu:** „tętnienie 2,5×" i `min = 0` pochodziły z wartości
brzegowych fragmentu. W jeździe ustalonej wspomaganie **nie spada do zera ani razu**
(0,0 % próbek). Uczciwa miara to rozstęp p5–p95 ≈ **±50 % wokół średniej**.

**I jest to rytm nóg, nie terenu.** Autokorelacja żądania Iq (F04, 90 rpm): minimum przy
lag 3–4, odbicie do **+0,46 przy lag 7 = 350 ms**, ujemna przy lag 9–10. Jeden nacisk nogi
przy 90 rpm trwa **333 ms**. Zgodność w granicach próbkowania 50 ms.

To zamyka lukę, która była najsłabszym punktem diagnozy: wcześniej jedynym powiązaniem
wyjścia z naciskami była korelacja `r = 0,33` wobec zmierzonego prądu baterii — za słaba,
żeby cokolwiek przesądzić.

## 9. Rozdzielczość PAS — sprzeczność pozorna, ROZSTRZYGNIĘTE

> **KOREKTA 2026-09-10:** pierwsza wersja tego rozdziału zgłaszała otwartą sprzeczność
> 64 vs 96 i zalecała pomiar na rowerze. **To było błędne** — odpowiedź istnieje w kodzie
> od FW-086, w `inc/config.h:683`, a autor jej nie znalazł, szukając w `main.c`
> i `inc/torque_input.h`. Pomiar O-2 zostaje odwołany.

`inc/config.h:678-699` rozstrzyga to arytmetycznie:

```text
MS.cadence = 10000 / ticks = C × N/96
```

Równa się prawdziwej kadencji `C` **wyłącznie** dla N = 96, a odczyt kadencji jest
prawidłowy w terenie — więc N = 96, a stała 10000 to dokładnie to założenie.

Sprzeczność jest pozorna i tego samego rodzaju co wcześniejsza 48 vs 96: **liczone są różne
rzeczy**. 24 pary biegunów dają 4×24 = 96 przejść kwadratury. Stock KB §11 opisuje 16 cykli
→ 64 przejścia, czyli czujnik o **16 parach biegunów** — inny sprzęt, nie ten rower.

Dla architektury poniżej i tak bez znaczenia: **nie ma w niej okien kątowych.** Liczba musi
pozostać poprawna wyłącznie dla kadencji, i jest.

---

# CZĘŚĆ II — ARCHITEKTURA DOCELOWA

## 10. Zasada

Tor ma **jedną pamięć wyjściową** i **jeden zestaw pamięci wejściowych o jawnym celu**.
Wygładzanie przenosimy z wejścia na wyjście. Wejście ma być szybkie i uczciwe; to wyjście
ma być spokojne.

```text
demand = base + correction

base        wolno pełzający stan w dziedzinie Iq, kroki ABSOLUTNE, niesymetryczne,
            wstrzymywany gdy pedałowanie potwierdzone  -> trzyma średnią między nogami
correction  człon proporcjonalny od bieżącego wysiłku   -> daje natychmiastową reakcję
```

Tętnienie tłumi `base` (bo jego krok nie zależy od wielkości wychylenia). Responsywność
daje `correction`. **Nie kupujemy jednego kosztem drugiego** — to jest cała zmiana wobec
obecnego toru i wobec roboczego kontraktu z 2026-09-08.

Struktura jest zbieżna ze stockiem (`rampa + PI`), ale **nie jest jego kopią**: nasza
wielkość sterowana to phase Iq w FOC, stock steruje inną wielkością. Przenosimy zasadę
dekompozycji, nie liczby.

## 11. Warstwy i właściciele

Trzynaście kroków logicznych, siedem modułów, **po jednym właścicielu na pamięć**.

| # | Moduł | Odpowiedzialność | Rytm |
|---|---|---|---|
| 1 | `torque_sensor` | ADC → zero → skalibrowana siła [centikg]; jeden filtr szumu; walidacja | 4 kHz |
| 2 | `pas` | kwadratura, kierunek, kadencja, liveness, stop; **właściciel direction inhibit** | zdarzenia |
| 3 | `assist_state` | **jedyny** właściciel „czy wolno pomagać": IDLE/STARTING/RUNNING/COASTING/STOPPING/INHIBITED | 4 kHz |
| 4 | `assist_demand` | `base + correction`; charakterystyka trybu; boosty | 4 kHz |
| 5 | `assist_limits` | podłoga → cap źródła → arbitraż → limity globalne, **w tej kolejności** | 4 kHz |
| 6 | `iq_trajectory` | **jedyny** writer `MS.i_q_setpoint`; start/run/release jako polityki jednego właściciela | 16 kHz |
| 7 | `motor` | FOC/PI/mostek/QZERO — **bez zmian** | 16 kHz |

Ochrona sprzętowa (nadprąd, wyłączenie mostka) pozostaje niezależna i nie czeka na tor
komfortu.

## 12. Co znika — jawnie

Nie „zostaje na później". Znika, bo utrzymywanie tego jest kosztem bez pokrycia:

| Usuwane | Powód |
|---|---|
| bufor pierścieniowy kątowy + `run_window_steps` + suwak stopni | martwy dla wyjścia; stock dowodzi, że dziedzina kątowa nie jest potrzebna |
| filtr niesymetryczny FW-112.4 (120/250 ms) | zastąpiony przez `base` w dziedzinie wyjścia |
| automat recovery (3 stany, `seed/cancel/track` API) | pochłonięty przez `assist_state` |
| `pedal_assist_gate` jako osobny moduł | scalony z `assist_state`; koniec dwóch „sole ownerów" |
| `assist_hold_ticks` + `min_iq` jako lek na tętnienie | tętnienie rozwiązane wyżej; podłoga zostaje **tylko** jeśli obroni się osobną potrzebą |
| legacy EMA `torque_cumulated>>TQfilter`, `MS.torque_filtered`, `MS.p_human` (float) | brak konsumentów; drugi tor w jednym firmware |
| `rider_input_t.sample_tick`, `.torque_filtered` | martwe pola |
| `smooth_start` jako osobny mnożnik przed rampą | staje się polityką STARTING w `iq_trajectory` |
| pola power rise/fall filters | nieaktywne od FW-129; deprecjacja formatu, nie reaktywacja |
| `TORQUE_RUN_ATTACK_*` | mechanizm wyłączony w dostawie, zastąpiony przez `correction` |

**Zachowane funkcjonalnie** (użytkownik ma na nich profile): pięć trybów i ich
charakterystyki, startup boost, extended boost, cadence compensation, Walk, service,
kalibracja czujnika, limity prędkości/temperatury/baterii, QZERO. Zachowane jest
**zachowanie**, nie implementacja — każdy z nich dostaje jawny punkt działania i jawny
limit w nowej kolejności.

## 13. Jednostki i granice konwersji

Zasada FW-129 zostaje i jest jedyną rzeczą z obecnego toru, którą uznaję za w pełni
spójną: **za `torque_sensor` nie istnieje żadna wartość native/mV.** Sterowanie pracuje
na skalibrowanej sile; rekalibracja czujnika nie zmienia kształtu żadnej charakterystyki.

Nowa granica do ustalenia: `base` i `correction` żyją w dziedzinie Iq. Konwersja
siła → moment → moc → Iq zostaje **jedna** (`power_to_phase_iq`), z jawną decyzją, czy
limit mocy oznacza moc elektryczną pobieraną, czy mechaniczną oddawaną. To pozostaje
`OPEN` i musi zostać rozstrzygnięte **przed** implementacją, nie w jej trakcie.

## 14. Stan jazdy — jedna tabela

| Stan | Wyjście | Wejście → wyjście |
|---|---|---|
| `IDLE` | 0 | świeże fakty startu → `STARTING` |
| `STARTING` | polityka startu `iq_trajectory` | `base` napełniony → `RUNNING` |
| `RUNNING` | `base + correction` | potwierdzony stop → `STOPPING`; dolina **nie** zmienia stanu |
| `STOPPING` | jedna trajektoria do zera | koniec → `COASTING`/`IDLE` |
| `COASTING` | 0 | kontekst do szybkiego restartu, **bez** dodatniej pamięci żądania |
| `INHIBITED` | 0, natychmiast | reakcja wg przyczyny; restart wymaga świeżych warunków |

Walk i service są **własnością źródła**, nie stanami pedałowania. Nie mnożymy maszyny
stanów przez tryby jazdy.

## 15. Limity — jeden invariant

```text
dla każdego źródła:  wszystkie bonusy i minima  ->  cap źródła  ->  arbitraż  ->  limity globalne
```

Każdy procent ma nazwaną podstawę. Test graniczny z audytu (profil 1 %, podłoga 25 %) staje
się **testem regresji**: w nowym torze musi dać wynik ≤ cap profilu.

---

# CZĘŚĆ III — JAK TO ZWERYFIKOWAĆ

## 16. Wykorzystujemy istniejący tor testowy

Nie budujemy nowego środowiska. `tools/run_replay.py` kompiluje **produkcyjne moduły C**
przeciw host stubs — nowe moduły wchodzą w to samo miejsce, bez zmiany metody.

**Warunek wstępny — naprawa harnessu, dwie pozycje.** Obie przed jakąkolwiek zmianą
algorytmu:

- **0a.** `u_abs = 0` zastąpione modelem utylizacji napięcia (choćby prostym, zależnym od
  prędkości i napięcia). Inaczej replay dalej wykonuje gałąź launch anchor (§8).
- ~~**0b.**~~ **ODWOŁANE** — czujnik nigdy nie był kalibrowany, rower i harness pracują na
  tej samej krzywej domyślnej (§8b). Nic do wczytania.
- **0c.** Nowy baseline W1 na naprawionym harnessie. Dopiero ta liczba tętnienia opisuje
  ten rower.

Bez 0a porównanie A/B jest bezwartościowe — mierzyłoby gałąź launch anchor zamiast jazdy.
**Nic z roweru nie jest do tego potrzebne.**

| Warstwa | Narzędzie (istniejące) | Co rozstrzyga |
|---|---|---|
| 1 | `tests/host/` (70 testów) | jednostki, brak regresji start/stop/limity |
| 2 | `tools/run_replay.py` + **W1** | tętnienie i responsywność na **rzeczywistym** nacisku |
| 3 | `tools/run_electrical_sil.py`, `run_level4.py` | zamknięta pętla z obciążeniem |
| 4 | rower | **końcowa walidacja, nie źródło strojenia** |

**70 testów hosta przypina obecne zachowanie.** Część z nich musi upaść — to jest
zamierzone. Każdy taki test jest **re-baseline'owany świadomą decyzją z uzasadnieniem**,
nigdy rozluźniany, żeby nowy kod przeszedł.

## 17. Kryteria akceptacji

- **Płynność:** p-p/mean tętnienia Iq przy 30/40/60/72/90/110 rpm, nogi symetryczne
  i niesymetryczne. Cel wstępny: **≤ połowa obecnego tętnienia względnego przy wzroście
  średniej pomocy ≤ 5 %.** Próg ostateczny po A/B i rowerze.
- **Responsywność:** czas 10–90 % po świadomej zmianie nacisku, mierzony **obok** płynności.
  Poprawa płynności kosztem responsywności nie jest zaliczeniem.
- **Stop:** czas do target=0, ref=0, actual≈0, mostek cichy. Bez sumowania ogonów
  hold + boost + release.
- **Limity:** żaden dodatek nie przekracza capu swojego źródła (test 1 %/25 %).
- **Zasoby:** rzeczywisty target build, nie wnioskowanie z hosta.

## 18. Kolejność wykonania

Kolejność wynika z ryzyka, nie z wygody. Każdy etap kończy się porównaniem przed/po.

| Etap | Zakres | Warunek wejścia |
|---|---|---|
| 0 | naprawa harnessu: `u_abs` (0a) + rzeczywisty `span`/zero z roweru (0b) + nowy baseline W1 (0c) | odczyt `0x6025` |
| 1 | `assist_state`: scalenie sześciu pamięci permission w jedną tabelę | etap 0 |
| 2 | `assist_demand`: `base + correction`; usunięcie filtrów wejściowych | etap 1, A/B na W1 |
| 3 | `iq_trajectory`: rampy w krokach absolutnych; smooth start jako polityka | etap 2 |
| 4 | `assist_limits`: kolejność podłoga→cap→arbitraż→global | etap 3 |
| 5 | usunięcie martwego kodu (§12) | etapy 1–4 zaakceptowane |
| 6 | SIL/Level4 → rower | etap 5 |
| 7 | CANable pod ustalony kontrakt | etap 6 |

Etapy 1–4 dotykają `src/main.c`, `src/ride_control.c`, `src/torque_input.c`,
`src/assist_modes.c` — obowiązuje reguła **jednego piszącego** na plik w danym momencie.

---

# CZĘŚĆ IV — CO POZOSTAJE OTWARTE

Liczby, których nie wolno wymyślić. Każda ma tańszą drogę rozstrzygnięcia niż zgadywanie:

| # | Otwarte | Jak rozstrzygnąć | Blokuje |
|---|---|---|---|
| ~~O-1~~ | ~~skala czujnika (`CLAIM-004`)~~ | **NIEISTOTNE dla tej pracy** — czujnik nigdy nie kalibrowany, rower i harness na tej samej krzywej domyślnej; saturacja 0,43 % (§8b) | — |
| ~~O-2~~ | ~~PAS 64 vs 96~~ | **ROZSTRZYGNIĘTE** — FW-086, `config.h:683` (§9) | — |
| O-3 | kroki `base` (rise/fall) w counts/ms | A/B na W1 po etapie 0 | etap 2 |
| O-4 | wzmocnienie `correction` | A/B na W1 | etap 2 |
| O-5 | limit mocy: elektryczna pobierana czy mechaniczna oddawana | decyzja projektowa właściciela | §13 |
| O-6 | kadencja warstwy `0x0800BE70` w stocku | `EVD-AP-RE-01` (`§22.9`) | nic — informacyjne |
| O-7 | R4: warunki start/stop w stocku | `EVD-AP-RE-01` | nic — porównanie |

**O-1 i O-2 są zamknięte i nic z roweru nie jest potrzebne przed etapem 2.** Pozostałe
otwarte pozycje rozstrzygają się A/B na W1 (O-3, O-4) albo są decyzją projektową (O-5).

Zapis dla przyszłego czytelnika: pierwsza wersja tego dokumentu żądała dwóch pomiarów na
rowerze i odczytu kalibracji. **Wszystkie trzy okazały się zbędne** — dwa rozstrzygał już
kod (FW-086, spójność krzywej domyślnej), trzeci rozstrzygnęły dane z W1, które mieliśmy
od początku. Zanim ktokolwiek zażąda następnego pomiaru na rowerze, warto sprawdzić,
czy odpowiedź nie leży w `inc/config.h` albo w nagraniu.

**Żadna liczba w tym dokumencie nie została zmierzona na rowerze.** Cała diagnoza jest
CODE + zewnętrzny reverse. Architektura jest propozycją do review, nie decyzją.

---

## Ryzyko

To jest przebudowa toru `HIGH / SAFETY_CRITICAL` (permission, limity, wykonanie Iq).
Trzy rzeczy, które mogą pójść źle, i co je łapie:

1. **Regresja startu/stopu przy scalaniu permission (etap 1).** Sześć pamięci powstało
   z sześciu realnych przypadków. Łapie: `tests/host/` — te testy istnieją właśnie po to.
2. **`base` podnosi średnią pomoc zamiast tłumić tętnienie.** Łapie: kryterium
   „≤ 5 % wzrostu średniej" mierzone **razem** z tętnieniem, nie osobno.
3. **`base` trzyma moment po zaprzestaniu pedałowania.** Łapie: kryterium stopu; `base`
   musi być bezwarunkowo wygaszany przez `STOPPING`, a nie przez własną stałą czasową.
