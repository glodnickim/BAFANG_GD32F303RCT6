# FW-128 PRE-AUDIT — sterowanie prądem i limitery

**Status:** wyłącznie audyt źródeł i projekt architektury. **Zero zmian w kodzie, zero buildów, zero testów sprzętowych.**
**Wejście:** FW-126 ZAMKNIĘTE · FW-127 ZAMKNIĘTE (akwizycja zamrożona) · commit `e1edca4`

> **Uwaga o materiale wejściowym.** Dokument *„BAFANG M820 STOCK CURRENT CONTROL / LIMITER
> REVERSE — FW-128 DESIGN INPUT — FINAL"* **nie znajduje się w repozytorium**. Graf stocku (§B)
> zbudowałem wyłącznie z **dziesięciu zamrożonych wniosków podanych w zleceniu**, a nie z
> dokumentu, który mógłbym niezależnie zweryfikować. Wszystko w §A i §D pochodzi z kodu
> eVistDrive, który przeczytałem.

---

## A. GRAF STEROWANIA eVistDrive — stan faktyczny

```
rider / torque                    assist_modes, assist_start, extended_boost
        │                         ride_control.c
        ▼
   iq_target
        │
        ▼
assist_limits_apply()             src/assist_limits.c  — KASKADA map(), nie min()
   map(voltage_raw, Vmin, Vmin+176, 0, iq_request)
   map(temp_c,      75,   90,        limited, 0)
   map(speed_x100,  limit,limit+200, limited, 0)     (tylko legal && !offroad && !walk)
        │
        ▼
assist_dynamics_apply()           rampa; iq_scale = phase_current_max_scaled
        │
        ▼
motor_core_set_command()          motor_core.c:22  — JEDYNY producent
        ▼
   MS.i_q_setpoint
        │
        ▼
runPIcontrol()                    main.c:3509
   ├─ BC_limit_flag  (main.c:3512-3515)
   ├─ PI_iq  ← feedback PRZEŁĄCZANY  (main.c:3517-3527)   ✘ BUG
   ├─ PI_id  ← MS.i_d, setpoint MS.i_d_setpoint
   └─ limiter kołowy → MS.u_d / MS.u_q                     ✘ brak anti-windup
        │
        ▼
arm_inv_park_q31 → svpwm → switchtime → [FW-127 zamrożone]
```

### Własność sygnałów (z kodu, nie z nazw)

| sygnał | typ / skala | producent | konsument | reset | uwaga |
|---|---|---|---|---|---|
| `MS.i_q` | q31→int, filtr `>>3` | `FOC.c:140` | `runPIcontrol` | `FOC.c:73` gdy brak Halla | jedyny pomiar q |
| `MS.i_d` | j.w. | `FOC.c:145` | `runPIcontrol`, trip nadprądowy | j.w. | |
| `MS.i_q_setpoint` | prąd fazowy, skala `CAL_I=95` | **`motor_core.c:22` (jedyny)** | `runPIcontrol`, ~30 miejsc diagnostycznych | `motor_core.c:17` | |
| `MS.i_d_setpoint` | j.w. | `motor_core.c:23` | `PI_id.setpoint` | `motor_core.c:18` | **przypisuje się sam** — patrz D6 |
| `MS.Battery_Current` | **mA** | `main.c:2319` | `BC_limit_flag`, `PI_iq` w trybie limitu | — | |
| `MP.battery_current_max` | **mA** (`Para1[1]*1000`) | `parser.c:161` | j.w. | `main.c:924` | |
| `BC_limit_flag` | FlagStatus | `main.c:3513/3515` | `runPIcontrol`, diagnostyka | brak | |
| `phase_current_max_scaled` | prąd fazowy | `main.c:1156/1229` | **tylko** `iq_scale` rampy | `main.c:524` | nigdy nie porównywany z pomiarem |
| `PI_iq.out` / `PI_id.out` | napięcie, skala 2048 | `FOC.c PI_control` | `q31_u_q/d_temp` | brak przy saturacji | **żądane** Uq/Ud |
| `MS.u_q` / `MS.u_d` | j.w. | limiter kołowy `main.c:3538-3545` | `arm_inv_park_q31` | `main.c:1333` | **zastosowane** Uq/Ud |
| `MS.u_abs` | j.w., limit `_U_MAX=1920` | `main.c:3536` | limiter kołowy, **warunek wyjścia BC** | `main.c:1333` | |

**Brak w ogóle:** `Iq_requested`, `Iq_allowed`, `Iq_ramped` jako osobne, nazwane wielkości.
Istnieje jedna zmienna `iq_target`, przepuszczana przez funkcje i nadpisywana w miejscu.
**Nie ma kanonicznego `Iq_allowed`.**

---

## B. GRAF STOCKU (z zamrożonych wniosków, nie z dokumentu)

```
oś0 = d, oś1 = q                          (wniosek 1)
prądy fazowe → Clarke → Park → Id/Iq       (wniosek 2)

q:  Iq_ref → PI(feedback = zmierzony Iq) → Uq          (wniosek 3)
d:  Ud generowany BEZPOŚREDNIO z komendy, brak szybkiego PI Id  (wniosek 3)

limiter baterii: OSOBNY PI, feedback = zmierzony prąd baterii,
                 wyjście tylko REDUKUJE żądanie,
                 PI prądu q ZAWSZE trzyma zmierzony Iq        (wniosek 4)

ochrona fazowa: T1 łagodna → T2 mocna → T3 podtrzymane = twarde wyłączenie  (5)
rampy zależne od stanu (6) · clampy całki i wyjścia PI (7)
limiter wektora radialny/kołowy (8) · brak back-calculation (9)
reset całek przed startem (10)
```

---

## C. STOCK vs eVistDrive 1:1

| cecha | STOCK | eVistDrive DZIŚ | ZALECANE FW-128 | klasyfikacja |
|---|---|---|---|---|
| sterowanie Id | Ud bezpośrednio z komendy | pełne PI_id, feedback `MS.i_d`, setpoint 0 | zachować PI_id | **EVIST LEPSZY** |
| sterowanie Iq | PI, feedback zawsze zmierzony Iq | PI, **feedback przełączany** | feedback zawsze `MS.i_q` | **EVIST BUG** |
| limiter baterii | osobny PI, redukuje żądanie | wewnątrz PI_iq, podmiana feedbacku | limiter **upstream** na `Iq_allowed` | **STOCK LEPSZY** |
| limiter fazowy | T1/T2/T3 na pomiarze | **brak na pomiarze**; tylko cap żądania | dodać ochronę stopniowaną | **EVIST BUG** |
| ochrona nadprądowa | stopniowana | tylko `i_d > 4×PH_MAX` → `while(1){}` | kontrolowane wyłączenie | **EVIST BUG** |
| termika | derating | `map(temp,75,90)` w kaskadzie | jako jedno `allowed_thermal` | **JUST DIFFERENT** |
| napięcie | derating | `map(voltage,Vmin,Vmin+176)` | jako `allowed_voltage` | **JUST DIFFERENT** |
| prędkość legal | — | `map(speed,...)` | jako `allowed_speed` | **EVIST LEPSZY** |
| arbitraż | — | **kaskada sekwencyjna**, zależna od kolejności | jedno `Iq_allowed` | **SHARED LIMITATION** |
| rampa | zależna od stanu | jedna w `assist_dynamics` + osobne cięcia | jedna, jawnie posiadana | **EVIST LEPSZY** |
| reset PI | przed startem | `main.c:1330-1331` przy starcie mostka | zachować | **ZGODNE** |
| anti-windup PI | clamp całki i wyjścia | clamp całki, wyjścia, `max_step` | + świadomość saturacji | **SHARED LIMITATION** |
| saturacja wektora | radialna, brak back-calc | radialna, **brak sprzężenia do PI** | tracking/back-calculation | **SHARED LIMITATION** |
| bumpless wejście/wyjście limitu | wyjście PI tylko redukuje | **brak jakiegokolwiek** | wymagane | **EVIST BUG** |

---

## D. POTWIERDZONE BŁĘDY eVistDrive

### D1 — FEEDBACK DOMAIN SWITCHING *(priorytet 1, potwierdzone)*

`main.c:3517-3527`. Odpowiedzi na wymagane pytania:

1. **Czy `BC_limit_flag` nadal zmienia feedback `PI_iq` ze zmierzonego Iq na `Battery_Current`? — YES**
2. **Czy `PI_iq` zachowuje ten sam stan całki przy tej zmianie? — YES.** `PI_control()` nie dotyka `integral_part` przy przełączeniu; nic go nie zeruje ani nie przelicza.
3. **Czy istnieje jawny bumpless transfer? — NO.**
4. **Czy `PI_iq` może wyjść z trybu limitu z całką wypracowaną dla złej wielkości? — YES.**

**Klasyfikacja: EVIST BUG — FEEDBACK DOMAIN SWITCHING.**

Skala błędu jest policzalna, nie hipotetyczna:

| | tryb normalny | tryb limitu |
|---|---|---|
| `setpoint` | `MS.i_q_setpoint` (prąd fazowy, `CAL_I=95`) | `battery_current_max>>6` = 15000 mA >> 6 = **234** |
| `recent_value` | `MS.i_q` (prąd fazowy) | `Battery_Current>>6` (mA >> 6) |

**Inna wielkość fizyczna i inny rząd wielkości**, a `integral_part` (clamp `limit_i = 1920`)
przechodzi między nimi bez zmiany.

**Warunek wyjścia jest dodatkowo wadliwy.** `main.c:3515` testuje
`(MS.i_q_setpoint * CAL_I * MS.u_abs)>>11 < battery_current_max*0.9` — czyli **przewidywany**
prąd baterii z **komendy**, nie z pomiaru. W trybie limitu `MS.i_q_setpoint` **nie jest
redukowany** (limiter nie działa upstream), więc wyjście zależy wyłącznie od spadku `MS.u_abs`,
czyli od wyjścia tej samej pętli, której feedback został podmieniony. Sprzężenie zwrotne przez
własne wyjście, bez histerezy na wielkości mierzonej.

### D2 — brak ochrony nadprądowej osi q *(bezpieczeństwo)*

Jedyna ochrona oparta na pomiarze prądu w całym firmware to `FOC.c:148`:

```c
if(MS_FOC->i_d > (PH_CURRENT_MAX<<2)){        // 4 × 700 = 2800, tylko oś d, tylko dodatnia
    ... neutralne CCR ...
    timer_primary_output_config(TIMER0, DISABLE);
    while(1){}                                 // wewnątrz ISR
}
```

- testuje **wyłącznie `i_d`** — prąd momentotwórczy `i_q`, ten, który realnie rośnie pod
  obciążeniem, **nie jest sprawdzany nigdzie**;
- test jest **jednostronny** (`>`), więc duże ujemne `i_d` przechodzi;
- **brak debounce** — pojedyncza zaszumiona próbka zatrzaskuje;
- `while(1){}` **w obsłudze przerwania**, bez przeładowania watchdoga → wyjście wyłącznie przez
  reset watchdoga: brak zgłoszenia CAN, brak kodu na HMI, restart w trakcie jazdy.

### D3 — `phase_current_max` nigdy nie jest porównywany z pomiarem

Wszystkie użycia to cap **żądania** (`main.c:1156/1229`) i skala **rampy**
(`assist_dynamics.c:146`). Nie istnieje ścieżka „zmierzony prąd fazowy przekroczył próg".

### D4 — limiter baterii działa wewnątrz pętli prądu, nie na żądaniu

Wszystkie pozostałe limitery (napięcie, termika, prędkość) redukują żądanie **przed** PI.
Limiter baterii jako jedyny sięga do wnętrza regulatora. To źródło D1.

### D5 — brak anti-windup przy saturacji wektora

`main.c:3536-3545`: limiter kołowy pisze `MS.u_q`/`MS.u_d`, ale `PI_iq.out` i `PI_id.out`
zachowują wartości **sprzed** ograniczenia. Ani całka, ani `out` nie wiedzą o saturacji.
Przy nasyceniu całka rośnie dalej aż do `limit_i`, a `max_step` odmierza kroki od
nieograniczonego `out`.

### D6 — `MS.i_d_setpoint` przypisuje się sam

`main.c:2883` `.current_id = MS.i_d_setpoint` → `ride_control.c:1000` `.id_target = input->current_id`
→ `motor_core.c:23` `state->i_d_setpoint = command->id_target`. Pętla bez producenta.
Realnie zapisują je tylko `stop_command` (0) i `autodetect()` (200). Działa, bo 0 jest właściwą
wartością — ale własność jest fikcyjna.

### D7 — arbitraż zależny od kolejności

`assist_limits_apply()` to sekwencja `map()`, gdzie każdy kolejny interpoluje **wynik
poprzedniego** w dół do zera. Dwa częściowe limity składają się multiplikatywnie, ale nigdzie
nie jest to powiedziane, a wynik zależy od kolejności wywołań. Nie ma jednej wielkości
`Iq_allowed` ani żadnego zapisu, **który** limiter jest wiążący.

---

## E. WZORCE STOCKU WARTE PRZENIESIENIA

1. **Limiter baterii jako osobny regulator działający na żądanie** — nie na feedback pętli prądu.
   To rozwiązuje D1 u źródła, a nie łatką.
2. **Stopniowana ochrona fazowa** (łagodna → mocna → podtrzymane wyłączenie) zamiast jednego
   zatrzasku.
3. **Zasada „wyjście limitera może tylko redukować"** — czyni wejście/wyjście z limitu bumpless
   z definicji.

**Nie przenosić:** stałych, skal, `Ud` bezpośrednio z komendy (eVistDrive ma lepsze PI_id),
progów T1/T2/T3, promienia 30800, kroków −5/−500.

## F. WZORCE eVistDrive DO ZACHOWANIA

- pełne PI_id z pomiarem `MS.i_d` (stock tego nie ma),
- limiter prędkości legal z rozróżnieniem pedał/nie-pedał,
- jedna rampa w `assist_dynamics` z ownership czasu narastania/opadania per profil,
- reset PI przy starcie mostka (`main.c:1330-1331`) — spójny z FW-117/126.7/127.

## G. OGRANICZENIA WSPÓLNE — do poprawienia ponad oba

1. **Brak sprzężenia saturacji wektora do PI** (stock też go nie ma, wniosek 9). Tracking
   anti-windup jest tańszy niż skutki windupu i rozwiązuje to raz dla obu osi.
2. **Brak jednej kanonicznej wielkości `Iq_allowed`** i brak informacji, który limiter jest
   wiążący — u obu.

---

## H. ZALECANA ARCHITEKTURA FW-128

```
iq_requested
     │
     ├── allowed_battery   ← osobny, wolny limiter na ZMIERZONYM prądzie baterii
     ├── allowed_phase     ← stopniowana ochrona na ZMIERZONYM Iq
     ├── allowed_thermal
     ├── allowed_voltage
     └── allowed_speed
     │
     ▼
  Iq_allowed = min(...)        + zapis, KTÓRY limiter jest wiążący
     │
     ▼
  jedna rampa → Iq_ref
     │
     ▼
  PI_iq   feedback = MS.i_q  ZAWSZE, bez wyjątków
     │
     ▼
  Uq_requested ─┐
  Ud_requested ─┴→ limiter kołowy → Ud/Uq_applied
                        │
                        └→ back-calculation → korekta całek obu PI
```

Zasada nadrzędna: **limiter nigdy nie sięga do wnętrza regulatora.** Limitery produkują
pozwolenie; regulator prądu ma jedno zadanie i jeden feedback.

---

## I. PROPONOWANE KARTY

| karta | cel | usuwa | nowy producent → konsument | reset / stan | testy hosta | sprzęt |
|---|---|---|---|---|---|---|
| **FW-128A** | kanoniczny łańcuch Iq: nazwane `iq_requested` / `Iq_allowed` / `Iq_ref`, zapis wiążącego limitera | anonimowe nadpisywanie `iq_target` w miejscu | nowy moduł `iq_chain` → `motor_core_set_command` | bez stanu poza zapisem wiążącego | tak | nie |
| **FW-128B** | **limiter baterii upstream**, usunięcie podmiany feedbacku | `main.c:3517-3527`, `BC_limit_flag` jako przełącznik | `battery_limiter` → `allowed_battery` | reset przy starcie mostka | tak | nie |
| **FW-128C** | stopniowana ochrona fazowa na **zmierzonym** Iq; kontrolowane wyłączenie zamiast `while(1)` | `FOC.c:148-153` | `phase_guard` → `allowed_phase` + ścieżka fault | debounce, reset przy starcie | tak | nie |
| **FW-128D** | arbitraż: jedno `Iq_allowed` | kaskada `map()` w `assist_limits.c` | `limit_arbiter` → `Iq_allowed` | bezstanowy | tak | nie |
| **FW-128E** | jedna rampa, bumpless wejście/wyjście limitu | rozproszone cięcia | `assist_dynamics` → `Iq_ref` | reset przy starcie | tak | nie |
| **FW-128F** | anti-windup od saturacji wektora (back-calculation) | brak sprzężenia | limiter kołowy → korekta całek | reset przy starcie | tak | nie |

**Kolejność wymuszona:** A przed D (nie ma czego arbitrażować bez nazwanych wielkości),
B i C przed D (produkują pozwolenia), E po D, F niezależne.

**Granica rollbacku:** każda karta to osobny commit; A/D/E są czysto strukturalne,
B/C/F zmieniają zachowanie i to one wymagają walidacji sprzętowej.

## J. PLAN TESTÓW HOSTA

- **A:** `Iq_allowed ≤ iq_requested` zawsze; wiążący limiter zgodny z minimum; brak utraty znaku
- **B:** limiter tylko redukuje; wejście/wyjście bumpless; **feedback `PI_iq` niezmienny w każdym
  scenariuszu** (test regresji na D1); brak windupu przy podtrzymanym limicie
- **C:** T1/T2/T3 z debounce; pojedyncza zaszumiona próbka **nie** wyzwala; fault → kontrolowane
  wyłączenie, nie pętla; oś q pokryta
- **D:** min() z N pozwoleń; niezależność od kolejności; równoczesne limity
- **E:** monotoniczność rampy; brak skoku przy zmianie wiążącego limitera
- **F:** całka nie rośnie przy nasyceniu; powrót bez przeregulowania; obie osie

## K. JEDEN SKONSOLIDOWANY TEST SPRZĘTOWY (projekt, nie wykonanie)

Po **A+B+C+D+E+F** i wszystkich testach hosta. Jeden obraz, jeden flash, jedna sesja, jeden log.
Instrumentacja pasywna, rozszerzenie istniejącego 0x602F: histogram wiążącego limitera, czas
w limicie baterii, `max |i_q|` i `max |i_d|`, liczniki T1/T2/T3, licznik saturacji wektora,
peak całek obu PI. Sesja ma naturalnie objąć: start, małe obciążenie, podjazd do limitu baterii,
zbieg, normalny stop.

---

## L. PYTANIA OTWARTE

1. **Dokumentu reverse nie ma w repo.** Dziesięć wniosków przyjmuję jako zamrożone, ale nie mogę
   ich zweryfikować ani sięgnąć po szczegóły, gdyby projekt B ich potrzebował.
2. **Skala `MS.i_q` względem prądu fazowego w amperach** nie została w tym audycie ustalona —
   potrzebna do progów T1/T2/T3 w FW-128C. Wyprowadzalna z `CAL_I` i toru pomiarowego, ale
   wymaga osobnego wyliczenia.
3. **Czy `i8_reverse_flag`/`MP.reverse` mają wchodzić do `Iq_allowed`, czy dopiero do `Iq_ref`** —
   dziś mnożą setpoint wewnątrz `runPIcontrol`.
4. **Czy `while(1)` w `FOC.c` był świadomym wyborem** (twardy reset jako ostateczność), czy
   niedopatrzeniem. Nie zmienia zalecenia, ale zmienia ton karty C.

---

## M. WERDYKT

> **CZY JEST DOŚĆ DOWODÓW, ŻEBY WDROŻYĆ FW-128?**
>
> ## YES

Wszystkie pięć warunków STOP jest rozstrzygniętych z kodu:

| warunek | status |
|---|---|
| własność feedbacku `PI_iq` | **rozstrzygnięte** — przełączany, `main.c:3517-3527` |
| zachowanie `BC_limit_flag` | **rozstrzygnięte** — wejście/wyjście/histereza wyśledzone |
| producent końcowego `i_q_setpoint` | **rozstrzygnięte** — `motor_core.c:22`, jedyny |
| umiejscowienie limitera fazowego | **rozstrzygnięte** — cap żądania + skala rampy; brak na pomiarze |
| własność żądane vs zastosowane napięcie | **rozstrzygnięte** — `PI_c->out` vs `MS.u_q/u_d`, rozjeżdżają się bez sprzężenia |

**Zastrzeżenie:** FW-128C potrzebuje skali `MS.i_q` (§L2) **przed** ustaleniem progów. To
wyliczenie, nie test sprzętowy — ale karty C nie wolno zamknąć bez niego.

**STOP. Nie implementuję.**
