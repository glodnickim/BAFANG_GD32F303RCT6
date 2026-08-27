# FW-128A — kanoniczna własność sterowania prądem q

**Status:** wdrożone, testy hosta zielone, **test sprzętowy NIEWYMAGANY**
**NORMAL:** 0.0447 · **DIAG:** 0.0447 · **Wejście:** pre-audyt `58e5268`

---

## 1. Co audyt faktycznie zastał

Pre-audyt spodziewał się kilku niepowiązanych zmiennych po cichu reprezentujących różne wersje
„żądanego prądu q". **Kod tego nie potwierdza.** Łańcuch **już** był jednokierunkowy i miał
jednego końcowego właściciela:

- `ride_control_update()` trzyma **jedną** lokalną `int32_t iq_target` i nadpisuje ją w miejscu
  na kolejnych etapach;
- `MS.i_q_setpoint` ma **dokładnie jednego pisarza** w całym firmware — `motor_core.c:22`.

Nie było więc rozdartej własności do naprawy. Brakowało **nazw i obserwowalności**: „żądanie
przed limiterami" i „żądanie po limiterach" istniały wyłącznie jako dwie ulotne wartości tej
samej lokalnej. Właśnie dlatego pre-audyt nie potrafił powiedzieć, **który limiter jest wiążący**,
i dlatego żaden test nie mógł przypiąć zachowania limiterów bez sięgania do wnętrza `ride_control`.

> To zmieniło zakres karty w dół, nie w górę. FW-128A **nie zmienia żadnego zachowania
> sterowania** — dodaje nazwy, jednego jawnego właściciela wejść PI_iq i testy własności.

## 2. Graf PRZED

```
iq_target (lokalna, nadpisywana w miejscu)
   ├─ walk / tryb wspomagania / extended boost / bramki           ride_control.c
   ├─ assist_limits_apply()  ×2 (pedał, manetka) → max()
   ├─ assist_start_apply_smooth()  (tylko obniża)
   ├─ gear preload cap
   └─ assist_dynamics_apply() → iq_reference → motor_core_set_command()
                                                    ↓
                                             MS.i_q_setpoint
                                                    ↓
runPIcontrol():  if(!BC_limit_flag){ ref=i_q_setpoint; fb=i_q }
                 else              { ref=bat_max>>6;   fb=Battery_Current>>6 }   ← inline
```

## 3. Graf PO

```
iq_target (lokalna, ta sama arytmetyka, ta sama kolejność)
   ├─ walk / tryb / boost / bramki
   ├─ iq_chain_note_requested()        ← Iq_requested, przed limiterami
   ├─ assist_limits_apply() ×2 → max()
   ├─ assist_start_apply_smooth()
   ├─ gear preload cap
   ├─ iq_chain_note_allowed()          ← Iq_allowed, przed rampą
   └─ assist_dynamics_apply() → motor_core_set_command() → MS.i_q_setpoint = Iq_ref
                                                                 ↓
pi_iq_apply_inputs():   ← JEDEN właściciel wejść PI_iq
      LEGACY_BC_OVERRIDE  (TODO FW-128B)  ← jedyne znane naruszenie niezmiennika
      normalnie:  ref = Iq_ref (MS.i_q_setpoint) ; fb = MS.i_q
```

## 4. Kanoniczna tabela sygnałów

| sygnał | typ / skala | producent | konsument | reset | znaczenie |
|---|---|---|---|---|---|
| **Iq_requested** | `int32_t`, skala prądu fazowego (`PH_CURRENT_MAX=700`) | `iq_chain_note_requested()`, **1 miejsce** w `ride_control.c` | `iq_chain_get()`, diagnostyka, testy | `iq_chain_reset()` przy starcie mostka | żądanie kierowcy/wspomagania **przed** limiterami |
| **Iq_allowed** | j.w. | `iq_chain_note_allowed()`, **1 miejsce** | j.w. | j.w. | po wszystkich limiterach epoki FW-128A, **przed** rampą |
| **Iq_ref** | `int16_t` w `MS.i_q_setpoint` | **`motor_core.c:22`, jedyny** | `pi_iq_apply_inputs()`, ~30 miejsc diagnostycznych | `motor_core.c:17` | referencja, za którą podąża PI_iq |
| `MS.i_q` | j.w. | `FOC.c:140` | `pi_iq_apply_inputs()` | `FOC.c:73` | **zmierzony** Iq |
| `MS.i_d_setpoint` | j.w. | `motor_core.c:23` | `PI_id.setpoint` | `motor_core.c:18` | **nietknięte w tej karcie** — patrz §7 |

**`Iq_ref` NIE jest przechowywane w `iq_chain`.** `MS.i_q_setpoint` już je trzyma i już ma jednego
pisarza; skopiowanie go do struktury stworzyłoby dokładnie ten duplikat stanu, którego ta karta ma
unikać. **Krok 5 rozstrzygnięty na wariant A:** `MS.i_q_setpoint` **jest** kanonicznym `Iq_ref`,
bez lustra kompatybilności.

## 5. Jeden właściciel wejść PI_iq

`pi_iq_apply_inputs()` w `main.c` — cała arytmetyka **przeniesiona dosłownie** z miejsca, gdzie
siedziała inline w `runPIcontrol()`. Tryb normalny jest teraz widoczny jako niezmiennik, wokół
którego zbudowana jest cała seria FW-128:

```c
PI_iq.recent_value = MS.i_q;                                    /* zmierzony Iq */
PI_iq.setpoint = MP.reverse*i8_reverse_flag*MS.i_q_setpoint;    /* Iq_ref       */
```

## 6. LEGACY_BC_OVERRIDE — jedyne naruszenie, zarezerwowane dla FW-128B

**Lokalizacja:** `src/main.c`, funkcja `pi_iq_apply_inputs()`, blok oznaczony
`TODO FW-128B - DELETE LEGACY FEEDBACK-DOMAIN SWITCH`.

Zachowane **bez jednej zmiany w równaniach**, żeby FW-128A pozostało czystą kartą własności.
Co robi i dlaczego musi zniknąć:

- podmienia **feedback** PI_iq ze zmierzonego Iq (domena prądu fazowego) na zmierzony prąd
  baterii (mA >> 6), a **setpoint** z `MS.i_q_setpoint` na `battery_current_max >> 6` — inna
  wielkość fizyczna, inny rząd wielkości;
- `PI_iq.integral_part` przechodzi przez tę podmianę bez zmiany — brak resetu, brak bumpless
  transfer, brak back-calculation;
- warunek wyjścia liczy **przewidywany** prąd baterii z **komendy** `MS.i_q_setpoint`, której ten
  limiter nie redukuje; flaga może opaść wyłącznie przez spadek `MS.u_abs` — wyjścia tej samej
  pętli, której feedback podmieniono.

**Cel usunięcia w FW-128B:** cały blok. Limiter baterii ma działać **upstream** na żądaniu,
produkując `allowed_battery`, tak żeby PI_iq miał jeden feedback przez całe życie.

## 7. Id — nietknięte, świadomie

Karta zabrania zmian w `q31_u_d_temp = -PI_control(&PI_id)` i tak zostało. `MS.i_d_setpoint`
nadal przypisuje się sam przez trzy moduły (pre-audyt D6). **Nie redukuję tego**: dowód, że
zachowanie jest identyczne, wymagałby przeanalizowania wszystkich ścieżek `autodetect`/`stop`,
a karta mówi wprost „jeśli nie da się dowieść — zostaw i udokumentuj". **Zostaje jako osobny
punkt.**

## 8. Cykl życia

`iq_chain_reset()` wołane razem z `current_sample_ctx_reset()` i `current_feedback_reset()` przy
starcie mostka — żaden nowy przebieg nie odczyta żądania z poprzedniego. Reset PI pozostaje
własnością istniejącego lifecycle'u (`main.c:1330-1331`), nietknięty.

**Stop awaryjny nie czeka na nic z tej karty:** ścieżka `motor_core_set_command(&stop_command)`
wraca z `ride_control_update()` **zanim** kanoniczny łańcuch zostanie w ogóle osiągnięty.
Pilnuje tego test A8/A9.

## 9. Usunięty przestarzały stan

**Żaden.** Audyt nie znalazł zduplikowanego stanu ani osieroconych pisarzy w łańcuchu q —
to właśnie ustalenie z §1. Nic nie zostało dodane „obok" niczego: `iq_chain` rejestruje etapy,
`pi_iq_apply_inputs()` **przenosi** kod zamiast go dublować.

## 10. Testy hosta

`tests/host/fw128a_iq_chain_host.c` — dwie połowy: prawdziwy moduł `iq_chain.c` linkowany
wprost, plus strażnik tekstu źródłowego na `main.c` i `ride_control.c` (własność „dokładnie jeden
producent" nie jest własnością czasu wykonania).

Pokrycie: A1 (jeden producent na etap, kolejność), A2, A3, A6, A7 (reset), A8/A9 (izolacja stopu),
A10 (wejścia PI_iq w jednym miejscu), A11 (legacy w jednym, oznaczonym miejscu), A12 (Id bez
zmian), A13 (FW-127 bez zmian), S4 (rampa i limitery niedotknięte — dowód strukturalny).

> **A4/A5 celowo nie przeliczają arytmetyki rampy.** Karta wymaga zachowania zachowania; dowodem
> jest to, że **kodu rampy nie dotknięto**, a to sprawdzają S4a–S4c strukturalnie. Przepisanie
> arytmetyki w teście byłoby słabszym dowodem niż wykazanie, że jej nie ruszono.

**Uwaga z przebiegu:** pierwsza wersja testu liczyła wystąpienia w całym pliku i uznała
inicjalizację (`PI_iq.setpoint = 0;` w setupie PI) oraz definicję globalną
(`FlagStatus BC_limit_flag=0;`) za producentów. Przepisane na liczenie **w obrębie właściciela**
— to mocniejsza asercja, a kod okazał się czystszy, niż początkowo asertowałem.

## 11. Build

| | NORMAL 0.0447 | DIAG 0.0447 |
|---|---|---|
| FLASH | 102 140 B (43,35 %) | 147 808 B (62,76 %) |
| RAM | 12 224 B (24,87 %) | 45 992 B (93,57 %) |
| SHA256 | `46F39000…351A371C` | `CFEB8F0A…ADF6AF64` |

Delta wobec FW-127 (0.0446): FLASH **+136 B** NORMAL / **+128 B** DIAG, RAM **+16 B** obu —
to struktura `iq_chain_t` (2×int32 + flaga) plus wywołania. DIAG RAM sprawdzony: 93,57 %,
wolne **3 160 B**.

**Testy hosta:** baseline bez zmian — `T14` i `T9` w `rolling_no_assist`, oba sprzed tej karty.
**0 nowych regresji.**

> Przy okazji: włączenie `iq_chain.h` do `ride_control.c` uczyniło `iq_chain.c` **realną
> zależnością linkowania** ośmiu pakietów hostowych, które linkują `ride_control.c`. Dopisana
> tam, gdzie należy — to nie obejście, tylko prawdziwa zależność.

## 12. Sprzęt

**NIEWYMAGANY.** Karta nie zmienia zachowania sterowania; pokrycie stanowi dowód źródłowy plus
testy regresji hosta.

---

## Zarezerwowane dla FW-128B

Usunięcie bloku `LEGACY_BC_OVERRIDE` z `pi_iq_apply_inputs()` i zastąpienie go limiterem baterii
działającym **na żądaniu**, produkującym `allowed_battery`, tak żeby feedback PI_iq był
`MS.i_q` **zawsze**, bez wyjątku.
