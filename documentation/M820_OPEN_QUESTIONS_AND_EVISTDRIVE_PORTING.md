# M820 — otwarte pytania i plan przenoszenia do EVistDrive

**Stan:** 2026-08-20

# 1. Najważniejsze otwarte pytania

## 1.1 Dokładny phase-current gain [A/count] — PRIORYTET 1

Wiemy:

```text
Iinternal = 16*(ADCzero-ADC)
ADCzero ≈2048
```

Mamy mocny cross-check EBiCS około `95 mA/raw count`, ale stockowy fizyczny gain nadal nie jest zamknięty.

### Drogi do dowodu bez aparatury

1. znaleźć stockowy BESST/service diagnostic wystawiający Iphase/Id/Iq w A,
2. znaleźć stockowe równanie Iphase->Ibat z fizycznym PA0,
3. znaleźć Rshunt i amplifier gain ze zdjęcia PCB/schematu,
4. porównać dwie stockowe wersje z różnym produktem/limitem i znaleźć skalowaną stałą,
5. wykorzystać stabilne runtime logi Ibat+duty+Iq tylko jako regresję/cross-check, nie jedyny dowód.

## 1.2 Exact physical U/V/W order

Do ustalenia:

```text
PA2/PA3/PA5 -> U/V/W
PC0/PC1/PC2 -> U/V/W
TIM1 CH1/2/3 -> U/V/W
```

Najlepiej zamknąć przez wspólny xref timer-sector-current/phase-voltage lub continuity PCB.

## 1.3 Physical gain PC0/PC1/PC2 [V/count]

Nie jest potrzebny do stockowego relative-BEMF algorytmu, ale przyda się do:

- diagnostyki faz,
- model observera,
- wykrywania MOSFET/phase fault,
- porównania Vphase/Vbus.

## 1.4 Właściwy thermal derating

Mamy temperature-dependent OC threshold generator, ale rosnące progi sugerują kompensację toru pomiarowego, a nie prosty foldback termiczny.

Trzeba znaleźć osobno:

```text
temperature -> torque/current command reduction
```

oraz rozdzielić:

```text
measurement compensation != thermal protection
```

## 1.5 Pole pairs / mechaniczna skala rotora

Firmware potwierdza 6 UVW transitions/electrical revolution. Liczba pole pairs nadal nie jest podpisana dostatecznie pewnie.

## 1.6 VREFINT correction

ADC1 regular rank zawiera VREFINT. Trzeba prześledzić, czy i gdzie stock używa go do kompensacji VDDA / fizycznych pomiarów.

## 1.7 AUX ADC

Otwarte funkcje:

- PA1,
- PA4,
- PA6,
- PB0,
- PC4,
- PC5.

PA7 jest bardzo mocnym kandydatem głównego torque/load analog front-end.

---

# 2. EVistDrive — rekomendowana kolejność wdrażania

## P0 — najpierw acquisition/control integrity

### 2.1 Current acquisition service

Docelowy interfejs:

```c
typedef struct {
    int16_t ia_ctrl;
    int16_t ib_ctrl;
    int16_t ic_ctrl;
    uint8_t sector;
    uint8_t sample_quality;
    uint16_t sample_ccr4;
} phase_current_sample_t;
```

Quality:

```text
GOOD
ALTERNATE_WINDOW
INVALID_HOLD_LAST
```

### 2.2 Runtime calibration

Nie kopiować tylko `16 samples` dlatego, że stock tak robi. EVistDrive może zrobić lepiej:

```text
64/128 samples
mean
min/max
peak-to-peak
variance/noise
range validation
last-known-good
retry
fault reason
```

Zachować sprzętową nominalną okolicę `2048`, ale nie hardcode'ować jej jako jedynego zera.

### 2.3 2-of-3 reconstruction

Zaimplementować jako osobny moduł zależny od sector observability, nie jako przypadkowe ify w ISR.

### 2.4 Start/stop lifecycle

Stanowo:

```text
IDLE
PREPARE_START
RUN
RAMP_TO_ZERO
NEUTRALIZE
IDLE
FAULT_STOP
```

Normalny stop i fault stop nie mogą dzielić jednej ścieżki odcięcia.

---

# 3. P1 — safety layer

## 3.1 Overcurrent warstwowy

Propozycja EVistDrive:

```text
OC_WARN
OC_LIMIT
OC_TRIP
```

Każdy próg ma:

- threshold,
- persistence count/time,
- action,
- reset/hysteresis rule,
- event counter.

Hard trip ma niezależnie móc wyłączyć MOE.

## 3.2 Hall/UVW plausibility

Walidować:

- invalid `000/111`,
- legal transition only,
- direction,
- max transition rate,
- timeout/stall,
- mismatch z angle estimator.

## 3.3 ADC health

Per kanał:

```text
OFFSET_OUT_OF_RANGE
OFFSET_NOISY
STUCK
SATURATED
SLEW_IMPLAUSIBLE
```

## 3.4 Brownout / battery protection

Rozdzielić:

```text
battery current limit
phase current limit
power limit
voltage sag/brownout protection
```

Nie mieszać ich w jednym `current_max`.

---

# 4. P2 — fizyczne jednostki / telemetryka

FOC może działać w natywnych jednostkach fixed-point, a warstwa telemetryczna konwertuje:

```text
raw ADC -> ctrl units -> A/V/°C/Nm
```

Zaleta: zmiana calibration gain nie zmienia matematyki krytycznej pętli, a diagnostyka jest czytelna.

Logować minimum:

```text
timestamp
motor_state
sector/UVW
angle
Iq_ref / Iq_meas
Id_ref / Id_meas
Ia/Ib/Ic raw+ctrl+A
ADC offsets
sample_quality
CCR1/2/3/4
MOE/CEN
Vbus
Ibat
temp
fault flags/counters
```

---

# 5. Co stock robi dobrze, ale EVistDrive może zrobić lepiej

| Stock idea | Zachować | Ulepszyć w EVistDrive |
|---|---|---|
| 16-sample offset calibration | tak | 64/128 + variance |
| invalid current sample -> hold last | tak | dodać quality metric + counter |
| multilevel overcurrent | tak | jawne time-based persistence + telemetry |
| neutral PWM before/after run | tak | formalny state machine contract |
| relative BEMF baselines | tak | health score + per-channel diagnostics |
| fixed-point control domain | tak | osobna physical-units view |
| temperature compensation | po audycie | model per hardware revision |

---

# 6. Czego nie przenosić ślepo

- `ARR=2000`, `CCR4=1999`, margins 243/372/129 bez przeliczenia czasu,
- stockowej LUT NTC do innego sensora,
- offset bounds 1920..2176 do innego front-end,
- 9-level current tables do innego produktu,
- CAN mapping bez profilu urządzenia,
- fault thresholds bez poznania physical current gain,
- kompensacji temperaturowej bez potwierdzenia jej celu.

---

# 7. Następne zadania reverse — kolejność

1. **Porównać CR X30P 2.1 z CR X30P 5.0** i zidentyfikować odpowiedniki current ADC / OC / start-stop.
2. Dokończyć wszystkie xref threshold #1/#2/#3 i actions niższych poziomów.
3. Znaleźć service/BESST paths dla prądów/diagnostyki.
4. Szukać bezpośredniego bridge `Iq/phase current -> battery current`.
5. Zidentyfikować właściwy thermal foldback.
6. Zamknąć physical U/V/W order.
7. Zamknąć VREFINT usage.
8. Dopiero potem wpisać finalny `PHASE_CURRENT_A_PER_COUNT` jako PEWNE.
