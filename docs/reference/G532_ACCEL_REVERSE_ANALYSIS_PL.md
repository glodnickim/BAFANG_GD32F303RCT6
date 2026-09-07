# G532 — analiza mechanizmu ACCEL / akceleracji w torze mocy

## 1. Zakres analizy

Analiza dotyczy dokładnie obrazu:

`CRX30PC3615F805001.0_G532_250W_25_700-2185_Git-67a37d78_20251117_1756(1).bin`

- identyfikacja w obrazie: `CR X30P.250.FC 5.0`
- build: `Sample_67a37d78`
- SHA-256: `93c85ab1db4c4e9108ba07014cccf859ad4e53b3d7ce2371458f3edd0e7398a8`

Obraz ma 32-bajtowy nagłówek kontenera. W starszej dokumentacji adresy kodu były podawane po przemapowaniu payloadu na `0x08000000`. W niniejszym raporcie podaję oba adresy:

- `adres rebased` — zgodny ze starszym reverse,
- `adres fizyczny` — rzeczywisty adres po załadowaniu payloadu pod `0x0800A800`.

Przeliczenie:

```text
adres fizyczny = adres rebased + 0xA800
```

---

## 2. Najważniejszy wniosek

W G532 parametr `ACCEL`:

1. nie jest limitem mocy w watach,
2. nie jest limitem prądu,
3. nie zmienia wzmocnień PI,
4. nie steruje bezpośrednio PWM,
5. nie jest końcową rampą `Iq` w szybkim FOC.

Jest to **rampa narastania znormalizowanego żądania asysty**. Wynik rampy ma postać współczynnika `Q12` od `0` do `4096`. Współczynnik ten skaluje wyznaczoną wcześniej amplitudę żądania momentu/prądu w wolnej warstwie sterowania, zanim powstanie surowa para `Id/Iq`.

Dopiero dalej występuje niezależny, wewnętrzny limiter zmian `D/Q`, umieszczony tuż przed regulatorami PI.

W praktyce G532 ma więc co najmniej dwie odrębne warstwy wygładzania:

```text
ACCEL poziomu asysty
    ↓
wygładzenie żądania w warstwie supervisory
    ↓
generator amplituda + kąt → raw Id/Iq
    ↓
ograniczenie kołowe wektora prądu
    ↓
niezależny D/Q slew
    ↓
PI Id / PI Iq
    ↓
SVPWM
```

---

## 3. Gdzie zapisane są wartości ACCEL

Obiekt CAN `0x6010` jest 64-bajtowym blokiem konfiguracji. Jego bufor znajduje się pod:

```text
RAM 0x2000430D
```

Aktywna tablica akceleracji to:

```text
0x6010 data[1..9]
```

czyli dziewięć bajtów dla dziewięciu zapisanych slotów poziomu asysty.

`data[0]` jest osobnym polem. W tej rewizji ma domyślnie `50`, jest mnożone przez `50` i przenoszone do innego pola sterowania. Nie bierze udziału w równaniu budującym dziewięć kroków narastania, więc nie należy nazywać go bez dowodu „globalnym ACCEL”.

Domyślna zawartość aktywnych slotów:

```text
data[1..9] = 3, 4, 4, 5, 6, 6, 7, 8, 8
```

Funkcja kopiująca konfigurację do runtime:

```text
rebased:  0x080119C0
physical: 0x0801C1C0
```

kopiuje dziewięć bajtów do tablicy runtime. Dodatkowo firmware tworzy syntetyczny slot poziomu `0` z wartością `0`.

---

## 4. Rzeczywisty zakres obsługiwany przez firmware

Funkcja budująca parametry ramp:

```text
rebased:  0x08002D88
physical: 0x0800D588
```

dla każdego slotu wykonuje logicznie:

```c
uint8_t a = accel_code;

if (a >= 8) {
    a = 8;
}
if (accel_code == 0) {
    a = 1;
}
```

Efektywny zakres wynosi więc:

```text
1...8
```

Zachowanie wartości spoza zakresu:

```text
0       → 1
1...7   → bez zmiany
8...255 → 8
```

Wartość większa oznacza **szybsze** narastanie.

To jest ważniejsze od zakresów pokazywanych przez aplikacje. Interfejs może pozwalać wpisać `0...100`, `0...255` albo pokazywać globalny suwak `1...9`, ale ta konkretna rewizja G532 redukuje aktywny kod poziomu do `1...8`.

---

## 5. Jak kod ACCEL jest zamieniany na szybkość rampy

Firmware ma dwie ukryte granice czasowe:

```text
T_fast = 25
T_slow = 250
```

Następnie oblicza:

```text
q = floor((T_slow - T_fast) / 7)
q = floor((250 - 25) / 7)
q = 32
```

Dla kodu `a`:

```text
denom(a) = T_slow + q - q*a
         = T_slow - q*(a - 1)

rise_step(a) = floor(0xA000 / denom(a))
```

gdzie:

```text
0xA000 = 40960
```

jest pełną skalą wewnętrznego stanu rampy.

Tabela dla ustawień fabrycznych:

| Kod ACCEL | Mianownik | Krok na aktualizację | Nominalny czas 0→100% |
|---:|---:|---:|---:|
| 1 | 250 | 163 | 2,52 s |
| 2 | 218 | 187 | 2,20 s |
| 3 | 186 | 220 | 1,87 s |
| 4 | 154 | 265 | 1,55 s |
| 5 | 122 | 335 | 1,23 s |
| 6 | 90 | 455 | 0,91 s |
| 7 | 58 | 706 | 0,59 s |
| 8 | 26 | 1575 | 0,27 s |

Czasy są policzone dla skoku z `0` do pełnego celu, bez dominacji innych limiterów:

```text
czas = ceil(40960 / rise_step) × 10 ms
```

Nie są to sekundy dojścia do konkretnej liczby watów. Jest to czas dojścia wewnętrznego współczynnika żądania od `0` do `1,0`.

---

## 6. Domyślne czasy według slotów poziomu

Firmware posiada dziesięć logicznych indeksów `0...9`:

- indeks `0` jest tworzony wewnętrznie i ma kod surowy `0`, który zostaje zamieniony na efektywne `1`,
- indeksy `1...9` pochodzą z `0x6010 data[1..9]`.

| Slot | Kod zapisany | Kod efektywny | Nominalny czas 0→100% |
|---:|---:|---:|---:|
| 0 | 0 | 1 | 2,52 s |
| 1 | 3 | 3 | 1,87 s |
| 2 | 4 | 4 | 1,55 s |
| 3 | 4 | 4 | 1,55 s |
| 4 | 5 | 5 | 1,23 s |
| 5 | 6 | 6 | 0,91 s |
| 6 | 6 | 6 | 0,91 s |
| 7 | 7 | 7 | 0,59 s |
| 8 | 8 | 8 | 0,27 s |
| 9 | 8 | 8 | 0,27 s |

Które sloty są widoczne użytkownikowi zależy od liczby poziomów skonfigurowanej w HMI.

---

## 7. Częstotliwość aktualizacji

Główny scheduler:

```text
rebased:  0x080118CC
physical: 0x0801C0CC
```

jest uruchamiany z pętli taktowanej przez `SysTick`.

Konfiguracja `SysTick`:

```text
rebased:  0x08014786
physical: 0x0801EF86

LOAD = 0x1A5DF = 107999
CTRL = 5
```

Przy zegarze rdzenia `108 MHz` daje to okres:

```text
(107999 + 1) / 108000000 = 1 ms
```

Scheduler wywołuje funkcję aktualizującą ACCEL tylko dla jednego z dziesięciu stanów licznika modulo 10:

```text
rebased:  0x08002F5E
physical: 0x0800D75E
```

Dlatego właściwy stan rampy ACCEL jest aktualizowany co:

```text
10 ms = 100 Hz
```

---

## 8. Prawo aktualizacji stanu

W funkcji `0x08002F5E` wybrany krok poziomu trafia do pola bieżącego kroku narastania. Następnie wykonywane jest logicznie:

```c
delta = target - state;

if (delta > rise_step) {
    state += rise_step;
} else if (delta < fall_step_negative) {
    state += fall_step_negative;
} else {
    state = target;
}
```

Najważniejsze rozdzielenie:

```text
rise_step          ← tablica ACCEL wybranego poziomu
fall_step_negative ← osobny parametr, liczony niezależnie
```

Parametr ACCEL steruje więc dodatnią gałęzią narastania. Nie jest źródłem kroku opadania.

Po aktualizacji:

```text
gain_q12 = state / 10
```

czyli:

```text
state 0...40960
        ↓ /10
gain  0...4096
```

`4096` odpowiada `1,0` w formacie `Q12`.

---

## 9. Dokładne miejsce w torze momentu / mocy

Warstwa supervisory:

```text
rebased:  0x080015D0
physical: 0x0800BDD0
```

kopiuje współczynnik ACCEL do swojej struktury i w aktywnej gałęzi wykonuje mnożenie typu:

```c
ramped_demand = (candidate_demand * accel_gain_q12) >> 12;
```

Instrukcje odpowiedzialne za użycie współczynnika znajdują się m.in. w okolicy:

```text
rebased:  0x08001852...0x0800185E
physical: 0x0800C052...0x0800C05E
```

Wynik trafia do pola docelowej amplitudy żądania. Następnie ta sama funkcja stosuje jeszcze osobny slew wyjściowy:

```text
target:        context + 0x58
current state: context + 0x5A
rise step:     context + 0x62
fall step:     context + 0x4A
```

Dopiero wynik dalszego skalowania zostaje zapisany jako supervisory request.

Generator wektora:

```text
rebased:  0x080116C0
physical: 0x0801BEC0
```

zamienia amplitudę i kąt na dwie składowe prądu:

```text
axis_1 ≈ magnitude × cos(angle)
axis_2 ≈ magnitude × sin(angle)
```

W normalnej jeździe kąt jest w pobliżu `90°`, więc:

```text
Id_raw ≈ 0
Iq_raw ≈ magnitude
```

Pełny tor:

```text
torque / cadence / stan jazdy / wybrany poziom
    ↓
wyznaczenie docelowego żądania asysty
    ↓
limity i logika supervisory
    ↓
ACCEL: rampa znormalizowanego współczynnika 0...1
    ↓
mnożenie kandydackiej amplitudy żądania przez współczynnik ACCEL
    ↓
dodatkowy slew wyjścia supervisory
    ↓
amplituda wektora prądu
    ↓
generator magnitude + angle
    ↓
raw Id / raw Iq
    ↓
kołowy limiter wektora prądu
    ↓
niezależny D/Q slew
    ↓
PI Id / PI Iq
    ↓
inverse Park / SVPWM / CCR
```

Dlatego od strony fizycznej `ACCEL` najbardziej bezpośrednio kształtuje narastanie żądania momentotwórczego `Iq`. Moc elektryczna rośnie w ślad za nim, ale nie jest regulowana wprost w jednostkach `W/s`.

---

## 10. Niezależna rampa przed PI

Szybki FOC ma własny limiter zmian składowych `D/Q`.

Funkcja slew:

```text
rebased:  0x08010606
physical: 0x0801AE06
```

Struktura ma oddzielnie:

```text
target
rise step
fall step
current state
output
```

Domyślne wartości dla osi Q w tej rewizji są ładowane z konfiguracji jako dwa osobne kroki. Wartość fabryczna obu pól wynosi `10` wewnętrznych jednostek na wywołanie szybkiej pętli.

Ta rampa:

- jest po kołowym ograniczeniu wektora prądu,
- jest bezpośrednio przed PI,
- nie jest zasilana tablicą `0x6010`,
- działa niezależnie od użytkowego ACCEL.

To wyjaśnia, dlaczego nawet przy kodzie ACCEL `8` odpowiedź nie musi być idealnym skokiem: późniejsza rampa `Iq` nadal może ograniczyć zbocze.

---

## 11. Co z „global acceleration 1...9” z aplikacji

Ogólne aplikacje Bafang pokazują dwa różne typy danych:

1. globalne pole `acceleration` z suwakiem `1...9`,
2. tablicę akceleracji poziomów, często edytowaną w UI jako `0...100`.

Nie wolno przenosić tych zakresów bezpośrednio do tej rewizji G532.

Bezpośredni reverse wykazuje:

```text
aktywny zakres kodu poziomu w G532 = 1...8
```

oraz:

```text
0 → 1
>=8 → 8
```

Nie znalazłem dowodu, że ogólne pole aplikacji „global acceleration 1...9” jest tym samym polem, co `0x6010 data[0]` w tej binarce. `data[0]` ma domyślnie `50` i nie uczestniczy w równaniu dziewięciu kroków narastania.

Wniosek implementacyjny:

- jako pewnik traktować `0x6010 data[1..9]`,
- jako natywny zakres traktować `1...8`,
- nie nazywać `data[0]` globalnym ACCEL bez dalszego potwierdzenia dataflow,
- nie mapować suwaka `0...100` liniowo na czas, bo zależność w firmware jest nieliniowa.

---

## 12. Znaczenie dla zatrzymywania

Fabryczny `ACCEL` nie steruje normalnym zejściem momentu do zera. Dodatnia i ujemna gałąź mają inne źródła kroku.

Jeżeli w EVistDrive zmiana parametru ACCEL wyraźnie zmienia czas zatrzymywania, oznacza to najprawdopodobniej jedną z sytuacji:

1. ten sam parametr jest używany błędnie dla ramp-up i ramp-down,
2. stan rampy ACCEL nie jest odłączany od ścieżki STOP,
3. późniejszy supervisory slew lub końcowy Q-slew utrzymuje niezerowy stan,
4. STOP oczekuje na wyzerowanie kilku kaskadowych ramp.

Wierne odtworzenie G532 wymaga osobnego ownership:

```text
ACCEL_RISE
SUPERVISORY_FALL
FINAL_Q_RISE
FINAL_Q_FALL
STOP_STATE / PWM-OFF
```

Nie należy naprawiać długiego STOP przez przyspieszanie użytkowego ACCEL.
