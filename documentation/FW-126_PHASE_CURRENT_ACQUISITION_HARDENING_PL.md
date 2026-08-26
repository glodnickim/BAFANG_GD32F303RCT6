# FW-126 — akwizycja prądu fazowego: hardening i diagnostyka

> **STATUS 2026-08-26.** Zbocze `TIMER0_CH3` jest **ZMIERZONE: DOWN-count match**
> (FW-126.2, log 19:28, DIAG 0.0430; nachylenia +1,05/+0,90, CONV+L = 588 zliczeń = 4900 ns,
> 1 konwersja na okres PWM). FW-126.2 = **CLOSED**.
> FW-126 pozostaje **OPEN wyłącznie** z powodu równoważności ścieżki kalibracji — patrz
> [FW-126.3](FW-126.3_CALIBRATION_PATH_EQUIVALENCE_PL.md).

## Granica bezpieczeństwa

FW-126.0 nie zmienia Clarke/Park, PI, SVPWM, offsetów runtime ani produkcyjnej matematyki okna próbkowania. Kierunek zbocza `TIMER0_CH3` musi wynikać z pomiaru na sprzęcie; do tego czasu nie wolno wyprowadzać nowego `DYNAMIC_ADC_THRESHOLD`, CH3 ani progu AB/AC/BC.

Usunięto tylko bezużyteczny eksperyment FW-121 z MOE=OFF. Log z 2026-08-24 wykazał w nim zero injected ISR, więc nie daje odpowiedzi o zboczu.

## FW-126.0: pomiar w istniejącym neutral dwell

Pierwszy kwalifikujący się start w obrazie DIAG używa istniejącej sekwencji:

`IqRef > 0 → CCR_A/B/C = _T/2 → neutral_dwell_active → MOE ON → FOC`.

W trzech pierwszych prawdziwych wejściach `ADC0_1_IRQHandler` DIAG zapisuje CH3 `3740`, `3700` i `3660`. CCR0/1/2 nie są zmieniane. Przed przyjęciem każdej próbki wymagane są jednocześnie:

- `neutral_dwell_active == 1`;
- `CCR0 == CCR1 == CCR2 == _T/2`;
- FOC nadal blokowany przez dwell.

Naruszenie warunku kończy pomiar i przywraca `CH3=3740` w tym samym ISR. Po punkcie 2 CH3 także jest przywracany przed zwolnieniem FOC. Nie wydłużono dwell: trzy punkty mieszczą się w domyślnych czterech cyklach.

Przy wejściu ISR zamrażane są `CNT`, `DIR`, `control_time_ticks`, lifecycle, MOE, dwell, CCR0/1/2 oraz EOIC ADC0/1/2. Przed odejmowaniem offsetu zapisywane są JDR: `A=ADC2`, `B=ADC1`, `C=ADC0`. Punkt wymaga ADC1 EOIC; po każdym punkcie czyszczone są EOIC trzech ADC, więc kolejny punkt nie może być re-odczytem starego JDR.

## CAN FW-121: schema 7

Zachowano zakres `0x10240–0x10246`.

- `0x10240`: stan, liczba punktów, sekwencja ISR, maksimum ISR/control-tick oraz liczniki `ADC0_LATE`, `ADC1_LATE`, `ADC2_LATE`.
- `0x10241–0x10243`: punkt, CH3, `CNT_ISR`, `ISR_SEQ`, lifecycle oraz `DIR`, `ADC0_READY`, `ADC1_READY`, `ADC2_READY`, `MOE`, `DWELL`.
- `0x10244–0x10246`: po trzy 16-bitowe raw JDR dla A, B i C.

> **CORRECTION BEFORE HW VALIDATION (2026-08-25):** wcześniejszy opis odwracał interpretację
> UP/DOWN. Poprawiono zgodnie z implementacją i liczeniem TIMER0 (`inc/adc_trigger_diag.h`
> wiersze 28–33). Sam wynik UP/DOWN/BOTH nadal pozostaje **NOT HW VERIFIED** do czasu testu
> na obrazie v0.0429. Dekoder `tools/decode_fw126_ch3.ps1` realizuje wersję poprawioną i
> **nie** został dostosowany do poprzedniego, błędnego zdania.

Interpretacja. Licznik jest center-aligned, `_T = 3750`, a CCR3 leży blisko szczytu. `CONV` to
długość konwersji, `L` to opóźnienie trigger→ISR — nieznane, ale **stałe**, i dlatego rozstrzyga
dopiero sweep, a nie pojedynczy odczyt:

- **match przy zliczaniu W GÓRĘ:** licznik dobija jeszcze `_T - CCR3` do szczytu i zawraca, więc
  ISR wypada na zboczu opadającym: `CNT = 2*_T - CCR3 - (CONV + L)` → nachylenie **−1**;
- **match przy zliczaniu W DÓŁ:** licznik po prostu schodzi dalej: `CNT = CCR3 - (CONV + L)` →
  nachylenie **+1**.

Czyli przy CH3 malejącym o 40: **CNT rosnący o około 40 oznacza UP, a CNT malejący o około 40
oznacza DOWN.** Punkt przecięcia daje przy okazji `CONV + L`.

Uwaga: `DIR` przy wejściu do ISR **nie rozstrzyga niczego** — obie hipotezy wchodzą do ISR na
zboczu opadającym. Rozstrzyga wyłącznie nachylenie. Wartość bezwzględna `CNT` też nie: przy
produkcyjnym CCR3 obie hipotezy dzieli tylko `2*(_T - CCR3) = 20` zliczeń.

Dwa klastry lub podwójna częstotliwość to BOTH; brak zależności to UNRESOLVED. W obu przypadkach nie wdrażać produkcyjnego okna.

`ISR_PER_CONTROL_TICK` jest zmierzone, nie wywnioskowane z komentarza. Dla 4 kHz control tick wynik 4 odpowiada jednemu ISR na okres PWM, a około 8 jest warunkiem STOP. Każdy `ADC*_LATE > 0` też blokuje produkcyjną akwizycję A/B/C.

## Świeżość i stale Iq/Id

- `foc_current_sample_seq` rośnie tylko po nowym ADC1 EOIC;
- `foc_current_sample_tick` i `foc_current_fresh` opisują tę konwersję;
- `foc_current_valid` pozostaje `0`, bo fizyczne okno PWM nie zostało jeszcze wyprowadzone;
- `foc_current_feedback_reset()` zeruje filtry oraz `MS.i_q`, `MS.i_d`, fresh i valid przed bridge re-enable;
- każde bridge OFF oraz soft/hard cut-off natychmiast unieważnia feedback.

Rolling-no-assist recorder nie może uznać starego `MS.i_q` za CASE C: CASE C wymaga `foc_current_valid`. Bit statusu schema 4 pokazuje tę ważność.

## Startup calibration i CAL DUMP

Kalibracja używa runtime path: A=`ADC2 inserted`, B=`ADC1 inserted` (slave ADC0), C=`ADC0 inserted`. Każdy punkt wykonuje software-trigger ADC0+ADC2, czeka na EOIC wszystkich trzech ADC, czyta JDR i dopiero wtedy zwiększa count. ADC1 nie jest triggerowany osobno.

Przed i w trakcie zbierania sprawdzany jest `TIMER0 CCHP.POEN`. Funkcja nigdy nie modyfikuje MOE; obserwacja POEN kończy attempt jako `CURRENT_CAL_MOE_ON`. DIAG self-test wymaga dokładnie 128 świeżych próbek, bez timeout i z MOE=OFF.

Read-only `0x602D` (Canable source 5, READ) zwraca 55 B `CC` schema 1 z CRC16-CCITT: status/reason/source, count attemptu, valid/fallback, offsety, residual mean/P2P, verify mean/P2P, software-trigger mode, issued i confirmed-fresh conversions oraz dowód MOE-off.

## Compact FOC START TRACE

Tylko obraz DIAG włącza kompaktowy trace na transporcie FW-117 (schema 2, `0x10234–0x1023A`). Ring ma 70 próbek przy 250 Hz: 72 ms pre i 208 ms post START. Triggerem jest `PWM_ON 0→1`.

Próbka niesie timestamp, MOE/PWM/dwell/fresh/valid, Hall, lifecycle, Iq/Id, A/B/C po korekcji i rekonstrukcji, CH3, CCR A/B/C, current sequence/age, PI i napięcie FOC. Raw JDR nie wchodzi do małego trace; jest obecne w trzech punktach FW-126.0.

## Test sprzętowy

1. Odciąż koło, uruchom DIAG i rozpocznij normalne wspomaganie.
2. Zapisz pełny CAN log oraz dump FW-121.
3. Sprawdź trzy kompletne punkty, wszystkie READY, `ADC*_LATE=0` i ISR rate.
4. Odczytaj `0x602D`: `last_sample_count=128`, valid=1, fallback=0, verify valid=1, MOE-off verified=1.
5. Dopiero po jednoznacznym UP/DOWN i braku late conversions można projektować FW-127 sampling-window.

Rollback: normal image nie zawiera neutral-dwell sweepu, CAN trace ani trace RAM; przy problemie na DIAG wróć do normalnego obrazu bez zmiany motor-control.
