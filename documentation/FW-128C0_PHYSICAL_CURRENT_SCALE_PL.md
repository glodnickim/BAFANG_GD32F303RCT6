# FW-128C0 — dowód fizycznej skali prądu

**Wynik:** **STOP** — połowa łańcucha udowodniona dokładnie, druga połowa **nie istnieje w źródłach**
**Tryb:** audyt statyczny + dowód matematyczny + obliczenie hostowe. Zero zmian w sterowaniu.
**Wejście:** FW-128A `ebfaa8b`, kontrakt FW-128B `2c15455`

---

## 0. Wynik w jednym zdaniu

Cała droga **od LSB przetwornika do `MS.i_q` jest udowodniona dokładnie i ma wzmocnienie 1**;
droga **od amperów do LSB nie jest nigdzie zapisana** — brakuje rezystancji bocznika, wzmocnienia
wzmacniacza i napięcia odniesienia. Nieznana jest **jedna liczba** mnożąca cały łańcuch, a nie
rozsiany zbiór niewiadomych.

```
1 jednostka MS.i_q  =  1 LSB przetwornika  =  K amperów      K = NIEZNANE
1 jednostka MS.i_d  =  1 LSB przetwornika  =  K amperów      to samo K
1 jednostka Ia/Ib/Ic =  1 LSB przetwornika  =  K amperów      to samo K
```

`K = VDDA / (4096 · G · R_bocznik)`. Żadnej z tych trzech wielkości nie ma w repozytorium.

## 1. Kompletny łańcuch źródłowy

| etap | wejście | wyjście | znak | mnożenie / dzielenie | format | nasycenie | skala jednostki |
|---|---|---|---|---|---|---|---|
| ADC (12-bit, inserted) | napięcie | 0..4095 | bez znaku | — | u16 | zakres | **LSB** |
| sprzętowy IOFF (`main.c:1711/1716/1721`) | raw | raw − IOFF | **ze znakiem** | odejmowanie | i16 | — | **×1, offset** |
| offset programowy (`main.c`, `current_cal.valid`) | JDR | JDR − offset | ze znakiem | odejmowanie | i16 | — | **×1, offset** |
| rekonstrukcja FW-127 (`sample_window.c:96-98`) | dwie fazy | trzecia | ze znakiem | `−a−b` (Kirchhoff) | i16 | — | **×1** |
| Clarke (`arm_clarke_q31`) | Ia, Ib | α, β | ze znakiem | `×0x24F34E8B >>30`, `×0x49E69D16 >>30` | Q30 → i32 | `__QADD` | **×1 (amplitudowa)** |
| Park (`arm_park_q31`) | α, β | d, q | ze znakiem | `×sin/cos >>31` | Q31 → i32 | `__QADD/__QSUB` | **×1 (obrót)** |
| filtr IIR (`FOC.c:132-139`) | q | `MS.i_q` | ze znakiem | `−fil>>3; +x; >>3` | i32 | — | **×1 (punkt stały fil = 8x)** |

**Nigdzie na tej drodze nie ma mnożenia zmieniającego jednostkę.** To jest cała treść części A.

## 2. Równanie ADC

```
delta_LSB = raw_ADC − IOFF_sprzętowy − offset_programowy
I_faza[A] = delta_LSB · K
K = VDDA / (4096 · G · R_bocznik)          ← NIEZNANE
```

VDDA nie jest korygowane przez VREFINT — kanał VREFINT **nie jest w EVistDrive czytany
w ogóle** (sprawdzone: brak jakiegokolwiek odwołania w `src/`). Zatem nawet nominalne 3,3 V jest
założeniem płytki, nie pomiarem.

## 3. Clarke — dowód, że nie zmienia skali

```c
alpha = Ia
beta  = (Ia · 0x24F34E8B >> 30) + (Ib · 0x49E69D16 >> 30)
```

`0x24F34E8B / 2^30 = 0,5773502691` = 1/√3, błąd względny **−2,04·10⁻¹⁰**
`0x49E69D16 / 2^30 = 1,1547005381` = 2/√3, ten sam błąd

Dla zrównoważonego zestawu `Ia = A·cos θ`, `Ib = A·cos(θ−120°)` wychodzi `β = A·sin θ` —
czyli **transformata amplitudowo-niezmiennicza**: `|(α,β)| = A = amplituda szczytowa fazy`.
Zmierzone hostowo: największe odchylenie **2 LSB na 72 kątach**.

## 4. Park — dowód, że nie zmienia skali

```c
d = (α·cos >>31) + (β·sin >>31)
q = (β·cos >>31) − (α·sin >>31)
```

Czysty obrót z sin/cos w Q31 → wzmocnienie 1. `arm_sin_cos_q31` jest w prekompilowanej
bibliotece `libarm_cortexM4lf_math.a` (brak źródła w repo), ale to nie ma znaczenia dla skali:
niezmienniczość amplitudy zależy od `sin²+cos²=1`, a nie od dokładności tablicy.

## 5. Filtr wyjściowy

```c
fil -= fil>>3;  fil += x;  MS.i_q = fil>>3;
```
Punkt stały: `fil = 8x` → `MS.i_q = x`. **Wzmocnienie stałoprądowe dokładnie 1.** Filtr kosztuje
czas ustalania i podprogowe przesunięcie od zaokrąglania w dół, nie jednostkę.

## 6. Konwencja d/q — to trzeba mówić razem z liczbą

Transformaty są **amplitudowo-niezmiennicze**, więc:

> `MS.i_q = 700` znaczy **amplituda szczytowa prądu fazowego = 700 LSB**,
> a nie wartość skuteczna i nie prąd baterii.

Przy zrównoważonym zestawie i `i_d = 0`: `I_faza_RMS = MS.i_q / √2`.
Nie jest to konwencja mocowo-niezmiennicza (nie ma czynnika √(2/3)).

## 7. Granica zaokrągleń

Każde przesunięcie w łańcuchu to **przesunięcie arytmetyczne na typie ze znakiem**, czyli
zaokrągla **w dół**, a nie w stronę zera. Zmierzona hostowo koperta błędu całego łańcucha na
120 kątach: **−2 … +2 LSB**. Liniowość sprawdzona na `A = 50…1600 LSB`: największe odchylenie
**2 LSB**, bez krzywizny i bez członu stałego.

## 8. STOP — dokładnie czego brakuje

| wielkość | gdzie wchodzi | czy jest w repo |
|---|---|---|
| **R_bocznik** | LSB → napięcie → prąd | **NIE** |
| **wzmocnienie wzmacniacza G** | to samo miejsce | **NIE** |
| **VDDA / V_ref** | LSB → napięcie | **NIE** (brak VREFINT, brak stałej) |
| rozdzielczość ADC | LSB → napięcie | TAK (12 bit, sprzętowo) |
| kierunek znaku (dodatni LSB = prąd w którą stronę) | znak prądu | **NIE** — pochłaniany przez `MP.reverse`/`angle_correction` |

Otwarte pytanie jest już zapisane w repozytorium jako priorytet 1:
`M820_OPEN_QUESTIONS_AND_EVISTDRIVE_PORTING.md` §1.1, „znaleźć Rshunt i amplifier gain
ze zdjęcia PCB/schematu".

## 9. Dlaczego `CAL_I = 95` NIE jest tą stałą — i dlaczego naiwne odczytanie go myli o 1,5×

`config.h:30` mówi wprost: *„Zurückgerechnet aus Batteriestrom = Tastverhältnis · Motorstrom"*
— **wsteczne wyliczenie z prądu baterii**. To konsekwencja innej stałej, nie pomiar toru
analogowego. Ale jest gorzej niż „nieudowodnione":

Firmware liczy (`main.c:3546`, `assist_modes.c:746/830`):
```
I_bat[mA] = (i_q · CAL_I · u_abs) >> 11
```
Ze skalowania SVPWM (`FOC.c:238`, `FOC.h:52`) wynika `u_abs / 2048 = V_faza_szczyt / V_bus`.
Bilans mocy przy transformacie **amplitudowo-niezmienniczej** to
`P = 1,5 · V_faza_szczyt · I_faza_szczyt`, więc:

```
I_bat = 1,5 · (u_abs/2048) · I_faza_szczyt
```

Porównanie z równaniem firmware'u daje **`I_faza_szczyt[mA] = i_q · CAL_I / 1,5 ≈ i_q · 63,3`**.

> Czyli `CAL_I` **nie jest** mA na LSB. Jest stałą zbiorczą bilansu mocy, która **pochłonęła
> czynnik 3/2** transformaty. Kto odczyta `CAL_I` jako „95 mA na jednostkę", pomyli się
> **o 1,5×**. To wciąż **nie jest dowód** wartości 63,3 — to obserwacja spójności wewnętrznej,
> pokazująca dodatkowy błąd systematyczny **ponad** nieznane `K`.

## 10. Kontrola zdrowego rozsądku (tylko kontrola, nie dowód)

- `parser.c:163`: `phase_current_max = Para1[9]·1000/CAL_I` — **konfiguracja użytkownika jest
  w amperach** i przechodzi przez `CAL_I`. Jeśli `CAL_I` jest błędne, wszystkie ampery pokazywane
  i ustawiane w UI są błędne o ten sam czynnik — ale **zachowanie** definiuje liczba wewnętrzna,
  więc jest to błąd etykiety, nie sterowania.
- `parser.c:25`: górna granica rozsądku `80000/CAL_I` = 842 jednostek (czyli „80 A").
- `PH_CURRENT_MAX = 700` to **34 %** maksimum możliwego do przedstawienia dla zrównoważonego
  zestawu (~2075 LSB). Zostaje zapas.
- **`CAL_BAT_I = 37,0` mA/LSB też nie ma udokumentowanego pochodzenia.** Rejestr dowodów
  (`M820_REVERSE_EVIDENCE_LEDGER.md` w. 33) podaje dla stocku **39,215686** mA/count (10/255 A)
  jako PEWNE z kontraktu CAN 0x3201. Różnica **5,65 %**, i to w kierunku **niebezpiecznym**:
  zaniżony odczyt prądu baterii pozwala popłynąć **większemu** prądowi niż ustawiony limit.
  **To jest wejście dla FW-128B, nie dla tej karty.**

## 11. Co z tego wynika dla FW-128B — karta NIE jest zablokowana

Kontrakt FW-128B §12 mówił: STOP, jeśli skala amperowa okaże się potrzebna. **Nie jest
potrzebna**, i to jest główny praktyczny wynik C0:

- limiter baterii porównuje **zmierzony prąd baterii [mA]** z **konfigurowanym limitem [mA]** —
  obie wielkości są w tej samej domenie i **żadna nie przechodzi przez `K`**;
- jego wyjściem ma być **redukcja żądania**, którą można wyrazić **bezwymiarowo** (ułamek
  bieżącego żądania) albo jako **przyrost na sekundę w jednostkach żądania**;
- w obu postaciach **dokładność stanu ustalonego zależy wyłącznie od pomiaru prądu baterii**,
  a `K` wpływa najwyżej na dynamikę, którą i tak stroi się empirycznie.

**Wniosek: FW-128B może ruszyć.** Warunek: limiter **ciągły** (§4 kontraktu) — limiter binarny
z progiem w amperach fazowych wymagałby `K`, ciągły nie wymaga.

## 12. Co z tego wynika dla FW-128C (T1/T2/T3) — podstawa jednostkowa, bez progów

Przeliczenie na przyszłość, **bez wybierania wartości**:

```
próg X amperów szczytowo   →   X / K  LSB          (K nadal nieznane)
```

Najlepszy sygnał ochronny: **`max(|Ia|, |Ib|, |Ic|)`** po walidacji i rekonstrukcji FW-127 —
bo to jedyna wielkość, która nie zależy od kąta rotora ani od podziału d/q, i jest w tej samej
jednostce, w której działa cała ochrona.

**Ustalenie, które FW-128C musi znać (K niepotrzebne):**

> Istniejący wyłącznik w `FOC.c:148` — `MS.i_d > (PH_CURRENT_MAX<<2)` = **2800** — leży
> **powyżej** maksimum osiągalnego dla zrównoważonego zestawu (~**2075** LSB, bo pojedyncza faza
> nie może wyjść poza ±2075 po odjęciu IOFF ≈ 2020 z 12 bitów). **Nie może zadziałać przy
> prawdziwym, zrównoważonym przetężeniu, jakkolwiek dużym.** Zadziała wyłącznie przy
> niezrównoważonej lub uszkodzonej akwizycji. **To nie jest ochrona nadprądowa** — mimo nazwy
> i mimo tego, że zatrzymuje mostek i wchodzi w `while(1){}`.

## 13. Testy hosta

`tests/host/fw128c0_current_scale_host.c` — dwie połowy:

- **model numeryczny**: dokładna arytmetyka całkowitoliczbowa Clarke/Park/IIR, wraz z
  zaokrąglaniem w dół i nasycającym `__QADD/__QSUB`;
- **strażnik źródeł** na `arm_math.h`, `FOC.c`, `sample_window.c`, `main.c` — znak po znaku
  sprawdza, że modelowana arytmetyka to ta, która naprawdę jest w drzewie.

Model jest konieczny, bo `arm_clarke_q31`/`arm_park_q31` używają `__QADD/__QSUB` (brak na hoście),
a `arm_sin_cos_q31` nie ma źródła w repo. **Strażnik jest tym, co czyni model wiążącym.**

Pokrycie G1–G10 zgodnie z kartą. Wyniki:

| test | wynik |
|---|---|
| G2 Clarke, 72 kąty | najgorsze odchylenie **2 LSB** |
| G3 Park przy θ zgodnym | `A=1200 → i_d=1199, i_q=−1` |
| G4 wektor tylko-q, 24 kąty | najgorsze **2 LSB** |
| G6/G7 znak | `+q=900`, `−q=−901`, symetria w granicach 3 |
| G8 liniowość `A=50…1600` | najgorsze **2 LSB** |
| G9 koperta zaokrągleń, 120 kątów | **−2 … +2 LSB** |
| G10 zero | dokładnie 0; pojedynczy LSB nie ginie |

**Wynik pakietu: ALL CHECKS PASSED. Baseline bez zmian (T14/T9 w `rolling_no_assist`) → 0 nowych
regresji.**

## 14. Sprzęt

**NIEWYMAGANY dla tej karty** — nic nie zmieniono w produkcji, a wszystko, co dało się udowodnić,
udowodniono ze źródeł.

**Domknięcie `K` wymaga pomiaru**, i to takiego, którego nie da się zrobić z fotela: albo odczyt
`R_bocznik` i wzmocnienia ze schematu/PCB, albo pomiar prądu cęgami przy jednoczesnym logu
`MS.i_q`. **Nie proponuję tego teraz** — FW-128B nie potrzebuje `K` (§11).
