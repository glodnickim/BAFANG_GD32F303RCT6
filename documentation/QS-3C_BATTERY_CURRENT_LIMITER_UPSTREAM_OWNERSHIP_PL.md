# QS-3C — bateryjny limiter prądu jako UPSTREAM Iq-cap (przed finalnym slew)

## 1. Baseline commit

- Baseline: `08a62c9bcb29241e2ec5a6e2a5ad3a777001ac30` (`QS-3B: document 4 kHz vs 16 kHz slew decision`).
- Ta karta **nie alokuje** nowego canonical i **nie staguje/nie commituje** niczego. Po akceptacji
  review zostanie zacommitowana wyłącznie zawartość QS-3C (bez niezwiązanych plików SOC).
- Canonical NORMAL pozostaje `0.0469`. Zgodnie z regułą QS-3B żaden nowy canonical nie jest
  alokowany, dopóki zmiana nie zostanie zaakceptowana/testowana na sprzęcie.
- Drzewo zawiera niezależne prace SOC (`inc/soc_coulomb.h`, `src/soc_coulomb.c`,
  `tests/host/fw130a_soc_timebase_host.c`, edycje `inc/battery_current.h`, `inc/config.h`,
  `src/battery_current.c`, `build_firmware.ps1`, `scripts/sources-m820.txt`). Te pliki NIE są
  częścią QS-3C; zakres karty to wyłącznie limiter bateryjny i ownerstwo PI_iq.

**Werdykt karty: PASS / IMPLEMENTED (warunkowo, bez hardware).** Defekt opisany w FW-128A —
zamiana domeny feedback/setpoint PI_iq — jest usunięty: limiter bateryjny przestał być PI na prąd
baterii i stał się **upstream min-cap w domenie Iq PRZED finalnym slew** (`assist_dynamics_apply`).
PI_iq pozostaje zawsze regulatorem q-osiowego prądu silnika z `PI_iq.setpoint = MS.i_q_setpoint`.

## 2. Defekt (FW-128A) i jego sedno

Legacy limiter bateryjny zamieniał domenę regulatora PI_iq. Zamiast regulatora prądu q-osiowego
silnika, PI_iq stawał się regulatorem prądu baterii:

```text
// legacy src/main.c:3650-3653 (przed QS-3C)
PI_iq.recent_value = MP.reverse*i8_reverse_flag*MS.Battery_Current>>6;  // feedback = prąd baterii (mA>>6)
PI_iq.setpoint     = MP.reverse*i8_reverse_flag*(MP.battery_current_max>>6); // setpoint = limit mA>>6
```

Efekt:

1. **Zerwanie invariantu domeny.** Wszystkie pozostałe tryby (normalna jazda, WA, kalibracja)
   patrzą na `PI_iq.recent_value = MS.i_q` (prąd q-osiowy silnika). Gdy BC aktywował, ten sam
   regulator nagle liczył błąd prądu baterii. `MS.i_q` był wówczas ignorowany, a prawdziwy prąd
   silnika nie był regulowany — jedynie ograniczany pośrednio.
2. **Brak współdzielenia z finalnym slew ownerem.** Normalny `assist_dynamics_apply`
   produkuje `MS.i_q_setpoint` w domenie Iq. Legacy override bezwarunkowo nadpisywał
   `PI_iq.setpoint` prądem baterii, omijając `Iq_allowed`, finalny ramp i `MS.i_q_setpoint`.
3. **Zakaz migracji 16 kHz (QS-3D).** Dopóki `PI_iq.setpoint` nie pochodzi zawsze z pojedynczej
   domeny Iq i jednego finalnego ownera, nie istnieje gwarancja „Iq_allowed -> one slew -> PI”.

## 3. Weteryzja review QS-3C (blokery i dyrektywy)

Recenzja wydała werdykt **NOT final PASS** i nakazała (autoritative, nadrzędne nad wcześniejszym
projektem z post-slew clampem):

1. **Cap PRZED `assist_dynamics_apply()`:**
   `Iq_requested -> ...caps -> battery_iq_cap -> Iq_allowed -> assist_dynamics_apply -> MS.i_q_setpoint -> PI_iq.setpoint`.
2. **Normalny runtime: `PI_iq.setpoint = MS.i_q_setpoint`** — NIE `min(MS.i_q_setpoint, battery_cap)`.
3. **Brak post-slew / ukrytego clamp bateryjnego**; żaden drugi downstream slew/clamp.
4. **Klasyfikacja odpowiedzi** ograniczania bateryjnego (A/B/C/D); szybka ścieżka ma trafiać do
   istniejącego finalnego ownera slew (np. `ramp_down_fast`, `profile_release_ms`), a nie do
   nowego clampu.
5. **Dowód równania capa** z istniejącego predykatora prądu baterii (9 pod-punktów).
6. **Guards statyczne**: cap nie pisze `PI_iq.setpoint`; `pi_iq_apply_inputs()` bez
   `min(iq_ref, battery_cap)`; źródłem setpointa jest `MS.i_q_setpoint`; cap wchodzi w `Iq_allowed`
   przed finalnym slew; `PI_iq.recent_value = MS.i_q`; dokładnie JEDEN finalny Iq owner;
   ograniczanie bateryjne pozostaje skuteczne.
7. **Hardware: NO**. Jeśli poprawione + host/static/build przechodzą → alokacja next canonical
   > `0.0469` przez alokator wersji (bez ponownego użycia `0.0469`).

Niniejsza karta implementuje wszystkie powyższe.

## 4. Model fizyczny capa

Power balance do przyjęcia (ten sam związek, na którym opierała się legacy exit condition):

```text
battery_current ~ Iq * CAL_I * u_abs / 2048
```

gdzie:

- `CAL_I = 95` — skala prądu fazowego na jednostkę (`inc/config.h`), ten sam stały, który
  FW-128C0 pokazał, że NIE jest „mA na jednostkę” — jest prądem fazowym na jednostkę Iq;
- `u_abs` — magnituda wektora napięcia, skalowana do `2048 = 2^11` (`_U_MAX` w `FOC.h`,
  domena duty `u/2048`).

Po rozwiązaniu za Iq, które pobrałoby dokładnie `battery_current_max`:

```text
Iq_max = battery_current_max * 2048 / (CAL_I * u_abs)
```

Jest to prosty, fizycznie uzasadniony przelicznik: zrównuje (w przybliżeniu) chwilową moc DC
pobieraną przez zadany Iq przy danym wypełnieniu z limitem prądu baterii. Legacy exit robiła
dokładnie ten sam rachunek (`(MS.i_q_setpoint*CAL_I*MS.u_abs)>>11 < battery_current_max*0.9`),
więc karta nie wynajduje nowej kalibracji — używa relacji już istniejącej w źródle.

### 4.1 Dowód równania capa (wymagany przez review, 9 pod-punktów)

1. **Model predykatora** (legacy exit, `MS.Battery_Current` przy `src/main.c:2367`):
   związek `battery_current ≈ Iq * CAL_I * u_abs / 2048` ma tę samą stałą `CAL_I` co przelicznik
   prądu fazowego na jednostkę Iq — `battery_current` jest linią DC uzyskaną z tego samego
   `iq_setpoint * CAL_I / _U_MAX * u_abs` (u_abs w domenie `2048`).
2. **Przekształcenie algebraicne** względem `Iq`: `Iq ≈ battery_current * 2048 / (CAL_I * u_abs)`.
3. **Podstawienie limitu**: chwilowy `Iq` nie może przekroczyć wartości, która dałaby
   `battery_current == battery_current_max`, więc
   `Iq_max = battery_current_max * 2048 / (CAL_I * u_abs)`.
4. **Zgodność jednostek**: `battery_current` i `battery_current_max` w mA; `Iq` i `Iq_max` w
   jednostkach Iq; `CAL_I` w jednostkach prądu fazowego na jednostkę Iq; `u_abs/2048` bezwymiarowe
   (wypełnienie). Wynik-linia `Iq_max` jest tej samej domeny co `Iq_requested`.
5. **Unikalna stała**: nie wprowadza nowych współczynników kalibracji — używa `CAL_I`,
   `u_abs`, `battery_current_max`, `PH_CURRENT_MAX`, wszystkie już w drzewie.
6. **Reguła „im wyższy u_abs, tym niższy dopuszczalny Iq”**: przy wysokim wypełnieniu ten sam
   limit mocy zmusza do niższego prądu fazowego — zgodne z fizyką mocy DC (monotoniczny spadek
   `Iq_max` ze wzrostem `u_abs` dla ustalonego `battery_current_max`).
7. **Brak dzielenia przez zero / degeneracji**: `u_abs<=0` lub `cal_i<=0` → brak sensownego
   dzielnika → „brak ograniczenia” (`phase_current_max`); `u_abs==0` = silnik nie napędzany.
8. **Zacisk deterministyczny**: `Iq_max` zaciskany do `[0, phase_current_max]` oraz do bieżącego
   żądania `iq_ref` (bumpless demand); `phase_current_max<1` podnoszone do 1, by `min()` był
   deterministyczny.
9. **Skuteczność ograniczania**: aktywny cap `Iq_max` jest najwyżej prądem pobieranym dokładnie
   `battery_current_max`, więc `min(pid_domain Iq_requested, Iq_max)` nie może wyprowadzić
   baterii ponad limit — limiter pozostaje NA SKUTECZNYM torze żądania przed finalnym slew.

Konfiguracja (niezmieniona): `BATTERYCURRENT_MAX=15000` mA (limit), `CAL_BAT_I=37.0`,
`PH_CURRENT_MAX=700` (`inc/config.h`); `MS.Battery_Current =
(int32_t)((float)battery_current_filtered_adc()*CAL_BAT_I)` (`src/main.c:2367`).

## 5. Nowe ownerstwo limitera (QS-3C, upstream)

### Tor normalnej jazdy (po zmianie — cap PRZED finalnym slew)

```text
rider_input / assist mode
  -> assist_modes_calculate()                         src/assist_modes.c
  -> ride_control lokalny iq_target                   src/ride_control.c
  -> latch/floor/boost/hard-cut                       src/ride_control.c
  -> Iq_requested snapshot                            src/ride_control.c
  -> assist_limits_apply() pedal + throttle           src/ride_control.c
  -> battery_iq_cap_update(...)          <= QS-3C cap, src/ride_control.c:1062
        iq_target = min(iq_target, iq_battery_cap)      src/ride_control.c:1066-1067
  -> iq_chain_note_allowed(iq_target)   // Iq_allowed, src/ride_control.c:1071
  -> assist_dynamics_apply()  [finalny 4-kHz slew]      src/ride_control.c:1073
  -> motor_core_set_command()  -> MS.i_q_setpoint     src/motor_core.c
  -> pi_iq_apply_inputs()                             src/main.c:3638-3641
        PI_iq.recent_value = MS.i_q                   src/main.c:3640
        PI_iq.setpoint     = MP.reverse*i8_reverse_flag*MS.i_q_setpoint  src/main.c:3641
  -> PI_control(&PI_iq)                               src/main.c:3657
```

Cap jest jedynym miejscem wejścia limitera bateryjnego do łańcucha: wchodzi do `Iq_allowed`
**przed** `iq_chain_note_allowed` i przed finalnym slew `assist_dynamics_apply`. To jest dokładnie
położenie nakazane przez review (`battery_iq_cap` -> `Iq_allowed` -> `assist_dynamics_apply`).

### Nowy moduł

`inc/battery_iq_cap.h` + `src/battery_iq_cap.c`: `battery_iq_cap_update()` i
`battery_iq_cap_reset()`. Moduł **wyłącznie produkuje Iq-domain cap**; nigdy nie pisze
`PI_iq.setpoint` ani `PI_iq.recent_value`, ani żadnego post-slew clampu.

- **Wejście** — mierzony prąd baterii przekracza `battery_current_max`.
- **Wyjście** — mierzony prąd baterii spada poniżej `90 %` limitu (histereza).
- Między pasmami zatrzask `bc_active` trzyma poprzedni stan (brak chattera).
- Aktywny: emituje `iq_battery_cap = battery_current_max*2048/(cal_i*u_abs)`, zacieśniony do
  `phase_current_max` i do bieżącego żądania (bumpless).
- Nieaktywny: emituje `phase_current_max` == „brak ograniczenia” dla `min()`.
- `u_abs<=0` lub `cal_i<=0`: brak sensownego dzielnika → „brak ograniczenia” (nie dzielenie
  przez zero).

## 6. PI_iq ownership po zmianie

**PI_iq.setpoint WRITERS (dokładnie 2):**

1. init `PI_iq.setpoint = 0` — `src/main.c:983`;
2. runtime normal Iq_ref — `src/main.c:3641`:
   `PI_iq.setpoint = MP.reverse * i8_reverse_flag * MS.i_q_setpoint;`

Dawne przypisanie 3 (override bateryjny) zostało usunięte DO CZASU. Zgodnie z dyrektywą review
normalny runtime PI_iq **nigdy** nie robi `min(MS.i_q_setpoint, battery_cap)` — źródłem jest
tylko `MS.i_q_setpoint` (z bramką reverse).

**PI_iq.recent_value WRITERS:** dokładnie 1 — `PI_iq.recent_value = MS.i_q` (`src/main.c:3640`).

**CURRENT FINAL IQ SLEW OWNERS:** `ONE` — `assist_dynamics.c` (`assist_dynamics_apply`, wywoływane
raz w `ride_control_update`). WA pozostaje wzajemnie wykluczającą się trajektorią
(`walk_speed_controller`); nie istnieją dwie finalne rampy szeregowe. JEDEN finalny Iq owner.

**BATTERY LIMITER OWNER:** `battery_iq_cap.c` — jedyny właściciel bateryjnego capa; jego stan
żyje jako `static battery_iq_cap_output_t ride_battery_cap;` w `ride_control.c` i wchodzi do
łańcucha wyłącznie upstream. Nie istnieje żaden post-slew clamp bateryjny.

## 7. Klasyfikacja odpowiedzi ograniczania bateryjnego (wymagane przez review)

Karta klasyfikuje działanie bateryjnego limitowania jako **Klasa A — ciągły limiter operacyjny**
(continuous operating limiter), nie safety-release ani catastrophic hard-fault:

| Klasa | Definicja | Zastosowanie bateryjne |
|---|---|---|
| **A — continuous limiter** | ciągłe, w pętli, wielokrotne ograniczanie do limitu operacyjnego; po usunięciu przyczyny wraca do normalnej pracy; brak latch twardy | **BATERYJNY CAP (wybrane)** — `battery_iq_cap` obniża `Iq_allowed` upstream; jazda kontynuuje na niższym Iq; `bc_active` to miękki latch histerezowy; release przy spadku poniżej 90 % |
| B — safety-release | jednorazowe zwolnienie/rozładowanie ograniczenia z powrotem do bezpiecznej wartości | — |
| C — catastrophic hard-fault | zatrzymanie/safety off; niezależna ścieżka fault | overcurrent `FOC.c`, hard-cut 200 ms — niezależne, nienaruszone |
| D — kombinacja | mieszanka powyższych | — |

**Skuteczność w obecnym torze:** cap wchodzi do `Iq_allowed` tuż przed finalnym slew, więc po
jednym cyklu 4 kHz `assist_dynamics_apply` doprowadza `MS.i_q_setpoint` do nowego, niższego
`Iq_allowed`. Czas odpowiedzi jest rzędu jednego cyklu main na poziomie napięcia zadania — bez
nowego, drugiego downstream clampu. Szybka ścieżka (gdyby kiedykolwiek była potrzebna) musiałaby
iść przez istniejące raty finalnego ownera (`ramp_down_fast`/`profile_release_ms` w
`assist_dynamics.c`), ale dla Klasy A nie jest wymagana.

## 8. Dlaczego min-cap upstream, a nie PI na prąd baterii ani post-slew clamp

- **PI na prąd baterii = drugi regulator.** Wymaga własnych Kp/Ki, antwindupu i — co
  najważniejsze — produktu dwóch regulatorów w pętli, z niejawnym timingiem 4 kHz. Odrzucony.
- **Min-cap jest bezstanowy co do dynamiki.** Nie wnosi fazy ani wzmocnienia do pętli; działa
  jak zwykły górny limit w arbitrażu `min()`, dokładnie jak limitowanie termiczne czy
  napięciowe przez obniżenie `Iq_allowed`. Bumpless: cap ewoluuje ciągle wraz z `u_abs`.
- **Dlaczego upstream, a nie w `pi_iq_apply_inputs`:** review odrzuciło post-slew clamp, bo
  clamp za finalnym slew tworzyłby drugi, niejawny slew owner (łamanie „single final Iq owner”)
  i dzielił domenę `PI_iq.setpoint`. Przeniesienie capa PRZED slew utrzymuje jeden finalny owner
  i czysty `PI_iq.setpoint = MS.i_q_setpoint`.
- **Pozostały invariant jednoźródłowy.** PI_iq widzi zawsze jedną referencję Iq od jednego
  finalnego ownera (`MS.i_q_setpoint`). To jest warunek, który otwiera migrację 16 kHz (QS-3D).

Ponieważ cap jest funkcją `u_abs` (napięcia), nie jest to „hard cut”; limiter pozostaje ciągłą,
wolną funkcją ograniczającą (Klasa A). Ścieżki twardego off (overcurrent w `FOC.c`, hard-cut
200 ms w `ride_control.c`) są nienaruszone i niezależne (Klasa C).

## 9. Histereza i bumpless

- **Entry:** `battery_current_mA > battery_current_max` (ściśle większe — zachowana semantyka
  legacy `BC_limit_flag`).
- **Exit:** `battery_current_mA < battery_current_max*9/10` (90 %); zatrzask trzyma wartość
  między pasmami.
- **Bumpless entry/re-entry:** w chwili wejścia cap jest dokładnie w punkcie, który pociągnął
  `battery_current_max`, i ewoluuje ciągle z `u_abs`. Brak skoku w wielkości, którą `min()`
  arbitruje.
- **Bumpless release:** wyjście z aktywnego stanu emituje `phase_current_max`, więc `min()`
  zwraca wtedy `iq_target` — gładkie oddanie kontroli.
- **Bumpless demand:** cap jest zacieśniany do bieżącego `iq_target`, więc moduł nie podnosi
  wartości powyżej żądania kierowcy.

## 10. Numeryka i seguridad przepełnienia

- `numerator = (double)battery_current_max * 2048`; `denominator = (double)cal_i * u_abs`;
  wynik w `double`, jedno dzielenie w skali końcowej. `int32_t` przepełniłby całkę, dlatego
  pośredni licznik/dzielnik są w `double`.
- Wynik zacisnięty do `[0, phase_current_max]`.
- `u_abs<=0` lub `cal_i<=0` → „brak ograniczenia” (`phase_current_max`), bez dzielenia przez
  zero; przy `u_abs==0` silnik nie jest napędzany, więc cap jest nieistotny.
- Ujemny (regen) prąd baterii nie zatrzaskuje aktywnego.
- `phase_current_max<1` jest podnoszony do 1, by `min()` pozostał deterministyczny.

## 11. Bezpieczeństwo (safety)

Limiter bateryjny jest **funkcją ciągłego ograniczania (Klasa A)**, nie ścieżką fault:

| Przypadek | Nowa semantyka | Klasa (bez zmian) |
|---|---|---|
| phase overcurrent `i_d > PH_CURRENT_MAX<<2` | neutral CCR, MOE OFF, lifecycle FAULT (`FOC.c:148-158`) | C — hard off, niezależny |
| brake / direction inhibit / critical overtemp / torque fault / load calibration | target 0 + firmware-owned 200 ms (`ride_control.c`) | C/B — ten sam final owner, inny rate |
| thermal/voltage/speed limiter, spadek limitu | obniża `Iq_allowed`; fin. fall dochodzi do nowego celu | A/A — niezależny |
| comm loss | bezpośredni command zero (`main.c:1332-1343`) | C — bypass |
| WA aktywny | własny Q8 slew, immediate safety ceilings | oddzielna trajektoria |
| **battery over-current** | upstream `min()` cap w domenie Iq przed slew | **A — limiter ciągły, bez hard-off** |

`BC_limit_flag` jest zachowany i nadal publikuje diagnostykę: bit `0x40` w `main.c:4292` oraz
`dg[34]` bit0 (`src/CAN_Display.c:1023`, FW-033). **Teraz** flaga jest sterowana z zatrzasku
modułu: `BC_limit_flag = ride_control_battery_limit_active() ? 1 : 0;` (`src/main.c:3011`, zaraz
po `ride_control_update`) — nie z `pi_iq_apply_inputs`. To zachowuje identyczną observację
aktywności ograniczenia jak w legacy.

## 12. Integracja z ARMED_ZERO i cold start

Bez zmian w lifecycle. ARMED_ZERO przejście `RUN -> ARMED_ZERO` nadal zależy od
`MS.i_q_setpoint==0` (`src/main.c:1453-1464`). Limiter bateryjny nie wprowadza exact-zero
w żadnym trybie: cap jest `0` tylko gdy `iq_target==0` (bo jest zaciskany do `iq_target`) — nie
generuje samodzielnego zera. Cold gate reaguje na dodatnie `MS.i_q_setpoint` (`src/main.c:1365`);
`battery_iq_cap_update` jest bezstanowo inicjalizowany zero-inicjalizacją globala
(`bc_active=false`), więc przed pierwszym napędem cap jest „brak ograniczenia”.

## 13. Relacja z finalnym slew / 4 kHz

Finalny slew pozostaje w `assist_dynamics_apply()` przy 4 kHz (`CONTROL_TIMEBASE_HZ=4000U`,
`src/assist_dynamics.c`), wywoływany dokładnie raz w `ride_control_update`. Cap bateryjny jest
jedynym miejscem wejścia limitera i siedzi PRZED tym slew, więc JEDEN finalny właściciel produkuje
`MS.i_q_setpoint`. QS-3D (przeniesienie slewa do 16 kHz) jest osobną przyszłą kartą; ta karta
usuwa największy blocker: podwójny, obco-domenowy nadawca `PI_iq.setpoint`.

## 14. Guards statyczne (wymagane przez review)

Focused host `qs3c_battery_cap_host.c` (G1–G14) i zaktualizowane `fw128a_iq_chain_host.c` (A11a/A11c/
A11d/A11e), `qs3_final_iq_slew_host.c` (T10), `fw128b0_battery_scale_host.c` (F1/F2) text-guardują:

1. **Cap moduł nie pisze `PI_iq.setpoint`** — w `battery_iq_cap.c` brak `PI_iq.setpoint` /
   `PI_iq.recent_value`.
2. **`pi_iq_apply_inputs()` bez `min(iq_ref, battery_cap)`** — brak jakiegokolwiek `min(...cap)`
   w `src/main.c`; setpoint źródłem jest tylko `MS.i_q_setpoint`.
3. **Źródło setpointa** — dokładnie 2 pisarki `PI_iq.setpoint`: init (983) + runtime `MS.i_q_setpoint`
   (3641).
4. **`PI_iq.recent_value`** — dokładnie 1 pisarka: `MS.i_q` (3640).
5. **Cap w `Iq_allowed` przed slew** — `battery_iq_cap_update(...)`, `iq_battery_cap` oraz
   `assist_dynamics_apply(` w `ride_control.c`; kolejność `cap < iq_chain_note_allowed <
   assist_dynamics_apply`; brak `min_apply` w `pi_iq_apply_inputs`.
6. **JEDEN finalny Iq owner** — pojedyncze wywołanie `assist_dynamics_apply` w `ride_control_update`;
   brak drugiego downstream clampu.
7. **`BC_limit_flag` z zatrzasku** — dokładnie 3 wystąpienia w `main.c` (deklaracja 152, zapis 3011,
   bit CAN 4292); zapis sterowany `ride_control_battery_limit_active()`.

## 15. Zmiany w source (diff summary)

- `src/ride_control.c`:
  - `#include "battery_iq_cap.h"` (linia 2);
  - `static battery_iq_cap_output_t ride_battery_cap;` (linia 100, POZA `#if CAN_DIAGNOSTICS_ENABLE`
    — zawsze kompilowane, używane i przez `ride_control_update`, i przez accessor);
  - `bool ride_control_battery_limit_active(void)` (linia 102) — zwraca `ride_battery_cap.bc_active`;
  - w `ride_control_update`, PRZED `iq_chain_note_allowed` i `assist_dynamics_apply`
    (linie ~1062-1067): `battery_iq_cap_update(input->battery_current_mA,
    input->battery_current_max, input->phase_current_max, iq_target, input->u_abs, input->cal_i,
    &ride_battery_cap);` potem `if (ride_battery_cap.iq_battery_cap < iq_target)
    { iq_target = ride_battery_cap.iq_battery_cap; }`.
- `inc/ride_control.h`: dodane pola wejściowe `battery_current_mA`, `battery_current_max`, `u_abs`,
  `cal_i`; deklaracja `ride_control_battery_limit_active`.
- `src/main.c`:
  - usunięty `#include "battery_iq_cap.h"` i global `battery_cap_out` (cap żyje w `ride_control.c`);
  - `pi_iq_apply_inputs()` (linie ~3638-3641) — CZYSTY Iq-domain:
    `PI_iq.recent_value = MS.i_q;` i `PI_iq.setpoint = MP.reverse*i8_reverse_flag*MS.i_q_setpoint;`
    — usunięty legacy override i post-slew clamp;
  - `ride_input` wypełnione `.battery_current_mA = MS.Battery_Current,
    .battery_current_max = MP.battery_current_max, .u_abs = MS.u_abs, .cal_i = CAL_I`;
  - po `ride_control_update` (linia 3011): `BC_limit_flag = ride_control_battery_limit_active() ? 1 : 0;`.
- `src/battery_iq_cap.c`, `inc/battery_iq_cap.h`: nowy moduł (opis wyżej; bez post-slew; z ostrzeżeniem
  „musi być stosowany PRZED finalnym slew”).
- `scripts/sources-m820.txt`: dodano `src/battery_iq_cap.c`.
- Testy host: `tests/host/qs3c_battery_cap_host.c` (nowy focused, G1–G14), zaktualizowane
  `fw128a_iq_chain_host.c`, `qs3_final_iq_slew_host.c`, `fw128b0_battery_scale_host.c`;
  `tests/host/run-host-tests.ps1` (dodano `src/battery_iq_cap.c` do każdego suitu kompilującego
  `ride_control.c`; `-DRIDE_CONTROL_C_PATH` dla qs3c/fw128b0).

NIEdotknięte: `ARMED_ZERO`, MOE/BLDC enable, finalny 4-kHz slew, PI Kp/Ki, ścieżki fault/safety,
release 200 ms, kalibracja, diagnostyka CAN `dg[34]`.

## 16. Testy

- Nowy focused host `qs3c_battery_cap_host.c` — realny `battery_iq_cap.c`: 18 scenariuszy +
  corner case numeryczne + source guard ownerstwa (G1–G14, w tym upstream placement, single slew,
  brak post-slew clamp) — **PASS / ALL CHECKS PASSED**.
- `fw128a_iq_chain_host.c` (canonical q-current ownership, A11a/A11c/A11d/A11e) — **PASS / ALL
  CHECKS PASSED**.
- `qs3_final_iq_slew_host.c` (T10 wiring guard) — **PASS / ALL CHECKS PASSED**.
- `fw128b0_battery_scale_host.c` (F1/F2 do nowej architektury upstream) — **PASS / ALL CHECKS
  PASSED**.
- `fw128b1_battery_timebase_host.c`, `fw128c0_physical_scale_host.c`, `fw126_7`, `fw129b`,
  `fw112.1`, `fw112.2` (kompilują/łączą `ride_control.c`) — **PASS / ALL CHECKS PASSED**.
- Cross-compile ARM (`arm-none-eabi-gcc`, Cortex-M4, `-Wall -Wextra`):
  - `battery_iq_cap.c` — EXIT 0;
  - `ride_control.c` — EXIT 0 dla `CAN_DIAGNOSTICS_ENABLE=1` ORAZ `=0`;
  - `main.c` — EXIT 0 (jedyne warningi to istniejące, niezwiązane sygnalizacje
    `soc_state_load/save` i `fw_ver` — poza zakresem QS-3C).
- Full host suite: jedyne nieudane suity to **niezależny baseline**
  `rolling_no_assist_diag_host` (schema-3 harness kontra schema-4 source — 514 FAILs, znany
  problem QS-3A/B, niedotknięty QS-3C). Wszystkie suity QS-3C/FW-128*/FW-112.1/2, FW-129B PASS.

## 17. Build / version

- Nie wykonano builda canonical, nie alokowano nowego numeru. Aktualny canonical:
  - `0.0469_M820_BL820.bin`
  - SHA-256 `8951B6997B8E4F2E536DAF9E81EFADE24293BD8C608D9D4CB15AA87E879B1564`
- Zgodnie z dyrektywą review: po akceptacji (host/static/build PASS) kolejny canonical > `0.0469`
  zostanie alokowany przez alokator wersji (bez ponownego użycia `0.0469`).
- #GIT: NIE stagowane, NIE commitowane. Po akceptacji — stage wyłącznie plików QS-3C.

## 18. Open risks

1. Model `Iq_max = battery_current_max*2048/(CAL_I*u_abs)` jest przybliżeniem power balance; na
   sprzęcie należy sprawdzić, czy uzyskany prąd baterii przy limitowanym Iq jest w akceptowalnym
   sąsiedztwie limitu `battery_current_max` we wszystkich punktach `u_abs` (niski/normalny/wysoki
   duty).
2. `u_abs` podczas opóźnienia lub przy skoku obciążenia może się szybko zmieniać; cap reaguje
   w każdym cyklu kontroli 4 kHz, więc latencja ograniczenia jest rzędu jednego cyklu main —
   do potwierdzenia dynamiką na sprzęcie.
3. Twarde overcurrent `FOC.c` pozostaje niezależnym bezpiecznikiem (Klasa C); bateryjny cap
   (Klasa A) nie zastępuje go — celowo.
4. QS-3D (migracja 16 kHz) wymaga jeszcze pozostałych blockerów QS-3B sekcja 15 (mailbox,
   volatile contract, budżet ISR, same-tick zero, sticky zero event).

## 19. Następne kroki

1. Akceptacja review (ta karta, werdykt PASS warunkowo / hardware NO).
2. Zatwierdzenie na bench/hardware: zweryfikować `dg[34]` oraz `Iq_allowed`/`iq_battery_cap`
   przy zadaniu > limit; sprawdzić brak oscylacji przy progu; potwierdzić, że prąd baterii
   trzyma się blisko `battery_current_max` przy limitowanym Iq.
3. Alokować next canonical > `0.0469` przez alokator wersji i wykonać pełny build.
4. Stage/commit wyłącznie plików QS-3C (bez niezwiązanych plików SOC).
5. Po zamknięciu pozostałych przesłanek — QS-3D: przenieść finalny slew do 16 kHz z atomowym
   command mailbox.

## 20. Podsumowanie

- Legacy defekt (FW-128A) — zamiana domeny PI_iq — został usunięty.
- Limiter bateryjny jest teraz **upstream min-cap w domenie Iq PRZED finalnym slew**
  (`battery_iq_cap.c`, stan w `ride_control.c`); jedyny właściciel bateryjnego capa; nie dotyka
  feedbacku ani domeny setpointu PI_iq; brak post-slew clamp.
- `PI_iq.setpoint` ma dokładnie 2 pisarki (init + normalny `MS.i_q_setpoint`); `PI_iq.recent_value`
  ma 1 (mierzony Iq).
- Klasyfikacja odpowiedzi: **Klasa A — ciągły limiter operacyjny**; twardy off (overcurrent
  FOC, 200 ms) pozostaje niezależną Klasą C.
- Dowód równania capa — 9 pod-punktów (sekcja 4.1); guards statyczne G1–G14 (sekcja 14).
- Finalny 4-kHz slew, ARMED_ZERO, cold start, safety, release 200 ms i diagnostyka CAN są
  nienaruszone.
- Osiągnięto warunek „Iq_allowed -> one slew -> PI`, który QS-3B wskazał jako pierwszy krok do
  migracji 16 kHz.
