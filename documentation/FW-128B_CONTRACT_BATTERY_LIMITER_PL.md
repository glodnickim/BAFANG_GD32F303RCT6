# FW-128B — KONTRAKT (propozycja, NIE wdrożone)

**Status:** kontrakt do akceptacji. **Ani jednej linii kodu FW-128B nie napisano.**
**Wejście:** FW-128A `ebfaa8b`, NORMAL/DIAG 0.0447.

---

## 1. Jedno zdanie

Usunąć blok `LEGACY_BC_OVERRIDE` z `pi_iq_apply_inputs()` i zastąpić go limiterem prądu baterii
działającym **na żądaniu, w górę łańcucha**, żeby PI_iq miał **jeden feedback przez całe życie**.

## 2. Niezmiennik, który FW-128B ma domknąć

Po FW-128B `pi_iq_apply_inputs()` ma mieć **dwie linie** i żadnej gałęzi:

```c
PI_iq.recent_value = MS.i_q;                                    /* zawsze zmierzony Iq */
PI_iq.setpoint = MP.reverse*i8_reverse_flag*MS.i_q_setpoint;    /* zawsze Iq_ref       */
```

To jest cały cel karty. Wszystko poniżej jest po to, żeby to zdanie stało się prawdą bez utraty
ochrony baterii.

## 3. Co dokładnie znika

| element | miejsce dziś | los |
|---|---|---|
| `if(MS.Battery_Current>MP.battery_current_max) BC_limit_flag=1;` | `main.c:3545` | **usunąć** |
| `if((MS.i_q_setpoint*CAL_I*MS.u_abs)>>11 < (MP.battery_current_max*0.9)) BC_limit_flag=0;` | `main.c:3546` | **usunąć** |
| podmiana `PI_iq.recent_value` na prąd baterii | `main.c:3550` | **usunąć** |
| podmiana `PI_iq.setpoint` na `battery_current_max>>6` | `main.c:3551` | **usunąć** |
| globalna `FlagStatus BC_limit_flag` | definicja + `main.h` | **usunąć całkowicie** |

**Kryterium zamknięcia:** `grep -rn "BC_limit_flag" src/ inc/` zwraca **zero** wierszy.

## 4. Dlaczego binarna flaga musi zniknąć, a nie tylko przenieść się wyżej

Trzy wady legacy (opisane w FW-128A §6) mają **jedną wspólną przyczynę**: limiter jest
**przełącznikiem trybu**, a nie limiterem.

- przełącznik zmienia wielkość sterowaną → `PI_iq.integral_part` przechodzi bez bumpless transfer;
- przełącznik potrzebuje **warunku wyjścia**, a warunek wyjścia liczony ze **zmierzonego** prądu
  baterii jest z definicji cykliczny: limiter obniża prąd, prąd spada poniżej progu, limiter
  puszcza, prąd rośnie — **cykl graniczny**;
- dlatego autor stocku policzył warunek wyjścia z **przewidywanego** prądu z **komendy**, której
  ten limiter nie redukuje — i tak powstała wada trzecia.

**Wniosek kontraktowy:** FW-128B ma dostarczyć limiter **ciągły** (bez żadnej flagi trybu).
Ciągły limiter nie ma warunku wyjścia, więc problem cyklu granicznego **nie powstaje**, a nie jest
„rozwiązywany". To jest wymóg architektury, nie preferencja stylu.

## 5. Gdzie ma stanąć — dokładne miejsce

`src/ride_control.c`, w `ride_control_update()`:

```
   … limitery epoki FW-128A (assist_limits ×2 → max, assist_start, gear preload cap)
   ├─ battery_limit_apply(iq_target, …)      ← NOWE, tu
   ├─ iq_chain_note_allowed(iq_target)       ← istniejące, ZOSTAJE PO limiterze
   └─ assist_dynamics_apply() → rampa → MS.i_q_setpoint
```

**Kolejność jest częścią kontraktu.** Limiter baterii jest limiterem, więc stoi **przed**
`iq_chain_note_allowed()`. Dzięki temu `Iq_allowed` zachowuje swoje znaczenie z FW-128A
(„po wszystkich limiterach, przed rampą") i **schemat `iq_chain` nie wymaga zmiany** — żadnego
czwartego pola, żadnej migracji.

## 6. Częstotliwość — kluczowe ustalenie, żeby nie stracić szybkości reakcji

To jest jedyny realny zarzut wobec przeniesienia limitera w górę i **da się go rozstrzygnąć
z kodu, bez pomiaru**:

| | dziś (legacy) | po FW-128B |
|---|---|---|
| limiter wykonuje się | `runPIcontrol()` ← `FOC.c:166`, **~16 kHz** | `ride_control_update()` ← `reg_ADC_processing()`, **~4 kHz** |
| ale czyta `MS.Battery_Current`, który powstaje w | `reg_ADC_processing()` `main.c:2321`, **~4 kHz** | to samo miejsce, **ten sam tick** |
| filtr sygnału | IIR `>>6` ≈ 64 ticki ≈ **16 ms** | bez zmian |

Legacy wykonuje się cztery razy na każdą **nową** próbkę i trzy z tych czterech przebiegów widzą
**tę samą liczbę**. Sygnał jest dodatkowo wygładzony stałą ~16 ms. **Przeniesienie limitera do
4 kHz nie odbiera mu żadnej informacji, której dziś używa.**

Dodatkowo `main.c:2321` (obliczenie prądu) wykonuje się **przed** `main.c:2902`
(`ride_control_update()`) w tej samej funkcji, więc limiter zobaczy prąd **z bieżącego ticku**,
a nie sprzed jednego.

> Jeśli po pomiarze okaże się, że 4 kHz to za mało — to jest wynik, nie założenie. Wtedy limiter
> przenosi się do `reg_ADC_processing()` tuż za linię 2321 i oddaje `iq_target` przez zmienną, ale
> **nadal działa na żądaniu, nie na feedbacku PI**. Niezmiennik z §2 zostaje w mocy w obu
> wariantach.

## 7. Nowy moduł

`inc/battery_limit.h` + `src/battery_limit.c` — jak `assist_limits`: **czyste liczby na wejściu,
ograniczony prąd na wyjściu**, bez `main.h`, bez globali, bez dostępu do sprzętu. Wtedy jest
testowalny na hoście, tak jak `iq_chain` i `current_cal`.

Proponowana sygnatura (do zatwierdzenia razem z kartą):

```c
int32_t battery_limit_apply(int32_t iq_request,
                            int32_t battery_current_ma,
                            int32_t battery_current_max_ma,
                            battery_limit_state_t *state);
```

## 8. Czego kontrakt CELOWO nie ustala

**Postaci prawa sterowania i żadnej stałej.** Kontrakt wymaga, żeby limiter był ciągły,
monotoniczny (większy nadmiar prądu → nie mniejsza redukcja) i żeby nie miał windupu.
**Nie przesądza**, czy to ma być człon całkujący po błędzie prądu baterii, czy skalowanie
proporcjonalne.

Powód jest ten sam co w FW-127: **nie kopiujemy stałych stocku i nie zgadujemy**. Dobór wymaga
`MP.battery_current_max` (konfigurowalne przez użytkownika, `parser.c:161`, walidowane
1000–40000 mA, domyślnie `BATTERYCURRENT_MAX` = 15000 mA) oraz znanej skali `MS.i_q` w amperach —
a ta skala jest **otwartym punktem FW-128C**. Ustalanie stałych przed FW-128C byłoby zgadywaniem.

**Nie zmieniamy:** wzmocnień PI, matematyki FOC, akwizycji FW-127, ścieżki Id, kolejności
`assist_limits`, rampy `assist_dynamics`.

## 9. Kierunek bezpieczny przy błędzie

Limiter **tylko obniża** żądanie i nigdy go nie podnosi. Przy braku poprawnego odczytu prądu
baterii ma zachować się jak przy prądzie **równym limitowi** (ograniczaj), a nie jak przy zerze.
Bateria jest jedynym elementem toru, którego przeciążenie jest nieodwracalne.

## 10. Kontrakt testów hosta

- limiter nigdy nie podnosi żądania (własność, nie przypadek);
- monotoniczność względem nadmiaru prądu;
- brak windupu: długi stan ograniczenia, potem zanik nadmiaru → wyjście wraca **bez zwłoki
  proporcjonalnej do długości ograniczenia**;
- brak cyklu granicznego: symulacja zamkniętej pętli prąd↔żądanie ma się ustalić, nie oscylować;
- strażnik źródeł: `BC_limit_flag` **nie występuje** nigdzie; `pi_iq_apply_inputs()` ma dokładnie
  **jedno** przypisanie `PI_iq.setpoint` i **jedno** `PI_iq.recent_value` (liczone **w obrębie
  właściciela**, nie w pliku — patrz pułapka z FW-128A §10);
- regresja: FW-127 i `iq_chain` bez zmian.

## 11. Sprzęt

**WYMAGANY** — w odróżnieniu od FW-128A, ta karta **zmienia zachowanie sterowania**. Test ma
obejmować podjazd/rozruch przy pełnym żądaniu, gdzie limiter baterii faktycznie wchodzi.
Zgodnie z polityką testów: **jeden** test sprzętowy, skonsolidowany z FW-128C, jeśli obie karty
wejdą razem.

## 12. Warunek STOP

Jeśli w trakcie FW-128B okaże się, że skala `MS.i_q` w amperach jest potrzebna do doboru limitu —
**STOP i najpierw FW-128C**. Limiter prądu baterii bez znanej skali prądu fazowego to zgadywanie
tego samego rodzaju co stałe stocku.
