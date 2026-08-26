# FW-119 — Bezpieczeństwo kalibracji prądu fazowego

**Status:** `VERIFIED_SOFTWARE / HW_PENDING`
**Bazuje na:** FW-117 (START NEUTRAL DWELL), FW-118 (runtime current calibration)
**Nie zmienia:** metody pomiaru FW-118 ani progów MIN/MAX/P2P (nadal PROVISIONAL)

> **FW-125 zmieniła metodę pomiaru** opisaną w tym dokumencie (domena ADC0-regular →
> adc_inserted_data_read na własnym ADC każdej fazy; `zero_adc`/`lkg_zero_adc` →
> `residual_mean`/`lkg_residual_mean`, teraz ze znakiem). Polityka retry/LKG/fallback opisana
> niżej **nie zmieniła się** — patrz
> [FW-125_PHASE_CURRENT_SAME_PATH_CALIBRATION_PL.md](FW-125_PHASE_CURRENT_SAME_PATH_CALIBRATION_PL.md)
> po aktualną metodę pomiaru.

---

## 1. Po co to jest — prostym językiem

FW-118 mierzy przy starcie, ile ADC pokazuje na fazach silnika, gdy prąd jest zerowy, i tę
wartość odejmuje w dalszych obliczeniach. Ale FW-118 nie miał **żadnej polityki na wypadek
niepowodzenia**: jeżeli pomiar się nie udał, sterownik po cichu wracał do starych, wpisanych na
sztywno offsetów i ustawiał bajt statusu, którego nikt nie sprawdzał. Nie dało się też odróżnić
„nigdy nie zmierzono" od „zmierzono dobrze, a potem coś poszło źle".

FW-119 dokłada wokół tego pomiaru jasną politykę:

```
ponów pomiar  →  użyj ostatniego dobrego  →  ścieżka legacy albo blokada startu
```

**Co zmienia się na rowerze dzisiaj: nic.** Domyślna polityka to `LEGACY_FALLBACK`, więc
zachowanie jest identyczne jak na FW-118 — tylko teraz sterownik wykonuje do 3 prób zamiast
jednej i mówi wprost, w jakim stanie jest.

---

## 2. Dlaczego NIE ma twardej blokady domyślnie

Progi FW-118 (`CURRENT_ZERO_MAX_DEVIATION` = ±200 LSB, `CURRENT_ZERO_MAX_P2P_ADC` = 200) są
oznaczone jako **PROVISIONAL** — nikt ich nie zmierzył na sprzęcie. Twarda blokada startu
oparta na zgadniętej liczbie to sposób na unieruchomienie sprawnego roweru.

Dlatego polityka jest przełącznikiem, a nie decyzją wpisaną na stałe:

| `CURRENT_CAL_START_POLICY` | Zachowanie przy braku użytecznych offsetów |
|---|---|
| **0 = `LEGACY_FALLBACK`** (domyślne) | stara ścieżka „tylko offset sprzętowy", jednoznacznie oznaczona jako degraded |
| 1 = `STRICT` | start FOC zablokowany do czasu udanej kalibracji |

`STRICT` wolno włączyć **dopiero po** pomiarze warsztatowym z listy HW_PENDING FW-118.

---

## 3. Polityka startu

```text
CAL OK
  → offsety runtime            source = RUNTIME    valid=1  degraded=0  FOC dozwolony

CAL FAILED + poprawny LKG
  → offsety LKG                source = LKG        valid=1  degraded=1  FOC dozwolony

CAL FAILED + brak LKG + LEGACY_FALLBACK
  → tylko offset sprzętowy     source = LEGACY     valid=0  degraded=1  FOC dozwolony

CAL FAILED + brak LKG + STRICT
  → żadnych przypadkowych      source = NONE       valid=0  degraded=1  FOC ZABLOKOWANY
    offsetów
```

Nieudana próba **nigdy** nie nadpisuje dobrych offsetów — ani tych, których używa ISR, ani
zestawu LKG.

---

## 4. Zakres LKG (ważne — przeczytać przed poleganiem na tym)

**LKG żyje w RAM i ginie razem z zasilaniem.** To nie jest wartość przeniesiona z poprzedniej
jazdy. W obrębie jednego załączenia oznacza dokładnie tyle: ostatnia próba, która przeszła
walidację.

Co z tego wynika praktycznie:

1. nieudana ponowna próba nie może zepsuć zestawu, który już przeszedł;
2. gdyby kalibracja była kiedyś uruchamiana ponownie na żywo (dziś nie jest, ale lifecycle to
   już obsługuje), pracujące FOC zostaje przy dobrych offsetach zamiast spadać do legacy
   w środku jazdy.

Trwały zapis do flash **nie został dodany** — wymagałby infrastruktury składowania, której ta
karta świadomie nie wprowadza.

---

## 5. Statusy

| Wartość | Nazwa | Znaczenie |
|---|---|---|
| 0 | `CURRENT_CAL_UNCALIBRATED` | żadna próba nie została jeszcze oceniona |
| 1 | `CURRENT_CAL_OK` | próba przeszła, offsety runtime aktywne |
| 2 | `CURRENT_CAL_OUT_OF_RANGE` | średnia fazy poza oknem PROVISIONAL |
| 3 | `CURRENT_CAL_TOO_NOISY` | P2P fazy powyżej limitu PROVISIONAL |
| 4 | `CURRENT_CAL_SAMPLE_TIMEOUT` | pętla próbkowania nie doczekała się próbek |
| 5 | `CURRENT_CAL_USING_LKG` | wszystkie próby padły, aktywny wcześniejszy dobry zestaw |
| 6 | `CURRENT_CAL_LEGACY_FALLBACK` | brak użytecznego zestawu, ścieżka sprzed FW-118 |
| 7 | `CURRENT_CAL_HARD_FAILED` | brak zestawu + `STRICT` → start FOC zablokowany |

Wartości 0–3 zachowują dokładnie znaczenie z FW-118, więc każdy dekoder czytający stary
`current_calibration_status` czyta je nadal poprawnie.

Nazwa statusu 4 to `SAMPLE_TIMEOUT`, a nie `TIMEOUT`, bo `CURRENT_CAL_TIMEOUT` jest już zajęte
przez stałą FW-118 określającą budżet iteracji oczekiwania na ADC.

---

## 6. Diagnostyka

Cały stan jest w jednej strukturze `current_cal` (`inc/current_cal.h`), zamiast ośmiu luźnych
globali FW-118:

| Pole | Opis |
|---|---|
| `attempts` | liczba zużytych prób |
| `status` | finalny status polityki (tabela wyżej) |
| `failure_reason` | powód ostatniej nieudanej próby |
| `source` | `NONE` / `RUNTIME` / `LKG` / `LEGACY` |
| `valid` | 1 = ISR odejmuje offsety programowe |
| `degraded` | 1 = jedziemy, ale nie na świeżo potwierdzonym zestawie |
| `zero_adc[3]`, `p2p[3]` | zmierzone wartości ostatniej próby |
| `lkg_offset[3]`, `lkg_zero_adc[3]`, `lkg_valid` | ostatni zaakceptowany zestaw |

Mapowanie ze starych nazw FW-118:

| FW-118 | FW-119 |
|---|---|
| `current_offset_a/b/c` | `current_cal.offset[0..2]` |
| `current_calibration_valid` | `current_cal.valid` |
| `current_calibration_status` | `current_cal.status` |
| `current_zero_a/b/c_adc` | `current_cal.zero_adc[0..2]` |
| `current_zero_p2p_a/b/c` | `current_cal.p2p[0..2]` |

Bez ruchu na CAN/UART i bez pracy w ISR: ISR tylko **czyta** `valid` i `offset[]`.
Ekspozycja przez CAN/HMI jest przygotowana architektonicznie (jedna struktura, gotowa do
serializacji), ale protokół **nie został rozszerzony** — nie było takiej potrzeby.

---

## 7. Lifecycle — jedna maszyna startowa, nie dwie

FW-119 **nie tworzy** drugiego automatu startu. Bramkuje dokładnie ten sam warunek, na którym
wisi lifecycle FW-117:

```c
if(MS.i_q_setpoint > 0 && current_cal_foc_allowed(&current_cal)){
    ... BRIDGE_LIFECYCLE_NEUTRAL_COMMIT → MOE ON → NEUTRAL_DWELL → FOC_RELEASE → RUN
```

Przy domyślnej polityce `current_cal_foc_allowed()` zawsze zwraca 1, więc sekwencja FW-117 jest
nietknięta. Cała kalibracja kończy się **przed** pętlą główną, więc offsety nigdy nie zmieniają
się pod pracującym FOC.

---

## 8. Co NIE zostało ruszone

- metoda pomiaru FW-118 (regular ADC scan, mostek OFF, `CURRENT_CAL_SAMPLES` próbek);
- progi MIN/MAX/P2P — nadal PROVISIONAL, nadal HW_PENDING;
- kalibracja prądu baterii PA0 (`bat_current_offset`) — osobna, sekwencyjnie wcześniejsza,
  bez ani jednego symbolu FW-119 w środku (pilnuje tego test W6);
- Clarke/Park, PI, skalowanie `CAL_I`;
- sector-aware reconstruction, dynamic ADC trigger, sample-valid, BEMF, A/count;
- protokół CAN, blob banku, cokolwiek w aplikacji Canable.

---

## 9. Poprawka przy okazji

FW-118 liczył średnią jako `sum >> 6`, czyli na sztywno przez 64, mimo że
`CURRENT_CAL_SAMPLES` jest konfigurowalne. Zmiana tej stałej dawała po cichu złą średnią,
a przez nią zły offset. Teraz dzielnikiem jest liczba faktycznie pobranych próbek. Przy
domyślnym 64 wynik jest identyczny — metoda pomiaru się nie zmienia.

---

## 10. Testy

Zestaw hosta: **40/40 PASS** (było 38, dwie nowe suity FW-119).

`tests/host/fw119_current_cal_host.c` — linkuje prawdziwy `src/current_cal.c` i go wykonuje:

| Test | Co dowodzi |
|---|---|
| T1 | pierwsza próba OK → offsety runtime aktywne, nie degraded |
| T2 | porażka → ponowienie → druga próba OK wygrywa |
| T3 | wszystkie próby padły + istnieje LKG → LKG zostaje aktywny |
| T4 | porażka (OOR / NOISY / TIMEOUT) nie nadpisuje LKG ani offsetów na żywo |
| T5 | wszystko padło + brak LKG + `LEGACY_FALLBACK` → ścieżka legacy, degraded, rower jedzie |
| T6 | `STRICT` + wszystko padło + brak LKG → start FOC zablokowany; `STRICT` z LKG nadal jedzie |
| T7 | TIMEOUT kończy sekwencję po dokładnie `CURRENT_CAL_MAX_ATTEMPTS` — brak deadlocka |
| T8 | średnia dzieli przez faktyczną liczbę próbek (poprawka z §9) |

`tests/host/fw119_current_cal_wiring_host.c` — kontrola tekstu źródłowego `src/main.c`
(main.c jest punktem wejścia ARM i nie da się go zlinkować na PC), pokrywa T8 i T9 z karty:

| Test | Co dowodzi |
|---|---|
| W1 | sekwencja jest w kształcie retry i domknięta dokładnie jednym `current_cal_finalize()` |
| W2 | `CURRENT_CAL_TIMEOUT` jest uzbrajany w każdej próbie — martwy TIMER1/DMA nie zawiesi startu |
| W3 | kalibracja kończy się przed bramką startu i przed ISR; main.c nigdy nie pisze `current_cal.valid` |
| W4 | bramka rozszerza istniejący warunek startu; w main.c jest dokładnie jeden taki warunek |
| W5 | sekwencja neutral dwell FW-117 nadal na miejscu i w kolejności (T8) |
| W6 | kalibracja PA0 nietknięta i niezależna (T9) |

Suita `STEP 2A neutral-dwell wiring guard` (6 kontroli FW-117) przechodzi bez zmian.

---

## 11. Buildy

| Wariant | Flash | RAM | Wynik |
|---|---|---|---|
| DIAG=0 (`normal`) | 99 848 B / 230 KB (42,4%) | 12 032 B / 48 KB | PASS |
| DIAG=1 (`diagnostic`) | 138 680 B / 230 KB (58,9%) | 45 072 B / 48 KB (91,7%) | PASS |

Tylko istniejące wcześniej ostrzeżenia (`-Wpointer-sign` w `CAN_Display.c`).

`sizeof(current_cal_t)` = 60 B wobec ~20 B ośmiu globali FW-118 → **netto +40 B RAM**.
W obrazie diagnostycznym zostaje ~3 KB wolnego RAM.

---

## 12. HW_PENDING

Wszystko z listy FW-118 §18.8 pozostaje otwarte — FW-119 niczego z tego nie zamyka:

| Element | Opis |
|---|---|
| `zero_adc[A/B/C]` | zmierzone wartości zerowe PA2 / PA3 / PA5 |
| `p2p[A/B/C]` | zmierzony P2P per faza |
| Finalne MIN/MAX | do strojenia po pomiarach |
| Finalny limit P2P | do strojenia po pomiarach |
| `CURRENT_CAL_START_POLICY` | przełączenie na `STRICT` — dopiero gdy progi wyżej są realne |

Dodatkowo otwarte, wykryte przy audycie i **nie ruszane** w tej karcie: kalibracja mierzy
surowe wartości z **regular** ADC, a ISR odejmuje offset od odczytów **injected** (JDR).
Czy oba tory mają to samo wyrównanie i tę samą skalę, rozstrzygnie dopiero pomiar warsztatowy —
to jest ta sama niepewność, która stoi za progami PROVISIONAL.

---

## 13. Pliki

| Plik | Zmiana |
|---|---|
| `inc/current_cal.h` | nowy — typy, statusy, API polityki |
| `src/current_cal.c` | nowy — polityka retry / LKG / fallback, bez sprzętu |
| `inc/config.h` | `CURRENT_CAL_MAX_ATTEMPTS`, `CURRENT_CAL_START_POLICY` + walidacja compile-time |
| `src/main.c` | pętla prób wokół pomiaru FW-118, bramka FOC, ISR czyta strukturę |
| `scripts/sources-m820.txt` | `src/current_cal.c` |
| `tests/host/fw119_current_cal_host.c` | nowy — T1–T8 |
| `tests/host/fw119_current_cal_wiring_host.c` | nowy — W1–W6 (T8, T9 z karty) |
| `tests/host/run-host-tests.ps1` | rejestracja dwóch suit |

Canable (`C:\Projekty\bafang_canable_pro`): **bez zmian**. Przejrzana lista kontrolna —
`bafang-parser.js`, `canbus.js`, `ui/js/evistdrive/*.js`, opisy sekcji w `ui/index.html`,
testy round-trip, presety. FW-119 nie dodaje pola zapisywanego przez CAN ani nie zmienia wersji
bloba: `CURRENT_CAL_MAX_ATTEMPTS` i `CURRENT_CAL_START_POLICY` są wyłącznie compile-time.
