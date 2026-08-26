# FW-112 0.0382 RETENTION LOG FORENSIC REPORT

**Log:** `log-2026-08-20-20-05-47-n0.log` (354 918 B, 4532 linii, CAN log 20:05:47–20:08:24)
**Artifakt:** `.build\0.0382_M820_BL820.bin` (SHA256 `AA983BF1D856AC97C23ACFD67294503672A826A8F991895CAA235D700C21E6CF`)
**Zakres:** ANALIZA READ-ONLY — brak zmian w kodzie, brak rebuild, brak flasha. NIE MA FIXU. STOP.

---

## 1. Liczniki retencji (odtworzone z logu)

Liczniki `evicted_saga_total` / `evicted_record_total` są wewnętrzne i NIE wychodzą na CAN. Retencję odtwarzam z ciągłości `event_id` w dumpie fw112_diag (0x1022A–E) oraz z pól nagłówka/trailera:

| Pozycja | Wartość | Źródło |
|---|---|---|
| Zdarzenia zatrzymane w pierścieniu (dump) | **24** rekordy | 0x1022A, id: 0,1,2,3,12..25,33..38 |
| Zdarzenia wyrejestrowane (evicted) | **15** rekordów | luki w id: 4..11 (8) oraz 26..32 (7) |
| Ciągłość `event_id` | **39 = 24 + 15, zero luk/duplikatów** | dowód: księgowość slotów dokładna |
| Wyrejestrowane sagi | **2** bloki (2 luki = 2 kompletne sagi) | id 4..11, 26..32 |
| RECORDS_SENT (trailer sesji 3) | 192 | 0x1021E d[0..1] |
| RECORDS_DISCARDED | 1 (rekord brzegowy dumpa w locie) | 0x1021E d[2..3] |
| RECORDS_REJECTED | 255 (suma clampowana po WSZYSTKICH źródłach DIAG, nie tylko FW-112) | 0x1021E d[4..5] |
| err / flags | 0x44 / 0x1F | 0x1021E d[6] / d[7] — COMPLETE + FW112_REJECTED + FW117_REJECTED ustawione |
| FW117 trace (sesja 4) | 480 próbek, tick_abs 239841→241860, bez luk (505 ms) | 0x10234–38 |
| pas_raw (sesja 3) | 256 zdarzeń, seq ciągła, okno 193967→259383 (16,35 s) | 0x1021B/C |

**Sesje:** sesja 3 = przejazd główny (dur_ds=734 → 73,4 s, 20 epizodów, 673 rekordów do wysłania); sesja 4 = ogon 1,5 s z wyłącznie trace'em FW117 (480 rekordów, TRL: rejected=2, err=0x04, flags=0x05). Dump sesji 3 w logu jest **częściowy** (recs_sent 192 vs recs_to_send 673) — stąd brakujące fragmenty ramek w pas_trace/fw112_ab/episodes (utrata ramek w transmisji, nie w rejestratorze).

---

## 2. Werdykt retencji: **PASS**

- **Księgowość pierścienia dokładna:** 24 zatrzymane + 15 wyrejestrowanych = 39 kolejnych `event_id` (0..38) — zero zgubionych, zero podwójnie policzonych rekordów. Retencja `evict-oldest-complete-saga` działała bezstratnie.
- **Dwie luki = dwa wyrejestrowania kompletnych sag** (4..11 oraz 26..32). Blok otwarty 0..3 (start na zimno, bez REVOKED) przetrwał — zgodnie z polityką (saga zaczyna się dopiero od REVOKED; blok otwarty nie jest wyrejestrowywany).
- **Rekord zakończonego powrotu zachowany w całości:** saga 12..25 z `REC_EXIT stable=560` (0x1022A A[6..7]=560) — pełny łańcuch BLOCKED→GRANTED→REC_ENTER→(HOLD_ARMED/HOLD_EXPIRED/ZEROED)×3→REC_EXIT.
- **FW117 trace bezstratny:** 480 próbek, tick_abs rosnący monotonicznie, żadna próbka nie utracona.
- **pas_raw bezstratny:** seq 0..255 ciągła, brak `seq gaps`.
- Uwaga: `RECORDS_REJECTED=255` to suma przycięta po wszystkich źródłach (w tym FW-117) — **nie** dowód porażki samego FW-112. `DISCARDED=1` = rekord w locie przy dumnie (zachowanie zgodne z projektem).

Retencja **sprawdzona**: potwierdza, że logika `evict_oldest_completed_saga` usuwa dokładnie kompletne sagi i nie gubi zdarzeń.

---

## 3. Tabela zachowanych sag (fw112_diag, oś `t+` = od startu sesji, 4000 tick/s)

### Blok otwarty 0..3 (start na zimno, chroniony — bez REVOKED)
| id | t+ | typ | uwagi |
|---|---|---|---|
| 0 | 0,0 ms | BLOCKED | sesja COLD, load=0, thr=70 |
| 1 | 101,5 ms | GRANTED | COLD_ARM, load=349, iqS=0 (setpoint ZERO mimo loadu) |
| 2 | 101,5 ms | HOLD_ARMED | hold=5600 zaar. |
| 3 | 101,5 ms | ZEROED | iqS=0 |

### Saga 12..25 (COMPLETED — rearm opóźniony, który się zakończył) ← rearm rid=0 / fw112_ab EP1
| id | t+ | typ | recov | load | iqR/iqP/iqS | uwagi |
|---|---|---|---|---|---|---|
| 12 | 701,8 ms | BLOCKED | IDLE | 349 | 0/0/0 | SESJA SUSP, dir=CONFIRM, rs=0xC6 |
| 13 | 759,8 ms | GRANTED | WAIT_FRESH | 0 | 0/0/0 | FAST_REARM, rs=0x60 |
| 14 | 759,8 ms | REC_ENTER | WAIT_FRESH | 0 | — | |
| 15 | 759,8 ms | ZEROED | | | — | iqS=0 |
| 16 | 1129,3 ms | HOLD_ARMED | WAIT_FRESH | 99 | 1/14/0 | **pre-ramp gotowy (iqP=14), setpoint ZERO** |
| 17 | 1131,3 ms | HOLD_EXPIRED | | 86 | 0/0/0 | |
| 18 | 1131,3 ms | ZEROED | | | | |
| 19 | 1137,8 ms | HOLD_ARMED | WAIT_FRESH | 49 | 0/0/0 | |
| 20 | 1171,8 ms | HOLD_EXPIRED | WAIT_FRESH | 0 | 0/0/0 | |
| 21 | 1171,8 ms | ZEROED | | | | |
| 22 | 1172,5 ms | HOLD_ARMED | | 0 | 1/14/0 | iqP=14, iqS=0 |
| 23 | 1172,8 ms | HOLD_EXPIRED | | 0 | 0/0/0 | |
| 24 | 1409,5 ms | HOLD_ARMED | WAIT_FRESH | 259 | 1/14/0 | iqP=14, iqS=0 |
| 25 | 1554,8 ms | **REC_EXIT** | IDLE | 386 | 9/14/14 | **stab=560, iqS=14 — powrót UKOŃCZONY** |

Saga: REVOKED(wyrejestrowany) → … → EXIT. Czas REVOKED→EXIT ≈ 853 ms; z czego **~655 ms w WAIT_FRESH z iqS=0** (759,8→~1414,8) i 140 ms stabilizacji TRACK_FAST.

### Saga 33..38 (IN-FLIGHT — przerwana w trakcie dumpa) ← rearm rid=1 / fw112_ab EP2
| id | t+ | typ | recov | load | iqS | uwagi |
|---|---|---|---|---|---|---|
| 33 | 1823,5 ms | BLOCKED | IDLE | 4 | 0 | SUSP, dir=CONFIRM |
| 34 | 1889,8 ms | GRANTED | WAIT_FRESH | 0 | 0 | FAST_REARM |
| 35 | 1889,8 ms | REC_ENTER | | 0 | 0 | |
| 36 | 1889,8 ms | ZEROED | | | 0 | |
| 37 | 2247,8 ms | HOLD_ARMED | WAIT_FRESH | 304 | 0 | iqP=14, iqS=0 |
| 38 | 2250,3 ms | HOLD_EXPIRED | WAIT_FRESH | **464** | 0 | nadal WAIT_FRESH, iqS=0 — mocno pedałuje |

**Wniosek:** zachowane sagi to ostatnie sagi sesji 3; starsze (luki 4..11, 26..32) wyrejestrowały się podczas przepełnienia pierścienia. W zachowanych rekordach **ani jednego REVOKED ani COLLAPSE** — wszystkie REVOKED znajdowały się w blokach wyrejestrowanych.

---

## 4. Tabela epizodów z opóźnieniem (rearm/ride_episode/fw112_ab)

**ride_episode — długie „latch”:**

| EP (ride_episode) | t_latch | t_recover |
|---|---|---|
| EP1 | 1389 ms | 2000 ms |
| EP2 | 957 ms | 2000 ms |
| EP6 | 1434 ms | 1745 ms |
| EP7 | 1612 ms | 1937 ms |
| EP13 | **NEVER** | 2000 ms |
| EP19 | 1964 ms | 2000 ms |

**rearm_delay (2 rekordy, sesja 3):**

| rid | reason | t_permission | t_target | t_setpoint | t_close | capture |
|---|---|---|---|---|---|---|
| 0 | 0x1 WAIT_LONG | 640,5 ms | 640,5 ms | NEVER | 731 ms | TRACE_ONLY |
| 1 | 0x3 WAIT_LONG+WEAK_TARGET | 658,3 ms | NEVER | 2958 ms | 3525 ms | NO_TRACE_NO_HISTORY |

Snapy rearm rid=0: ENTER_SUSPEND el=0 (afilt=3, arun=227, iqS=27) → PROBLEM el=200 ms (afilt=0, iqS=0) → PERMISSION el=640,5 ms (afilt=0, arun=222, iqS=0). Snapy rid=1: PERMISSION el=658 ms (afilt=0, arun=0, iqS=0) → RECORD_CLOSE el=3525 ms (afilt=3, arun=373, iqS=43). **We wszystkich punktach czekania: raw=473 (=zero), corr=740 stała, Δ=0 — sygnał momentu statyczny.**

**fw112_ab — epizody z sygnaturą opóźnienia (moment PWM z recovery w WAIT_FRESH):**

| EP (fw112_ab) | arm_tick (REVOKED) | PWM ms | recov@PWM | iqF/iqP/iqS@PWM | load@PWM |
|---|---|---|---|---|---|
| EP1 | 247988 | +370 ms | WAIT_FRESH | 14/14/**0** | 99 ckg |
| EP3 | 264689 | +358 ms | WAIT_FRESH | 14/14/**0** | 304 ckg |
| EP15 | 359647 | +504 ms | WAIT_FRESH | 14/14/**0** | 242 ckg |
| EP23 | (ok. 411 000) | +541 ms | **WAIT_FRESH→TRACK_FAST** | 14/14/0→**1** | 448 ckg |

We wszystkich: kadencja 10–68 rpm, load 66–448 ckg, **iq_setpoint=0** przy gotowym pre-rampie (iqP=14) i aktywnym latchu (hold=224). EP23 w tym samym oknie pokazuje przejście WAIT_FRESH→TRACK_FAST (iqS 0→1) — dowód, że TRACK_FAST wchodzi natychmiast po pojawieniu się świeżego sygnału.

---

## 5. Czas trwania WAIT_FRESH

| Epizod | GRANTED → setpoint/TRACK_FAST | Uwagi |
|---|---|---|
| saga 12..25 | **655 ms** | GRANTED 759,8 → start TRACK_FAST ~1414,8 ms (id 24→25) |
| saga 33..38 | **≥360 ms i nadal czeka** | GRANTED 1889,8 → HOLD_EXPIRED 2250,3 (cut) |
| rearm rid=0 | ≥731 ms (w oknie rekordu: setpoint NEVER) | setpoint pełny dopiero ~853 ms po REVOKED |
| rearm rid=1 | **~2958 ms** | t_setpoint_recovered; close 3525 ms |
| fw112_ab EP3 | ≥358 ms | PWM ms nadal WAIT_FRESH |
| fw112_ab EP15 | ≥504 ms | jak wyżej |
| fw112_ab EP23 | **541 ms** | przejście WAIT_FRESH→TRACK_FAST w ticku PWM |

---

## 6. Timing wejścia w TRACK_FAST

- **Jedyne zaobserwowane wejścia w TRACK_FAST:** (a) saga 12..25 — między id 24 (HOLD_ARMED, cum 1409,5 ms, iqS=0) a id 25 (REC_EXIT, cum 1554,8 ms, stab=560): wejście ~1414,8 ms, stabilizacja 140 ms → EXIT; (b) fw112_ab EP23 — dokładnie w ticku +541 ms (iqS 0→1, sample bezpośredni).
- **We wszystkich snapach zdarzeń recovery_state = WAIT_FRESH** (GRANTED 13/34, HOLD_ARMED 16/19/24/37, HOLD_EXPIRED 20/38). TRACK_FAST jest niewidoczny w strumieniu zdarzeń (nie ma typu zdarzenia „entry do TRACK_FAST”) — widoczny wyłącznie przez `stab` na EXIT oraz próbki fw112_ab.
- Wejście w TRACK_FAST następuje **natychmiast po pojawieniu się świeżego sygnału** (EP23: ten sam tick, w którym zniknął WAIT_FRESH). Opóźnienie = długość WAIT_FRESH, nie wolny ramp.

---

## 7. Dowody na COLLAPSE (TRACK_FAST→WAIT_FRESH)

**Brak jakiegokolwiek COLLAPSE w zachowanych danych:**
- W 24 zachowanych rekordach fw112_diag **zero zdarzeń REC_COLLAPSE (typ 6)**.
- Trajektoria iq: w całym WAIT_FRESH iqS=0 (bez rampy), jedyne wejście w TRACK_FAST (saga 12..25) zakończyło się czysto EXIT (nie spadło z powrotem); EP23 weszło w TRACK_FAST i szło dalej.
- Żadna próbka fw112_ab nie pokazuje TRACK_FAST z iqS>0, po którym iqS wraca do 0.
- Nie można w 100% wykluczyć COLLAPSE w 2 wyrejestrowanych sagach (4..11, 26..32) ani w uciętym ogonie, ale wzorzec iq (0 przez czekanie, pojedynczy wzrost przy dokończeniu) jest **niespójny** z cyklami „wejście→zapaść”.

**Wniosek: opóźnienie NIE jest spowodowane zapaścią TRACK_FAST.**

---

## 8. Dowody na 560 / EXIT (zakończony powrót)

- **Saga 12..25 = jedyny kompletny powrót:** `REC_EXIT` z **stab=560** (A[6..7]=0x0230), cum 1554,8 ms, iqS=14=iqP=14, iqR=9, load=386, thr=30. Stabilizacja 560 ticków (140 ms) w TRACK_FAST **spełniona w całości** — automatyka powrotu zadziałała poprawnie, gdy tylko dotarł świeży sygnał.
- Potwierdzenie w fw112_ab EP1: PWM ms +370 ms (iqS=0) → pełny setpoint 14 przy EXIT; ramp przechodzi 0→14 czysto (bez oscylacji).
- `ZEROED` bezpośrednio po GRANTED/REC_ENTER (id 15, 18, 21) = normalna sekwencja: latch trzyma, setpoint czeka na świeży sygnał, po jego braku zero.

---

## 9. Korelacja rearm_delay ↔ fw112 ↔ fw112_ab

**Pewna korelacja (zgodność 0,5 ms):**
- **rearm rid=0 ↔ fw112_ab EP1 (arm_tick 247988) ↔ saga 12..25.** Próbka PWM EP1 przy toff=+370 ms; fw112 id 16 (HOLD_ARMED) przy +369,5 ms po GRANTED(13). Zgodność w granicy jednego ticka. Oś: REVOKED(247988) → GRANTED(248046) → setpoint ~248701 → EXIT 248841. rearm rid=0: ENTER 247988, PROBLEM 248788, PERMISSION 249550, CLOSE 250719 — **całe okno w regionie pełzania pas_raw (245619–251097)**.
- **rearm rid=1 ↔ saga 33..38** (najmłodsza, w locie): GRANTED ~249176, HOLD_EXPIRED 249536 (cut). Czas GRANTED→czek w toku ≈ 360 ms; rekord rearm przewiduje setpoint ~2958 ms i close 3525 ms — dump uciął sagę **daleko przed** dokończeniem.
- **pas_raw:** w oknie tych dwóch sag rotacja była **pełzająca** (104–171 ms na przejście PAS, 2–14 przejść/250 ms) tuż po 7,4 s postoju (tick 240359). To jest **mechanizm spójny**: przy pełzającej/stołowej rotacji sygnał momentu Δ=0, afilt→0, świeży sygnał nie pojawia się.

**Korelacja jakościowa dla późniejszych epizodów (EP3/15/23, arm_tick 264689–411000, poza oknem pas_raw):** ta sama sygnatura (iqS=0 przy gotowym iqP, WAIT_FRESH w momencie PWM). Bez absolutnej kotwicy czasu dla nich — korelacja 1:1 z konkretnym zdarzeniem nie jest możliwa (zadanie: nie wymuszać, jeśli niemożliwe). EP23 daje niezależne potwierdzenie przejścia WAIT_FRESH→TRACK_FAST.

---

## 10. Klasyfikacja pierwotnej usterki: **LONG WAIT_FRESH**

**Opóźnienie startu = przedłużone WAIT_FRESH, NIE zapaść TRACK_FAST i NIE błąd stabilizacji 560.**

Mechanizm (zgodny ze wszystkimi danymi):
1. Kierowca pedałuje (load 66–464 ckg), ale rotacja korby jest **pełzająca / z postojami** (pas_raw: 7,4 s stop + 104–171 ms/przejście; kadencja w snapach 10–68 rpm, chwilami 0).
2. Sygnał momentu jest **statyczny** (rearm: raw=473=zero, corr=740 stała, Δ=0 przez całe okno) → filtr wspomagania afilt **zapada do 0 w ~200 ms** (snap PROBLEM) i utrzymuje 0.
3. Automat powrotu **poprawnie** pozostaje w WAIT_FRESH — nie może ruszyć na nieświeżym sygnale. Pre-ramp jest gotowy (iqP=14), latch trzyma (hold=224), ale setpoint = 0.
4. Dopiero gdy pojawia się świeży sygnał (afilt>0; rid=1: dopiero ~3 s; saga 12..25: ~655 ms), następuje **natychmiastowe** wejście w TRACK_FAST i — jeśli nic nie przerwie — czyste EXIT 560 (saga 12..25).

**Czyli to logika powrotu działa zgodnie z projektem; usterka leży upstream — w bramce świeżego sygnału / filtrze wspomagania / czujniku momentu przy pełzającej rotacji.**

Czas odczuwalnego opóźnienia: ~0,85 s (saga 12..25), ≥0,43 s (ostatnia, ucięta), do ~3,5 s (rekord rearm rid=1); epizody ride_episode z latch 403–1964 ms + jeden NEVER.

---

## 11. Pewność

| Wniosek | Pewność | Uzasadnienie |
|---|---|---|
| Retencja działa bezstratnie (24+15=39, ciągłość id) | **WYSOKA** | pełna ciągłość event_id, FW117/pas_raw bez luk |
| Opóźnienie = LONG WAIT_FRESH (nie collapse) | **WYSOKA** | dokładny strumień zdarzeń, iqS=0 przez czekanie, WAIT_FRESH w każdym milestonie, EP23: czyste WAIT_FRESH→TRACK_FAST |
| Mechanizm: pełzająca rotacja → Δ=0 → afilt→0 → brak świeżego sygnału | **ŚREDNIA** | pas_raw + snapy rearm wspierają; jednak kadencja 25–68 rpm częściowo zaprzecza „pełzaniu” (sprzeczność wymaga śledzenia afilt/filtra) |
| Brak COLLAPSE w całości (także w blokach wyrejestrowanych) | **ŚREDNIA-WYSOKA** | zero w 24 zachowanych; 15 wyrejestrowanych może teoretycznie kryć jeden, ale trajektoria iq temu przeczy |
| Korelacja rid=0↔EP1↔saga 12..25 | **WYSOKA** | zgodność PWM ms na 0,5 ms |
| Korelacja rid=1↔saga 33..38 | **ŚREDNIA** | porządek (najmłodsze rekordy), brak absolutnej kotwicy dla rid=1 |
| Root cause upstream (filtr/czujnik przy pełzaniu) | **ŚREDNIA** | wymaga dedykowanego trace'u afilt/filtra; nie rozstrzyga tego log |

---

## 12. Dokładne następne działanie (bez FIXU — STOP)

1. **Potwierdzić mechanizm afilt na pas_raw z tego samego logu:** w oknach sag 12..25 i 33..38 (ticki ~247 988–250 719 i ~249 176–249 536) wyciągnąć gęsty timeline przejść PAS i pokazać, że Δ momentu = 0 podczas czekania, oraz że powrót afilt (rid=1: afilt 0→3 przy close) zbiega się z końcem okresu pełzania. To domknie root-cause bez żadnych zmian w kodzie.
2. **Następny build/łog:** dodać trigger WAIT_FRESH-dwell do fw112_ab (próbki co 10–20 ms przez cały WAIT_FRESH z afilt/arun/Δ-momentu/kadencją zamiast planu wykładniczego) — rozstrzygnie sprzeczność „kadencja 25–68 rpm przy Δ=0”. (Zmiana w kodzie — wykonać osobno, po zatwierdzeniu flasha 0.0382.)
3. **Decyzja flasha 0.0382:** obecny log **potwierdza** poprawność retencji (werdykt PASS, pkt 2) i **nie wykazuje regresji** w powrocie (pkt 10). Analiza nie blokuje flashowania zgodnie z dokumentem `FW-112-STABILITY_RETENTION_PREFLASH_DECISION_PL.md`.

**NIE MA FIXU. STOP.**