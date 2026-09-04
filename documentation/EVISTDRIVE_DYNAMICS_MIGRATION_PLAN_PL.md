# eVistDrive — EWOLUCYJNY PLAN MIGRACJI DYNAMIKI

Data: 2026-09-02
Punkt bazowy: `77e4743` (STOP-CLICK-C1), kandydat HW `0.0475_M820_BL820.bin`
Status: **plan do akceptacji. Żaden krok nie jest wdrożony.**

Zasada nadrzędna: **jedna zmiana semantyczna na kartę + lokalne porządki + test + jazda.**
Kolejność jest tak dobrana, że pierwsze dwa kroki nie dotykają firmware w ogóle.

---

## STEP 0 — Eksperyment konfiguracyjny (BEZ firmware, do zrobienia od razu)

**CEL FUNKCJONALNY:** sprawdzić na rowerze, ile z odczuwanej ociężałości pochodzi z filtra
RUN asym, zanim ktokolwiek napisze linijkę kodu.

**AKTUALNY PROBLEM:** hipoteza „ogon 350 ms dominuje Ramp Down" jest wyprowadzona ze źródła,
ale nie zmierzona na rowerze.

**CO ZROBIĆ:** w CANable, karta Dynamics, ustawić **RUN torque smoothing = 0** i przejechać
znany odcinek. Potem wrócić do 180.

**CZEGO SIĘ SPODZIEWAĆ:** przy 0 wspomaganie ma reagować i puszczać wyraźnie szybciej, kosztem
możliwego pulsowania raz na nogę przy niskiej kadencji pod obciążeniem.

**MODUŁY:** żadne. **ZMIANA ZACHOWANIA:** tak, odwracalna z aplikacji.
**TESTY:** jazda. **PUNKT ROLLBACK:** wpisać z powrotem 180.
**ZALEŻNOŚCI:** brak. **RYZYKO:** niskie — to ustawienie, które i tak jest w aplikacji.

---

## STEP 1 — Prawda w opisach CANable (Opcja A)

**CEL:** żadne pole w aplikacji nie może opisywać mechanizmu, którego nie ma.

**PROBLEM:** `Run deadband` nie ma konsumenta; `Sustain through dead-spot` powołuje się na
`Run deadband`; `RUN torque smoothing` obiecuje okno kąta korby, a jest włącznikiem filtra
czasowego; `Startup boost end cadence` nie działa w domyślnym trybie.

**SEMANTYKA DOCELOWA:** opis pola = to, co robi firmware na tym baseline.

**MODUŁY:** `ui/js/evistdrive/dynamics.js`, `profiles.js`. Firmware nietknięty.

**ZMIANA ZACHOWANIA:** żadna. **PORZĄDKI BEZ ZMIANY ZACHOWANIA:** oznaczyć `Run deadband`
jako nieaktywne (tak jak zrobiono z filtrami mocy), przepisać help `RUN smoothing`
i `Startup boost end cadence`.

**TESTY:** `npm run build:css` jeśli dojdzie nowa klasa; przegląd tekstów.
**JAZDA:** niewymagana. **ROLLBACK:** commit UI. **ZALEŻNOŚCI:** brak. **RYZYKO:** brak.

---

## STEP 2 — Inwentarz zamknięty: martwe pola i martwy kod udokumentowane

**CEL:** jeden dokument, który mówi, co jest martwe i dlaczego jeszcze istnieje.

**PROBLEM:** `assist_dynamics.c` (0 wywołań), `IQ_RAMP_*_TICKS`, `IQ_SLEW_*`, globalne rampy
w tuning blob, `TQ_GATE_RELEASE` w schemacie — każde z nich kosztuje czas kolejnego agenta.

**MODUŁY:** dokumentacja + `protocol/evistdrive_config_schema.yaml` (usunąć odwołanie do
nieistniejącej stałej). **BEZ usuwania kodu w tym kroku.**

**ZMIANA ZACHOWANIA:** żadna. **TESTY:** brak. **JAZDA:** nie.
**ROLLBACK:** commit dokumentacji. **RYZYKO:** brak.

---

## STEP 3 — Ramp Down dostaje jednego właściciela

**CEL FUNKCJONALNY:** gdy rider zmienia „Deceleration", ma się zmieniać dokładnie to, co czuje.

**AKTUALNY PROBLEM:** odczuwane opadanie to RUN asym fall (tau 350 ms, ~2 s do zera), potem
podłoga 1400 ms, a dopiero na końcu rampa, którą rider ustawia.

**SEMANTYKA DOCELOWA:** maksymalne tempo redukcji dozwolonego finalnego Iq ustala **wyłącznie**
`fast_iq_slew` na podstawie `iq_fall_*` i `release_ms`. Filtr sensora przestaje kształtować
opadanie.

**MODUŁY:** `torque_input.c` (asymetria fall), `inc/torque_input.h`, CANable (nowe pole
siły filtrowania), dokumentacja.

**ZMIANY ZACHOWANIA:** tak — to jest właściwa zmiana odczucia jazdy tej karty.
**PORZĄDKI BEZ ZMIANY ZACHOWANIA:** nazwa `run_asym_q` na kanoniczną, komentarz opisujący
prawdziwą rolę.

**TESTY:** `tests/host/torque/torque_run_asym_host.c` S1–S8 (rozszerzyć o przypadek „fall
symetryczny"); regresje FW-112 rearm; QS-3D slew.
**JAZDA:** odcinek referencyjny, porównanie z wynikiem STEP 0.
**ROLLBACK:** commit + poprzedni BIN. **ZALEŻNOŚCI:** STEP 0 (pomiar), STEP 1 (opisy).
**RYZYKO:** powrót pulsowania raz na nogę — mierzyć przy 20 i 40 rpm pod obciążeniem.

---

## STEP 4 — Ramp Up dostaje jednego właściciela

**CEL:** to samo dla narastania. **PROBLEM:** RUN asym rise 120 ms opóźnia odpowiedź przed
rampą `iq_rise_*`, więc rider stroi jedno, a czuje sumę dwóch.

**SEMANTYKA DOCELOWA:** narastanie ustala wyłącznie `fast_iq_slew`.

**MODUŁY:** jak STEP 3. **TESTY:** host S1–S8 + „pierwsze dodatnie żądanie" (dziś ~193 ms).
**JAZDA:** ruszanie i chwyt w ruchu przy 15 i 25 km/h.
**ROLLBACK:** commit. **ZALEŻNOŚCI:** STEP 3 wykonany i przejechany. **RYZYKO:** średnie.

---

## STEP 5 — Porządek w filtrowaniu czujnika

**CEL:** warstwa sensora robi tylko odszumienie i nic więcej.

**PROBLEM:** trzy mechanizmy w jednym pliku, z czego dwa nie mają wpływu na sterowanie
(średnia po kącie korby — wynik nadpisywany; fast attack — wyłączony stałą 0).

**SEMANTYKA DOCELOWA:** jeden filtr, jedno pole konfiguracji, jedno znaczenie.

**MODUŁY:** `torque_input.c`, `inc/torque_input.h`, `tuning_config.c` (znaczenie offsetu 20),
CANable.

**ZMIANA ZACHOWANIA:** żadna, jeśli nowy filtr odtwarza charakterystykę wybraną w STEP 3/4.
**PORZĄDKI:** usunięcie `run_buffer`, `run_sum`, `run_attack_steps` (192 B RAM odzyskane).
**TESTY:** cały zestaw torque host. **JAZDA:** potwierdzenie braku regresji.
**ZALEŻNOŚCI:** STEP 3 i 4. **RYZYKO:** niskie po STEP 3/4, wysokie przed nimi.

---

## STEP 6 — Decyzja o podłodze min-Iq i hold

**CEL:** rozstrzygnąć, czy „nie pulsuj w martwym punkcie" ma jednego właściciela.

**PROBLEM:** po STEP 3–5 podłoga min-Iq zostaje jedynym mechanizmem podtrzymania — i wtedy
albo jest potrzebna i trzeba ją nazwać uczciwie, albo jest zbędna i trzeba ją usunąć.
Niezależnie od wyniku: dziś podłoga **omija sufit `max_iq_pct` poziomu** (AD-005) i to jest
błąd do naprawy w tej karcie.

**SEMANTYKA DOCELOWA:** jeden jawny mechanizm podtrzymania z jednym parametrem, ograniczony
sufitem poziomu.

**MODUŁY:** `ride_control.c` (blok ride latch), `tuning_config.c`, CANable.
**ZMIANA ZACHOWANIA:** tak. **TESTY:** FW-112 host (S4/S12 zależą od dojścia do dokładnego 0).
**JAZDA:** podjazd przy niskiej kadencji — klasyczny przypadek pulsowania.
**ZALEŻNOŚCI:** STEP 3–5. **RYZYKO:** średnie. **DECYZJA WŁAŚCICIELA WYMAGANA.**

---

## STEP 7 — Dynamika startu jako jawnie oddzielna

**CEL:** start (smooth start + startup boost + gear preload) ma zostać jedną, świadomą
i nienakładającą się dynamiką.

**PROBLEM:** trzy mechanizmy działają w tym samym oknie czasowym; `startup_boost_end_rpm`
nie działa w domyślnym trybie (PARTIAL).

**MODUŁY:** `assist_start.c`, `ride_control.c` (preload), CANable.
**ZMIANA ZACHOWANIA:** minimalna, głównie naprawa `end_rpm`.
**ZALEŻNOŚCI:** STEP 4. **RYZYKO:** niskie. **UWAGA:** zgodnie z `project-startup-boost`
zmiany startu robimy jako **zamianę**, nigdy jako dodatek.

---

## STEP 8 — Usunięcie martwego kodu i martwej konfiguracji

`assist_dynamics.c/h`, `IQ_RAMP_*_TICKS`, `IQ_SLEW_*`, globalne rampy w tuning blob (wire
zostaje, znika tylko obietnica), odwołanie do `TQ_GATE_RELEASE` w schemacie.

**ZMIANA ZACHOWANIA:** żadna. **TESTY:** pełny runner + build ARM (kontrola `.map`).
**ZALEŻNOŚCI:** STEP 3–7. **RYZYKO:** niskie, ale **nie robić wcześniej** — te pliki są
dziś punktem odniesienia dla testów QS-3.

---

## STEP 9 — Nazewnictwo w obszarach już zmigrowanych

Tylko tam, gdzie karta i tak dotykała kodu. **Zakaz masowego rename w całym repo.**
Źródło nazw kanonicznych: [słownik](EVISTDRIVE_TERMINOLOGY_ALIAS_PL.md).

---

## Kolejność i punkty rollback

| Krok | Firmware? | Jazda? | Punkt bazowy do rollbacku |
|---|---|---|---|
| 0 | nie | **tak** | ustawienie w aplikacji |
| 1, 2 | nie | nie | commit CANable / dokumentacji |
| 3 | tak | **tak** | `77e4743` + BIN 0.0475 |
| 4 | tak | **tak** | commit STEP 3 |
| 5 | tak | tak | commit STEP 4 |
| 6 | tak | **tak** | commit STEP 5 |
| 7 | tak | tak | commit STEP 6 |
| 8 | tak | nie | commit STEP 7 |
| 9 | tak | nie | commit STEP 8 |
