# FW-127 — spójna akwizycja prądu: A + B + C + D

**Status:** **ZAMKNIĘTE** — test sprzętowy #2 PASS (2026-08-27, DIAG 0.0446)
**NORMAL:** 0.0446 · **DIAG:** 0.0446 · **Wejście:** FW-126.7 = `1668b00`
**Poprzednik:** [FW-127 PRE-AUDIT](FW-127_PRE_AUDIT_SAMPLE_CONTEXT_COHERENCY_PL.md) (werdykt: PARTIAL)

---

## 1. Stara architektura i co dokładnie było zepsute

```
3761  dyn_adc_state_select(switchtime, ...)   ← geometria N   ✔ rekonstrukcja N
3792  dyn_adc_trigger_update()                ← geometria N, PROGRAMUJE CH3 dla N+1   ✘
3830  FOC_calculation()                       → switchtime N+1
3838  CCR0/1/2 ← switchtime                   ← N+1
```

Konsument (`dyn_adc_trigger_update`) był wołany **38 linii przed swoim producentem**.
Do tego: brak clampa, brak modelu ważności, decyzja na wartości żądanej, sektor/maska/trigger/seq
w czterech różnych miejscach.

## 2. Niezmiennik docelowy — osiągnięty

```
kontekst N zatrzaśnięty w poprzednim ISR
   → trigger N → konwersja N → IRQ
   → consume ctx N → walidacja → rekonstrukcja wg ctx N
   → Clarke/Park → PI → inv. Park → SVPWM N+1
   → CLAMP (A) → decyzja okna z geometrii ZASTOSOWANEJ (C)
   → zapis CCR0/1/2/3 + atomowa publikacja ctx N+1 (B)
```

Żaden konsument nie używa geometrii z innej transakcji niż próbka, którą opisuje.

## 3. FW-127A — clamp zastosowanej geometrii · `5d93740`

`switchtime[]` to teraz **żądanie** (`int32_t`, może być nielegalne), `pwm_applied[]` to
**geometria zastosowana** (`uint16_t`, zawsze w `[0, ARR]`). Oba mają realnych konsumentów, więc
to rozróżnienie żądane/zastosowane, a nie zdublowana tablica.

Znak jest istotny: arytmetyka SVPWM legalnie schodzi poniżej zera, a wartość ujemna zapisana
bez znaku stawała się dużym dodatnim compare — niewidoczna dokładnie tam, gdzie najbardziej boli.

**Clamp to dowód, nie naprawa.** Trafienie oznacza, że regulator napięcia poprosił o coś, czego
mostek nie umie wyrazić. Liczniki (`clamp_total`, per faza, peak/min żądany i zastosowany) są
pasywne i istnieją po to, żeby **jedna** sesja sprzętowa odpowiedziała na pytanie o praktyczną
osiągalność, które pre-audyt celowo zostawił otwarte.

> Test `A4b` uruchamia **własną arytmetykę firmware'u** na całym zakresie limitu kołowego,
> potwierdza, że żądanie **rzeczywiście przekracza ARR**, i że clamp to zawiera. Defekt jest
> teraz dowiedziony testem wykonywalnym, nie tylko na papierze.

## 4. FW-127B — atomowy kontekst transakcji · `89e0545`

Dwa konteksty: **LATCHED** (opisuje konwersję właśnie konsumowaną, seq N) i **NEXT**
(przygotowywany, seq N+1). `seq` jest nadawane wyłącznie wewnątrz modułu, więc „jedna publikacja
= jedna transakcja" jest strukturalne, a nie umowne.

**O atomowości uczciwie:** publikacja i konsumpcja dzieją się w `ADC0_1_IRQHandler`, który nie
może wywłaszczyć sam siebie — stąd zwykłe przypisanie struktury. Czytelnik diagnostyczny z pętli
głównej **może** się z tym ścigać, dlatego `current_sample_ctx_snapshot()` sprawdza `seq` przed
i po i **zgłasza rozerwany odczyt** zamiast zwracać pół kontekstu.

Konwersja, której nikt nie opisał, jest wymuszana na INVALID z wyczyszczonymi polami i liczona
jako sierota. To legalne przy pierwszej konwersji po załączeniu mostka — ale nigdy nie może być
po cichu odczytane tak, jakby kontekst istniał.

## 5. FW-127C — decyzja za SVPWM, z fizyki tej płyty · `f08ad21`

**Stałych stocku nie skopiowano.** Każda liczba pochodzi z konfiguracji tej płyty:

| wielkość | wartość | źródło |
|---|---|---|
| zegar timera | 120 MHz → 1 takt = 8,3333 ns | — |
| ARR | `_T` = 3750, center-aligned → 62,5 µs, 16 kHz | `timer_initpara.period` |
| zegar ADC | APB2/6 = **20 MHz** | `rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV6)` |
| czas próbkowania | 55,5 cykli ADC | `adc_inserted_channel_config` |
| dead-time | DTCFG 32, fDTS = fTIMER → 32 takty = 267 ns | `timer_breakpara` |
| połówka triggera CH3 | **DOWN** | FW-126.2, **zmierzone** |

```
T_ACQ  = 55,5 / 20 MHz = 2775 ns = 333 takty     (apertura sample-and-hold)
T_CONV = 12,5 / 20 MHz =  625 ns =  75 taktów
333 + 75 = 408, a FW-126.2 zmierzył CONV+L = 588 → ~180 taktów latencji ISR
```

Ta zgodność jest **niezależnym sprawdzianem**, że liczby opisują prawdziwy sprzęt.

Na zboczu opadającym dolny klucz fazy *i* przewodzi przy `CNT > CCR_i`, a apertura biegnie od
`CH3` w dół o `T_ACQ`, więc:

```
faza i mierzalna wprost  ⟺  CCR_i + GUARD ≤ CH3
GUARD = T_ACQ + T_DEAD + T_SETTLE = 333 + 32 + 32 = 397 taktów
```

`T_SETTLE` to **jedyna** wartość niezmierzona — zapas na dzwonienie, przyjęty jako jeden
dead-time. Myli się w **bezpieczną** stronę: za duży oznacza tylko rekonstrukcję fazy, którą dało
się zmierzyć (koszt: dokładność, nigdy ważność). Za mały wpuściłby śmieć wprost do Clarke.
Jeśli sesja sprzętowa pokaże ALTERNATE znacznie częściej niż oczekiwano — to jest liczba do rewizji.

### Trigger się NIE przesuwa, i to jest ustalenie, nie przeoczenie

Stary kod zjeżdżał z CH3 do `(najwyższe wypełnienie − offset)`. Na **tym** sprzęcie to pogarsza
okno: niższy CH3 startuje aperturę **bliżej** przejść CCR, więc kwalifikuje się **mniej** faz.
Najlepsza pozycja to możliwie najwyższa — tam, gdzie `TRIGGER_DEFAULT` już siedzi, i z którą
zebrano **każdy** dowód sprzętowy z FW-126.x. Trigger jest więc stałą wyprowadzoną, a decyzja
dotyczy wyłącznie tego, do których faz ta stała sięga.

### PRIMARY / ALTERNATE / INVALID

| stan | warunek | zachowanie |
|---|---|---|
| **PRIMARY** | wszystkie trzy boczniki przewodzą przez całą aperturę | nic nie rekonstruowane |
| **ALTERNATE** | jedna faza niemierzalna | 2-z-3 odtwarza ją z pozostałych |
| **INVALID** | dwie lub więcej niemierzalne | zwracane zamiast „najbliższego możliwego" triggera |

PRIMARY przy zwykłym wypełnieniu (1875 + 397 = 2272 wobec triggera 3740) jest **świadomym
ulepszeniem**: stary kod zawsze rekonstruował fazę o najwyższym wypełnieniu, nawet gdy była
doskonale mierzalna. Odczyt bezpośredni bije rekonstruowany, kiedy tylko jest wiarygodny.

## 6. FW-127D — ważność i last-valid · `a83a100`

Do tej pory jedynym predykatem było „konwersja się zakończyła". To dowodzi, że ADC skończył —
i nic o tym, czy boczniki przewodziły. Teraz INVALID jest **zastępowany ostatnim wiarygodnym
odczytem, zanim dotrze do pętli sterowania**, i liczony.

Dlaczego last-valid, a nie zero ani ciche przytrzymanie: zero powiedziałoby PI, że silnik nie
pobiera prądu, i zaprosiło do windupu; nietknięte `MS.i_q` to dokładnie to, co firmware robił
przypadkiem przed FW-126 (ISR po prostu nie wchodził) i co całkowicie ukrywało nieświeżość.

**Maksymalny wiek celowo NIEUSTALONY.** Wybór progu przed poznaniem rozkładu byłby wymyślaniem
liczby — dokładnie ten sam błąd, który FW-125 popełnił z oknem akceptacji. Wiek jest
instrumentowany; próg to późniejsza decyzja z dowodem.

Jeden przypadek nie ma uczciwej odpowiedzi: INVALID **przed** pierwszą dobrą próbką w danym
przebiegu. Wtedy `update()` zwraca 0, a ISR **trzyma neutralną geometrię** zamiast całkować
zgadywankę. To nie może się utrzymać: przy zwykłym wypełnieniu okno jest PRIMARY z dużym
zapasem, a trzymana geometria **jest** zwykłym wypełnieniem.

Ścieżka geometrii i publikacji została wyniesiona poza gałąź FOC, więc biegnie w **każdym** ISR.
Pominięta publikacja osierociłaby następną konwersję — dokładnie ta klasa defektu, którą ten
zestaw kart usuwa.

## 7. Usunięta stara architektura

| element | powód |
|---|---|
| `dyn_adc_trigger_update()` | wyprowadzał trigger N+1 z geometrii N |
| `src/dyn_adc_state.c`, `inc/dyn_adc_state.h` | wybór przeniesiony do `sample_window_decide()`, rekonstrukcja do `sample_window_reconstruct()` |
| `DYNAMIC_ADC_THRESHOLD` | próg równy `_T`; okno jest teraz wyprowadzane z czasów |
| `MS.char_dyn_adc_state` | producent bez konsumenta, gdy sektor jedzie w kontekście |
| `char_dyn_adc_state_old` | martwa od dawna |
| `fw120_1_isr_order_wiring_host.c`, `fw120_1_reconstruction_timing_host.c` | własność zachowana, ale egzekwowana wobec **nowej** kolejności przez `fw127c_wiring_host.c` |

**Nic nie zostało obok nowej ścieżki.** Strażnik `X1` wywala się, jeśli któraś z tych nazw wróci
do `main.c`.

## 8. Testy hosta

| test | zakres |
|---|---|
| `fw127a_pwm_geometry_host.c` | A1–A7 + **A4b** (prawdziwa arytmetyka SVPWM przekracza ARR) |
| `fw127b_sample_ctx_host.c` | B1–B7, w tym rozerwany odczyt i rearm |
| `fw127c_sample_window_host.c` | C2–C10, Kirchhoff, cała przestrzeń geometrii bez nielegalnego triggera |
| `fw127c_wiring_host.c` | C1: decyzja **za** SVPWM; kolejność, jedno miejsce publikacji, X1 |
| `fw127d_current_feedback_host.c` | D1–D8, histogram wieku, głodzenie |

**Baseline: 2 czerwone** — `T14` i `T9` w `rolling_no_assist`, oba sprzed tych kart.
**Zero nowych regresji** po każdym z czterech commitów.

## 9. Build

| | NORMAL 0.0446 | DIAG 0.0446 |
|---|---|---|
| FLASH | 102 004 B (43,29 %) | 147 680 B (62,71 %) |
| RAM | 12 208 B (24,84 %) | 45 976 B (93,54 %) |
| SHA256 | `D9E5CE43…13DA0526` | `C1C16075…842C9CE6` |

## 10. Raport 0x602F (DIAG) — dowody dla jednej sesji

Kompaktowe liczniki, nie rejestrator: każda decyzja, którą ma odblokować, to „jak często" albo
„czy w ogóle". Niesie: `clamp_total` i per faza, peak/min żądany i zastosowany,
PRIMARY/ALTERNATE/INVALID, `reuse_count`, `max_sample_age`, histogram wieku (1 / 2–3 / 4–7 / 8+),
`starved_count`, licznik sierot. Nic z tego nie jest czytane przez pętlę sterowania.

## 11. Procedura testu sprzętowego #2 — jedna sesja

**Jeden obraz, jeden flash, jedna sesja, jeden surowy log.** Bez powtórnych wgrywek między
podtestami.

1. Wgraj **DIAG 0.0446**.
2. Włącz zasilanie, ~3 s bez pedałowania (kalibracja FW-126.7).
3. **Start Walk Assist** albo niskoobciążeniowy — mały moment.
4. Normalna jazda: małe, średnie, potem naturalnie wyższe obciążenie. Nie wymuszaj sztucznie.
5. Normalne zatrzymanie.
6. Sniffer: **Read CAL 0x602D**, potem **0x602F**. Zapisz surowy log.

### Kryteria zaliczenia

1. silnik startuje i jedzie normalnie
2. `clamp_total` — **odczytać, nie interpretować jako awarię**; to jest odpowiedź na H1
3. brak nielegalnego CH3 (strukturalnie niemożliwe po C — flaga w raporcie to potwierdza)
4. licznik sierot = 0 poza pierwszą konwersją po starcie
5. rozkład PRIMARY/ALTERNATE/INVALID zgodny z geometrią
6. `starved_count` = 0 po ustabilizowaniu
7. `max_sample_age` bez ucieczki
8. brak regresji startu/stopu i kalibracji
9. zachowanie przy małym Iq zarejestrowane w tej samej sesji

### Obserwacja małego Iq

Sesja pasywnie pomoże w starym objawie (IqRef ~14 → brak momentu): raport koreluje
PRIMARY/ALTERNATE/INVALID i `sample_age` z tym, co widzi pętla. **Nie modyfikujemy żądania
momentu, żeby wymusić ten stan** — jeśli wystąpi naturalnie, będzie w logu.

---

## 12. TEST SPRZĘTOWY #2 — PASS (2026-08-27, DIAG 0.0446)

Jedna sesja, jeden obraz, jeden log. Ładunek 0x602F schema 1 zdekodowany kanonicznym specem
(`protocol/fw127_report_schema.json`, wyprowadzonym z offsetów samego serializera — nie z wartości
w payloadzie). CRC 0x5B8F/0x5B8F OK, magic AQ v1. **Spec i payload nie są sprzeczne.**

| pole | wartość |
|---|---|
| `flags` | 0x03 — snapshot spójny, **nielegalny CH3 nigdy nie zaprogramowany** |
| `clamp_total` / A / B / C | **0** / 0 / 0 / 0 |
| `peak_requested` / `min_requested` | 3208 / 542 |
| `peak_applied` / `min_applied` | 3208 / 542 — **identyczne z żądaniem** |
| `primary_count` | 985 743 |
| `alternate_count` | **0** |
| `invalid_count` | 7 (≈0,0007 %) |
| `reuse_count` | **0** |
| `max_sample_age` | **0**, wszystkie kubełki 0 |
| `starved_count` | 7 |
| `orphan_count` | 1207 |

### Co te liczby znaczą

**Clamp nigdy nie zadziałał, ale defekt się nie zmienił.** Szczyt żądania 3208 przy środku 1875
to odchylenie **1333 z dostępnych 1875 — 71 % drogi do clampa**. Reżim przemodulowania został
więc *zbliżony*, nie osiągnięty. H1 = **nie wystąpiło w tej jeździe**; istnienie defektu jest
nadal dowiedzione arytmetycznie i testem A4b, a zapas wynosił około 1,4×. Twardsza jazda
(dłuższy podjazd, niższe napięcie pakietu) ten zapas domyka.

**INVALID = starved = 7, reuse = 0.** Każdy INVALID był „głodny", czyli wystąpił **przed**
pierwszą dobrą próbką w swoim przebiegu — to dokładnie jeden na start mostka. Siedem INVALID =
siedem startów. **Podmiana last-valid nie zadziałała ani razu** (`reuse_count` 0,
`max_sample_age` 0, wszystkie kubełki 0). Zaprojektowana ścieżka zadziałała i natychmiast się
rozwiązała, zgodnie z przewidywaniem: geometria trzymana w tym stanie JEST zwykłym wypełnieniem,
więc następna konwersja jest PRIMARY.

**ALTERNATE = 0 — i to jest uczciwe ograniczenie tej sesji.** Szczyt 3208 nie dobił do progu
mierzalności 3343 (zapas 135 taktów = 1,1 µs), więc rekonstrukcja 2-z-3 **nie została ani razu
wykonana na sprzęcie**. Pokrywają ją testy hosta, ale nie ta jazda. Z tego samego powodu sesja
**nie waliduje** przyjętego `T_SETTLE` = 32 takty: nic nie zmusiło tej wartości, żeby miała
znaczenie.

### `orphan_count` = 1207 — rozliczone, i słabość własnej instrumentacji

`current_sample_ctx_consume()` biegnie na szczycie **każdego** ISR, a publikacja tylko wtedy, gdy
realnie działa FOC. Każdy ISR z żywym mostkiem i nieaktywnym FOC konsumuje bez publikacji:

```
miękkie odcięcie   40 ticków @4 kHz = 160 cykli ISR × 7 startów = 1120
wybieg kalibracyjny (1×)                                       =   41
wybiegi normalne    (6 × 4)                                    =   24
pierwsza próbka FOC po każdym starcie (te same, co INVALID)     =    7
                                                          razem = 1192
zmierzone                                                       = 1207   (≈2 cykle/start luzu)
```

Rozliczenie domyka się do ~2 cykli na start ze 172 — to zmienność długości wybiegu, nie usterka.

> **Znane ograniczenie:** ten licznik **łączy przypadek łagodny** (FOC nie działa: wybieg,
> kalibracja, miękkie odcięcie) **z patologicznym** (pominięta publikacja przy działającym FOC).
> Sam z siebie ich nie rozróżni. Tę drugą sytuację pokazują `invalid_count` i `starved_count`,
> a tutaj wszystkie 7 jest w pełni rozliczone jako pierwsza próbka po starcie. Rozdzielenie
> wymagałoby liczenia sierot tylko przy `PWM_ON && !dwell && !cutoff` — **nie zmieniam tego**,
> akwizycja FW-127 jest zamrożona i to nie jest regresja.

### Obserwacja jakościowa

Właściciel zgłasza, że **załączanie wspomagania stało się wyraźnie powtarzalne** — teraz włącza
się praktycznie w tym samym punkcie, w przeciwieństwie do wcześniejszego zachowania.

Najbardziej prawdopodobny mechanizm, podany jako **hipoteza, nie pomiar**: stary kod **zawsze**
rekonstruował fazę o najwyższym wypełnieniu, więc jedno z dwóch wejść Clarke było zawsze sumą
obliczoną, dziedziczącą błędy dwóch pozostałych. Przy `alternate_count` = 0 w tej jeździe **obie
wielkości wejściowe były pomiarami bezpośrednimi przez cały czas**. To różnica działająca na
każdej próbce i najsilniejsza przy małym prądzie — czyli dokładnie tam, gdzie żyje próg
załączenia. Związek przyczynowy z odczuciem nie jest zmierzony.

### Werdykt

**PASS.** Kolejny test sprzętowy **nie jest wymagany**.

---

## Czego ta karta NIE robi

- nie zmienia matematyki FOC (Clarke, Park, PI, inv. Park, SVPWM bez zmian),
- nie stroi PI, nie rusza limitera prądu baterii ani anti-windupu — to FW-128,
- nie przeprojektowuje saturacji wektora napięcia; clamp dotyczy wyłącznie geometrii timera,
- nie wprowadza maksymalnego `sample_age`.
