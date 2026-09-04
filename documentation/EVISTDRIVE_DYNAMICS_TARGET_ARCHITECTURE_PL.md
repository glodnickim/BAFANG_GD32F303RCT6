# eVistDrive — ARCHITEKTURA DOCELOWA DYNAMIKI

Data: 2026-09-02
Status: **blueprint kierunkowy. Nie jest zgodą na implementację.**
Warunek wdrożenia dowolnego kroku: zaakceptowana karta FW-XXX (protokół współpracy).

---

## 1. Zasada porządkująca: cztery warstwy, cztery różne pytania

| Warstwa | Odpowiada na pytanie | Wolno jej mieć pamięć? |
|---|---|---|
| **SENSOR CONDITIONING** | „jaki jest teraz nacisk na pedał, bez szumu" | tak, ale tylko tyle, ile wymaga jakość sygnału |
| **RIDER INTENT / ASSIST** | „ile pomocy chce rider" | nie — ma być funkcją bieżących wejść |
| **LIMITING** | „ile wolno dostarczyć" | tylko histereza, nigdy rampa odczucia |
| **FINAL Iq DYNAMICS** | „jak szybko wolno zmieniać prąd" | **tak — to jedyne miejsce na Ramp Up / Ramp Down** |

Reguła, która rozstrzyga wszystkie sporne przypadki:

> Jeżeli mechanizm zmienia **ile** wspomagania rider dostaje — należy do warstwy 2.
> Jeżeli zmienia **jak szybko** to się dzieje — należy do warstwy 4.
> Filtr sensora nie ma prawa robić żadnej z tych dwóch rzeczy.

Dzisiejszy RUN asym fall 350 ms łamie tę regułę: siedzi w warstwie 1, a robi warstwę 4.

---

## 2. Tor docelowy

```text
CZUJNIK NACISKU
   |
   +-- auto-zero, deadband, kalibracja wzmocnienia        [bez zmian]
   +-- FILTR SENSORYCZNY: jeden, symetryczny, krotki      [dzis: FAST 35 ms]
   |
RIDER LOAD (kg)
   |
   +-- ASSIST: tryb, krzywa, cadence comp, startup boost  [bez zmian]
   |
RIDER Iq DEMAND
   |
   +-- LIMITY: predkosc, bateria, temperatura, napiecie   [bez zmian, bezstanowe]
   |
Iq ALLOWED
   |
   +-- JEDEN FINALNY WLASCICIEL DYNAMIKI                  [fast_iq_slew @16 kHz]
   |      Ramp Up / Ramp Down / release / powrot spod limitu
   |
MS.i_q_setpoint -> PI -> FOC
```

Osobne, jawne dynamiki, które **zostają** (rozdział 27 mandatu): SAFETY FALL 200 ms,
START (smooth start + boost + preload), WALK, CALIBRATION, COAST RELEASE, FAULT.

---

## 3. Wymagana TABELA STANU DOCELOWEGO (sekcja 31 mandatu)

| FUNKCJA KANONICZNA | CEL UŻYTKOWNIKA | DOMENA DOCELOWA | WŁAŚCICIEL DOCELOWY | KONFIG. UŻYTKOWNIKA | PER LEVEL / GLOBAL | RELACJA DO BEZPIECZEŃSTWA | MECHANIZMY DO WYCOFANIA | KROK MIGRACJI |
|---|---|---|---|---|---|---|---|---|
| Assist | ile pomocy z nacisku | rider intent | `assist_modes.c` | tryb + parametry trybu | per level | brak | — | — |
| Power | jak wysoko moc | limiting | `finish_power_request` + limitery | max power, max torque | per level + global | tak (sufity) | podłoga omijająca sufit | STEP 6 |
| **Ramp Up** | jak szybko narasta | final dynamics | `fast_iq_slew` | `iq_rise_slow/fast_ms` | per level | brak | RUN asym rise 120 ms | STEP 4 |
| **Ramp Down** | jak szybko maleje | final dynamics | `fast_iq_slew` | `iq_fall_slow/fast_ms` + `release_ms` | per level | brak | **RUN asym fall 350 ms**, hold+podłoga | STEP 3, STEP 6 |
| **Torque Measurement Filter** | czysty sygnał czujnika | sensor | `torque_input.c` | jedno pole „siła filtrowania" | global | brak | uśrednianie po kącie korby (martwe), fast attack (wyłączony) | STEP 5 |
| Dead-spot bridging | brak pulsowania w martwym punkcie | **decyzja** | jeden z dwóch | dziś 2 pola | global | brak | jeden z pary | STEP 6 |
| Smooth Start | miękkie ruszanie | start-only | `assist_start.c` | on + czas | per level | brak | — | — |
| Startup Boost | mocniejsze ruszanie | rider intent | `assist_start.c` | siła, tryb, zanik | per level + global | brak | `end_rpm` w trybie CADENCE | STEP 8 |
| Extended Boost | ciąg po ustaniu pedałowania | rider intent | `assist_extended_boost.c` | próg, siła, czas | per level | **tak** | — | — |
| Speed Limit | legalność | limiting | `assist_limits.c` | limit + flaga | global | tak | — | — |
| Battery Current Limit | ochrona pakietu | limiting | `battery_iq_cap.c` | max prąd baterii | global | tak | — | — |
| Thermal Limit | ochrona sterownika | limiting | `assist_limits.c` | brak (stałe 75/90) | global | tak | — | — |
| Walk Assist | prowadzenie roweru | osobny tor | `walk_speed_controller.c` | prędkość, prąd, cut-off | per bank | tak | — | — |
| Safety Release | natychmiastowe odcięcie | osobna dynamika | `fast_iq_slew` SAFETY | brak, celowo | firmware | tak | — | — |

---

## 4. Trzy warianty uproszczenia (rozdział 16 mandatu)

### OPCJA A — minimalne, zachowawcze porządkowanie

Co robi: **nic nie zmienia w kodzie sterującym**. Naprawia wyłącznie warstwę
kontraktu: opisy w CANable przestają kłamać, martwe pola są oznaczone, dodany jest jawny
suwak siły filtrowania czujnika o realnym działaniu (mapowany na istniejące `TORQUE_RUN_ASYM_*`).

- Korzyści: zero ryzyka jazdy, rider natychmiast wie, co faktycznie ustawia, i **może
  wyłączyć ogon 350 ms jednym kliknięciem** (RUN smoothing = 0), bez nowego firmware.
- Ryzyka: przy RUN = 0 może wrócić pulsowanie raz na nogę przy niskiej kadencji — to jest
  dokładnie ten kompromis, dla którego FW-085/112.4 powstały.
- Koszt migracji: mały (CANable + dokumentacja).
- Ryzyko testu na rowerze: **niskie** — jedna zmiana konfiguracji, odwracalna z aplikacji.
- Złożoność rollbacku: trywialna.

### OPCJA B — REKOMENDOWANA: jeden właściciel Ramp Up/Down

Co robi:
1. Filtr sensoryczny zostaje **jeden i symetryczny** (35 ms), z jawnym polem w CANable.
2. Asymetria (rise 120 / fall 350) **znika z warstwy sensora** i staje się częścią kontraktu
   finalnego slewu: Ramp Up i Ramp Down to od tej pory dokładnie `iq_rise_*` i `iq_fall_*`.
3. Aby nie stracić funkcji „nie pulsuj w martwym punkcie", finalny slew dostaje **jawny,
   nazwany parametr** tej funkcji — do wyboru: albo dzisiejsza podłoga min-Iq, albo minimalne
   tempo opadania. Jeden z nich, nigdy oba.
4. Martwe: średnia po kącie korby, fast attack, `assist_dynamics.c`, globalne rampy w tuning
   blob, `IQ_RAMP_*_TICKS` — usuwane etapami, każde przy swojej karcie.

- Korzyści: użytkownik zmienia Ramp Down i **dokładnie jeden** mechanizm za to odpowiada;
  zostaje pełna konfigurowalność per level; odczucie da się nastroić w jednym miejscu.
- Ryzyka: to jest zmiana odczucia jazdy — wymaga serii jazd testowych z jednym parametrem naraz.
- Koszt migracji: średni, ale rozłożony na 4 karty.
- Ryzyko testu: średnie, każdy krok osobno na rowerze.
- Rollback: per karta, każdy krok ma własny punkt bazowy.

### OPCJA C — agresywne uproszczenie

Co robi: dodatkowo usuwa podłogę min-Iq i hold całkowicie, likwiduje adaptacyjny wybór
slow/fast (jedna para rampa-w-górę / rampa-w-dół na poziom zamiast czterech), i redukuje
`release_ms` do specjalnego przypadku Ramp Down.

- Korzyści: najprostszy możliwy model — 2 liczby na poziom zamiast 5.
- Ryzyka: **traci konfigurowalność, którą właściciel jawnie ceni** (rozdział 8 mandatu:
  per-mode Ramp Up/Down zostaje). Ryzyko powrotu pulsowania i utraty charakteru ECO kontra SPORT.
- Koszt migracji: duży, zmienia format bloba (255 B to twardy sufit — patrz `assist_modes.h`).
- Ryzyko testu: **wysokie**.
- Rollback: kosztowny — dotyka zapisanych profili.

**Rekomendacja: A natychmiast (bez firmware), potem B krok po kroku. C odrzucić** — łamie
wymaganie 1 z rozdziału 16 mandatu (zachować per-mode Ramp Up/Ramp Down).

---

## 5. Czego ten blueprint NIE zmienia

- Ownership finalnego Iq (jeden pisarz, 16 kHz) — bez zmian.
- ARMED_ZERO, ciągłość PI, brak resetów integratorów — bez zmian.
- Zachowanie hard-fault i hard-cut — bez zmian.
- Kompatybilność protokołu CAN i format zapisanych banków — bez zmian.
- Stałe G532 **nie są kopiowane**; QS-3E pozostaje wstrzymane do osobnej decyzji.

---

## 6. Odpowiedź na pytanie końcowe (rozdział 39 mandatu)

> Jak powinna wyglądać najprostsza, czytelna i nadal bogato konfigurowalna architektura
> eVistDrive?

**Cztery warstwy, jeden właściciel dynamiki, reszta bez pamięci.**

Konkretnie, i bez przepisywania projektu:

1. **Czujnik daje jedną liczbę** — nacisk w kg, odszumiony jednym symetrycznym filtrem. Filtr
   nie ma prawa decydować, jak długo rower ciągnie. To jedyna zmiana, która naprawia
   dzisiejszy główny objaw.
2. **Warstwa wspomagania zostaje taka, jaka jest** — tryby TSDZ-owe, per level, per bank.
   FW-129 już ją uporządkował: jedno przejście moc → prąd, bez filtrów w torze zadania.
3. **Limity zostają bezstanowe** — `assist_limits.c` jest wzorcem: liniowy taper, zero pamięci,
   zero ukrytej rampy. Battery cap ma jawną histerezę i to jest w porządku.
4. **Cała dynamika odczucia mieszka w jednym miejscu** — `fast_iq_slew` @16 kHz. Ramp Up,
   Ramp Down, release i powrót spod limitu to jego cztery tryby, sterowane pięcioma liczbami
   na poziom, które rider już dziś widzi w aplikacji.
5. **Osobne dynamiki zostają osobne i jawne**: start, walk, kalibracja, safety, wybieg.
   Każda ma własny trigger i własnego właściciela — i żadna nie nakłada się na normalną jazdę.

Bogactwo konfiguracji **nie maleje** — rośnie, bo pierwszy raz każde pole robi to, co obiecuje.
Sukces nie jest mierzony liczbą usuniętych linii, tylko tym, że na pytanie „co odpowiada za
Ramp Down?" istnieje jedna odpowiedź.

---

## 7. Pytania referencyjne do G532 / TSDZ (rozdział 35 mandatu)

Agent audytujący **nie** miesza reverse engineeringu z tym zadaniem. Pytania zapisane do
osobnego wątku:

**REFERENCE QUESTION 1**
- ŹRÓDŁO: G532
- PYTANIE: jak szybko standardowe zadanie momentu/obciążenia dochodzi do zera podczas
  **kontynuowanego pedałowania**, gdy rider zmniejsza nacisk (nie przy zatrzymaniu korby)?
- DLACZEGO WAŻNE: to jest dokładnie scenariusz, w którym eVistDrive ma dziś ~2 s ogona.
  Odpowiedź mówi, czy stock ma odpowiednik naszego RUN asym fall, czy kształtuje to dopiero
  na finalnym slewie.
- BLOKUJE DECYZJĘ: **NIE** — STEP 0 i STEP 3 można wykonać bez niej; wpłynie na strojenie.

**REFERENCE QUESTION 2**
- ŹRÓDŁO: G532
- PYTANIE: czy istnieje jakikolwiek odpowiednik podłogi minimalnego momentu utrzymywanego po
  zaniku nacisku (nasze min-Iq hold 1400 ms)?
- DLACZEGO WAŻNE: rozstrzyga STEP 6 od strony referencyjnej.
- BLOKUJE DECYZJĘ: **NIE** — decyzja i tak należy do właściciela.

**REFERENCE QUESTION 3**
- ŹRÓDŁO: pomiar na rowerze (nie reverse)
- PYTANIE: jakie `u_abs` sterownik faktycznie osiąga przy 60 rpm (0x6029 raportuje `u_abs`
  i cadence razem)?
- DLACZEGO WAŻNE: weryfikuje `ASSIST_LAUNCH_REFERENCE_U_ABS = 1024` (AD-013).
- BLOKUJE DECYZJĘ: **NIE**, ale blokuje sensowne strojenie startu (STEP 7).
