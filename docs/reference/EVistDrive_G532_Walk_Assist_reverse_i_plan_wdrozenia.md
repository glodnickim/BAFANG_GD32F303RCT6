# EVistDrive — G532 Walk Assist
## Reverse engineering G532 + konserwatywny plan wdrożenia Walk Assist do EVistDrive

**Firmware referencyjny:** `CRX30PC3615F805001.0_G532_250W_25_700-2185_Git-67a37d78_20251117_1756(3).bin`  
**SHA-256:** `93c85ab1db4c4e9108ba07014cccf859ad4e53b3d7ce2371458f3edd0e7398a8`  
**Cel dokumentu:** odtworzyć charakter działania stockowego Walk Assist G532 bez ponownego projektowania funkcji od zera, a następnie przenieść ten charakter do EVistDrive z jedną świadomą zmianą: podstawowym sygnałem ograniczającym prędkość ma być **RPM zębatki / wyjścia przekładni**, a nie prędkość koła.

---

# 1. Najważniejszy wniosek

Walk Assist G532 nie wygląda jak klasyczny regulator:

```text
target speed
   -
actual speed
   ↓
PI speed
   ↓
Iq
```

W szczególności nie znaleziono dowodu, że podczas Walk G532 przechodzi z osobnej fazy startowej na klasyczny regulator prędkości.

Najlepszy model wynikający z reverse jest następujący:

```text
HMI WALK
   ↓
walidacja / debounce
   ↓
sprawdzenie warunków dopuszczenia
   ↓
STATE 5 = WALK
   ↓
własny Walk demand
   ↓
ograniczenie przez dostępny supervisory ceiling
   ↓
wolna rampa narastania
   ↓
wspólne skalowanie / limitery
   ↓
current / torque command
   ↓
Iq / FOC
```

Po ruszeniu silnik **pozostaje w state 5**. Nie ma potwierdzonego przejścia:

```text
STATE 5 → STATE 6
```

jako „start Walk → normalny Walk”.

To była ważna korekta względem wcześniejszego roboczego modelu.

**State 6 nie jest drugą fazą Walk.**

---

# 2. Co chcemy zachować w EVistDrive

Najważniejsza obserwacja z jazdy jest praktyczna:

- G532 rusza w Walk płynnie,
- nie daje gwałtownego strzału momentu,
- nie jest nerwowy,
- nie wykazuje charakteru źle nastrojonego speed-PI,
- start ma wyraźny, spokojny narost.

Dlatego celem EVistDrive nie jest „zaprojektować lepszy Walk od zera”.

Celem jest:

> **zachować stockowy charakter G532, ale zamiast uzależniać ograniczanie prędkości od koła, użyć RPM zębatki / wyjścia przekładni.**

---

# 3. Wejście Walk z HMI / CAN

W badanym G532 występuje odbiór rodziny:

```text
0x6300
```

4-bajtowy payload trafia do bufora od:

```text
0x20004219
```

Parser:

```text
0x080179FA
```

rozkłada pola m.in. tak:

```text
byte0:
  high nibble → osobne pole
  low nibble  → osobne pole

byte1:
  → pole command/state

byte2:
  bit0 → osobna flaga
  bit1 → osobna flaga
  bit5 → osobna flaga
```

W obserwowanej komendzie:

```text
ID:      0x03106300
payload: 05 06 00 01
```

wartość:

```text
byte1 = 0x06
```

jest wejściową komendą Walk.

## 3.1. Ważne: jedna ramka nie wystarcza

G532 nie traktuje pojedynczego `0x06` jako natychmiastowej zgody na napęd.

Blok około:

```text
0x0800353C
```

utrzymuje:

```text
candidate
accepted
stability counter
```

i dopiero po kilku kolejnych zgodnych aktualizacjach przyjmuje nowy stan.

Reverse wskazuje na:

```text
4 zgodne próbki
```

przed zaakceptowaniem nowej komendy.

Praktyczny model:

```text
HMI wysyła WALK
   ↓
candidate = WALK
   ↓
kolejne zgodne próbki
   ↓
debounce passed
   ↓
accepted WALK
```

Wniosek dla EVistDrive:

- nie reagować na pojedynczą przypadkową ramkę,
- zachować prosty debounce/stability gate,
- traktować Walk jako **podtrzymywany stan**, nie pojedynczy impuls „START”.

---

# 4. Wewnętrzne mapowanie Walk

Po zaakceptowaniu komendy HMI `0x06`, Walk jest mapowany na specjalny wewnętrzny selector:

```text
WALK selector = 0xFF
```

Normalne poziomy jazdy używają innych dodatnich wartości.

To daje ważne rozdzielenie:

```text
HMI code 0x06
      ↓
debounce
      ↓
internal selector 0xFF
      ↓
motor-control state machine
```

---

# 5. Główna maszyna stanów

Główna funkcja motor-control:

```text
0x080015F0
```

utrzymuje stan w strukturze około:

```text
0x20001304 + 0x50
```

i posiada wiele stanów wewnętrznych.

Dla Walk kluczowe są:

```text
STATE 1
STATE 5
```

## 5.1. STATE 1

State 1 jest stanem nieaktywnym / neutralnym dla tej gałęzi.

Dla wejścia do Walk sprawdzane są warunki podobne do:

```text
selector == 0xFF
dynamic speed/value <= 600
condition A == valid
walk eligibility == valid
condition B != 0
```

Interpretacja:

```text
600 ≈ 6.00 km/h
```

jest mocna, ale należy ją traktować jako **STRONG**, a nie absolutne potwierdzenie jednostki, dopóki writer pola nie zostanie w 100% opisany.

Jeżeli warunki są poprawne:

```text
STATE 1 → STATE 5
```

Ważny szczegół:

w cyklu przejścia do state 5 kod state 5 nie musi jeszcze wykonać pełnego aktywnego targetu. Właściwa obsługa state 5 następuje od kolejnej iteracji automatu.

---

# 6. STATE 5 = właściwy Walk Assist

Handler state 5 znajduje się około:

```text
0x080017B4
```

To jest kluczowy stan Walk.

## 6.1. Co ustawia state 5

Reverse wskazuje m.in. na:

```text
+0x68 = 2
```

oraz aktywację zewnętrznego statusu/markera Walk.

Dodatkowo:

```text
+0x74 = 1
```

stanowi mocny marker aktywnego Walk.

State 5 ustawia również:

```text
+0x72 = 900
```

oraz:

```text
+0x62 = +0x40
```

czyli wybiera **Walk-specific rise step**.

Kluczowy target powstaje jako:

```text
+0x58 = min(+0x44, +0x06)
```

Semantycznie:

```text
WALK_TARGET =
min(
    WALK_CONFIGURED_CEILING,
    CURRENT_SUPERVISORY_AVAILABLE_CEILING
)
```

To jest jedna z najważniejszych cech architektury G532.

Walk nie omija ograniczeń systemu.

---

# 7. Co właściwie reguluje G532 w state 5

State 5 nie wygląda na bezpośredni regulator mechanicznego momentu w Nm.

Nie wygląda też na klasyczny regulator prędkości koła typu:

```text
error = target_speed - actual_speed
PI(error) → motor current
```

Zamiast tego buduje:

```text
Walk demand
```

który następnie przechodzi przez wspólne ograniczenia i skalowanie, a ostatecznie wpływa na:

```text
current / torque command
→ Iq
→ FOC
```

Dla PMSM/BLDC:

```text
Iq ↑  → moment elektromagnetyczny ↑
Iq ↓  → moment elektromagnetyczny ↓
```

Dlatego w EVistDrive najbardziej naturalnym odpowiednikiem wykonawczym state 5 jest **Iq request**, ale nie oznacza to, że należy kopiować liczbę `5500` bezpośrednio jako ampery.

---

# 8. Najważniejsza cecha G532: rampa startowa

W stockowej/default konfiguracji badanego BIN-u znaleziono następujący charakter rampy state 5:

```text
Walk full-scale target ≈ 5500 jednostek

RAMP-UP:
+10 jednostek / 1 ms

RAMP-DOWN:
-50 jednostek / 1 ms
```

Z tego:

```text
0 → 5500:
~550 ms

5500 → 0:
~110 ms
```

To są wartości dla pełnej skali.

Jeżeli supervisor ogranicza target np. do 50% pełnej skali:

```text
target = 2750
```

to przy stałej prędkości `+10/ms` czas wejścia wynosi około:

```text
275 ms
```

Czyli stockowy G532 nie stosuje zawsze jednego stałego „550 ms timer”.

Stosuje **stałą szybkość narastania**, a czas zależy od wysokości aktualnego targetu.

## 8.1. Wersja znormalizowana

Dla wdrożenia niezależnego od liczby jednostek:

```text
full-scale rise rate:
1 / 0.550 s ≈ 1.818 full-scale / s

full-scale fall rate:
1 / 0.110 s ≈ 9.091 full-scale / s
```

To pozwala zachować stockowy charakter nawet jeśli EVistDrive pracuje w amperach Iq.

---

# 9. Dlaczego G532 rusza płynnie

Najbardziej prawdopodobny mechanizm odpowiedzialny za odczucie jest prosty:

```text
button
  ↓
Walk accepted
  ↓
target jest dodatni
  ↓
ale command startuje od 0
  ↓
0
10
20
30
...
  ↓
dopiero po czasie dochodzi do targetu
```

Dzięki temu nie występuje:

```text
0 → max moment w jednym cyklu
```

To jest dokładnie zachowanie, które należy zachować.

---

# 10. State 5 nie przełącza się na state 6

To jest kluczowa korekta reverse.

Nie znaleziono:

```text
STATE 5
  ↓
motor ruszył
  ↓
STATE 6
```

State 5:

- pozostaje state 5 podczas aktywnego Walk,
- przelicza Walk target,
- podlega limiterom,
- korzysta ze wspólnej rampy,
- wychodzi po anulowaniu / zmianie warunków.

Typowe wyjście:

```text
STATE 5 → STATE 1
```

## 10.1. Czym jest state 6

State 6 należy do innej gałęzi sterowania.

Używa innego zestawu współczynników i innej rampy.

Rzeczywiste przejście przypominające zmianę control-law występuje w zwykłej gałęzi jako:

```text
STATE 7 → STATE 6
```

a nie:

```text
STATE 5 → STATE 6
```

**Wniosek dla EVistDrive: nie implementować state 6 jako „drugiej fazy Walk”.**

---

# 11. Jak state 5 pozostaje aktywny

State 5 sprawdza m.in.:

```text
selector == 0xFF
```

oraz dodatkowe warunki hold/run.

Jeżeli Walk przestaje być żądany lub wymagany warunek znika:

```text
STATE 5 → STATE 1
```

To oznacza, że Walk jest stale rewalidowany.

Nie jest to mechanizm:

```text
otrzymałem START
→ jadę do timera końcowego
```

Tylko:

```text
while WALK is valid:
    licz target
    stosuj limity
    aktualizuj napęd
```

---

# 12. Wygaszenie Walk

Po puszczeniu przycisku:

```text
HMI przestaje raportować Walk
   ↓
debounce nowego stanu
   ↓
selector != 0xFF
   ↓
STATE 5 → STATE 1
```

W następnym przebiegu target zostaje sprowadzony do:

```text
0
```

a wspólna rampa zaczyna redukować bieżący command.

Stockowa/default szybkość:

```text
-50 jednostek/ms
```

co dla pełnej skali odpowiada około:

```text
110 ms
```

Następnie działają jeszcze downstream mechanizmy, w tym znany z wcześniejszego reverse finalny Q slew.

Dlatego wygaszenie należy traktować jako:

```text
Walk upstream ramp-down
      ↓
supervisory/current path
      ↓
final Q slew
      ↓
Iq → 0
```

---

# 13. Final Q slew — nie mylić z rampą Walk

Dla G532 wcześniej potwierdzono osobny downstream final Q slew około:

```text
~56.4 ms full-scale rise
~56.4 ms full-scale fall
16 kHz
```

Nie wolno traktować tego jako rampy Walk.

To są dwa różne etapy:

```text
WALK RAMP
   ↓
...
   ↓
FINAL Q SLEW
```

W EVistDrive należy szczególnie uważać, aby nie dodać trzeciej, dodatkowej długiej rampy i nie uzyskać ponownie zbyt ospałego lub „gumowego” sterowania.

---

# 14. Blokady i warunki bezpieczeństwa

Motor supervisor posiada nadrzędne warunki inhibit/fault.

W reverse pojawiła się m.in. maska:

```text
0x1830
```

która może wymusić brak aktywnego napędu.

Nie wszystkie bity tej maski są jeszcze przypisane 1:1 do nazw błędów.

Dlatego nie należy kopiować samej liczby `0x1830` do EVistDrive.

Należy skopiować zasadę:

```text
WALK REQUEST
      ↓
interlocks / safety
      ↓
dopiero WALK ACTIVE
```

W EVistDrive źródłem prawdy powinny być istniejące flagi:

- brake,
- controller fault,
- FOC fault,
- Hall/rotor validity,
- phase/battery overcurrent,
- thermal limit/fault,
- motor lifecycle readiness,
- communication validity,
- wheel-speed safety cutoff,
- ewentualne inne istniejące inhibit flags.

---

# 15. CAN — czy G532 czeka na dedykowany Walk ACK

Na obecnym poziomie reverse **nie znaleziono dowodu na osobny obowiązkowy handshake**:

```text
HMI: WALK REQUEST
controller: WALK ACK
HMI: CONFIRM
controller: RUN
```

Lepszy model jest taki:

```text
HMI stale raportuje bieżący Walk state
     ↓
G532 waliduje / debounce
     ↓
supervisor sam decyduje, czy napęd może być aktywny
```

Sterownik nadal wysyła normalne ramki statusowe/telemetryczne, ale nie ma potwierdzonej dedykowanej ramki:

```text
WALK_ACK
```

od której zależałoby uruchomienie silnika.

Jeżeli wymagane będzie odtworzenie zachowania HMI 1:1, należy wykonać osobny narrow reverse:

```text
WALK request
→ wszystkie TX G532
→ exact state-dependent reply map
```

Nie jest to jednak konieczne do pierwszego wdrożenia mechaniki Walk w EVistDrive, jeżeli obecna komunikacja HMI już poprawnie dostarcza request.

---

# 16. Co prawdopodobnie daje wrażenie „trzymania prędkości”

G532 state 5 nie wygląda na pełny regulator PI prędkości.

Znacznie bardziej pasuje model:

```text
stały / rampowany Walk demand
          +
speed-dependent available ceiling
```

czyli:

```text
Walk chce dać X
      ↓
supervisor mówi:
„przy tej prędkości wolno już tylko Y”
      ↓
state 5 daje min(X, Y)
```

To daje zachowanie podobne do regulatora prędkości, ale bez integratora i bez agresywnej pętli speed PI.

Taki mechanizm jest bardzo cenny dla EVistDrive, bo naturalnie ogranicza przestrzelenie.

---

# 17. Problem starego EVistDrive Walk

W poprzedniej konstrukcji występowały dwa powiązane problemy:

1. słaby start,
2. przestrzelenie zadanej prędkości.

Typowy mechanizm powodujący oba problemy wygląda tak:

```text
target RPM > 0
actual RPM = 0
      ↓
duży error
      ↓
speed PI od pierwszej chwili
      ↓
ograniczone Iq / słaby start
      ↓
integrator nadal zbiera błąd
      ↓
silnik wreszcie rusza
      ↓
integrator nadal mocno dodatni
      ↓
target osiągnięty
      ↓
Iq nadal wysokie
      ↓
przestrzelenie RPM
```

Dlatego **nie należy ponownie używać klasycznego PI prędkości od 0 RPM** jako głównego sterowania Walk.

---

# 18. Docelowa filozofia EVistDrive

Najbezpieczniejsza architektura:

```text
G532-like Walk demand ramp
           +
RPM zębatki jako soft governor / limiter
           +
istniejące limity EVistDrive
           +
final Iq / FOC
```

Nie:

```text
RPM target
   ↓
PI speed
   ↓
Iq
```

---

# 19. Dlaczego RPM zębatki zamiast prędkości koła

Prędkość koła zależy od:

- wybranego biegu,
- obwodu koła,
- przełożenia napędu,
- konfiguracji roweru.

Dla Walk bardziej naturalnym punktem odniesienia dla zachowania samego silnika jest:

```text
RPM zębatki / wyjścia przekładni
```

Jeżeli utrzymujemy podobne RPM zębatki:

- silnik pracuje w podobnym punkcie,
- zachowanie jest bardziej niezależne od wybranego biegu,
- na lekkim biegu rower jedzie wolniej i ma większą siłę na kole,
- na cięższym biegu rower jedzie szybciej przy tej samej prędkości zębatki.

Prędkość koła nadal powinna pozostać jako **hard safety/legal cutoff**, ale nie jako główne sprzężenie określające charakter Walk.

---

# 20. Jak uzyskać RPM zębatki

Nie jest potrzebny osobny czujnik, jeżeli EVistDrive ma poprawną informację z Halli / rotora.

Tor:

```text
Hall / electrical speed
       ↓
motor electrical RPM
       ↓
motor mechanical RPM
       ↓
stałe przełożenie przekładni
       ↓
gear/output RPM
```

Należy uważać, aby nie pomylić:

- electrical RPM,
- mechanical rotor RPM,
- output gear RPM,
- wheel RPM.

Docelowa zmienna powinna być jawna:

```c
walk_gear_rpm_actual
```

i mieć jednoznacznie udokumentowaną jednostkę.

---

# 21. Proponowany regulator prędkości: soft governor / droop limiter

Zamiast PI należy zastosować spokojny ogranicznik.

Przykład:

```text
gear RPM          speed factor
--------------------------------
< taper_start       1.00
...
target region       maleje liniowo
...
>= cutoff           0.00
```

Matematycznie:

```text
if rpm <= rpm_full:
    factor = 1

else if rpm >= rpm_zero:
    factor = 0

else:
    factor = (rpm_zero - rpm)
             / (rpm_zero - rpm_full)
```

Następnie:

```text
Iq_allowed_by_speed =
WALK_IQ_MAX × factor
```

i:

```text
Iq_request =
min(
    ramped_walk_iq,
    Iq_allowed_by_speed,
    other_supervisory_limits
)
```

To jest odpowiednik idei:

```text
min(Walk demand, available ceiling)
```

z G532.

---

# 22. Dlaczego taper nie powinien kończyć się dokładnie na target = 0 momentu

Jeżeli ustawimy:

```text
100 rpm = 0% Iq
```

to pod obciążeniem punkt równowagi będzie zwykle nieco poniżej 100 rpm.

To jest naturalne dla regulatora typu droop.

Dlatego lepszy model do testów:

```text
rpm_full < desired_rpm
rpm_zero > desired_rpm
```

Przykładowo koncepcyjnie:

```text
pełny moment do ~90% celu
miękki taper przez okolice celu
zero dopiero lekko powyżej celu
```

Nie należy jednak wpisywać tych procentów na stałe przed logami.

Najpierw trzeba zebrać dane z jazdy.

---

# 23. Start EVistDrive — kopiujemy charakter G532

Najważniejszy element:

## NIE używać speed PI podczas startu.

Start:

```text
Walk accepted
   ↓
ramped Walk demand od 0
   ↓
~stockowy full-scale rise 550 ms
   ↓
Iq request rośnie płynnie
   ↓
silnik zaczyna pracować
```

Najbezpieczniejsza implementacja znormalizowana:

```c
walk_ramp_norm ∈ [0.0, 1.0]

rise_rate = 1.0 / 0.550 s
fall_rate = 1.0 / 0.110 s
```

W każdym ticku:

```c
walk_ramp_norm =
    slew(
        walk_ramp_norm,
        walk_requested_norm,
        rise_rate,
        fall_rate,
        dt
    );
```

Następnie:

```c
walk_base_iq =
    walk_ramp_norm * WALK_IQ_MAX;
```

To zachowuje stockowy czas i proporcjonalne skrócenie rampy przy niższym target.

---

# 24. Ważne: nie kopiować `5500` jako amperów

`5500` jest wartością wewnętrzną G532.

Nie wolno napisać:

```c
Iq = 5.5 A
```

tylko dlatego, że stock ma `5500`.

W EVistDrive należy odwzorować **kształt i czas rampy**, a rzeczywiste:

```text
WALK_IQ_MAX
```

ustalić w domenie prądowej EVistDrive i poddać istniejącym limitom.

---

# 25. Proponowany pełny tor Walk w EVistDrive

```text
HMI WALK REQUEST
      ↓
CAN validation / debounce
      ↓
walk_request_valid
      ↓
SAFETY / INTERLOCKS
      ↓
WALK ACTIVE
      ↓
normalized Walk demand
      ↓
G532-LIKE RAMP
~550 ms full-scale UP
~110 ms full-scale DOWN
      ↓
base Walk Iq
      │
      ├────────────────────────────┐
      │                            │
      │                     GEAR RPM ACTUAL
      │                            │
      │                            ▼
      │                    SOFT RPM GOVERNOR
      │                            │
      │                            ▼
      │                     speed Iq ceiling
      │                            │
      └─────────────── min() ──────┘
                       │
                       ▼
             existing EVistDrive limits
                       │
             battery / phase / thermal
                       │
                       ▼
                   FINAL Iq
                       │
                       ▼
                      FOC
```

Prędkość koła działa obok:

```text
wheel speed
   ↓
hard Walk safety/legal cutoff
```

ale nie jest podstawowym feedbackiem prędkości Walk.

---

# 26. Proponowane stany EVistDrive

Nie trzeba kopiować numerów stanów G532.

Czytelniejszy model:

```c
WALK_OFF
WALK_REQUESTED
WALK_ACTIVE
WALK_RELEASE
```

Opcjonalnie `WALK_ACTIVE` może mieć wewnętrzną informację:

```text
ramp progress
```

bez osobnego „start mode”.

To najlepiej odpowiada odkryciu z G532:

> start jest pierwszą częścią tego samego state 5, a nie osobnym state 5 → 6.

---

# 27. WALK_OFF

Warunki:

```text
brak request
```

Stan:

```text
walk_ramp_norm = 0
walk_base_iq = 0
walk_speed_factor = 0
walk_iq_request = 0
```

Jeżeli aktywny final Q / lifecycle nadal wygasa, nie resetować siłowo stanów FOC poza istniejącym mechanizmem systemu.

---

# 28. WALK_REQUESTED

Po poprawnym request:

sprawdzić:

- CAN/HMI request valid,
- brake inactive,
- no blocking fault,
- Hall/rotor state acceptable,
- motor lifecycle ready,
- wheel speed poniżej wejściowego limitu Walk,
- brak innych nadrzędnych inhibitów.

Jeżeli OK:

```text
WALK_REQUESTED → WALK_ACTIVE
```

Jeżeli nie:

```text
pozostań OFF / REQUESTED
walk_iq_request = 0
```

Logować powód blokady.

---

# 29. WALK_ACTIVE

W każdej iteracji:

```text
1. sprawdź request
2. sprawdź safety
3. aktualizuj rampę G532-like
4. policz gear RPM
5. policz speed factor
6. policz speed Iq ceiling
7. wybierz najmniejszy limit
8. opublikuj jeden finalny Walk Iq request
```

Kluczowa zasada ownership:

> Walk powinien mieć **jednego właściciela swojego Iq request**, ale finalny Iq nadal musi przechodzić przez wspólny supervisor/limitery EVistDrive.

Nie wolno robić bezpośredniego write do PI/FOC z modułu Walk.

---

# 30. WALK_RELEASE

Wyzwalacze:

- puszczenie przycisku,
- brake,
- fault,
- communication loss,
- hard wheel-speed cutoff,
- invalid Hall,
- inne krytyczne inhibit.

Dla zwykłego puszczenia:

```text
walk_requested_norm → 0
```

i stosujemy stock-like:

```text
~110 ms full-scale upstream fall
```

Dla błędu krytycznego można użyć istniejącej ścieżki safety shutdown, nawet jeśli jest szybsza.

Nie wolno opóźniać krytycznego odcięcia tylko po to, żeby zachować komfortową rampę.

---

# 31. Rampa a istniejący final Q slew

EVistDrive już ma downstream mechanizmy Iq/Q.

Dlatego przed wdrożeniem trzeba wykonać audit:

```text
Walk request
   ↓
Walk ramp
   ↓
?
   ↓
FINAL Iq
   ↓
final Q slew
```

i upewnić się, że nie istnieje dodatkowo:

```text
Walk ramp
+
ride ramp
+
generic current ramp
+
final Q slew
```

jeżeli każda z nich dodaje znaczący czas.

Cel:

- jedna świadoma upstream rampa Walk,
- wspólne limitery,
- znany final Q slew,
- brak przypadkowego „podwójnego wygładzania”.

---

# 32. Początkowe parametry do implementacji

Nie są to jeszcze finalne wartości tuningowe.

## Charakter G532

```text
WALK_RISE_FULL_SCALE_MS = 550
WALK_FALL_FULL_SCALE_MS = 110
```

## Speed governor

Do pierwszego testu należy zdefiniować:

```text
WALK_GEAR_RPM_TARGET
WALK_GEAR_RPM_FULL
WALK_GEAR_RPM_ZERO
WALK_GEAR_RPM_HARD_CUTOFF
```

Nie wpisywać docelowych RPM bez logów i przeliczenia przełożenia.

## Current

```text
WALK_IQ_MAX
```

powinien być normalnym parametrem EVistDrive i zawsze podlegać:

- phase current limit,
- battery current limit,
- thermal derating,
- FOC/current safety.

---

# 33. Filtr RPM zębatki

Nie używać bardzo długiego filtra.

Za długi filtr:

```text
RPM real rośnie
      ↓
filter nadal pokazuje mało
      ↓
governor za długo pozwala na pełne Iq
      ↓
overshoot
```

Za krótki / surowy:

```text
Hall jitter
   ↓
factor skacze
   ↓
Iq skacze
```

Rekomendacja:

- korzystać z istniejącej stabilnej estymacji Hall/RPM,
- dodać tylko minimalne wygładzenie konieczne do stabilnego speed factor,
- logować RAW i FILTERED RPM,
- stroić na podstawie jazdy.

---

# 34. Histereza i brak oscylacji

Soft governor powinien być ciągły.

Nie:

```text
99 rpm → full Iq
100 rpm → zero
99 rpm → full Iq
100 rpm → zero
```

Tylko:

```text
pełne Iq
   ↓
płynny taper
   ↓
małe Iq
   ↓
0
```

Jeżeli nadal będzie hunting:

- najpierw poszerzyć zakres taper,
- potem minimalnie filtrować RPM,
- dopiero na końcu rozważyć mały bounded trim.

Nie zaczynać od PI z integratorem.

---

# 35. Opcjonalna hybryda — tylko jeżeli droop jest za duży

Jeżeli po wdrożeniu okaże się:

```text
bez obciążenia:
RPM blisko celu

pod dużym obciążeniem:
RPM znacząco spada
```

można dodać mały correction term.

Najbezpieczniejszy wariant:

```text
base G532-like Walk
        +
bounded P-only RPM trim
```

Nie pełne PI.

Przykład architektury:

```text
Iq_base
   +
clamp(Kp × rpm_error, ±TRIM_MAX)
   ↓
Iq_request
```

Warunki:

- trim ograniczony małym procentem Walk range,
- brak integratora,
- trim wyłączony podczas pierwszej części startu,
- trim nie omija speed governor ani safety limits.

Dopiero jeżeli to nie wystarczy, można rozważyć integrator.

---

# 36. Czego NIE robić

## 36.1. Nie uruchamiać speed PI od 0 RPM

To ponownie tworzy ryzyko:

- weak start,
- windup,
- overshoot,
- agresywne przejęcie.

## 36.2. Nie kopiować state 6 jako drugiej fazy Walk

G532 tego nie robi.

## 36.3. Nie sterować Walk bezpośrednim write do FOC

Walk powinien produkować:

```text
Iq request
```

a nie omijać ownership toru prądowego.

## 36.4. Nie używać koła jako głównego feedbacku zachowania

Koło zostaje jako hard safety/legal cutoff.

## 36.5. Nie robić twardego speed cutoff w normalnym obszarze regulacji

Normalne dojście do prędkości ma używać taper.

Hard cutoff jest tylko ostatnim zabezpieczeniem.

## 36.6. Nie dodawać kolejnych długich ramp bez audytu

Każda dodatkowa rampa może powodować:

- ospałość,
- długi overrun,
- słaby response,
- trudne strojenie.

---

# 37. Plan wdrożenia — etap 0: audit ownership

Przed zmianą kodu znaleźć:

```text
current Walk request
   ↓
current PUSHASSIST_CURRENT / target
   ↓
all writers to Iq request
   ↓
all ramps
   ↓
FINAL Iq
```

Raport:

- kto produkuje Walk Iq,
- kto może go nadpisać,
- jakie rampy istnieją,
- gdzie są phase/battery limits,
- gdzie jest final Q slew.

Cel:

```text
ONE WALK OWNER
→ ONE FINAL Iq PATH
```

---

# 38. Etap 1: diagnostyka przed zmianą zachowania

Dodać logi:

```text
timestamp
walk_request_raw
walk_request_debounced
walk_state
walk_block_reason

hall_valid
motor_rpm
gear_rpm_raw
gear_rpm_filtered
wheel_speed

walk_ramp_norm
walk_base_iq
walk_speed_factor
walk_speed_iq_ceiling
walk_iq_request

final_iq_request
actual_iq

battery_current
phase_current_limit
battery_current_limit
thermal_limit
```

Bez tego późniejsze strojenie będzie ponownie „na czuja”.

---

# 39. Etap 2: wejście request / debounce

Zachować zachowanie podobne do G532:

```text
raw WALK
   ↓
stability counter
   ↓
valid WALK
```

Nie musi to być dokładnie 4 próbki, jeśli scheduler EVistDrive jest inny.

Najważniejsze jest zachowanie podobnego czasu, a nie literalnej liczby iteracji.

Jeżeli uda się ustalić dokładny okres stockowego parsera, odwzorować czas.

---

# 40. Etap 3: state machine Walk

Dodać jawny moduł:

```text
WALK_OFF
WALK_REQUESTED
WALK_ACTIVE
WALK_RELEASE
```

Każdy transition musi mieć:

- trigger,
- cancel condition,
- log reason.

Nie używać niejawnych flag rozrzuconych po kilku funkcjach.

---

# 41. Etap 4: G532-like ramp bez RPM regulation

Najpierw uruchomić tylko:

```text
button
→ safety
→ Walk active
→ 550 ms full-scale rise
→ Iq
```

bez nowego speed governor.

Celem testu jest odpowiedzieć:

> Czy sam start jest równie miękki jak G532?

Jeżeli nie:

- nie dodawać regulatora prędkości,
- najpierw naprawić rampę / current path / duplicate ramp.

To jest bardzo ważne.

---

# 42. Etap 5: dodać gear RPM measurement

Zweryfikować:

```text
Hall → rotor mechanical RPM → gear/output RPM
```

Test statyczny:

- ręczny obrót,
- wolna jazda,
- porównanie z oczekiwanym przełożeniem.

Nie włączać jeszcze ogranicznika.

Tylko logować.

---

# 43. Etap 6: soft gear-RPM governor

Dopiero po potwierdzeniu RPM:

```text
gear RPM
   ↓
continuous factor
   ↓
Iq ceiling
```

Następnie:

```text
walk_iq_request =
min(
    walk_base_iq,
    gear_speed_iq_ceiling,
    other_limits
)
```

Najpierw szeroki taper.

Wąski taper zwiększa ryzyko szarpania.

---

# 44. Etap 7: wheel-speed hard cutoff

Prędkość koła nie steruje podstawowym Walk.

Ale nadal:

```text
wheel_speed >= legal/safety limit
→ WALK inhibit / release
```

Dodać histerezę, aby nie przełączać stanu na granicy.

---

# 45. Etap 8: release

Sprawdzić osobno:

## Zwykłe puszczenie

```text
button release
→ ~110 ms full-scale upstream fall
→ downstream Q slew
```

## Brake / fault

```text
safety path
→ szybciej, zgodnie z istniejącym safety lifecycle
```

Komfortowa rampa nie może spowalniać safety stop.

---

# 46. Etap 9: test przeciążenia / stall

Scenariusze:

```text
Walk + ciężki bieg
Walk + podjazd
Walk + przytrzymane koło
Walk + brak ruchu mimo Iq
```

Nie pozwolić, aby stały `WALK_IQ_MAX` był utrzymywany bez końca przy pełnym stall.

Użyć istniejących:

- phase current limits,
- battery limits,
- thermal limits,
- motor protection.

Jeżeli trzeba, dodać osobny Walk stall timeout dopiero po analizie logów.

---

# 47. Etap 10: opcjonalny bounded RPM trim

Wdrożyć tylko jeśli:

- start jest dobry,
- taper jest stabilny,
- ale prędkość pod obciążeniem za mocno siada.

Najpierw P-only.

Bez integral.

---

# 48. Testy drogowe bez stanowiska

## Test A — koło uniesione

Cel:

- sprawdzić rampę,
- RPM,
- taper,
- cutoff.

Nie używać do końcowego strojenia momentu.

## Test B — płasko, lekki bieg

Sprawdzić:

- start,
- brak strzału,
- dojście do RPM,
- brak hunting.

## Test C — płasko, cięższy bieg

Sprawdzić niezależność od biegu.

## Test D — podjazd

Sprawdzić:

- czy nie brakuje momentu,
- czy governor nie ogranicza za wcześnie,
- czy RPM droop jest akceptowalny.

## Test E — zmiana biegu podczas Walk

Bardzo ważny dla przewagi RPM zębatki nad wheel-speed regulation.

## Test F — szybkie puszczenie

Ocenić release.

## Test G — brake podczas Walk

Musi mieć priorytet nad komfortowym ramp-down.

---

# 49. Kryteria akceptacji

## Start

- brak kliknięcia / strzału,
- brak nagłego skoku Iq,
- odczucie co najmniej tak płynne jak G532,
- czas pełnego narastania zbliżony do charakteru ~550 ms.

## Stabilność

- brak cyklicznego pompowania,
- brak oscylacji Iq,
- brak przestrzelenia znanego ze starego speed-PI,
- brak windup.

## RPM

- prędkość zębatki dochodzi spokojnie do obszaru docelowego,
- pod obciążeniem dopuszczalny niewielki droop,
- brak gwałtownego odcięcia przy pojedynczym przekroczeniu.

## Safety

- brake natychmiast ma wyższy priorytet,
- fault ma wyższy priorytet,
- wheel-speed cutoff działa niezależnie od gear RPM,
- current/thermal limits nie są omijane.

---

# 50. Minimalny pseudokod EVistDrive

```c
void walk_update(float dt_s)
{
    walk_request_valid = walk_request_filter(raw_walk_request);

    if (!walk_request_valid) {
        walk_requested_norm = 0.0f;
    } else if (walk_safety_ok()) {
        walk_requested_norm = 1.0f;
    } else {
        walk_requested_norm = 0.0f;
    }

    /* G532-like normalized ramp */
    walk_ramp_norm = slew_asymmetric(
        walk_ramp_norm,
        walk_requested_norm,
        1.0f / 0.550f,   // rise full-scale per second
        1.0f / 0.110f,   // fall full-scale per second
        dt_s
    );

    float walk_base_iq =
        walk_ramp_norm * WALK_IQ_MAX;

    float gear_rpm =
        get_walk_gear_rpm_filtered();

    float speed_factor;

    if (gear_rpm <= WALK_GEAR_RPM_FULL) {
        speed_factor = 1.0f;
    }
    else if (gear_rpm >= WALK_GEAR_RPM_ZERO) {
        speed_factor = 0.0f;
    }
    else {
        speed_factor =
            (WALK_GEAR_RPM_ZERO - gear_rpm) /
            (WALK_GEAR_RPM_ZERO - WALK_GEAR_RPM_FULL);
    }

    float speed_iq_ceiling =
        WALK_IQ_MAX * speed_factor;

    float walk_iq =
        min(walk_base_iq, speed_iq_ceiling);

    walk_iq =
        apply_existing_supervisory_limits(walk_iq);

    if (wheel_speed_hard_cutoff()) {
        walk_iq = 0.0f;
    }

    publish_walk_iq_request(walk_iq);
}
```

To jest pseudokod architektury, nie gotowy patch.

Przed wdrożeniem należy dopasować go do rzeczywistego ownership toru Iq EVistDrive.

---

# 51. Wariant jeszcze bardziej zbliżony do G532

Jeżeli chcemy zachować pełną analogię:

```text
G532:
Walk demand
→ speed-dependent available ceiling
→ min()
→ ramp / common path

EVistDrive:
Walk normalized demand
→ gear-RPM-dependent Iq ceiling
→ min()
→ final Iq ownership
```

Można zdecydować, czy governor ma działać:

### A. przed rampą

```text
target = min(Walk target, RPM ceiling)
→ ramp
```

bardziej podobnie do logiki target/ceiling.

### B. po rampie

```text
Walk ramp
→ min(ramped Walk, RPM ceiling)
```

prostsze, ale przy gwałtownym spadku ceiling może szybciej ograniczyć Iq.

**Rekomendowany audyt przed kodem:** ustalić dokładnie, w którym miejscu obecnego EVistDrive najbezpieczniej utrzymać pojedynczy owner/ramp.

---

# 52. Najważniejsze zasady wdrożenia

1. **Nie odkrywać Walk od nowa.**
2. Zachować około **550 ms full-scale ramp-up** charakteru G532.
3. Zachować wyraźnie szybszy fall około **110 ms full-scale** dla zwykłego release.
4. Nie używać state 6 jako części Walk.
5. Nie uruchamiać klasycznego speed PI od 0 RPM.
6. Podstawowym feedbackiem regulacji prędkości ma być **RPM zębatki**.
7. Prędkość koła zostawić jako **hard cutoff / safety**.
8. Speed control zrobić początkowo jako **continuous soft governor / droop limiter**.
9. Iq jest wykonawczą wielkością momentu, ale musi przechodzić przez wspólne limity.
10. Nie omijać final Iq ownership.
11. Nie dodawać kolejnych długich ramp bez audytu.
12. Najpierw logowanie i testy, potem ewentualny bounded P trim.
13. Integrator speed PI pozostawić wyłączony w pierwszej wersji.
14. Safety stop ma zawsze priorytet nad komfortową rampą.
15. Wszystkie nowe parametry mają mieć jednoznaczne jednostki.

---

# 53. Status wiedzy z reverse

## CONFIRMED / bardzo mocno potwierdzone

- komenda Walk wchodzi z HMI/CAN osobnym command/state,
- command jest filtrowany/debouncowany,
- Walk ma specjalny wewnętrzny selector `0xFF`,
- Walk korzysta z `state 5`,
- state 5 tworzy własny Walk target,
- target jest ograniczany przez dostępny supervisory ceiling,
- state 5 używa osobnego rise-step,
- po ruszeniu Walk pozostaje w state 5,
- nie istnieje potwierdzone `state 5 → state 6` jako faza Walk,
- state 6 należy do innej gałęzi,
- release Walk prowadzi z state 5 do state 1,
- Walk nie powinien być kopiowany jako bezpośredni write do PWM/FOC.

## STRONG

- `600` odpowiada około 6.00 km/h jako warunkowi prędkości,
- stockowy charakter rampy to około:
  - 550 ms full-scale rise,
  - 110 ms full-scale fall,
- speed-dependent supervisory ceiling współtworzy wrażenie utrzymywania prędkości,
- state 5 jest demand/current-torque based, nie klasycznym speed PI.

## Jeszcze do domknięcia, jeśli potrzebna jest kopia 1:1

- dokładna semantyka wszystkich pól inhibit G532,
- pełna mapa stockowych reply/status CAN podczas Walk,
- dokładna fizyczna jednostka każdego wewnętrznego Walk commandu,
- dokładne powiązanie `5500` z fizycznym Iq / prądem,
- dokładna stockowa krzywa speed-dependent ceiling,
- ewentualne NVM overrides parametrów domyślnych.

---

# 54. Decyzja architektoniczna dla EVistDrive

Rekomendowany kierunek:

```text
NIE:
rebuild Walk as speed PI

TAK:
G532-like Walk demand/ramp
+
gear-RPM soft governor
+
existing EVistDrive current/safety ownership
```

To najlepiej odpowiada celowi:

> zachować płynność i stabilność stockowego G532, a jednocześnie uzyskać sterowanie zależne od prędkości zębatki zamiast od prędkości koła.

---

# 55. Następny krok dla agenta implementującego

Agent nie powinien od razu pisać pełnego patcha.

Najpierw ma wykonać:

```text
1. audit obecnego Walk ownership
2. znaleźć wszystkie write'y do Walk/Iq
3. znaleźć wszystkie istniejące rampy
4. znaleźć źródło rotor/Hall RPM
5. wyprowadzić mechanical gear RPM
6. przygotować log signals
7. wdrożyć samą G532-like rampę
8. przetestować start
9. dopiero potem dodać gear-RPM governor
10. dopiero po logach ewentualnie stroić trim
```

Najważniejsze kryterium:

> **Nie pogorszyć istniejącej płynności napędu przez dodanie nowej konkurencyjnej pętli regulacji.**

---

# 56. Jednozdaniowa specyfikacja docelowa

**Walk Assist EVistDrive ma zachować G532-like płynny, około 550 ms full-scale narost żądania momentu/prądu i około 110 ms full-scale zwykłego wygaszenia, korzystać z jednego kontrolowanego toru Iq z istniejącymi limitami, a prędkość ma być stabilizowana spokojnym soft-governorem opartym na RPM zębatki, z prędkością koła pozostawioną wyłącznie jako nadrzędne zabezpieczenie/cutoff, bez klasycznego speed-PI od postoju.**
