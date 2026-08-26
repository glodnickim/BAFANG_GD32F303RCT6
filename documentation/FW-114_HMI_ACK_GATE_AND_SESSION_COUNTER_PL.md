# FW-114 — HMI multiframe ACK gate + licznik sesji 82F83000 (FINAL STATUS)

**STATUS: FW-114 — AUDIT COMPLETE, READY FOR FLASH.** To jest canonical summary karty FW-114
(D1 — multiframe ACK gate, D2 — `82F83000` licznik sesji) po audycie końcowym (G6/G7/G8).
Szczegółowe fakty protokołu CAN pozostają w `CAN_PROTOCOL_REFERENCE.md` (source of truth);
ten dokument opisuje mechanizm FW-114, status, checklistę walidacji sprzętowej i changelog.

Karty powiązane:
- [FW-110 — izolacja transportu CAN (multiframe v4, 0x6029)](FW-110_CAN_TRANSPORT_ISOLATION_PL.md) — automat
  stop-and-wait, na którym FW-114 dołożyło bramkę ACK.
- [CAN Protocol Reference](CAN_PROTOCOL_REFERENCE.md) — fakty protokołu (format ID, ramki, sekwencje).

**FLASH / COMMIT / PUSH = NO.** Karta dokumentacyjna nie wgrywała ani nie zacommitowała niczego.

---

## 1. D1 — multiframe HMI ACK gate

### Zachowanie fabryczne (dowód: `logi m510 original`, `ON - SERVICE MENU - CONTROLER INFO - BACK - OFF.log.txt`)

Fabryczny M510 po READ query `8311XXXX` wysyła LONG_START, potem **czeka na ACK z HMI**,
a dopiero potem DATA/END:

```
821C6000  Data:12                     <- LONG_START (18 B)
83126000  DLC:0                       <- NORMAL_ACK z HMI (~31-33 ms po START)
821D0000  Data:43 52 20 58 31 30 4E 2E <- LONG_TRANG ...
821E0002  Data:2E 30                   <- LONG_END
```

ACK: `source=3`, `target=2`, `op=NORMAL_ACK=2`, `command` zgodny z START-em. EBiCS przed FW-114
wysyłał START+DATA+END back-to-back i ignorował ACK.

### Implementacja FW-114

- `src/can_multiframe.c` — nowy stan automatu `CANMF_WAIT_HMI_ACK`:
  - bramka dotyczy **wyłącznie reply do `target=3`** (HMI read channel);
  - po potwierdzonym DONE ramki START faza DATA jest wstrzymana do nadejścia ACK lub timeoutu;
  - `can_multiframe_hmi_ack(command)` — ACK jest latchowany **tylko gdy**:
    - transfer `active`,
    - `target == 3`,
    - `command` zgodny z `mf_command`,
    - aktualny fragment == `CANMF_FRAG_START`;
  - `ERROR_ACK` **nie zwalnia** bramki;
  - fail-safe timeout: `CANMF_ACK_WAIT_TIMEOUT_TICKS = 400` ticków = **100 ms przy 4 kHz**;
  - test timeoutu: `(uint32_t)(now_tick - mf_ack_wait_tick) >= timeout` (bezpieczny dla wrapu uint32_t);
  - `target=5` (CANable/BESST) **nigdy nie czeka** na ACK HMI.
- `src/CAN_Display.c` — `processCAN_Rx()`: `case NORMAL_ACK` (op=2) dla `source==3` →
  `can_multiframe_hmi_ack(Ext_ID_Rx.command)`; `case ERROR_ACK` pusty. `processCAN_Rx()`
  jest już opakowany `if (Ext_ID_Rx.target == 2)` — ACK z source=3 trafia do kontrolera
  tylko przez target=2.
- `src/main.c` — `can_multiframe_step(control_time_ticks)` (timebase 4 kHz).

### Ochrona przed stale ACK

ACK ze **zakończonego/timeout'owanego** transferu nie jest latchowany (`active == false`).
Nowy transfer resetuje `mf_ack_seen` w `start_common()`. ACK jest przyjmowany wyłącznie w
kontekście właściwej aktywnej/pending transakcji target=3 i zgodnego command.
Dowód: test **G6** (patrz §6). Uwaga teoretyczna: ACK `83126000` jest byte-identyczny dla
tego samego command — ACK przychodzący *podczas* bramki nowego transferu jest nieodróżnialny
od prawdziwego ACK dla tego transferu (protokół nie niesie ID transakcji); jest to przypadek
bezpieczny (release wcześniejszy, nie błędny transfer).

### 0x6012 trailer ordering

Trailer `821B6012 Data:01 00 02 06` jest armowany **atomicznie z tym samym transferem**
(`send_multiframe_trailer()` → `can_multiframe_start_with_trailer()`); automat produkuje go
dopiero po DONE ramki END. Żadne miejsce po `send_multiframe*()` nie wysyła bezpośrednio
dodatkowej ramki w sposób łamiący kolejność. Efekt 0x6029 (`can_reply_effects_6029_armed()`) jest
rozwiązywany tylko po `CANMF_XFER_DONE` przez `can_reply_effects_poll()`. Dowód: test **G8**.

## 2. D2 — `82F83000` licznik sesji

Fabryczny payload (dowód: `logi m510 original`):

```
00 00 00 00   <- boot
01 00 00 00   <- po ~10 s
02 00 00 00   <- po ~20 s
03 00 00 00   <- po ~30 s
```

Implementacja FW-114 (`sendCAN_status_broadcast()`):

```
d[0] = (uint8_t)(control_time_ticks / (CONTROL_TIMEBASE_HZ * 10U));   // floor(uptime/10 s) mod 256
d[1] = d[2] = d[3] = 0;
```

- `CONTROL_TIMEBASE_HZ = 4000U` (4 kHz) — `control_time_ticks` startuje od 0 po boot;
- wrap `byte0` 255→0 jest naturalny (mod 256) i bezpieczny;
- **usunięte** stare `d[3] = 0x0B` (zamrożony licznik `00 00 00 0B`).

## 3. FW-114 FINAL STATUS

```
D1 Multiframe ACK gate:
VERIFIED

Stale ACK protection:
VERIFIED

uint32 timeout wrap:
VERIFIED

CANable/BESST target=5 compatibility:
VERIFIED

0x6012 post-transfer trailer ordering:
VERIFIED

82F83000 payload/counter:
VERIFIED

82F83000 transmit cadence:
HARDWARE VERIFICATION PENDING

Host tests:
26/26 PASS

Production changes after audit:
NONE

Final artifact:
0.0373_M820_BL820.bin

SHA256:
4D04834DF98383E179530FAF0CD4FD24A6272BA1A0913A0D704D1D62A21E7DE8
```

## 4. FW-114 HARDWARE VALIDATION

### A. Multiframe

Sniff sekwencji boot/info i zweryfikuj dla każdej z komend:

```
821C6000
-> 83126000
-> 821D...
-> 821E...
```

Analogicznie dla: `6001`, `6002`, `6003`, `6011`, `6012`.

### B. 6012

Zweryfikuj dokładną kolejność:

```
821C6012
-> 83126012
-> DATA x7
-> END
-> 821B6012 Data:01 00 02 06
```

### C. HMI

- Controller Info się renderuje;
- Settings/Info nie zostaje pusta;
- brak nowych regresji CAN/HMI.

### D. 82F83000

- payload: bajty 1..3 zawsze zero;
- byte0: `00` przy boot, `01` ok. 10 s, `02` ok. 20 s, `03` ok. 30 s;
- zmierz faktyczny interwał między kolejnymi ramkami `82F83000`;
- porównaj cadence z fabrycznym trace M510.

## 5. Otwarte pytanie (znane)

> **Known open question:** Exact stock transmit cadence of controller broadcast `0x82F83000`
> remains hardware-verification pending. Counter semantics and payload are verified.

To NIE jest bug — do potwierdzenia snifferem na realnym HMI/M510/M820. Aż do potwierdzenia
cadence NIE zmieniamy częstotliwości wysyłania (obecnie ~480 ms razem z `82FF1200`/`82F8320F`
w `sendCAN_status_broadcast()`).

## 6. Testy host

Suite `FW-110 can_multiframe stop-and-wait multiframe producer` (kompiluje prawdziwe
`src/can_multiframe.c` + `src/can_tx_queue.c` + scriptable fake CAN) — nowe testy FW-114:

- **G1** — reply do target=3 czeka na ACK: dokładnie START na magistrali przed ACK, bez DATA/END;
- **G2** — fail-safe timeout (brak ACK → DATA/END po ~400 tickach);
- **G3** — ACK z błędnym commandem ignorowany; tylko zgodny command zwalnia;
- **G4** — wczesny ACK (START w locie) jest latchowany i pomija czekanie;
- **G5** — target=5 (CANable/BESST) nigdy nie czeka na ACK;
- **G6** — stale ACK z poprzedniej zakończonej transakcji nie zwalnia nowego transferu o tym
  samym command (sekwencja: T1 timeout → spóźniony ACK przy braku aktywnego transferu → T2
  nadal czeka; zwalnia go dopiero własny ACK);
- **G7** — timeout ACK działa poprawnie przez wrap uint32_t (START przy `0xFFFFFF00`, release
  po dokładnie 400 tickach przez `0xFFFFFFFF → 0x00000000`);
- **G8** — 0x6012 target=3 z bramką: na magistrali dokładnie START → DATA x7 → END → TRAILER
  `821B6012 01 00 02 06`; przed ACK nie wychodzi END ani trailer.

**Wynik: 26/26 PASS** (wszystkie suity `run-host-tests.ps1`, w tym FW-100, FW-101/102/104,
FW-106, FW-109, FW-110, FW-111, FW-112.x, FW-113.x, FW-112-DIAG i guardsy źródłowe).

## 7. Build

| Parametr | Wartość |
|---|---|
| Artefakt | `.build/0.0373_M820_BL820.bin` |
| SHA256 | `4D04834DF98383E179530FAF0CD4FD24A6272BA1A0913A0D704D1D62A21E7DE8` |
| text / data / bss | 132940 / 268 / 29188 |
| Wariant | CAN DIAGNOSTICS ON (DIAG) |
| BOOTLOADER / ORIGIN | 820 / `0x08005000` |
| Warningi | tylko pre-existing: `-Wpointer-sign` (append_multiframe/send_multiframe*), unused `fw_ver` |

Produkcja po audycie: **NONE** — jedyna zmiana po 0.0372 to rozszerzenie testów host (G6/G7/G8);
binarki 0.0372 i 0.0373 różnią się wyłącznie numerem wersji w stringu 0x6001.

## 8. Changelog FW-114

```
FW-114 / 0.0372
- added HMI multiframe ACK gate
- added 82F83000 session counter semantics

FW-114 audit / 0.0373
- production code unchanged
- added G6 stale ACK test
- added G7 uint32 wrap test
- added G8 0x6012 ordering test
- 26/26 PASS
- READY FOR FLASH
- hardware cadence verification pending
```

## 9. Zakres zmian (produkcja, przed audytem)

- `src/can_multiframe.c` — stan `CANMF_WAIT_HMI_ACK`, `can_multiframe_hmi_ack()`,
  `can_multiframe_step(uint32_t now_tick)`, reset `mf_ack_seen` w `start_common()`;
- `inc/can_multiframe.h` — `CANMF_ACK_WAIT_TIMEOUT_TICKS`, nowa sygnatura `step`, deklaracja `hmi_ack`;
- `src/CAN_Display.c` — case `NORMAL_ACK`/`ERROR_ACK` w `processCAN_Rx()`, D2 w
  `sendCAN_status_broadcast()`, `extern volatile uint32_t control_time_ticks;`;
- `src/main.c` — `can_multiframe_step(control_time_ticks)`.

Audyt (G6/G7/G8) nie wymagał zmian w produkcji.