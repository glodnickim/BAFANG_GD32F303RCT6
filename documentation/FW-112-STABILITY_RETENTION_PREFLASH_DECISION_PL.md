# FW-112-STABILITY — RETENTION — RAPORT PRE-FLASH + DECYZJA 0.0382

> Data: 2026-08-20. Zakres: **EWIKCJA NAJSTARSZEJ ZAKOŃCZONEJ SAGI** w `fw112_diag.c`
> (pełny ring 24/24 przestaje odrzucać — zamiast tego ewiktuje najstarszy zakończony saga
> i przyjmuje nowe zdarzenie). Karta NIE obejmuje zmian w torze jazdy.
> Status: **komplet — do decyzji użytkownika (sekcja 3).**

---

## 1. Problem

Pełny ring rejestratora fw112_diag (24 rekordy, 32 B) nasycał się podczas długich jazd.
Audyt obu logów:

- **LINEAR (sess 3):** 24 rekordy pokrywały okno 0→4.7 s; pełny ring już przy ~4.7 s
  (górne ograniczenie okna ≤8.7 s; pokrycie 11–20%); **odrzuconych 227 zdarzeń, 16/19 epizodów
  odrzuconych** (`rejected_total` + `DIAG_ERR_CAPTURES_FULL` + bit `FW112_REJECTED` w trailerze).
- **eMTB (sess 5):** pełny ring przy ~11.33 s (ograniczenie ≤17.7 s; pokrycie 29–45%);
  **odrzuconych 108 zdarzeń, 9/12 epizodów odrzuconych**.

Skutek: najnowsze zdarzenia powrotu (najistotniejsze dla analizy zachowania pod koniec jazdy)
były **tracone** dokładnie wtedy, gdy miały największą wartość diagnostyczną.

## 2. Build

- Artefakt: `.build\0.0382_M820_BL820.bin` — **136032 B**, zbudowany **2026-08-20 17:42:48**.
- SHA256: `AA983BF1D856AC97C23ACFD67294503672A826A8F991895CAA235D700C21E6CF`.
- Konfiguracja: `CAN_DIAGNOSTICS_ENABLE=1`, `BOOTLOADER=820`, ORIGIN `0x08005000`, LENGTH 230K.
- Rozmiary pełnego obrazu (DIAG=1, `arm-none-eabi-size`): **text 135728 B, data 268 B, bss 44756 B**.
- Różnica flash vs ostatni build DIAG 0.0380 (135496 B): **+536 B**.

## 3. Zmiana (polityka retencji)

Przy pełnym pierścieniu `append_record` **nie odrzuca** — najpierw ewiktuje najstarszy
ZAKOŃCZONY saga, potem przyjmuje nowe zdarzenie. Odrzuca (`rejected_total++`) tylko wtedy, gdy
nie ma nic do ewiktowania (brak sagi w ring, albo tylko aktywna saga i przychodzące zdarzenie
nie jest REVOKED). Granice sagi:

- **START** = `PERMISSION_REVOKED` (typ 3);
- **TERMINAL** = pierwszy `RECOVERY_EXIT` (typ 5, **włącznie**);
- zamknięcie poprzedniej sagi na granicy kolejnego `REVOKED` (**wyłącznie**);
- saga ciągnąca się do końca ringa = **in-flight** (zawsze najnowsza) — ewiktowana TYLKO, gdy
  przychodzący event to nowy `REVOKED`, który ją zamyka;
- ewikcja = `remove_block(off, count)` — przesunięcie bloku w dół bez zmiany `q_head`,
  kolejność chronologiczna zachowana, poprawna także po zawinięciu przez fizyczne 0.

Nowe liczniki: `evicted_saga_total`, `evicted_record_total` (uint32, **+8 B RAM**, wewnętrzne,
nigdy na przewodzie; accessory `fw112_diag_queue_evicted_sagas()/evicted_records()` tylko dla
testów host).

## 4. Pasywność

Brak jakichkolwiek zmian w `torque_input`, `ride_control`, torze Iq, `ride_session`,
`rearm_delay`, `WAIT_FRESH_LOAD`/`TRACK_FAST`, dwell 560 ticków, deadband, `force_zero_reference`,
limit/ramp. **S10** retention + **S5/S6** fw112-stability: ta sama długa jazda uruchomiona dwa razy
(rekorder zasilany vs nigdy nie wołany) daje **bajt-identyczną trajektorię wyjścia `torque_input`**,
a rekorder jednocześnie dowodzi, że ewikcja zadziałała i nic nie zostało odrzucone.

## 5. Testy (S1–S10 + pełna suita host)

`tests/host/fw112_retention_host.c` (realny `fw112_diag.c` + `torque_input.c`):

- **S1** pojemność: 24 zdarzenia (6 kompletnych sag) — zero odrzuceń, zero ewikcji.
- **S2** pełny ring + 1 stary saga: 25. zdarzenie ewiktuje NAJSTARSZY zakończony saga, samo jest
  przyjęte, `rejected=0`.
- **S3** wiele sag: każdy append przy pełnym ringu ewiktuje tylko najstarszy — drugi-najstarszy
  i wszystkie nowsze przetrwają w porządku.
- **S4** ochrona aktywnej sagi: pełny ring z samą sagą in-flight odrzuca COLLAPSE i EXIT (brak
  czego ewiktować), saga nietknięta.
- **S5** nowy REVOKED zamyka niezakończoną sagę: pełny ring jednej otwartej sagi ewiktowany w
  całości, nowy REVOKED ląduje.
- **S6** wraparound: z `q_head` przesuniętym (częściowy dump) i ringiem zawiniętym przez 0 —
  ewikcja usuwa najstarszy saga, przeżywalne zostają w porządku chronologicznym.
- **S7** 100 sag: 94 ewikcje, **ZERO odrzuceń** — rekorder zachowuje dowody późnej jazdy.
- **S8** otwarta saga > pojemność ringa: odrzucenie dopiero gdy nic do ewiktowania; brak
  korupcji, pierwsze 24 rekordy przetrwają w porządku.
- **S9** kolejność dumpa: strumień po wielu ewikcjach ściśle chronologiczny (event_id +1,
  wzorzec sag nienaruszony).
- **S10** równoważność logger ON/OFF na realnym automacie powrotu `torque_input`.

Wynik: `fw112_retention_host: ALL PASS`; `fw112_diag_host: ALL PASS` (S10/S12/S13 zaktualizowane
do nowej polityki); pełna `run-host-tests.ps1`: **wszystkie suity PASS** (FW-100…FW-112.2,
FW-117.1 START/STOP; suita `fw117_trace_bad_selector` poprawnie odmówiła budowy).

## 6. Mutacje M1–M5 (każda wykryta i przywrócona)

| Mutacja | Opis | Wynik |
|---|---|---|
| M1 | ewikcja NAJNOWSZEGO zamiast najstarszego | 9 FAIL (S2/S3/S6/S7/S9; S3 zgodnie z doc) |
| M2 | ewikcja AKTYWNEJ (in-flight) sagi | 17 FAIL (S4/S8; S4 zgodnie z doc) |
| M3 | off-by-one zostawiający rekord starej sagi (`end=i` zamiast `i+1`) | 32 FAIL (S2/S3/S6/S7/S9) |
| M4 | błędne przesunięcie bloku przy wraparound (dst==src) | 9 FAIL (S2/S3/S6/S7/S9) |
| M5 | stary fallback odrzucania przy pełnym ringu (brak ewikcji) | 40 FAIL (S2/S3/S5/S6/S7/S9/S10) |

Każda mutacja wykrywana **behawioralnie** (realny moduł zlinkowany), nie skanem źródła. Po każdej
mutacji stan przywrócony; źródło zweryfikowane (obie suity ALL PASS po cyklu).

## 7. RAM / FLASH

- `fw112_diag.o` (flagi jak w realnym buildzie, DIAG=1): **text 2752 B, data 0 B, bss 912 B**
  (linia budżetu 976 B = 912 + 64 B headroom).
- Budżet diagnostyczny `DIAG_BUDGET_TOTAL_BYTES = 33656 B` ≤ sufit 34 KB (34816 B) →
  **zapas 1160 B** (sufit tymczasowo podniesiony przez FW-117 bridge trace).
- Delta RAM vs poprzedni: +8 B bss (dwa liczniki u32). Delta flash: +536 B (0.0380→0.0382 DIAG).

## 8. Ryzyko

Wyłącznie obserwowalność. Ewiktowane są najstarsze zakończone sagi (starsze niż okno
~5–17 s w zależności od jazdy), więc utrata dotyczy tylko starej historii; **ostatnie 6 sag
zawsze obecne** (24 rekordy / ~4 rekordy na sagę). Zero zmian toru Iq, zero nowych ramek,
brak blokowania, brak malloca. Ewentualne błędy rejestracji raportowane jako brak/niepełne
ramki — nie wpływają na jazdę.

## 9. Procedura weryfikacji sprzętowej

1. Flash `0.0382_M820_BL820.bin` na stanowisku serwisowym, bezpieczna trasa, nagrywanie całości
   sesji (canable).
2. Długa jazda LINEAR + eMTB (kilka epizodów powrotu > 24 rekordów, >~30 s).
3. Odczyt logów, dekodowanie `tools/decode_fw112_diag_*.ps1`.
4. Kryteria PASS: trailer nigdy nie raportuje `FW112_REJECTED`/`CAPTURES_FULL`; licznik
   ewikcji > 0; zdumowane sagi w porządku chronologicznym; w każdym oknie najnowsze sagi obecne;
   anomalia przy prawie-zerowym naciśnięciu obserwowana jak dotąd (bez fałszywych grantów).
5. Porównanie profilu jazdy z buildem bazowym (identyczne zachowanie wspomagania).

## 10. No-go / izolacja

- Nie flashować bez zgody (sekcja 11).
- 0.0382 zawiera wyłącznie zmianę retencji na bazie 0.0380-DIAG-behavior; nie łączyć z FW-114
  (0.0372/0.0373) — walidacja FW-114 to osobna karta.
- Nie używać logów z innego builda (wersja z 0x6001 musi być `eVD 0.0382`).

## 11. Zatwierdzenie

Wymaga jawnego **`OK` użytkownika na flash 0.0382**. Bez zgody — **STOP: bez flasha, bez commita,
bez pusha.**

---

**Podsumowanie: ewikcja najstarszej zakończonej sagi wdrożona, S1–S10 + M1–M5 wykrywalne,
pełna suita host PASS, budżet RAM/FLASH mieszczą się w progach. Flash 0.0382 = do zatwierdzenia
przez użytkownika (sekcja 11).**