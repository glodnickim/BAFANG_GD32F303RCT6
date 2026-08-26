# FW-124 — M820 START/STOP power-stage lifecycle audit (read-only)

- **Data:** 2026-08-24
- **Status:** AUDIT ZAKOŃCZONY. Zero zmian w motor-control. Poniżej: root-cause candidates,
  bezpieczny projekt poprawki (do zaakceptowania osobno) i wymagane sygnały śladu.
- **Zakres tej karty:** wyłącznie czytanie kodu (`src/main.c`, `src/FOC.c`, `src/current_cal.c`,
  `src/motor_core.c`), dokumentacji reversingu stocku (`documentation/M820_REVERSE_KNOWLEDGE_BASE.md`),
  `CHANGELOG.md` i kart FW-093/FW-117..122. **Żaden plik motor-control nie został zmieniony.**
- **Powiązane:** FW-093 (Hi-Z coast, WYCOFANE), STEP 2A / neutral dwell (`CHANGELOG.md`,
  0.0393/0.0394), FW-117 (bridge timing test), FW-118/119 (current cal safety), FW-122
  (rolling no-assist recorder — narzędzie do potwierdzenia hipotez tej karty na rowerze).

---

## 0. Prostym językiem

Sprawdziłem krok po kroku, jak sterownik włącza i wyłącza mostek MOSFET-ów (silnik), i
porównałem to z tym, co wiemy o oryginalnym firmwarze Bafanga.

**Dobra wiadomość:** obecny kod (nazwany w komentarzach „STEP 2A") jest dobrze zbudowany —
ma jedno, spójne miejsce, które włącza i wyłącza silnik dla wszystkich trybów (Torque, Walk
Assist, manetka, itd.), pilnuje neutralnego stanu przed włączeniem mostka (dokładnie jak
stock) i zeruje regulatory we właściwej kolejności. Kalibracja prądu (`current_cal_foc_allowed`)
jest ustawiana **raz, przy starcie sterownika**, i nie może migać w trakcie jazdy przy
domyślnych ustawieniach — to wyklucza ją jako przyczynę sporadycznego braku wspomagania.

**Znalazłem jedną realną lukę czasową:** jeśli rower stoi (silnik nie kręci się) dłużej niż
~1 sekundę, sterownik zaczyna 10-milisekundowe, płynne wygaszanie napięcia fazowego przed
wyłączeniem mostka. Jeśli w tym dokładnie 10-milisekundowym oknie pojawi się nowe żądanie
momentu (np. rowerzysta zaczyna pedałować), sterownik **przerywa** to wygaszanie i od razu
zaczyna nowy start — mostek nigdy się nie wyłącza (nie ma podwójnego kliknięcia), ale silnik
dostaje świeży rozruch zamiast płynnej kontynuacji. To tłumaczy najwyżej krótkie „zacięcie"
na ~10-15 ms, nie trwały brak wspomagania.

**Nie znalazłem** w statycznym kodzie mechanizmu, który tłumaczyłby **trwały** brak
wspomagania przy jeździe (silnik się kręci, rowerzysta pedałuje, a wspomaganie mimo to nie
wraca) — bo mostek software'owo **nigdy nie wyłącza się, gdy silnik faktycznie się kręci**
(warunek wyłączenia to zawsze „brak impulsu Halla przez ~1 s", czyli realny bezruch wirnika).
Jeśli taki przypadek naprawdę występuje na rowerze, przyczyna leży najprawdopodobniej **wyżej**
— w decyzji `ride_control`/`assist_dynamics`, że `final_iq` ma zostać 0, mimo że
torque+cadence istnieją (to jest dokładnie CASE A, który FW-122 już potrafi złapać) — a nie
w samym stopniu mocy. To jest hipoteza, nie potwierdzenie: rozstrzyga dopiero log z FW-122.

---

## 1. CURRENT EVISTDRIVE START FLOW

Źródło: `src/main.c:1289-1330` (main loop, poza slow-loop), gate `src/FOC.c` ISR
`src/main.c:3752-3778`.

```c
// main.c:1292 — jedyna brama startu, wspólna dla WSZYSTKICH źródeł momentu
if (MS.i_q_setpoint > 0 && current_cal_foc_allowed(&current_cal)) {
    if (!ui_8_PWM_ON_Flag) {                 // 1293 — start tylko gdy software mówi "bridge off"
        pwm_cutoff_active = 0;               // 1294 — anuluj ewentualne miękkie zwalnianie
        get_standstill_position();           // 1295 — seed kąta z bieżącego sektora Halla
        PI_iq.integral_part=0; PI_iq.out=0;  // 1301-1302 — FW-035 bumpless enable
        PI_id.integral_part=0; PI_id.out=0;
        MS.u_q=0; MS.u_d=0; MS.u_abs=0;
        switchtime[0..2] = _T>>1;            // 1304-1307 — CCR = neutral, ZAPISANE do timera
        timer_channel_output_pulse_value_config(...) x3
        bridge_lifecycle = NEUTRAL_COMMIT;   // 1309
        neutral_dwell_counter = 4;           // START_NEUTRAL_DWELL_CYCLES
        neutral_dwell_active = 1;
        foc_release_pending = 0;
        first_active_foc_sampled = 0;
        timer_primary_output_config(TIMER0, ENABLE);  // 1314 — MOE ON, CCR JUŻ neutralne
        uint16_half_rotation_counter = 0;
        bridge_lifecycle = MOE_ON;           // 1327
        ui_8_PWM_ON_Flag = 1;                // 1328
    }
}
```

ISR (16 kHz, `main.c:3752`): dopóki `neutral_dwell_active`, wymusza `_T/2` na wszystkich
trzech kanałach i dekrementuje licznik; przy zejściu do 0 ustawia `foc_release_pending=1`.
Main loop widzi tę flagę i przechodzi `NEUTRAL_DWELL → FOC_RELEASE → RUN`
(`main.c:1338-1347`). Dopiero w `RUN`/poza dwell ISR wywołuje `FOC_calculation()` i zaczyna
zapisywać aktywne (nie-neutralne) CCR.

**CONFIRMED**: CCR jest zawsze neutralne w chwili `MOE ON` (zapisane 4 linie wcześniej,
przed enable) — zgodnie ze stockiem (§13). Okno między `MOE ON` (1314) a
`ui_8_PWM_ON_Flag=1` (1328) jest nieszkodliwe: ISR sprawdza `ui_8_PWM_ON_Flag` (3752) i przy
wartości 0 **nic nie robi** — nie nadpisuje CCR, więc mostek trzyma zapisaną wcześniej
wartość neutralną niezależnie od tego, kiedy dokładnie ISR trafi w tę szczelinę.

Ścieżki S1-S7:

| # | Ścieżka | Wejście do startu | Różnica względem głównej ścieżki |
|---|---|---|---|
| S1 | cold/standstill | `MS.i_q_setpoint>0` po `assist_dynamics` | brak — to jest ścieżka wzorcowa opisana wyżej |
| S2 | normal re-start po stopie | jw. | brak, identyczna |
| S3 | rolling re-enable | jw. | patrz §10 — **w obecnym firmwarze bridge nigdy nie wyłącza się podczas realnego obrotu**, więc S3 sprowadza się do S1 (rotor już stoi, gdy dochodzi do STOP) |
| S4 | quick OFF→ON demand | jw. | patrz §9/§16 — jeśli trafia w okno `pwm_cutoff_active`, patrz race opisany tam |
| S5 | Walk Assist start | `motor_core_set_command()` → `MS.i_q_setpoint` (CONFIRMED, jedyny writer: `motor_core.c:22`) | brak osobnej ścieżki bridge; WA nie dotyka `ui_8_PWM_ON_Flag`/`bridge_lifecycle`/`timer_primary_output_config` nigdzie w `walk_assist_motor.c` (0 trafień grepem) |
| S6 | throttle | jw. (jeśli istnieje w danej konfiguracji, ten sam `motor_core_set_command`) | brak osobnej ścieżki |
| S7 | autodetect/service | `main.c:3476` `autodetect()` | **ODRĘBNA ŚCIEŻKA** — patrz niżej |

**S7 — autodetect() (`main.c:3476-3620`), STRONG finding:**
Ta funkcja steruje mostkiem **bezpośrednio**, z pominięciem `bridge_lifecycle`/neutral dwell:
`timer_primary_output_config(TIMER0,ENABLE); ui_8_PWM_ON_Flag=1;` (3497-3498) bez przejścia
przez `NEUTRAL_COMMIT`/`MOE_ON`/`NEUTRAL_DWELL`. `bridge_lifecycle` **pozostaje nietknięty**
(najczęściej `IDLE`) przez cały czas trwania autodetect, mimo że hardware MOE i software PWM
są aktywne. To jest legalna kombinacja `PWM=1, MOE=1, bridge_lifecycle=IDLE` — patrz tabela
§12, wiersz specjalny. Na końcu fazy 1 funkcja poprawnie sprowadza stan z powrotem do znanego
zera (`3600-3607`: MOE OFF, `ui_8_PWM_ON_Flag=0`, `pwm_cutoff_active=0`, licznik wyzerowany) i
oddaje kontrolę „normalnej ścieżce start" na fazę 2 (komentarz `3601-3603` to potwierdza
wprost). **Nie jest to błąd bezpieczeństwa** (autodetect ma własny, potwierdzony trójstopniowo
`hall_calibration_standstill_confirmed()` guard, `main.c:3476-3491`), ale `bridge_lifecycle`
w trakcie autodetect **nie jest wiarygodnym sygnałem diagnostycznym** — FW-122 log złapany w
tym oknie pokaże niespójność, którą trzeba interpretować jako „to jest autodetect", nie jako
błąd lifecycle.

---

## 2. CURRENT EVISTDRIVE NORMAL STOP FLOW

Dwie gałęzie w `inc/config.h`: `SOFT_CUTOFF_ENABLE=1` (produkcja) i wariant `#else` (stary,
natychmiastowy). Obie uruchamiają się **wyłącznie** z tego samego warunku:

```c
uint16_half_rotation_counter > POWER_STAGE_STOP_TICKS   // 4000 ticków @ 4 kHz = ~1 s produkcyjnie
```

**KOREKTA (FW-122.1 audit, 2026-08-24):** poprzednia wersja tego zdania mówiła „przy każdym
zboczu Halla" — to było niedokładne. Zweryfikowane ponownie xref po xref
(`TIMER2_IRQHandler`, `main.c:2017-2088`): spośród dwunastu przejść stanu Halla (6 w przód, 6
wstecz) `uint16_half_rotation_counter=0` wykonuje się **tylko w dwóch** — `case 13` (przód) i
`case 23` (wstecz), czyli raz na ~180° elektryczne w każdym kierunku (stąd nazwa „half
rotation"). Zmienna jest inkrementowana osobno, w `reg_ADC_processing()` (main-loop-context,
wywoływane z main() po fladze `reg_ADC_flag` ustawianej przez `TIMER1_IRQHandler` @ 4 kHz —
NIE bezpośrednio w ISR TIMER1, jak sugerowałaby nazwa; patrz `main.c:2619`). Wniosek
funkcjonalny audytu **się nie zmienia**: przy dowolnym realnym obrocie wirnika przejście przez
`case 13`/`23` następuje wielokrotnie w ciągu ~1 s, więc warunek nadal jest równoważny „silnik
nie wygenerował żadnego z tych dwóch przejść przez ~1 s" = **realny bezruch elektryczny
wirnika**, nie „Iq=0" — tylko dokładność opisu mechanizmu została poprawiona. Osobna zmienna
`uint16_full_rotation_counter` (zerowana na `case 64`/`54`) istnieje równolegle i **nie** jest
tym samym licznikiem — nie mylić przy dalszej pracy nad tym kodem.

### 2.1 SOFT_CUTOFF_ENABLE=1 (produkcyjna, `main.c:1368-1399`)

```c
if (half_rot > POWER_STAGE_STOP_TICKS && ui_8_PWM_ON_Flag && !pwm_cutoff_active) {
    ui_8_PWM_ON_Flag = 0;              // FOC ISR przestaje pisać CCR (3752 gate)
    bridge_lifecycle = IDLE;
    neutral_dwell_active = 0; foc_release_pending = 0;
    pwm_cutoff_st[0..2] = switchtime[0..2];   // snapshot ostatniej aktywnej CCR
    pwm_cutoff_tick = 0;
    pwm_cutoff_active = 1;
}
if (pwm_cutoff_active) {
    if (pwm_cutoff_tick >= SOFT_CUTOFF_TICKS) {   // 40 ticków @ 4kHz = 10 ms
        CCR = neutral x3;
        timer_primary_output_config(TIMER0, DISABLE);   // MOE OFF — dopiero teraz
        i8_recent_rotor_direction = 0;
        PI_iq.integral_part=0; PI_id.integral_part=0;
        pwm_cutoff_active = 0;
    } else {
        CCR = liniowa interpolacja pwm_cutoff_st -> neutral, w main loop (nie w ISR)
    }
}
```

**CONFIRMED**: podczas całego okna `pwm_cutoff_active` (do 10 ms) `ui_8_PWM_ON_Flag=0`, więc
ISR **nie** wywołuje `FOC_calculation()` (gate `main.c:3752`) — nie ma dwóch pisarzy CCR
naraz. Manualna interpolacja w main loop jest jedynym źródłem CCR w tym oknie.

### 2.2 Wariant `#else` (stary, `main.c:1400-1417`)

Ten sam warunek wejścia, ale bez fazy interpolacji: natychmiast CCR=neutral, `MOE DISABLE`,
`ui_8_PWM_ON_Flag=0`, reset PI, reset kierunku, `bridge_lifecycle=IDLE` — wszystko w jednym
takcie. To jest dokładnie „klik" wariant, którego SOFT_CUTOFF miał uniknąć (komentarz
`config.h:280-283`).

### 2.3 Kolejność względem stocku (§13)

| Krok | STOCK (§7.1) | EVISTDRIVE (soft cutoff) |
|---|---|---|
| 1 | command→0, FOC dalej aktywne, ramp current→0 (closed loop) | `ride_control`/`assist_dynamics` rampa Iq→0 (FW-040/041/072), FOC dalej aktywne, zanim `half_rot` w ogóle zacznie rosnąć |
| 2 | MOE OFF | *(dodatkowy krok EVistDrive, patrz niżej)* |
| 3 | reset PI/controller state | — |
| 4 | CCR=neutral (housekeeping, MOE już OFF) | — |
| — | *(stock nie ma tego kroku)* | **CCR interpolacja open-loop 10 ms, FOC OFF, MOE nadal ON** |
| 5 | IDLE | MOE OFF, reset PI, reset kierunku, IDLE |

**STRONG finding**: EVistDrive dokłada krok, którego stock nie ma — 10 ms open-loop
interpolacji CCR **przed** MOE OFF, z FOC już wyłączonym. To odpowiada wprost na pytanie z
§8 zamówienia (patrz niżej).

---

## 3. CURRENT EVISTDRIVE FAULT STOP FLOW

**CONFIRMED, odrębna ścieżka, nietknięta przez STEP 2A:**

- **Nadprąd** (`FOC.c:104-111`): `i_d > PH_CURRENT_MAX<<2` → CCR=neutral, `MOE DISABLE`,
  `while(1){}` — **zawieszenie do twardego resetu**, bez powrotu do `bridge_lifecycle`. To
  jedyna ścieżka fault w samym `FOC.c`. Uwaga: ten check żyje **wewnątrz** `FOC_calculation()`,
  czyli jest wykonywany tylko gdy `ui_8_PWM_ON_Flag && !neutral_dwell_active` (ISR gate,
  `main.c:3752`) — patrz OPEN item w §18.
- **Hamulec / cofanie / przegrzanie** (`ride_control.c`, `safety_cut`, FW-037): rampa 200 ms
  do zera, **przez tę samą** bramkę `MS.i_q_setpoint>0` w main.c — czyli fizycznie kończy się
  tym samym STOP flow co §2, nie osobnym mechanizmem. To jest zamierzone (FW-093 §5: „Iq=0 na
  jej końcu prowadzi teraz dodatkowo do Hi-Z" — ale Hi-Z zostało wycofane, więc dziś kończy
  się zwykłym soft/hard cutoff po ~1 s bezruchu).
- **Watchdog komunikacji** (`main.c:1270-1284`): po 3 s bez ramki HMI wysyła
  `motor_command_t{iq_target=0, enable=true, emergency_stop=false}` — **to jest graceful
  stop przez normalny kanał**, nie emergency. Po 10 s bez ramki i przy `Speedx100==0`:
  `power_off_controller()`.
- **`power_off_controller()`**: patrz `main.c:5314-5329` — jawnie resetuje
  `bridge_lifecycle=IDLE` (5314, komentarz „STEP 2A: reset lifecycle") i
  `ui_8_PWM_ON_Flag=0` (5329).

**Rozdział REQUESTED STOP vs FAULT STOP jest zachowany**: jedyna ścieżka, która nie przechodzi
przez `bridge_lifecycle`/`pwm_cutoff_*`, to nadprąd w `FOC.c` (blokada sprzętowa, `while(1)`).
Wszystko inne — hamulec, cofanie, przegrzanie, watchdog, zejście do zera — to warianty tego
samego „graceful" traktowania: `MS.i_q_setpoint` spada do 0 przez rampę, a fizyczne wyłączenie
mostka i tak czeka na ten sam warunek bezruchu (§2). To jest zgodne ze stockiem (§7.1: „stock
nie traktuje zwykłego odpuszczenia momentu jak awarii").

---

## 4. SOFTWARE PWM FLAG vs HARDWARE MOE

`timer_primary_output_config(TIMER0, ENABLE/DISABLE)` zapisuje/kasuje `POEN` w `TIMER_CCHP`
— to jest bezpośrednio hardware MOE, bez pośredniej flagi software'owej udającej hardware
(inaczej niż w wycofanej architekturze FW-093, gdzie MOE było przełączane w ISR przez
`pwm_enable_request` — **ten mechanizm już nie istnieje w kodzie**, potwierdzone brakiem
wystąpień `pwm_enable_request` w `src/`).

| PWM_FLAG | MOE | pwm_cutoff_active | Legalne? | Kiedy / jak długo | Znaczenie |
|---|---|---|---|---|---|
| 0 | 0 | 0 | tak | IDLE, dowolnie długo | prawdziwy standstill |
| 1 | 1 | 0 | tak | RUN, dowolnie długo | normalna jazda |
| 0 | 1 | 1 | **tak — legalny transient** | do 10 ms (`SOFT_CUTOFF_TICKS`) | miękkie wygaszanie w toku; ISR nie pisze CCR, main loop interpoluje |
| 0 | 1 | 0 | **tak — legalny transient, ~mikrosekundy** | okno `main.c:1314→1328` (start) LUB `dwell_timeout_counter` failsafe przed samym `DISABLE` | patrz §1 (start: CCR już neutralne, nieszkodliwe) i §16 (timeout failsafe kolejność: `DISABLE` linia 1355, `PWM_ON=0` linia 1356 — jest tu odwrotna kolejność, PATRZ NIŻEJ) |
| 1 | 0 | 0 | **NIE powinno wystąpić w normalnym locie** | — | nie znaleziono ścieżki, która ustawia PWM_FLAG=1 przed MOE ON |

**Odpowiedź na zamówienie §12**: `PWM_FLAG=0, MOE=1` **jest** legalnym, udokumentowanym w
kodzie transientem (dwa różne miejsca, dwa różne czasy trwania). **Nie wolno** — zgodnie z
przestrogą zamówienia — traktować `!ui_8_PWM_ON_Flag` jako równoznaczne z „hardware bridge is
off". Dokładnie to potwierdza analiza race w §9.

**Nowy, drobny finding przy okazji tej tabeli** (`main.c:1354-1363`, dwell-timeout failsafe):

```c
if (dwell_timeout_counter >= START_DWELL_TIMEOUT_CYCLES) {
    timer_primary_output_config(TIMER0, DISABLE);   // 1355 — MOE OFF
    ui_8_PWM_ON_Flag = 0;                            // 1356 — PWM flag OFF
    ...
}
```

Tu kolejność to MOE OFF **przed** PWM_FLAG=0 — jedyne miejsce w kodzie, gdzie te dwa zapisy
nie są w kolejności „software najpierw". Ponieważ oba zapisy są rozdzielone jedną linią C w
main loop (nie ma między nimi wywołania funkcji ani punktu, w którym ISR mogłaby coś
odczytać inaczej niż spójnie — ISR i tak nic nie pisze do CCR gdy `neutral_dwell_active`,
niezależnie od `ui_8_PWM_ON_Flag`), **to nie jest race** — czysto kosmetyczna niespójność
kolejności, zanotowana dla kompletności, HYPOTHESIS→OPEN tylko jako pytanie stylistyczne, nie
funkcjonalne.

---

## 5. SOFT-CUTOFF STATE ANALYSIS

Patrz §2.1 pełny kod. Kluczowe fakty, CONFIRMED w kodzie:

- Trigger: wyłącznie `half_rot > POWER_STAGE_STOP_TICKS` (~1 s bez Halla) — **nigdy** samo
  `Iq==0`. Rider, który przestaje pedałować, ale rotor jeszcze dokręca się bezwładnością
  (np. na rowerze bez sprzęgła jednokierunkowego między silnikiem a BB), **nie** wyzwala tej
  ścieżki, dopóki Hall milczy krócej niż 1 s.
- Podczas interpolacji `ui_8_PWM_ON_Flag=0` → FOC/PI **nie kontrolują już prądu** (ISR gate
  `3752` blokuje `FOC_calculation()` całkowicie, nie tylko zapis CCR). To wprost odpowiada na
  pytanie zamówienia §8: „czy regulator nadal kontroluje current? NIE — od chwili
  `ui_8_PWM_ON_Flag=0` sterowanie jest open-loop, wyłącznie geometryczna interpolacja CCR do
  neutral, bez odczytu rzeczywistego prądu.
- Ryzyko regeneracyjnego impulsu / phase lock: **niskie, HYPOTHESIS potwierdzona pośrednio**
  — trigger wymaga braku Halla przez 1 s, czyli SEM ≈ 0 (wirnik faktycznie stoi lub kręci się
  poniżej progu wykrywalności), więc open-loop wektor bliski neutralnemu nie ma znaczącego
  napięcia do wygenerowania prądu wstecznego. Nie zmierzone na rowerze (wymaga oscyloskopu lub
  ramki `0x00010207`-podobnej, patrz §20).

---

## 6. QUICK RE-DEMAND ANALYSIS (zamówienie §9)

**Scenariusz zamówienia jest CONFIRMED możliwy w kodzie**, z dokładną sekwencją:

```
t0:      half_rot > 4000 ticków  →  ui_8_PWM_ON_Flag=0, bridge_lifecycle=IDLE,
                                     pwm_cutoff_active=1, pwm_cutoff_tick=0
t0..t0+10ms:  main loop interpoluje CCR w stronę neutral (open-loop, MOE nadal ON)
t0+Δ (Δ<10ms):  MS.i_q_setpoint staje się >0 (nowy popyt) I current_cal_foc_allowed()==1
                → main.c:1293  if(!ui_8_PWM_ON_Flag)  ==  TRUE
                → 1294  pwm_cutoff_active = 0            // przerywa interpolację NATYCHMIAST
                → 1295  get_standstill_position()         // reseed kąta z Halla
                → 1301-1307  PI=0, CCR wymuszone na DOKŁADNY neutral (nie na wartość z
                              przerwanej interpolacji, ale re-zapisane na _T>>1)
                → 1309-1328  pełna sekwencja: NEUTRAL_COMMIT → MOE_ON (MOE NIE jest
                              przełączane OFF→ON, bo już było ON — timer_primary_output_config
                              wywołane ponownie z ENABLE jest no-opem na rejestrze) →
                              ui_8_PWM_ON_Flag=1
```

**Wynik, CONFIRMED**: MOE **nigdy nie przełącza się OFF→ON** w tym scenariuszu (był ON przez
cały czas) — więc **nie ma tu źródła podwójnego kliknięcia mechanicznego** z samego MOE.
Efekt odczuwalny przez rowerzystę to najwyżej: do 10 ms przerwanej interpolacji (CCR skacze z
wartości pośredniej na dokładny neutral — to jest **skok napięcia o mniej niż to, co
zostałoby pominięte przy zwykłym starcie z pełnego zera**, więc mniejszy niż zwykły start-click)
plus obowiązkowe **250 µs** (`START_NEUTRAL_DWELL_CYCLES=4` @ 16 kHz) nowego neutral dwell,
zanim FOC znów zacznie pisać aktywne CCR.

**To jest MODEL 2 z zamówienia (§17)** — „finish shutdown → restart from IDLE" — a nie
MODEL 1 („abort stop, keep MOE ON, resume current controller"). Kod faktycznie trzyma MOE ON
(nie robi pełnego MODEL-2 cyklu MOE-off/on), ale semantycznie traktuje to jako **nowy start od
zera** (reseed kąta, reset PI, dwell), nie jako bezszwowe wznowienie starego stanu regulatora.
To jest **hybryda**, bezpieczna, bo `get_standstill_position()` jest tu uzasadnione (rotor
faktycznie stał ≥1 s tuż przed tym momentem), ale **nieoptymalna pod względem płynności**
— wprowadza ~250 µs–10 ms przerwy w momencie, gdy fizycznie nic nie musiałoby się przerywać.

**Okno trafienia**: `SOFT_CUTOFF_TICKS=40` ticków @ 4 kHz = **10 ms** z każdego przejścia w
STOP. To wąskie okno, ale przy typowej jeździe z częstymi krótkimi przystankami/postojami
(światła, martwe punkty pedałowania) liczba przejść w STOP na przejażdżkę może być duża —
**STRONG hipoteza, że to jest źródło sporadycznych krótkich „zacięć" (nie trwałego braku
wspomagania)**, zgodna ze zgłoszonym objawem #1/#2 (klik), słabiej zgodna z objawem #3
(trwały brak wspomagania — ten mechanizm samo-naprawia się w ≤10-15 ms, więc nie tłumaczy
uporczywego "silnik nie podejmuje wspomagania").

---

## 7. ROLLING RE-ENABLE ANALYSIS (zamówienie §10)

**CONFIRMED, kluczowy wniosek**: w obecnym firmwarze **nie istnieje ścieżka „bridge OFF,
silnik nadal się fizycznie kręci"**. Jedyny warunek wyłączenia mostka to `half_rot >
POWER_STAGE_STOP_TICKS` — czyli **brak** impulsów Halla przez ~1 s, co z definicji oznacza,
że w chwili wyłączenia (i przez cały czas, gdy mostek jest OFF) wirnik **nie generuje**
wykrywalnego obrotu. Architektura z FW-093 (COAST↔DRIVE z SEM-latch dla naprawdę kręcącego
się, odłączonego silnika) **została wycofana w całości** (`5047de3`+`611507d`) i nie ma jej
śladu w obecnym `main.c`/`FOC.c` (brak `coast_u_q_latched`, `coast_erps_latched`,
`power_stage_enter_drive/coast` — zero trafień grepem).

Skutek: pytania zamówienia o „reset rotor_direction na normalnym stop" i „czy stock resetuje
direction przy normalnym MOE OFF" są **poprawnie postawione, ale w obecnym EVistDrive nie ma
przypadku, w którym trzeba by je odróżniać od standstill** — bridge OFF ⟺ standstill (z
dokładnością do ~1 s progu), zawsze. `i8_recent_rotor_direction=0` (§10 tabela, `main.c:1358,
1390, 1409`) jest więc zawsze uzasadnione fizycznie w chwili, gdy jest wykonywane.

**Konsekwencja dla „rolling no-assist" (objaw #3)**: skoro bridge nie wyłącza się podczas
realnego obrotu, **sporadyczny brak wspomagania przy jadącym rowerze nie może mieć źródła w
tym mechanizmie startu/stopu mostka**. Musi być albo:
(a) w warstwie wyżej — `ride_control`/`assist_dynamics`/`pedal_assist_gate` decyduje, że
`final_iq` ma pozostać 0, mimo obecności torque+cadence (to jest dokładnie CASE A z FW-122,
poza zakresem tej karty — czysto ride-control, nie power-stage), albo
(b) w samym FOC/PI podczas `RUN` (np. saturacja, źle wyliczony kąt przy niskim ERPS), co
objawia się jako `iq_actual` znacząco mniejsze niż `final_iq` mimo PWM=1, MOE=1 — to jest
dokładnie CASE C z FW-122.

Ta karta **nie rozstrzyga** (a) vs (b) — do tego służy log FW-122 na rowerze (§20).

---

## 8. CURRENT_CAL START BLOCK ANALYSIS (zamówienie §11)

`src/current_cal.c:228-232`:

```c
uint8_t current_cal_foc_allowed(const current_cal_t *s) {
    if (!s) return 1;
    return s->foc_allowed;
}
```

`s->foc_allowed` jest zapisywane w **dokładnie trzech miejscach**, wszystkie w
`current_cal_init()`/`current_cal_finalize()` (`current_cal.c:66, 187, 203, 212, 225`) —
funkcje wywoływane **raz, podczas sekwencji kalibracji przy starcie sterownika**, nie w
pętli głównej ani w ISR. Grep całego `src/` na `foc_allowed\s*=` nie zwraca żadnego trafienia
poza tymi.

| Wynik kalibracji | `foc_allowed` |
|---|---|
| `CURRENT_CAL_OK` (runtime) | 1 |
| `CURRENT_CAL_USING_LKG` (last-known-good, degraded) | 1 |
| `CURRENT_CAL_LEGACY_FALLBACK` (stary hardware-offset) | 1 |
| `CURRENT_CAL_HARD_FAILED`, **tylko przy `policy==STRICT`** | 0 |

Pamięć projektu (`[[project-fw119-current-cal-safety]]`) potwierdza: **STRICT musi zostać
OFF do pomiaru warsztatowego** — czyli przy domyślnej, obecnie używanej konfiguracji
`foc_allowed` jest **zawsze 1** po starcie i **nigdy nie zmienia się w trakcie jazdy**.

**CONFIRMED, definitywna odpowiedź**: `current_cal_foc_allowed()` **nie może** być źródłem
sporadycznej blokady startu w trakcie jazdy — to jest stan zapisywany raz, przy boocie, nie
pętla ani transient. Jedyny scenariusz `final_iq>0 AND foc_allowed==0` to trwałe
`CURRENT_CAL_HARD_FAILED` pod `STRICT`, które (a) nie jest domyślną konfiguracją i (b)
byłoby **persistent od startu sterownika**, nie sporadyczne w trakcie jazdy — sterownik po
prostu nigdy by nie ruszył tego dnia, co jest łatwe do zauważenia i niepodobne do zgłoszonego
objawu.

---

## 9. PI RESET ORDER

Zebrane z §1/§2/§3, w kolejności czasowej:

**START** (`main.c:1301-1303`): `PI_iq.integral_part=0; PI_iq.out=0; PI_id.integral_part=0;
PI_id.out=0;` — **przed** `MOE ON` (1314). Zgodne ze stockiem (§7.2: „reset FOC/controller"
jest pierwszym krokiem, przed „MOE ON").

**STOP (soft cutoff)** (`main.c:1390-1391`): reset PI **po** zakończeniu interpolacji CCR i
**po** `MOE DISABLE` (1388) — dokładnie zgodne ze stockiem (§7.1: „MOE OFF → reset PI/controller
state").

**STOP (dwell timeout failsafe)** (`main.c:1358-1359`): reset PI **po** `MOE DISABLE` (1355) —
spójne z powyższym.

**FW-028 integral-zero-on-zero-setpoint**: karta FW-093 §2b opisuje i usuwa dodatkowy reset
`if(MS.i_q_setpoint==0){ PI_iq.integral_part=0; ... }` w `reg_ADC_processing()`, bo blokował
zdolność regulatora do wyzerowania prądu na kręcącym się wirniku. **Ta zmiana została
wycofana razem z całą kartą FW-093** (`5047de3` cofnięty w całości) — czyli ten reset **wrócił**
do kodu produkcyjnego. Nie zweryfikowałem osobno, czy jest identyczny z opisem FW-093 §2b, bo
to poza zakresem tej karty (dotyczy regulacji podczas RUN, nie startu/stopu mostka) —
**OPEN**, wymaga osobnego sprawdzenia jeśli ktoś podejrzewa regulator prądu, nie lifecycle.

**Wniosek**: kolejność resetów PI jest **CONFIRMED zgodna ze stockiem** we wszystkich trzech
ścieżkach mostka.

---

## 10. ROTOR STATE RESET ORDER

Patrz §7. `i8_recent_rotor_direction` jest **jedynym** stanem „kierunku/pozycji" resetowanym
przy stopie (`=0` w trzech miejscach, wszystkie post-MOE-OFF lub w tym samym takcie co
MOE-OFF). `q31_rotorposition_hall`/`q31_rotorposition_absolute` **nie są resetowane** przy
stopie — ISR nadpisuje `q31_rotorposition_absolute` z bieżącego sektora Halla „on every
cycle" (komentarz `main.c:5213-5214`), więc kąt jest zawsze aktualny, niezależnie od stanu
mostka; `get_standstill_position()` przy starcie to tylko jednorazowy bootstrap, żeby
pierwszy cykl FOC (po dwell) miał wartość, a nie śmieci sprzed wyłączenia. **CONFIRMED**: nie
ma tu „skokowego" reseedu w sensie ryzykownym — kąt jest ciągle śledzony przez sprzęt Halla
niezależnie od software'owego stanu bridge.

---

## 11. STOCK M820 START / NORMAL STOP / FAULT STOP

Źródło: `documentation/M820_REVERSE_KNOWLEDGE_BASE.md` §6-7 (oznaczone w tym dokumencie
„PEWNE" = CONFIRMED przez reversing, nie założenie).

**START (§7.2, PEWNE):** `reset FOC/controller → CCR1=CCR2=CCR3=1000 (neutral, ARR=2000) →
Iq_ref=0/stan neutralny → MOE ON → dopiero potem ramp momentu.` Neutral CCR **przed** MOE ON —
identyczna kolejność jak EVistDrive STEP 2A.

**NORMAL STOP (§7.1, PEWNE):** `command→0 → FOC/PWM nadal aktywne → ramp current/torque→
dokładne 0 (closed loop) → MOE OFF → reset PI/controller state → CCR=1000 → IDLE.` Stock
**nie ma** żadnego etapu open-loop interpolacji CCR między „prąd=0" a „MOE OFF" — CCR=1000
jest ustawiane **po** MOE OFF, czysto jako housekeeping (elektrycznie nieistotne, bo wyjścia
są już Hi-Z). To jest architektoniczna różnica opisana w §2.3 tej karty.

**FAULT STOP (§6, PEWNE):** wielostopniowa ochrona nadprądowa z licznikiem kolejnych
przekroczeń (>5 w rzędzie) na **trzech progach** zależnych od temperatury toru pomiarowego,
kończąca się flagą fault + `MOE OFF`. To jest **bogatszy** mechanizm niż pojedynczy,
natychmiastowy próg w EVistDrive (`FOC.c:105-111`, jeden próg `PH_CURRENT_MAX<<2`,
natychmiastowy `while(1)` bez licznika kolejnych trafień, bez kompensacji temperaturowej).
**STRONG, ale poza zakresem tej karty** (dotyczy FOC/nadprądu, nie lifecycle start/stop) —
odnotowane jako materiał na osobną kartę, nie do zmiany tutaj.

**Główna lekcja stocku wskazana w reversingu** (§7.2, cytat): *„Rozdzielić API: graceful stop
/ emergency shutdown / set-neutral. Nie resetować PI i nie wyłączać mostka w przypadkowej
kolejności."* — **CONFIRMED, EVistDrive już to spełnia**: kolejność jest identyczna jak
stock (§9 tej karty), a fault (nadprąd) jest fizycznie odrębną ścieżką od graceful stop
(§3 tej karty). Jedyna rozbieżność to dodatkowy krok interpolacji CCR opisany w §2.3/§5.

---

## 12. VESC CROSS-CHECK

W repozytorium **nie ma** lokalnego źródła VESC — porównanie oparte wyłącznie na ogólnej
wiedzy inżynierskiej o architekturze FOC (zgodnie z zamówieniem: „nie kopiuj kodu", „nie
traktuj jako dowodu Bafanga").

Zasada architektoniczna VESC (i większości dojrzałych implementacji FOC): **current
controller zachowuje własność (ownership) do momentu, aż zmierzony prąd faktycznie osiągnie
zero**, dopiero potem PWM/gate driver jest wyłączany; restart podczas obracającego się
silnika wymaga odczytu/estymacji SEM zanim regulator zacznie sterować napięciem (żeby
uniknąć „fight with back-EMF").

- **Stock Bafang** niezależnie stosuje tę samą zasadę (§7.1: `ramp current→0` **przed**
  `MOE OFF`, closed-loop do samego końca).
- **EVistDrive** odstępuje od niej w jednym miejscu: soft-cutoff wyłącza `FOC_calculation()`
  (`ui_8_PWM_ON_Flag=0`) **przed** faktycznym MOE OFF i przejmuje sterowanie open-loop na
  10 ms (§2.3, §5).

Ponieważ stock i VESC **niezależnie** zgadzają się co do zasady „current control ownership
until zero, then disable" — to jest **STRONG DESIGN SUPPORT** za tą zasadą (zgodnie z
zamówieniem §14: nie dowód Bafanga, ale silny argument inżynierski). To nie jest jednak dowód,
że dzisiejsze odstępstwo EVistDrive jest **szkodliwe** — trigger tej ścieżki wymaga już
zmierzonego bezruchu (§5), więc ryzyko praktyczne jest ograniczone; to jest architektoniczna
rozbieżność do odnotowania, nie potwierdzony błąd.

Co do restartu podczas obracającego się silnika: nieistotne dla dzisiejszego EVistDrive,
ponieważ taki przypadek nie występuje (§7/§10) — mostek nie wyłącza się podczas realnego
obrotu, więc nie ma potrzeby estymacji SEM przy restarcie.

---

## 13. CONFIRMED DIFFERENCES (EVistDrive vs stock)

| # | Różnica | Status | Ryzyko |
|---|---|---|---|
| D1 | EVistDrive dokłada 10 ms open-loop interpolacji CCR między FOC-off a MOE-off; stock tego nie ma (§2.3, §5, §12) | CONFIRMED | Niskie — trigger wymaga już zmierzonego bezruchu; ale otwiera okno race opisane w D2 |
| D2 | Podczas okna z D1, nowy dodatni `Iq` demand przerywa cutoff i wymusza pełny re-start (reseed+dwell) zamiast płynnej kontynuacji (§6) | CONFIRMED | Niskie/średnie — max ~10-15 ms zacięcia, samo-naprawiające się; nie tłumaczy trwałego braku assist |
| D3 | Nadprąd EVistDrive: jeden próg, natychmiastowy `while(1)`; stock: trzy progi zależne od temperatury z licznikiem kolejnych przekroczeń (§11) | CONFIRMED (reversing), poza zakresem tej karty | Nieustalone — materiał na osobną kartę |
| D4 | `autodetect()` steruje mostkiem z pominięciem `bridge_lifecycle`, zostawiając go nieaktualnym podczas service-procedury (§1, S7) | CONFIRMED | Brak ryzyka funkcjonalnego (własne guardy standstill); ryzyko czysto diagnostyczne (FW-122 log z tego okna myli) |
| D5 | Kolejność zapisu `MOE DISABLE` przed `ui_8_PWM_ON_Flag=0` w ścieżce dwell-timeout-failsafe, odwrotna niż w pozostałych dwóch ścieżkach stopu (§4) | CONFIRMED, kosmetyczne | Brak — nie jest to race (uzasadnione w §4) |

---

## 14. CLICK START ROOT-CAUSE CANDIDATES

1. **STEP 2A neutral dwell** (`main.c:1309-1347`, `CHANGELOG.md` „STEP 2A") — to jest
   **już wdrożona poprawka** dokładnie na klik startowy, precyzyjnie odtwarzająca wzorzec
   stocku (neutral CCR przed MOE ON + opóźnione zwolnienie FOC). Status: **w kodzie
   NORMAL od 0.0393/0.0394, ale bez `VERIFIED_BENCH`** — `CHANGELOG.md:221-223` i
   `M820_REVERSE_KNOWLEDGE_BASE.md:707-711` jawnie wymagają pomiaru napięcia fazowego i
   prądu podczas dwell oraz porównania odczuwalności kliku przed/po, **niewykonanego
   dotychczas**. **HYPOTHESIS (silna, bo mechanizm jest poprawny na papierze) → wymaga
   testu stanowiskowego/rowerowego, żeby przejść w CONFIRMED.**
2. Wartość `START_NEUTRAL_DWELL_CYCLES=4` (250 µs) jest opisana jako „pierwsza, do
   walidacji" — jeśli klik nadal występuje po tej karcie, **kandydat #1 do przetestowania**
   to zbyt krótki dwell względem rzeczywistego czasu ustalenia się gate drivera/dead-time.
3. `autodetect()` (S7, D4) nie przechodzi przez dwell wcale — jeśli klik jest zgłaszany
   podczas serwisowej kalibracji Halla, to jest odrębna, nieobjęta przez STEP 2A ścieżka.
   **OPEN** — nie badane w tej karcie (poza zakresem: procedura serwisowa, nie jazda).

---

## 15. CLICK STOP ROOT-CAUSE CANDIDATES

1. **SOFT_CUTOFF** (`config.h:280-285`, `main.c:1368-1399`) jest już wdrożoną poprawką
   dokładnie na klik stopu (komentarz: „usuwa klik przy koncowym DISABLE po zatrzymaniu").
   Mechanizm: 10 ms interpolacji CCR do neutral **przed** MOE OFF. **HYPOTHESIS → wymaga
   potwierdzenia na rowerze** (brak wzmianki o testach FW-093-Test-3-podobnych po
   przywróceniu tej ścieżki po rewercie FW-093).
2. **D2 (§6, §13)**: jeśli klik utrzymuje się mimo SOFT_CUTOFF, sprawdzić, czy nie jest to
   właśnie przerwana-i-ponowiona sekwencja startu (D2) mylona z klikiem stopu — mechanizmy są
   blisko siebie w czasie i mogłyby być łatwo pomylone bez ramki diagnostycznej rozróżniającej
   `pwm_cutoff_active` vs `bridge_lifecycle` w danej chwili.
3. `POWER_STAGE_STOP_TICKS=4000` (~1 s) — zbyt krótki próg mógłby wywoływać cutoff w środku
   normalnej przerwy w pedałowaniu (np. martwy punkt korby), co dawałoby częstszy,
   wyczuwalny klik/zacięcie niż potrzeba. **OPEN** — wymaga danych z jazdy (rozkład czasów
   między impulsami Halla podczas normalnego pedałowania), nie jest oceniane w tej karcie bez
   dowodu.

---

## 16. NO-ASSIST ROOT-CAUSE CANDIDATES

W kolejności prawdopodobieństwa wynikającej z audytu:

1. **CASE A w sensie FW-122** (ride-control decyduje `final_iq=0` mimo torque+cadence) —
   **poza zakresem tej karty** (`ride_control.c`/`assist_dynamics.c`/`pedal_assist_gate.c`),
   ale audyt power-stage **wyklucza** power-stage jako przyczynę, jeśli log pokaże CASE A
   (bo wtedy `final_iq` nigdy nie dociera do bramki startu w ogóle — nie ma czego audytować
   w mostku). **HYPOTHESIS, najbardziej prawdopodobna** — bo audyt lifecycle nie znalazł
   mechanizmu trwałej blokady na poziomie mostka (§7, §8).
2. **CASE C w sensie FW-122** (PWM=1, MOE=1, ale `iq_actual` << `final_iq`) — FOC/PI podczas
   RUN, np. saturacja napięcia przy niskich obrotach lub błąd kąta. Poza zakresem tej karty
   (dotyczy regulacji, nie startu/stopu), ale lifecycle audit **potwierdza**, że jeśli to
   wystąpi, to NIE z powodu D1/D2 (te kończą się w ≤15 ms, CASE C w zgłoszeniu brzmi jak stan
   trwały).
3. **D2 (quick re-demand race, §6)** — **HYPOTHESIS, ale ograniczona skala skutku**: max
   ~10-15 ms przerwy, samo-naprawiająca się. Może tłumaczyć **krótkie, powtarzalne
   „zacięcia"**, ale nie „silnik czasami nie podejmuje ponownie wspomagania" w sensie
   trwałym, chyba że coś w ścieżce startu (np. zły seed kąta z `get_standstill_position()`
   w konkretnym sektorze Halla) sporadycznie powoduje, że FOC „się nie łapie" mechanicznie —
   **to jest już poza to, co statyczna analiza kodu może rozstrzygnąć bez pomiaru**.
4. `current_cal_foc_allowed()` — **WYKLUCZONE** definitywnie (§8).
5. Race software-PWM-flag vs hardware-MOE ogólnie — **WYKLUCZONE jako źródło no-assist**:
   każda legalna kombinacja z tabeli §4 albo jest nieszkodliwa (CCR już neutralne), albo
   samo-naprawia się w ≤15 ms.

**Nie znalazłem w statycznej analizie kodu mechanizmu tłumaczącego *trwały* brak
wspomagania przy jadącym rowerze na poziomie power-stage/lifecycle.** To jest zgodne z tym,
że FW-122 (rolling no-assist recorder) został zbudowany właśnie po to, żeby rozstrzygnąć
CASE A/B/C na żywym logu — ta karta go nie zastępuje.

---

## 17. RECOMMENDED LIFECYCLE

**Obecny lifecycle jest już bardzo blisko wzorca ze stocku i nie wymaga przebudowy
architektonicznej.** Jedyna zidentyfikowana, uzasadniona zmiana do rozważenia (NIE
wdrażana w tej karcie):

```
IDLE
  │  MS.i_q_setpoint>0 AND current_cal_foc_allowed()
  ▼
NEUTRAL_COMMIT  (CCR=neutral zapisane, PI=0)
  ▼
MOE_ON          (timer_primary_output_config ENABLE)
  ▼
NEUTRAL_DWELL   (ISR trzyma CCR neutral, liczy w dół)
  ▼
FOC_RELEASE     (pierwszy cykl FOC po dwell)
  ▼
RUN             (normalna praca)
  │  half_rot > POWER_STAGE_STOP_TICKS (brak Halla ~1 s)
  ▼
SOFT_CUTOFF     (FOC OFF, CCR interpolowane open-loop do neutral, MOE nadal ON)
  │
  ├── nowy Iq>0 w oknie SOFT_CUTOFF ──► PROPOZYCJA (nie wdrożona): zamiast pełnego
  │                                     restartu (NEUTRAL_COMMIT od zera), po prostu
  │                                     ANULOWAĆ SOFT_CUTOFF I WRÓCIĆ DO RUN — pomijając
  │                                     ponowny NEUTRAL_COMMIT/MOE_ON/DWELL, bo MOE nigdy
  │                                     nie zgasło i CCR nie musi przechodzić przez dwell
  │                                     drugi raz. To skróciłoby D2 z ~250 µs-10 ms do 0.
  ▼ (po SOFT_CUTOFF_TICKS)
MOE_OFF → IDLE

FAULT (dowolny stan) ──► natychmiastowe MOE OFF, bez przechodzenia przez lifecycle
                          (już zaimplementowane w FOC.c dla nadprądu — zachować bez zmian)
```

To jest **STOP COMPLETION przez czas** (`SOFT_CUTOFF_TICKS`), nie przez warunek fizyczny —
zgodnie z pytaniem zamówienia §16, rozważona alternatywa `abs(iq_actual)<threshold for N
cycles` **nie jest tu potrzebna**, bo w chwili wejścia w SOFT_CUTOFF prąd jest już od dawna
bliski zeru (rotor stoi od ≥1 s) — czas jest tu wystarczającym, prostszym kryterium niż próg
prądowy, i nie ma dowodu przeciwnego w tym audycie.

---

## 18. SAFETY — NIE NARUSZANE

**CONFIRMED, sprawdzone bezpośrednio w kodzie:**

- Nadprąd (`FOC.c:104-111`) pozostaje natychmiastowy, bezwarunkowy, niezależny od
  `bridge_lifecycle` i `pwm_cutoff_active` — **nie jest** przekształcany w graceful stop.
- `power_off_controller()` jawnie resetuje `bridge_lifecycle=IDLE` i `ui_8_PWM_ON_Flag=0`
  (`main.c:5314, 5329`) — nie zależy od zakończenia jakiegokolwiek toczącego się cutoff.

**OPEN, drobny, niski priorytet**: nadprądowy check w `FOC.c:105` jest wykonywany **wewnątrz**
`FOC_calculation()`, która nie jest wywoływana podczas `neutral_dwell_active` (ISR gate,
`main.c:3752-3765` — w tej gałęzi ISR pisze tylko neutral CCR, nie wchodzi do
`FOC_calculation()` wcale). Przez ~250 µs (`START_NEUTRAL_DWELL_CYCLES=4` @ 16 kHz) na
początku każdego startu ten konkretny check nadprądowy jest nieaktywny. Ryzyko oceniane jako
**niskie** (CCR jest w tym oknie dokładnie neutralne, więc nie ma sterowanego napięcia
mogącego wywołać nadprąd fazowy z tego źródła), ale nie zweryfikowałem, czy istnieje
**niezależny, sprzętowy** (nie software'owy) obwód ochrony nadprądowej na tej płycie
(comparator→break input TIMER0, niezależny od ISR) — to by zamykało lukę bez zmiany kodu.
**Nie badane w tej karcie** (wymaga schematu/datasheet płyty, poza zakresem audytu firmware).

---

## 19. ZWIĄZEK Z FW-122

**AKTUALIZACJA (FW-122.1, 2026-08-24):** poniższa lista braków została zamknięta. Schema
skoczyła do v3 — `pwm_cutoff_active` (bit w `status_flags`), `pwm_cutoff_progress` i
`hall_timeout_progress` (nowe pola DATA 5) są teraz na wire, przy zerowym dodatkowym koszcie
CAN. Zobacz `documentation/FW-122_ROLLING_NO_ASSIST_DIAGNOSTIC_PL.md` §19 dla pełnego opisu i
`_bridge_failure_subreason()`/`_cutoff_aborted_by_redemand()` w
`tools/trace_analyzer_lib/core.py` dla tego, jak D2 (§6 tej karty) jest teraz widoczne jako
subreason CASE B na złapanym logu — bez zmiany klasyfikacji A/B/C w firmware.

FW-122 (schema v2, `documentation/FW-122_ROLLING_NO_ASSIST_DIAGNOSTIC_PL.md`) **już
przechwytuje dokładnie te pola**, które ta karta zidentyfikowała jako potrzebne — potwierdzone
przez zgodność nazw zmiennych między tą kartą (wyprowadzonych z czytania `main.c`) a tabelą
sample w FW-122 §3:

| Pole potrzebne do audytu | Czy jest w FW-122 v2 sample |
|---|---|
| `final_iq` (`iq_setpoint`) | tak, offset 20 |
| `iq_actual` (raw, signed) | tak, offset 22 |
| software PWM (`ui_8_PWM_ON_Flag`) | tak, `flags` bit |
| hardware MOE | tak, `status_flags` bit |
| `bridge_lifecycle` | tak, offset 7, **UWAGA: podczas S7/autodetect ta wartość jest myląca (§1, D4) — filtrować próbki z autodetect przy analizie** |
| `neutral_dwell_active`/`counter` | tak, `status_flags` bit + offset 41 |
| `current_cal_foc_allowed` | tak, `status_flags` bit — **oczekiwana wartość: zawsze 1 pod domyślną (nie-STRICT) polityką (§8); jeśli log pokaże 0, to sam w sobie jest odkryciem wykraczającym poza tę kartę** |
| `rotor_direction` | tak, offset 40 |
| `angle_hall`, `angle_absolute` | tak, offset 36/38 |
| Hall state | tak, offset 8 |
| ERPS | tak, offset 28 |
| PI q/d | tak, offset 24/26 |

**Nic dodatkowego nie jest potrzebne do potwierdzenia hipotez D1/D2/§16 na tej karcie** —
FW-122 v2 jest kompletny dla tego celu. Jedyny brakujący element to **czas trwania** stanu
`pwm_cutoff_active`/interpolacji w milisekundach między próbkami (250 Hz próbkowanie = 4 ms
odstęp; okno D2 to 10 ms, czyli 2-3 próbki na przejście — wystarczające do złapania, ale na
granicy rozdzielczości; jeśli D2 okaże się kluczowe, warto rozważyć wyzwalacz zdarzeniowy na
`pwm_cutoff_active` zbocze zamiast/obok stałej częstotliwości 250 Hz — **to jest propozycja
na przyszłą kartę, nie wdrażana tutaj**).

**Oczekiwane ślady:**

```
GOOD TRACE (normalny restart po pełnym zatrzymaniu):
  ... PWM=0 MOE=0 lifecycle=IDLE (lub PWM=0 MOE=1 pwm_cutoff w trakcie) ...
  → final_iq>0, PWM=1 (kilka próbek później) MOE=1 lifecycle=NEUTRAL_COMMIT..RUN
  → iq_actual rośnie w kierunku final_iq w ciągu kilku-kilkunastu próbek

BAD QUICK RESTART (D2, przewidziane przez tę kartę):
  final_iq>0, PWM=0 (jeszcze), MOE=1, status_flags.pwm_cutoff_active=1
  → następna próbka: PWM=1, MOE=1, lifecycle=NEUTRAL_COMMIT/MOE_ON
  → to NIE jest błąd — to jest D2, oczekiwany i wyjaśniony w §6. Odróżnia się od CASE B
    tym, że `bridge_lifecycle` postępuje normalnie zaraz po zaobserwowaniu race, zamiast
    utykać.

BAD PERSISTENT NO-ASSIST (kandydat #1, §16):
  final_iq==0 przez wiele kolejnych próbek mimo obecności torque/cadence w kontekście A
  → to jest CASE A, poza power-stage, punkt wyjścia do osobnej karty ride-control

BAD STUCK FOC (kandydat #2, §16):
  final_iq>0, PWM=1, MOE=1, lifecycle=RUN, current_cal_foc_allowed=1,
  ale |iq_actual| << final_iq przez wiele kolejnych próbek
  → to jest CASE C, poza power-stage lifecycle, punkt wyjścia do audytu FOC/PI przy niskim ERPS
```

---

## 20. FUTURE HOST TESTS (do zaprojektowania, NIE wdrażane w tej karcie)

| ID | Scenariusz | Oczekiwana sekwencja lifecycle | Oczekiwane MOE | Oczekiwane PWM_FLAG | Oczekiwane Iq |
|---|---|---|---|---|---|
| A | cold start | IDLE→NEUTRAL_COMMIT→MOE_ON→NEUTRAL_DWELL→FOC_RELEASE→RUN | 0→1 (raz) | 0→1 | 0→ramp→target |
| B | normal stop (rotor już stał ≥1s, Iq=0) | RUN→(half_rot trigger)→SOFT_CUTOFF(10ms)→IDLE | 1→0 (raz, po 10ms) | 1→0 (na starcie cutoff) | już 0 przed triggerem |
| C | quick re-demand 5 ms po rozpoczęciu SOFT_CUTOFF (**D2**) | RUN→SOFT_CUTOFF(przerwane @5ms)→NEUTRAL_COMMIT→...→RUN | pozostaje 1 (brak toggle) | 0→1 w ciągu tej samej pętli | krótki spadek do wartości pośredniej z interpolacji, potem ramp od 0 |
| D | quick re-demand tuż przed końcem SOFT_CUTOFF (@9ms) | jw., praktycznie identycznie jak C | pozostaje 1 | jw. | jw., mniejszy spadek |
| E | re-demand dokładnie w takcie MOE OFF (10ms, wyścig z zapisem `pwm_cutoff_active=0` przy 1384 vs main-loop check przy 1292 w tym samym takcie) | zależne od kolejności w danym takcie main loop — **do zweryfikowania testem hosta, czy `if(pwm_cutoff_active)` (1382) i `if(MS.i_q_setpoint>0...)` (1292) w tej samej iteracji mogą się „minąć"** — **OPEN, nieprzetestowane w tej karcie** | — | — | — |
| F | rolling restart z `rotor_dir` ważnym | nie dotyczy — mostek nie wyłącza się podczas obrotu (§7) | — | — | — |
| G | rolling restart z `rotor_dir` resetowanym | jw., scenariusz niewystępujący w obecnej architekturze | — | — | — |
| H | hamulec (hard stop) | rampa 200 ms (FW-037) → ten sam STOP flow co B | jw. jak B | jw. | rampa do 0 przez safety_cut, potem jak B |
| I | nadprąd | natychmiastowe MOE OFF, `while(1)`, bez powrotu do lifecycle | 1→0 (raz, bez odzyskania) | nietknięte (zawieszenie) | — |
| J | current_cal zablokowany start (`STRICT` + `HARD_FAILED`) | IDLE na stałe, bez przejścia do NEUTRAL_COMMIT | 0 | 0 | final_iq>0 ale start nigdy się nie zaczyna |

Scenariusz **E** jest jedynym niedomkniętym elementem z klasycznej listy zamówienia
(quick re-demand tuż przed MOE OFF) — audyt statyczny kodu **nie rozstrzyga jednoznacznie**
kolejności wykonania dwóch niezależnych bloków `if` w tej samej iteracji main loop bez
prześledzenia wygenerowanego kodu maszynowego lub testu hosta na symulowanym stanie.
**OPEN**, zalecane jako pierwszy test hosta przed jakąkolwiek zmianą kodu.

---

## 21. SAFE PATCH PLAN (projekt, NIE wdrożony)

Wyłącznie jako propozycja do osobnej akceptacji (karta FW-125 lub kolejny numer), **zgodnie z
zamówieniem — nie wdrażana teraz**:

1. **Skrócić D2 do zera**: w bloku `main.c:1292-1330`, jeśli `pwm_cutoff_active==1` (a nie
   `!ui_8_PWM_ON_Flag`), zamiast pełnej sekwencji `NEUTRAL_COMMIT→MOE_ON→NEUTRAL_DWELL`,
   po prostu: `pwm_cutoff_active=0; bridge_lifecycle=RUN; ui_8_PWM_ON_Flag=1;` — MOE
   pozostaje ON (już jest), CCR wraca pod kontrolę FOC od następnego cyklu ISR, bez
   dodatkowego dwell (dwell miał sens tylko dla przejścia MOE-off→MOE-on, którego tu nie ma).
   **Wymaga**: testu hosta powtarzającego scenariusze C/D/E z §20 na starym i nowym kodzie,
   plus testu na rowerze (odczuwalność „zacięcia" przed/po).
2. **Rozstrzygnąć scenariusz E** (§20) testem hosta przed jakimkolwiek patchem — jeśli
   okaże się, że re-demand może trafić dokładnie w takt zapisu `MOE DISABLE`, potrzebna jest
   analiza, czy `pwm_cutoff_active` i `MS.i_q_setpoint>0` mogą być odczytane niespójnie w
   jednej iteracji (obie zmienne są pisane/czytane wyłącznie w main loop, więc **nie ma tu
   ISR-race** — to pytanie o kolejność `if`-ów w kodzie źródłowym, nie o wielowątkowość).
3. **`autodetect()` (D4)**: dopisać `bridge_lifecycle = MOE_ON` (lub nowy stan
   `SERVICE_OPEN_LOOP`) na wejściu do autodetect i `IDLE` na wyjściu z fazy 1, żeby FW-122
   log z tego okna nie wyglądał jak niespójność. Czysto diagnostyczne, zero wpływu na
   zachowanie mostka.

Żaden z powyższych punktów nie jest wdrożony w ramach tej karty.

---

## 22. RAPORT KOŃCOWY — STRESZCZENIE

1. **START FLOW**: wspólna bramka dla wszystkich źródeł momentu (Torque/WA/throttle) przez
   `motor_core_set_command()→MS.i_q_setpoint`; STEP 2A dodaje neutral dwell zgodny ze
   stockiem. S7 (autodetect) jest jedyną odrębną ścieżką, bezpieczną, ale
   diagnostycznie mylącą.
2. **NORMAL STOP FLOW**: trigger wyłącznie po ~1 s bez impulsu Halla (realny bezruch);
   soft-cutoff dokłada 10 ms open-loop interpolacji, której stock nie ma.
3. **FAULT STOP FLOW**: nadprąd fizycznie odrębny od graceful stop; hamulec/przegrzanie/
   watchdog to warianty graceful stop, nie osobne mechanizmy.
4. **PWM FLAG vs MOE**: `PWM=0,MOE=1` jest legalnym, dwoma różnymi transientami (start-gap
   mikrosekundowy nieszkodliwy; soft-cutoff do 10 ms, źródło D2).
5. **SOFT-CUTOFF**: FOC ownership kończy się przed MOE OFF (odstępstwo od stocku/VESC), ale
   bezpiecznie, bo trigger już wymaga zmierzonego bezruchu.
6. **QUICK RE-DEMAND (D2)**: CONFIRMED możliwe, MOE nigdy nie przełącza się OFF→ON w tym
   scenariuszu (brak podwójnego kliku), ale wymusza zbędny pełny re-start zamiast wznowienia
   — do ~15 ms skutku odczuwalnego, samo-naprawiające się.
7. **ROLLING RE-ENABLE**: nie istnieje w obecnej architekturze — bridge nigdy nie wyłącza
   się podczas realnego obrotu, więc S3 sprowadza się do S1.
8. **CURRENT_CAL START BLOCK**: `foc_allowed` zapisywane raz przy boocie, nie może migać w
   trakcie jazdy pod domyślną (nie-STRICT) polityką — wykluczone jako przyczyna sporadyczna.
9. **PI RESET ORDER**: zgodna ze stockiem we wszystkich trzech ścieżkach mostka.
10. **ROTOR STATE RESET ORDER**: `i8_recent_rotor_direction` resetowany wyłącznie przy
    prawdziwym bridge-off; kąt Halla śledzony ciągle niezależnie od stanu mostka.
11. **STOCK M820 START**: neutral CCR przed MOE ON — zgodne z EVistDrive.
12. **STOCK M820 NORMAL STOP**: ramp→0 closed-loop → MOE OFF → reset PI → CCR housekeeping;
    EVistDrive dokłada nieobecny w stocku krok open-loop interpolacji przed MOE OFF.
13. **STOCK M820 FAULT STOP**: wielostopniowa, temperaturowo-kompensowana ochrona
    nadprądowa — bogatsza niż jednoprogowa ochrona EVistDrive (poza zakresem tej karty).
14. **VESC CROSS-CHECK**: „current control ownership until zero" jako zasada — stock i VESC
    zgodne niezależnie; EVistDrive odstępuje od niej w D1, z ograniczonym ryzykiem.
15. **CONFIRMED DIFFERENCES**: D1-D5, patrz §13.
16. **CLICK START ROOT-CAUSE**: STEP 2A neutral dwell — poprawny mechanizm, **niepotwierdzony
    pomiarem** (brak `VERIFIED_BENCH`).
17. **CLICK STOP ROOT-CAUSE**: SOFT_CUTOFF — poprawny mechanizm, **niepotwierdzony na
    rowerze po rewercie FW-093**.
18. **NO-ASSIST ROOT-CAUSE**: najbardziej prawdopodobne źródło leży **poza power-stage**
    (CASE A ride-control lub CASE C FOC/PI przy niskim ERPS); D2 może tłumaczyć krótkie
    zacięcia, nie trwały brak wspomagania.
19. **RECOMMENDED LIFECYCLE**: obecny wystarczający architektonicznie; jedyna sensowna
    zmiana to skrócenie D2 (§21 pkt 1), nie przebudowa.
20. **REQUIRED FW-122 SIGNALS**: wszystkie potrzebne pola **już są** w schema v2 (§19).
21. **FUTURE HOST TESTS**: tabela A-J w §20; scenariusz E niedomknięty statyczną analizą.
22. **SAFE PATCH PLAN**: 3 punkty w §21, żaden nie wdrożony.
23. **MOTOR-CONTROL MODIFIED: NO.**

---

## VERDICT

**CURRENT START SEQUENCE:** PASS *(zgodna ze stockiem architektonicznie; STEP 2A
niepotwierdzony pomiarem — UNCERTAIN tylko co do wartości `START_NEUTRAL_DWELL_CYCLES=4`,
nie co do struktury)*

**CURRENT NORMAL STOP SEQUENCE:** NEEDS CHANGE (drobna) — D1/D2 to zidentyfikowane,
ograniczone odstępstwo od stocku/VESC; propozycja poprawki w §21 pkt 1, niewdrożona

**QUICK RE-DEMAND HANDLING:** BUG (niski impact) — D2 jest realnym, zbędnym re-startem
zamiast płynnej kontynuacji; nie powoduje kliku MOE ani utraty bezpieczeństwa, ale jest
źródłem zbędnej ~10-15 ms przerwy

**ROLLING RE-ENABLE:** PASS — architektonicznie nieistotne w obecnym firmwarze (mostek nigdy
nie wyłącza się podczas realnego obrotu)

**SOFTWARE FLAG / MOE CONSISTENCY:** PASS — wszystkie kombinacje z tabeli §4 są legalne,
zrozumiane i ograniczone czasowo

**LIKELY CLICK ROOT CAUSE:** już zaadresowany w kodzie (STEP 2A dla startu, SOFT_CUTOFF dla
stopu) — **niepotwierdzony pomiarem/jazdą**; jeśli klik nadal występuje mimo tych
mechanizmów, patrz kandydaci w §14/§15 (wartość dwell, próg 1 s, D2 mylony z klikiem stopu)

**POSSIBLE NO-ASSIST CONNECTION:** UNCERTAIN, przechylone w stronę NIE dla trwałego braku
wspomagania (mechanizm D2 samo-naprawia się w ≤15 ms) — najbardziej prawdopodobna przyczyna
leży poza power-stage lifecycle; wymaga logu FW-122 na rowerze do rozstrzygnięcia CASE A vs C

**SAFE TO IMPLEMENT BEFORE REAL-BIKE LOG:** N/A — ta karta nic nie implementuje. Propozycja
z §21 jest bezpieczna do zaprojektowania i przetestowania hostowo, ale **nie do wgrania bez
osobnej akceptacji i bez rozstrzygnięcia scenariusza E**.

**STOP.**
