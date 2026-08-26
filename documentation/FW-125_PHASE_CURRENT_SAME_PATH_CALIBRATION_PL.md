# FW-125 — Kalibracja prądów fazowych z tej samej ścieżki pomiarowej

**Status:** `IMPLEMENTED_SOFTWARE / AWAITING_REAL_BIKE_TEST`
**Bazuje na:** FW-118 (runtime current calibration), FW-119 (polityka retry/LKG/fallback),
FW-120.1 (rekonstrukcja sektorowa), FW-122 (diagnostyka CASE C)
**Nie zmienia:** progów PI, ride-control, min-Iq floor, lifecycle start/stop, dynamicznego okna
próbkowania ADC

---

## 1. Problem

Sterownik mierzy prąd trzech faz silnika trzema **osobnymi układami ADC** (ADC0, ADC1, ADC2 —
GD32F303 ma ich trzy, każdy to inny kawałek krzemu z własnym, niezależnym błędem zera). Przy
starcie FW-118/FW-119 mierzyły to zero na **jednym** z nich (ADC0, w trybie "regular scan") i
tym samym wynikiem korygowały odczyty ze **wszystkich trzech**. Dla fazy C to prawie ten sam
układ (ADC0), więc błąd jest mały. Dla faz A i B to zupełnie inny układ ADC (ADC2 i ADC1) — jego
własne zero mogło się różnić o dziesiątki jednostek ADC od zera zmierzonego na ADC0.

Przy dużym prądzie taki błąd ginie w szumie. Przy **bardzo małym** zadanym prądzie (typowo tuż po
starcie/wznowieniu, gdy `FinalIq` bywa przycięte do podłogi startowej, np. 14) ten sam błąd
potrafi zdominować wynik — po transformacji Clarke/Park odczyt prądu zaczyna zależeć od tego,
który sektor Halla akurat trwa, bo błąd w jednej fazie inaczej "wchodzi" w rachunek w zależności
od kąta.

## 2. Dowód z prawdziwego roweru (FW-122 CASE C)

FW-122 złapał realny przypadek: `FinalIq ≈ 14`, mostek aktywny, ale odpowiedź prądowa
niewystarczająca i silnie zależna od sektora — przy tym samym zadaniu `IqActual` zmieniało się od
+1 do -15 w zależności od aktualnego sektora Halla. Dopasowanie modelu "stały błędny wektor po
transformacji Park" do tych danych dało bardzo wysoką zgodność (R² > 0.9) — dokładnie taki ślad
zostawia błąd offsetu, nie błąd regulatora czy filtra momentu.

**`14` to nie jest przyczyna** — to wynik istniejącej podłogi minimalnego Iq przy starcie, którą
ta karta zostawia bez zmian (patrz punkt 21 karty FW-125). Przyczyną jest to, co dzieje się z
tym `14` po drodze przez pomiar prądu.

## 3. Stara ścieżka (przed FW-125)

```
KALIBRACJA (main(), przed pętlą główną):
  adc_value[7]  (ADC0 regular, PA2, "Faza A")   ┐
  adc_value[8]  (ADC0 regular, PA3, "Faza B")   ├─ ZAWSZE z ADC0
  adc_value[4]  (ADC0 regular, PA5, "Faza C")   ┘
        │
        ▼
  offset[phase] = mean(regular) − CURRENT_HW_OFFSET[phase]   (odjęcie stałej sprzętowej DRUGI RAZ)

RUNTIME (ADC0_1_IRQHandler, co cykl PWM/16 kHz):
  i16_ph1_current = adc_inserted_data_read(ADC2, ...)   ← Faza A, ale z ADC2!
  i16_ph2_current = adc_inserted_data_read(ADC1, ...)   ← Faza B, ale z ADC1!
  i16_ph3_current = adc_inserted_data_read(ADC0, ...)   ← Faza C, z ADC0 (ten sam układ)
        │
        ▼
  i16_phX_current -= offset[phase]     ← offset zmierzony na ADC0, odjęty od ADC2/ADC1/ADC0
```

Dla fazy C błąd jest mały (ten sam układ ADC0, inny tryb konwersji — regular vs. inserted, inny
czas próbkowania). Dla faz A i B offset pochodzi z **fizycznie innego przetwornika** niż ten,
który naprawdę mierzy prąd w czasie jazdy.

Dodatkowo: `adc_inserted_data_read()` w GD32F303 zwraca wynik **już pomniejszony w sprzęcie** o
rejestr `IOFFx` (ustawiany przez `adc_inserted_channel_offset_config()`, patrz punkt 6). Stara
ścieżka odejmowała stałą sprzętową (`CURRENT_HW_OFFSET_A/B/C`) od surowego odczytu regularnego
(w którym IOFFx **nie** jest stosowany), próbując zbudować korektę pasującą do domeny inserted —
dwa niezależne źródła błędu w jednym wzorze.

## 4. Nowa ścieżka (FW-125)

Zasada: **kalibracja mierzy dokładnie ten sam sygnał, który później wchodzi do FOC.**

```
KALIBRACJA i RUNTIME dzielą TEN SAM odczyt:

  ADC0_1_IRQHandler (16 kHz, działa od bardzo wczesnego boot — patrz punkt 8):
      i16_ph1_current = adc_inserted_data_read(ADC2, ...)   ← Faza A
      i16_ph2_current = adc_inserted_data_read(ADC1, ...)   ← Faza B
      i16_ph3_current = adc_inserted_data_read(ADC0, ...)   ← Faza C
            │
            ├──► phase_cal_acc  (aktywny TYLKO w oknie kalibracji startowej, mostek OFF)
            │       — akumuluje SUMA/MIN/MAX z surowych i16_phX_current
            │
            ▼
      if (current_cal.valid)
          i16_phX_current -= current_cal.offset[phase]   ← offset = ta sama średnia z phase_cal_acc

MAIN(): current_cal_submit(mean, min, max) → offset[phase] = mean   (BEZ drugiego odejmowania)
```

Kalibracja i runtime to teraz dosłownie ten sam wiersz kodu (`adc_inserted_data_read(ADCx, ...)`
na fazę), tylko czytany w innym oknie czasowym. Nie ma już żadnego kroku przechodzącego przez
ADC0 regular dla faz A/B.

## 5. Mapowanie ADC (potwierdzone w kodzie)

| Faza | Pin | ADC (runtime, inserted) | Kanał | IOFFx (sprzęt) |
|---|---|---|---|---|
| A | PA2 | **ADC2** | inserted ch0 | 2020 |
| B | PA3 | **ADC1** | inserted ch0 | 2028 |
| C | PA5 | **ADC0** | inserted ch0 | 2012 |
| Bateria | PA0 | ADC0 | regular ch0 | — (osobna kalibracja, `bat_current_offset`, nietknięta) |

Wyzwalanie: `TIMER0` kanał 3 (ten sam timer, który generuje PWM mostka) steruje wszystkimi trzema
konwersjami inserted jednocześnie — ADC0 jako master, ADC1 jako follower w trybie dual
"inserted parallel", ADC2 niezależnie przez własny trigger na to samo zdarzenie. `TIMER0` liczy
od `timer0_config()` (main.c, wywoływane przed pętlą kalibracji), ale **wyjście PWM (MOE) zostaje
wyłączone** aż do dużo późniejszego startu mostka — więc konwersje inserted już działają, a prąd
silnika jest naprawdę zerowy. `nvic_config()` (wywoływane jeszcze wcześniej) włącza przerwanie
`ADC0_1_IRQn`, więc `ADC0_1_IRQHandler` faktycznie już działa w momencie startu kalibracji.

## 6. Semantyka `adc_inserted_channel_offset_config()` / `IOFFx`

Rejestr `ADC_IOFFx` (konfigurowany przez `adc_inserted_channel_offset_config()`) jest odejmowany
**w sprzęcie** przed zapisaniem wyniku do `ADC_IDATAx`. `adc_inserted_data_read()` (patrz
`Firmware/GD32F30x_standard_peripheral/Source/gd32f30x_adc.c`) po prostu odczytuje `IDATAx` —
zwraca więc `raw − IOFFx` jako liczbę ze znakiem (stąd `int16_t i16_phX_current`, nie
`uint16_t`). To jest kluczowe: **domena FW-125 to mały sygnał ze znakiem wokół zera**, a nie
surowy 12-bitowy kod ADC (~2048).

## 7. Równanie programowego offsetu (residual)

```
REGISTER VALUE (raw ADC12)
   → adc_inserted_data_read()        =  raw − IOFFx                 (już w sprzęcie)
   → phase_cal_acc (przy zerowym prądzie, N próbek)   → residual_mean = mean(raw − IOFFx)
   → current_cal.offset[phase] = residual_mean          (ŻADNEGO drugiego odjęcia IOFFx)
   → runtime: i16_phX_current -= current_cal.offset[phase]
            = (raw − IOFFx) − residual_mean
            ≈ 0  przy zerowym prądzie, dokładnie proporcjonalnie do prądu poza tym
   → dyn_adc_state_reconstruct()      (rekonstrukcja sektorowa, PO korekcie — patrz punkt 8)
   → FOC_calculation()
```

## 8. Kolejność w ISR (`ADC0_1_IRQHandler`) i moment próbkowania

```
1. adc_inserted_data_read(ADC2/ADC1/ADC0)     → i16_ph1/2/3_current (surowe, po IOFFx)
2. phase_cal_acc   (jeśli aktywny — okno kalibracji startowej)      ← SUROWE dane
3. if(current_cal.valid) i16_phX_current -= current_cal.offset[..]  ← korekta programowa
4. phase_cal_verify_acc (DIAG only, jeśli aktywny — self-test)      ← dane PO korekcie
5. dyn_adc_state_reconstruct(...)             ← rekonstrukcja sektorowa (FW-120.1)
6. FOC_calculation(...)
```

Kalibracja **musi** czytać krok 1 (przed rekonstrukcją) — inaczej mierzylibyśmy już
zrekonstruowaną kombinację dwóch faz, a nie rzeczywisty pojedynczy kanał. Self-test (krok 4) musi
czytać PO kroku 3, bo ma udowadniać, że korekta faktycznie działa — nie powtarzać tego samego
surowego pomiaru.

## 9. Liczba próbek i czas trwania

`CURRENT_CAL_SAMPLES = 128` (poprzednio 64). ISR działa z częstotliwością PWM = **16 kHz**
(`START_NEUTRAL_DWELL_CYCLES` w `inc/config.h`: "4 cycles @16kHz PWM = 250 us"). 128 próbek to
więc **~8 ms** na próbę kalibracji — pomijalne wobec reszty sekwencji startowej, a dwukrotnie
lepsza pewność statystyczna średniej/P2P niż poprzednio.

Pętla w `main()` czeka na `phase_cal_acc.done` z ograniczonym licznikiem `guard` (nie realny
czas, licznik spinowy — ten sam styl co poprzedni `CURRENT_CAL_TIMEOUT`), więc martwy
TIMER0/ADC nadal nie może zawiesić startu.

## 10. Walidacja (nadal PROVISIONAL)

Stare progi (`CURRENT_ZERO_MIN_ADC`/`MAX_ADC` = 2048±200) były w domenie surowego ADC12 —
**nieprzenośne** do nowej domeny residual. Nowe progi:

```
CURRENT_CAL_RESIDUAL_MIN/MAX = ±300 LSB   (PROVISIONAL — zapas na niedoskonałe strojenie IOFFx)
CURRENT_ZERO_MAX_P2P_ADC     = 200 LSB    (PROVISIONAL — ta sama zgadnięta wartość co FW-118)
```

Obie wartości wymagają pomiaru warsztatowego (patrz punkt 29 karty i sekcja "Falsyfikacja"
poniżej), tak jak poprzednio.

## 11. Bezpieczeństwo / fallback (FW-119, bez zmian semantyki)

Polityka retry → LKG → legacy/strict **nie została zmieniona** — zmieniła się tylko domena
liczb, na których ta polityka operuje. `CURRENT_CAL_START_POLICY` pozostaje `0`
(`LEGACY_FALLBACK`) do czasu pomiaru warsztatowego z punktu 10. `current_cal_foc_allowed()` ma
dokładnie taki sam kontrakt jak w FW-119.

## 12. Self-test zerowego prądu (DIAG only)

Zaraz po `current_cal_finalize()`, w wariancie DIAG, sterownik zbiera drugą, niezależną porcję
128 próbek — tym razem **po** korekcie programowej (`phase_cal_verify_acc`, krok 4 z punktu 8) —
i zapisuje wynik w `fw125_zero_current_selftest_valid/mean[]/p2p[]`. Oczekiwanie: średnia bliska
zeru na wszystkich trzech fazach. To jest bezpośredni dowód, że korekta działa, a nie tylko
założenie.

## 13. Testy

- `tests/host/fw125_phase_current_calibration_host.c` — linkuje prawdziwy `current_cal.c`: T5/T6
  (dokładna średnia/P2P na niejednorodnych danych), T7 (atomowość publikacji), T8/T9 (STRICT nadal
  blokuje, LEGACY_FALLBACK nadal działa w nowej domenie), T10-T12 (korekta ze znakiem: 0, +5, −7),
  T13 (wyrocznia — trzy niezależne delty na trzech fazach, przechodzą przez kalibrację i korektę
  dokładnie takie, jakie były).
- `tests/host/fw125_wiring_guard_host.c` — main.c nie da się zlinkować (wejście ARM), więc to
  sprawdza SOURCE TEXT: T1-T3 (kalibracja i runtime czytają tę samą zmienną per faza), T4
  (`adc_value[4]/[7]/[8]` nie występuje już nigdzie — regresja niemożliwa po cichu), T13
  (kolejność: kalibracja przed rekonstrukcją, self-test po korekcie i przed rekonstrukcją), T14
  (self-test telemetry wyłącznie `#if CAN_DIAGNOSTICS_ENABLE`, sam fix — nie), T15 (DIAG
  eksponuje `fw125_zero_current_selftest_*`).
- `tests/host/fw119_current_cal_host.c` i `fw119_current_cal_wiring_host.c` — zaktualizowane do
  nowej, podpisanej domeny (offset = zmierzona średnia, bez drugiego odjęcia stałej sprzętowej);
  sama polityka retry/LKG/fallback (T1-T8) niezmieniona.
- Pełny `tests/host/run-host-tests.ps1`: **PASS** (wszystkie pakiety, w tym powyższe).

## 14. Oczekiwane zachowanie na rowerze i falsyfikacja

**Oczekiwane po poprawce:**
- Self-test DIAG pokazuje średnią bliską zeru na A/B/C przy zerowym prądzie.
- Zależność `IqActual` od sektora Halla przy stałym małym `FinalIq` (np. 14) znika lub drastycznie
  maleje.
- Powtarzalne "rolling no-assist" (silnik nie podejmuje lekkiego momentu mimo `CAL≈14`) znika.

**ROOT CAUSE CONFIRMED**, jeśli po poprawce: self-test zerowy jest dobry ORAZ zależność sektorowa
znika ORAZ rolling no-assist znika.

**Następny kandydat (osobna karta, np. FW-126), jeśli**: self-test jest dobry, ale CASE C nadal
występuje i błąd nadal zależy od sektora → dynamiczne okno próbkowania ADC (punkt 18 karty,
świadomie NIE ruszane w FW-125).

**Wraca do torque/mode-demand (osobno)**, jeśli: odpowiedź prądowa jest już poprawna, ale
`FinalIq` nadal nieoczekiwanie zostaje na `14`.

---

## Co się NIE zmieniło

Regulator PI (kp/ki/limity/anti-windup), ride_control, podłoga minimalnego Iq, sekwencja
start/stop mostka (D2, neutral dwell, MOE), kierunek wirnika, ścieżka momentu (torque zero, RUN
filter, coast re-zero) — żadne z tych miejsc nie zostało dotknięte przez tę kartę.
