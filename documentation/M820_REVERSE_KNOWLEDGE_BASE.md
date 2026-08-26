# Bafang M820 / CR X30P — baza wiedzy z reverse engineeringu

**Stan:** 2026-08-20  
**Cel:** zachować odzyskaną wiedzę o stockowym sterowniku jako bazę do nauki, audytu, poprawiania EVistDrive i przenoszenia sprawdzonych koncepcji do innych projektów.  
**Zasada:** nie kopiować ślepo implementacji. Rozdzielać: zachowanie algorytmiczne, stałe sprzętowe, protokół, hipotezy oraz elementy przenośne.

## 0. Skala pewności

- **PEWNE** — bezpośrednio wynika z kodu stockowego albo z dwóch niezależnych ścieżek dowodowych.
- **BARDZO MOCNE** — wiele spójnych przesłanek, ale brakuje jednego zamykającego dowodu fizycznego/protokolarnego.
- **HIPOTEZA** — sensowne wyjaśnienie zgodne z kodem, ale wymaga dalszej weryfikacji.
- **OTWARTE** — cel dalszego reverse engineeringu.

## 1. Obrazy źródłowe

### Stock referencyjny CR X30P 2.1

- plik: `CRX30PC3612E102003.5-CR X30P.250.FC 2.1.bin`
- rozmiar: `92620 B`
- SHA256: `c93f892c7dcb506f6674e56ef3f888307ea185494cf3e48caf00ecf0ff56a4e6`
- kontener update ma 32-bajtowy nagłówek; wektor aplikacji zaczyna się od offsetu `0x20`
- aplikacja jest linkowana od `0x08005000`
- Reset_Handler około `0x0800A3CD`

### FT 2026-05-22

- plik: `FT_2026_05_22_w1(1).bin`
- rozmiar: `93196 B`
- SHA256: `96f9e8d4a7f1e0dceb47c4a50cd85f2e9fa0c948adcb4fa7194dce11233bc32a`
- ten sam model 32-bajtowego nagłówka i aplikacji od `0x08005000`

### Nowszy obraz do porównania międzywersyjnego

- plik: `CRX30PC3615F805001.0_G532_250W_25_700-2185_Git-67a37d78_20251117_1756.bin`
- rozmiar: `154328 B`
- SHA256: `93c85ab1db4c4e9108ba07014cccf859ad4e53b3d7ce2371458f3edd0e7398a8`
- zawiera tekst `CR X30P.250.FC 5.0` i `Sample_67a37d78`
- **status:** materiał porównawczy; nie mieszać automatycznie jego stałych z wersją 2.1.

---

# 2. ADC — architektura stocku

## 2.1 GPIO analogowe — PEWNE

Stock konfiguruje jako analogowe:

- `PA0..PA7`
- `PB0..PB1`
- `PC0..PC5`

Oznacza to, że wszystkie 16 zewnętrznych kanałów ADC klasycznego mapowania F1/GD32 są dostępne i część z nich jest używana regularnie albo injected.

## 2.2 Regular ADC / DMA — PEWNE

Regularne ADC pracują w trybie dual/simultaneous i są DMA-owane do RAM. Regularne wyniki są **left-aligned**.

### ADC1 regular sequence

| Rank | Kanał | Pin | Znaczenie |
|---:|---:|---|---|
| 1 | 1 | PA1 | nieustalone / referencyjne |
| 2 | 0 | PA0 | ważny tor prądowy/referencyjny |
| 3 | 10 | PC0 | napięcie fazowe / BEMF #0 |
| 4 | 17 | internal | VREFINT |
| 5 | 12 | PC2 | napięcie fazowe / BEMF #2 |
| 6 | 13 | PC3 | bateria / DC-link voltage |
| 7 | 11 | PC1 | napięcie fazowe / BEMF #1 |
| 8 | 1 | PA1 | powtórka |

### ADC2 regular sequence

| Rank | Kanał | Pin | Znaczenie |
|---:|---:|---|---|
| 1 | 7 | PA7 | torque/load candidate, mocno przetwarzany |
| 2 | 14 | PC4 | AUX — otwarte |
| 3 | 15 | PC5 | AUX/plausibility — otwarte |
| 4 | 8 | PB0 | AUX raw10 — otwarte |
| 5 | 9 | PB1 | NTC silnika |
| 6 | 4 | PA4 | AUX — otwarte |
| 7 | 6 | PA6 | AUX — otwarte |
| 8 | 0 | PA0 | dodatkowy odczyt PA0 |

## 2.3 Injected ADC — PEWNE

Injected są zsynchronizowane z PWM i są szybkim torem prądowym:

- ADC1 rank1: `PA2 / ch2` — prąd fazowy #1
- ADC1 rank2: `PA0 / ch0` — dodatkowy szybki pomiar PA0
- ADC2 rank1: `PA3 / ch3` — prąd fazowy #2
- ADC2 rank2: `PA5 / ch5` — prąd fazowy #3

**Bardzo ważna pułapka:** regularny DR i injected JDR nie mają identycznej interpretacji przesunięcia przy left alignment. Dla injected w tej konfiguracji efektywny surowy 12-bitowy wynik odzyskuje się ze ścieżki odpowiadającej `JDR >> 3`; błędne założenie `>>4` prowadzi do dwukrotnego błędu skali.

---

# 3. Prądy fazowe — pomiar, kalibracja, rekonstrukcja

## 3.1 Fizyczne wejścia — PEWNE

| Pin | Rola logiczna |
|---|---|
| PA2 | phase-current #1 |
| PA3 | phase-current #2 |
| PA5 | phase-current #3 |

Dokładnego przypisania **U/V/W do PA2/PA3/PA5** nie wpisujemy bez potwierdzenia PCB/timera.

## 3.2 Zero prądowe — PEWNE, KOREKTA

Poprawna wartość stocku to:

```text
ADC_zero ≈ 2048
```

Zakres zaakceptowany podczas kalibracji:

```text
1920 < ADC_zero < 2176
```

Wcześniejsze rozumowanie `zero≈1024` było błędne z powodu pomylenia formatu injected JDR z regular DR.

## 3.3 Kalibracja offsetów A/B/C — PEWNE

Funkcja około `0x0800AF5C`:

- kalibruje każdy kanał osobno,
- pobiera **16 próbek na kanał**,
- ma poprzednie dobre offsety / retry / fault,
- nie zakłada wspólnego offsetu.

Wewnętrznie:

```text
offset_internal = 16 * average(ADC_zero)
```

Walidacja:

```text
0x7800 < offset_internal < 0x8800
```

co daje dokładnie:

```text
1920 < average_ADC_zero < 2176
```

Sekwencja kalibracji używa specjalnych konfiguracji TIM1 CCER:

- faza #1: `CCER = 0x0880`
- faza #2: `CCER = 0x0808`
- faza #3: `CCER = 0x0088`
- po kalibracji: `CCER = 0x0DDD`

MOE jest aktywowane tylko w kontrolowanym fragmencie procedury. Jednostki funkcji opóźniającej `wait(100)` / `wait(10)` nie są jeszcze podpisane na 100% — nie należy automatycznie nazywać ich milisekundami.

## 3.4 Normalna skala prądu w FOC — PEWNE

Stock sprowadza pomiar do:

```text
I_internal = 16 * (ADC_zero - ADC_sample)
```

Zatem:

```text
1 surowy ADC count = 16 stock-current-units
```

Znaki:

- `ADC_sample < zero` → dodatni prąd w tej konwencji,
- `ADC_sample > zero` → ujemny.

## 3.5 Fizyczny gain A/count — BARDZO MOCNE, JESZCZE NIE PEWNE

Stockowy BIN nie ujawnił jeszcze zamkniętego równania `phase ADC -> A`.

Niezależny port EBiCS dla M820 używa około:

```text
CAL_I = 95
```

jako empirycznej skali związanej z Iq i estymacją prądu baterii. To daje mocny kandydat około:

```text
95 mA / raw phase ADC count
```

Korekta porównawcza względem dokładniej odzyskanego PA0 sugeruje okolice `100 mA/count`, ale **100 mA/count nie jest jeszcze dowodem ze stockowego BIN-u**.

### Status

- `95 mA/count` — mocny cross-check z EBiCS M820,
- `~100 mA/count` — bardzo sensowna triangulacja,
- finalny `PHASE_CURRENT_A_PER_COUNT` — **OTWARTE**, dopóki nie znajdziemy stockowej telemetrii serwisowej, shunta/gainu analogowego albo równania fizycznego.

---

# 4. Dynamiczne próbkowanie prądu względem PWM — PEWNE, rozszerzony reverse 2026-08-25

To jeden z najcenniejszych elementów stocku do przeniesienia koncepcyjnie. Najnowszy reverse zamknął również kolejność `sample_state -> CCR4 -> następna próbka -> reconstruction`.

## 4.1 Funkcje i RAM — PEWNE

Kluczowe punkty stocku 2.1:

```text
0x08007604  fast current-loop path / kolejność konsumentów i producentów
0x0800A9E4  SVPWM + wybór okna current ADC + CCR1..CCR4
0x0800B17C  odczyt/reconstruction phase current + INVALID hold
```

Kluczowe RAM:

```text
0x200004E4  SVPWM sector 0..6
0x200004F0  CH4 polarity selector
0x200004F1  sample_state: 0=INVALID, 1=PRIMARY, 2=ALTERNATE
0x200004FA  next CCR1 mirror
0x200004FC  next CCR2 mirror
0x200004FE  next CCR3 mirror
0x20000500  next CCR4 mirror

0x20000660  phase current A/internal
0x20000664  phase current B/internal
0x20000668  phase current C/internal
0x20000684  previous/backup A
0x20000688  previous/backup B
0x2000068C  previous/backup C
```

**Korekta wcześniejszej interpretacji:** `0x200004E4` nie jest po prostu stanem Halla. Jedyny producent tej zmiennej w prześledzonej ścieżce wyznacza wartości `1..6` z geometrii/wektora SVPWM w `0x0800A9E4`; `0x0800B17C` używa potem tego samego sektora do wyboru obserwowalnych shuntów. Dlatego poprawna nazwa to **SVPWM sector / sampling sector**, nie `Hall sector`.

## 4.2 Kolejność cyklu — PEWNE

Fast path `0x08007604` ma kluczową kolejność:

```text
ADC trigger z poprzednio ustawionego CCR4
        ↓
injected conversion complete / wejście fast path
        ↓
0x0800B17C
  użyj LATCHED sector + sample_state z poprzedniego SVPWM
  odczytaj bieżące JDR tylko jeśli sample_state != INVALID
  wykonaj 2-of-3 reconstruction
        ↓
Clarke / Park / current control
        ↓
inverse transform
        ↓
0x0800A9E4
  policz NOWE SVPWM
  policz sector dla NASTĘPNEJ próbki
  policz sample_state dla NASTĘPNEJ próbki
  policz CCR4 dla NASTĘPNEJ próbki
  wpisz CCR1/CCR2/CCR3/CCR4
        ↓
następny PWM/current sample
```

To jest ważny wzorzec dla EVistDrive: **próbka N jest interpretowana stanem przygotowanym razem z triggerem N, a nie stanem obliczonym dopiero po odczytaniu próbki N**.

## 4.3 Dokładna logika okna — PEWNE

TIM1 pracuje center-aligned, `ARR = 2000`. Odzyskane stałe:

```text
ARR                 = 2000
fallback CCR4       = 1999
margin do końca     = 243 timer counts
minimalna luka      = 372 timer counts
offset próbki       = 129 timer counts
```

Dla każdego sektora stock najpierw sprawdza, czy istnieje podstawowe okno przy końcu okresu. Jeżeli tak:

```text
CCR4 = 1999
sample_state = 1  (PRIMARY)
```

Jeżeli podstawowe okno jest za małe, sprawdza różnicę odpowiednich dwóch compare. Jeśli luka ma minimum `372`:

```text
CCR4 = selected_compare - 129
sample_state = 2  (ALTERNATE)
```

Jeżeli nie ma wiarygodnego okna:

```text
CCR4 = 1999
sample_state = 0  (INVALID)
```

Dokładna tabela dla `CCR1=r0`, `CCR2=r1`, `CCR3=r2`:

| SVPWM sector | PRIMARY condition | ALTERNATE condition | ALTERNATE CCR4 |
|---:|---|---|---|
| 1 | `2000-r0 > 243` | `r0-r1 >= 372` | `r0-129` |
| 2 | `2000-r1 > 243` | `r1-r0 >= 372` | `r1-129` |
| 3 | `2000-r1 > 243` | `r1-r2 >= 372` | `r1-129` |
| 4 | `2000-r2 > 243` | `r2-r1 >= 372` | `r2-129` |
| 5 | `2000-r2 > 243` | `r2-r0 >= 372` | `r2-129` |
| 6 | `2000-r0 > 243` | `r0-r2 >= 372` | `r0-129` |

`sample_state=1` i `sample_state=2` są dla reconstruction równoważne jako **fresh/usable**; różnią się tylko miejscem pobrania próbki. `sample_state=0` oznacza **nie aktualizuj current feedback z bieżących JDR**.

## 4.4 CH4 polarity / edge — KOREKTA, PEWNE dla stock 2.1

Wcześniejsza dokumentacja mówiła, że stock dynamicznie przełącza polarity/edge CH4. Dokładny reverse stocku 2.1 tego **nie potwierdza**.

`0x0800A9E4` posiada ogólny kod:

```text
if (0x200004F0 == 0)
    TIM1_CCER &= ~0x2000;   // clear CC4P
else
    TIM1_CCER |=  0x2000;   // set CC4P
```

ale ta sama funkcja przed każdym sector/window selection robi:

```text
0x200004F0 = 0
```

i w prześledzonym obrazie `CRX30PC3612E102003.5 ... 2.1` nie znaleziono żadnego innego prawdziwego producenta `0x200004F0` ustawiającego `1`.

Zatem dla stocku 2.1:

```text
CC4P jest wymuszane na 0 w normalnym SVPWM/current-sampling path.
```

**Wniosek:** nie wolno używać wcześniejszego twierdzenia „Bafang przełącza edge co sektor” jako dowodu dla EVistDrive. Stock daje nam pewny wzorzec dynamicznego `CCR4`, ale dokładny edge/half-cycle EVistDrive nadal należy potwierdzić na jego własnym TIMER0/ADC.

## 4.5 INVALID handling — PEWNE

`0x0800B17C` na wejściu zachowuje poprzednie A/B/C do backupu. Następnie:

```text
sample_state != 0:
    odczytaj bieżące injected JDR dla dwóch obserwowalnych faz
    przelicz do internal current
    zrekonstruuj trzecią fazę

sample_state == 0:
    NIE używaj bieżących JDR
    przywróć poprzednie A/B
    C = -(A+B)
```

Czyli stock robi dokładnie:

```c
if (current_sample_valid) {
    update_from_fresh_adc();
} else {
    keep_last_valid_currents();
}
```

W tej wersji stocku nie znaleziono osobnego `sample_age`/timeoutu dla kolejnych INVALID. Jeżeli INVALID powtarza się, poprzedni poprawny zestaw może być trzymany kolejno. `sample_age` jest więc sensownym **ulepszeniem EVistDrive ponad stock**, a nie odwzorowaniem istniejącej funkcji Bafanga.

## 4.6 Znaczenie dla EVistDrive rolling-no-assist

Najważniejsza lekcja nie brzmi tylko „przesuń trigger”. Stock pilnuje spójnego pakietu:

```text
NEXT SVPWM geometry
+ NEXT sample sector
+ NEXT validity state
+ NEXT CCR4
        ↓
ADC sample
        ↓
consume exactly that latched state
```

To bezpośrednio wspiera podejrzenie, że w EVistDrive nie wolno rekonstruować próbki według `dyn_adc_state` obliczonego z niepowiązanego/poprzestawianego cyklu.

Dla objawu:

```text
IqRef=14
Battery_Current≈0
brak momentu
większy Iq uruchamia silnik
```

nadal bardzo mocnym kandydatem jest błędna świeża próbka prądu wskutek:

1. nieprawidłowego momentu triggera,
2. braku `INVALID` window validation,
3. nieprawidłowego powiązania reconstruction-state z konkretną conversion.

### Wniosek przenośny

Najpierw poprawić geometrię/synchronizację akwizycji, dopiero potem stroić PI. Filtr cyfrowy nie naprawi próbki pobranej w fizycznie nieobserwowalnym oknie PWM.

---

# 5. Rekonstrukcja 2-of-3 — PEWNE

Stock ma 3-shunt hardware, ale zależnie od sektora wykorzystuje dwie wiarygodne fazy, a trzecią rekonstruuje z:

```text
Ia + Ib + Ic = 0
```

| SVPWM sector | Bezpośrednio mierzone | Rekonstruowane |
|---|---|---|
| 1, 6 | B + C | A = -(B+C) |
| 2, 3 | A + C | B = -(A+C) |
| 4, 5 | A + B | C = -(A+B) |

**Przenośne do EVistDrive:** tak, jeśli hardware i topologia shuntów są zgodne.

---

# 6. Wielostopniowe zabezpieczenie nadprądowe — PEWNE

W ISR stock bierze wartości bezwzględne trzech prądów fazowych i porównuje je z trzema progami runtime:

- threshold #1 @ `0x2000035E`
- threshold #2 @ `0x20000360`
- threshold #3 @ `0x20000362`

Najwyższy poziom ma **licznik kolejnych przekroczeń**. Po więcej niż 5 kolejnych trafieniach stock:

- ustawia flagę fault,
- przechodzi do stanu błędu (`state 9` w tej ścieżce),
- wyłącza TIM1 MOE.

To jest lepsze niż pojedynczy `if(current > max) PWM_OFF`, ponieważ pojedynczy impuls/szum nie musi od razu powodować twardego odcięcia, a prawdziwe utrzymujące się przeciążenie jest szybko zatrzymywane.

## 6.1 Progi zależne od temperatury toru — PEWNE co do matematyki, OTWARTE co do fizycznego modelu

Funkcja około `0x0800E98C` bierze temperaturę `0x200002CE` w `0.1°C`, tworzy kawałkami liniowy współczynnik `0x2000035C`, a następnie:

```text
T1 = min((coef * 0x22E8) >> 8, 28500)
T2 = min((coef * 0x2BA2) >> 8, 30000)
T3 = min((coef * 0x345D) >> 8, 31500)
```

Przykład przy `20.0°C` (`r0=200`):

```text
coef = 248
T1 ≈ 8657 internal
T2 ≈ 10821 internal
T3 ≈ 12986 internal
```

Ponieważ `16 internal = 1 raw phase ADC count`, odpowiada to około:

```text
541 / 676 / 812 ADC counts
```

**Nie interpretować automatycznie jako thermal derating.** Progi rosną z temperaturą, więc może to być kompensacja charakterystyki toru pomiarowego. Właściwy model fizyczny jest nadal otwarty.

---

# 7. Start / stop / neutral PWM — PEWNE

## 7.1 Normal STOP

Stock nie traktuje zwykłego odpuszczenia momentu jak awarii.

Sekwencja:

```text
command -> 0
FOC/PWM dalej aktywne
ramp current/torque -> dokładne 0
MOE OFF
reset PI / controller state
CCR1 = CCR2 = CCR3 = 1000
IDLE
```

TIM1 **nie jest normalnie zatrzymywany przez CEN=0**; normalne wyłączenie mostka używa MOE.

## 7.2 Start

Stock najpierw przygotowuje znany neutralny stan:

```text
reset FOC/controller
CCR1=CCR2=CCR3=1000
Iq_ref=0 / stan neutralny
MOE ON
dopiero potem ramp momentu
```

Dla `ARR=2000`, `CCR1/2/3=1000` jest center vector/common-mode z zerowym napięciem międzyfazowym.

### Najważniejsza lekcja dla EVistDrive

Rozdzielić API:

```c
motor_request_stop();          // graceful
motor_emergency_shutdown(...); // fault
motor_pwm_set_neutral();
```

Nie resetować PI i nie wyłączać mostka w przypadkowej kolejności. To jest główny kandydat do eliminacji kliknięcia/twitch przy starcie/stopie.

---

# 8. Rotor position — UVW / Hall-style interface — PEWNE na poziomie firmware

Firmware czyta 3-bitowy stan z:

```text
PC6 / PC7 / PC8
```

TIM3 używa hall-interface/XOR/capture/reset do mierzenia czasu między przejściami.

Sekwencje:

```text
kierunek 1: 1 -> 5 -> 4 -> 6 -> 2 -> 3 -> 1
kierunek 2: 1 -> 3 -> 2 -> 6 -> 4 -> 5 -> 1
```

To daje 6 przejść na obrót elektryczny.

### Ważna interpretacja hardware

BIN dowodzi interfejsu UVW/Hall-style. Diagram płytki wskazuje **MT6816 w trybie UVW**, więc bardzo prawdopodobnie te trzy wejścia pochodzą z enkodera MT6816, a nie z trzech osobnych fizycznych Halli.

**Pole pairs:** nadal OTWARTE. Nie zgadywać.

---

# 9. Napięcia fazowe / BEMF PC0/PC1/PC2

## 9.1 Kanały — PEWNE jako zestaw 3-fazowy

- PC0 / ADC1_IN10
- PC1 / ADC1_IN11
- PC2 / ADC1_IN12

Nie przypisujemy jeszcze dokładnie U/V/W.

## 9.2 Przetwarzanie — PEWNE

- regular ADC + DMA,
- osobny baseline każdej fazy,
- średnia krocząca 4 próbek,
- podstawowa delta:

```text
D(Vx) = max(avg(Vx) - baseline(Vx), 0)
```

- część sektorowej logiki używa `D(V)/2`,
- dalej występuje dodatkowa stabilizacja/średnia i liczniki.

Przykładowa odzyskana sektorowa para:

| Hall | pierwsza | druga |
|---|---|---|
| 1 | D(V0) | D(V2)/2 |
| 2 | D(V2) | D(V1)/2 |
| 3 | D(V1) | D(V2)/2 |
| 4 | D(V1) | D(V0)/2 |
| 5 | D(V2) | D(V0)/2 |
| 6 | D(V0) | D(V1)/2 |

### Wniosek

W tej ścieżce producentowi nie są potrzebne fizyczne wolty. Interesuje go BEMF/relacja/sektor/plausibility. `V/count` PC0/1/2 jest przydatne do diagnostyki/modelu, ale nie jest warunkiem odtworzenia stockowej logiki BEMF.

---

# 10. Napięcie baterii / DC-link PC3 — PEWNE

PC3 / ADC1_IN13 jest 8-próbkowo uśredniany.

Odzyskana konwersja:

```text
internal = 10 * floor(adc12 * 693 / 4096)
```

Jeżeli wynik jest traktowany jako 0.01 V, pełna skala to około `69.3 V`. Kod celowo ma dość grubą kwantyzację przez kolejność dzielenia/mnożenia.

---

# 11. Prąd baterii PA0

## 11.1 Fizyczny gain — PEWNE

Z połączenia stockowej konwersji z fizyczną jednostką CAN `0x3201`:

```text
CAN_current = delta_ADC * 1000 / 255
CAN field   = current_A * 100
```

wynika:

```text
1 PA0 ADC count = 10/255 A
                = 0.039215686 A
                = 39.216 mA/count
```

Odwrotnie:

```text
25.5 ADC counts = 1 A
```

## 11.2 Kompensacja temperaturowa zera PA0 — BARDZO MOCNE

Funkcja około `0x08018850` śledzi temperaturę w krokach `13` jednostek `0.1°C`, czyli `1.3°C`, i koryguje zero PA0 o około jeden raw ADC count na krok.

Przy potwierdzonym gainie PA0 jest to około:

```text
39.216 mA / 1.3°C ≈ 30.2 mA/°C równoważnego przesunięcia zera
```

Warunki aktywacji i pełny lifecycle tej kompensacji wymagają jeszcze końcowego audytu przed przeniesieniem 1:1.

---

# 12. NTC silnika PB1 — PEWNE

- PB1 / ADC2_IN9
- raw ADC12 jest sprowadzany do ADC10 przez `>>2`
- 191-punktowa LUT obejmuje około `-40°C .. +150°C`
- krok tabeli = `1°C`
- interpolacja daje wynik `0.1°C`
- funkcja około `0x08010F38`
- LUT około `0x0801B158`

Stock nie używa w tej ścieżce Beta ani Steinhart-Hart.

---

# 13. PAS — PEWNE po korekcie

Dwa cyfrowe kanały:

```text
PD2 + PC12
```

Forward quadrature:

```text
00 -> 10 -> 11 -> 01 -> 00
```

Reverse:

```text
00 -> 01 -> 11 -> 10 -> 00
```

Odzyskane stockowe rozdzielenie:

```text
16 cykli quadrature / obrót korby
x4 transitions
= 64 zaakceptowane edges / obrót
```

Wcześniejsze `96` było błędem i nie może być używane jako stockowa wartość.

---

# 14. Parametry assist / limity

Stockowy fallback zawiera m.in.:

```text
base-like byte = 12
current/power-like levels = 20,25,30,35,42,50,60,70,100
parallel speed-like levels = 100,100,100,100,100,100,100,100,100
```

Interpretacja jako 9 poziomów Current Limit + Speed Limit jest **BARDZO MOCNA** i zgodna z rodziną protokołu `0x6011`, ale każde pole runtime powinno być nadal podpisywane na podstawie xref producent->konsument.

### Ważna korekta

Stała `700 / 0x2BC` widziana w stocku **NIE jest uznana za PH_CURRENT_MAX**. Xref prowadzi ją do innej struktury/telemetrii. Hipoteza `700 = phase current max` została odrzucona.

---

# 15. Co najlepiej przenieść do EVistDrive

## P0 — jakość sterowania

1. dynamiczne okno current ADC względem SVPWM,
2. odrzucanie invalid samples,
3. 2-of-3 reconstruction,
4. runtime offset calibration każdej fazy,
5. walidacja offsetów i last-known-good,
6. poprawny lifecycle START/STOP,
7. neutral PWM przed/po MOE,
8. osobne graceful stop i emergency stop.

## P1 — bezpieczeństwo

1. wielostopniowy overcurrent z persistence counter,
2. UVW transition plausibility,
3. ochrona przed użyciem danych niespójnych / nieobserwowalnych,
4. liczniki diagnostyczne odrzuconych current samples,
5. walidacja stabilności podczas kalibracji.

## P2 — engineering units i diagnostyka

1. dokładny phase A/count,
2. dokładny PC0/PC1/PC2 V/count,
3. VREFINT compensation,
4. telemetryczne Id/Iq w A,
5. logowanie duty/sektor/validity/CCR4/offsetów.

---

# 16. Reguły przenoszenia do innych projektów

Każdy odzyskany element klasyfikować jako:

### A. Uniwersalny algorytm

Przykład: `invalid current sample -> last valid`, graceful stop, retry kalibracji.

Można przenosić po adaptacji.

### B. Zależny od topologii

Przykład: 2-of-3 reconstruction, sektorowe shunty, CCR4 sample window.

Przenosić tylko po potwierdzeniu PWM/shunt layout.

### C. Zależny od hardware

Przykład: `ARR=2000`, progi 243/372/129, ADC offset 2048, NTC LUT.

Nie kopiować do innego kontrolera bez pomiaru/analizy jego sprzętu.

### D. Zależny od protokołu

Przykład: CAN 0x3201 scaling.

Trzymać w warstwie profilu urządzenia.

---

# 17. Korekty historyczne — obowiązkowa lista

| Stary wniosek | Aktualny stan | Przyczyna korekty |
|---|---|---|
| phase ADC zero ~1024 | **~2048** | regular DR i injected JDR mają inną efektywną interpretację left alignment |
| `Iinternal = 32*deltaADC` | **`16*deltaADC`** | jw.; injected signed/left-aligned path |
| PAS 96 edges/rev | **64 edges/rev** | ponowny tracing stockowego quadrature |
| `700 = PH_CURRENT_MAX` | **odrzucone** | xref stałej prowadzi do innej struktury/telemetrii |
| fizyczne 3 Hall sensory | **firmware widzi UVW; board wskazuje MT6816 UVW** | rozdzielenie interfejsu logicznego od fizycznego źródła |

Każda przyszła korekta powinna zostać dopisana tutaj, a nie po cichu nadpisana.
