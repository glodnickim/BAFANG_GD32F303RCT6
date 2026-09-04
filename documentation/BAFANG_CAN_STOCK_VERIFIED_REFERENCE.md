# Bafang CAN — STOCK VERIFIED reference

**Reference version:** 2.0 — 2026-08-22  
**Scope:** supplied stock controller firmware + supplied stock DPC245 C2.0/C3.0 firmware + three supplied stock CAN captures.  
**Evidence lock:** eVistDrive and third-party protocol descriptions are explicitly **not** protocol evidence.

> **Rule:** every observed/discovered byte is documented. A physical name is assigned only when stock code or a controlled capture proves it. Where the binary proves only width, endian, source RAM or an equation, that structure is documented and the physical semantic stays `UNKNOWN`.

## 1. Evidence set

| Artifact | SHA-256 | Notes |
|---|---|---|
| `MMG522C3619F802008.1.bin` | `03d1fad3c4e0bd515fe47055d2ef981f8261f68035521c8d5ef0f95a4f172834` | 84,512 B; image @ file `0x20`, runtime base `0x08004000` |
| `MMG522C3619F802051.2_20241109.bin` | `e513a5c10328933e46006d96818ad68d17c97e5e0bffeb5d6d69542bd933e0ef` | 147,648 B; image @ file `0x20`, runtime base `0x08004000` |
| `DPC245CF80261.3-DP C245.C 2.0(1).bin` | `f7fff082a60217d5a32de0d45d01ed3bdad0cc2b31ca8550ecca84e4455c0cc6` | 462,372 B; image @ file `0x20`, runtime base `0x08020000` |
| `DPC245CF80301.0-DP C245.C 3.0(1).bin` | `a0494b36e4b9990b7aaef07ff9a44f5c632e94150f87330c2e0521d1dba00e4f` | 456,400 B; image @ file `0x20`, runtime base `0x08020000` |

CAN captures: `ON - SERVICE MENU - CONTROLER INFO - BACK - OFF.log.txt`, `ON - SERVICE - AUTO OFF (OFF - 10min, 10min - OFF) - BACK - OFF.log.txt`, `ON - WA - OFF.log.txt`.

## 2. Physical/link layer — confirmed

- **Classic CAN**, 29-bit extended identifiers.
- **Bitrate: 250,000 bit/s.** Both stock HMI images configure 8 MHz HXTAL → 120 MHz SYSCLK, APB1=60 MHz and CAN timing prescaler 24, BS1=5 TQ, BS2=4 TQ, SJW=1 TQ. Therefore `60 MHz / (24 × (1+5+4)) = 250 kbit/s`. Sample point = 60%.
- The supplied text logger sets bit31 as its extended-frame marker. Use `wire_id = logged_id & 0x1FFFFFFF`; do not transmit the logger marker.

## 3. 29-bit identifier layout
```text
wire_id = command | (operation << 16) | (target << 19) | (source << 24)
command   =  wire_id        & 0xFFFF
operation = (wire_id >> 16) & 0x07
target    = (wire_id >> 19) & 0x1F
source    = (wire_id >> 24) & 0x1F
```
This extraction/composition is present in the stock controller/HMI CAN code.

### Nodes
| node | Role proven by supplied evidence |
|---:|---|
|2|Controller|
|3|DPC245 HMI/display|
|31 (`0x1F`)|Broadcast target|
|1|Boot-probed device slot — physical role **UNKNOWN**|
|4|Boot-probed device slot — physical role **UNKNOWN**|
|19|Receives HMI `6207` and `0000` frames — physical role **UNKNOWN**|

### Operation field
| op | Verified behavior |
|---:|---|
|0|normal/application data|
|1|READ|
|2|NORMAL ACK / normal response|
|3|ERROR response|
|4|LONG START|
|5|LONG DATA|
|6|LONG END|
|7|dedicated status/fault/event semantic class; manufacturer symbolic name **UNKNOWN**|

## 4. Long READ protocol
```text
Requester   op1 READ(original_command), DLC0
Responder   op4 LONG_START(original_command), total_length LE
Requester   op2 ACK(original_command), DLC0
Responder   op5 LONG_DATA(chunk_index=0), DLC8
            ...
Responder   op6 LONG_END(chunk_index=N), DLC1..8
```
The responder gates DATA on the ACK to START. The low 16-bit command field is reused as payload chunk index after START.

Captured controller identity transactions:
- `6000` → `CR X10N.510.FC 4.0` (18 B).
- `6001` → `MMG522C4814F804102.4` (20 B).

## 5. Boot/discovery sequence — repeated in all three captures
1. node3→node2 READ `6000`; node2 responds long.
2. node3→node2 READ `6001`; node2 responds long.
3. node3 probes node1 with READ `6000`, `6001`; no response in supplied captures.
4. node3 probes node4 with READ `6000`, `6001`, `6400`, `6401`; no response in supplied captures.

This is a boot/discovery sequence, not a Controller Info menu-only sequence. Node1/node4 roles are deliberately not guessed.

## 6. HMI → controller / broadcast application frames

### `0x6300`

Direction: HMI node3 → controller node2, op0, DLC4; controller has an explicit RX descriptor and replies op2 ACK. Capture cadence ≈100 ms.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0` | u8 low nibble | maximum selectable Assist level / selector upper bound | `CONFIRMED_FIRMWARE` |
| `1` | enum u8 | current Assist encoded: 0->00, 1->0B, 2->0D, 3->15, 4->17, 5->03; Walk Assist active overrides to 06 | `CONFIRMED_FIRMWARE+CAPTURE` |
| `2` | bitfield/event byte | normal path: event2=>0x02, event5=>0x04, event6=>0x08, plus state bit2=>0x01; special persistent-state path may carry/toggle additional bits including observed 0x20; exact physical key meaning of every bit is not proven | `STRUCTURE_CONFIRMED_SEMANTIC_PARTIAL` |
| `3` | u8 | trip-reset control: 1 normal; 0 while HMI executes Trip reset and clears Trip/Time/AVG accumulators/MAX/AVG | `CONFIRMED_FIRMWARE` |

### `0x6301`

Direction: HMI node3 → controller node2, op0, DLC8. Also supported by HMI `READ 6301` responder. Capture cadence ≈220–230 ms.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..2` | LE24 | ODO in whole km. C3=floor(internal_0.01km/100); C2=round-nearest via (internal+50)/100; unit km | `CONFIRMED_FIRMWARE+CAPTURE` |
| `3..5` | LE24 | Trip = internal_0.01km/10; unit 0.1 km/count | `CONFIRMED_FIRMWARE+CAPTURE` |
| `6..7` | LE16 | MAX speed; unit 0.1 km/h/count | `CONFIRMED_FIRMWARE+CAPTURE` |

### `0x6302`

Direction: HMI node3 → controller node2, op0, DLC5. Also supports `READ 6302` and a zero-reset WRITE path. Capture cadence ≈220–230 ms.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..1` | LE16 | AVG speed; unit 0.1 km/h/count | `CONFIRMED_FIRMWARE+CAPTURE` |
| `2..4` | LE24 | Service distance/progress = internal_0.01km/10; grows with distance, clamps at Service interval; resettable by WRITE 6302 zero form; unit 0.1 km/count | `CONFIRMED_FIRMWARE+CAPTURE` |

### `0x6303`

Direction: HMI node3 → controller node2, op0, DLC1; controller explicit RX + op2 ACK. Capture cadence ≈1.01 s.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0` | u8 | Auto-Off: 0xFF disabled; otherwise configured nonzero minute value is sent directly. Capture proves 0x0A = 10 min.; unit minutes for non-FF values | `CONFIRMED_FIRMWARE+CONTROLLED_CAPTURE` |

### `0x6304`

Direction: HMI node3 → controller node2, op0, DLC4. Capture cadence ≈220–230 ms.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0` | u8 constant | tag 0x05 | `CONFIRMED_FIRMWARE` |
| `1` | u8 | Brightness setting raw value | `CONFIRMED_FIRMWARE+CAPTURE` |
| `2` | u8 constant | tag 0x04 | `CONFIRMED_FIRMWARE` |
| `3` | u8 | AL sensitivity (ambient-light sensitivity) raw setting | `CONFIRMED_FIRMWARE+CAPTURE` |

### `0x630B`

Configuration command present in **both** stock DPC245 C2.0 and C3.0. HMI implements READ and WRITE.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0` | u8 constant | tag 0x04 in READ response | `CONFIRMED_FIRMWARE` |
| `1..3` | LE24 | Service interval; unit 0.1 km/count; range 10000..50000 raw = 1000.0..5000.0 km | `CONFIRMED_FIRMWARE_C2+C3` |
| `4..7` | undefined tail on READ response | READ response is DLC8 but responder only initializes bytes0..3; receiver must ignore tail and must not require zero | `CONFIRMED_FIRMWARE_QUIRK` |

### `0x3301`

Direction: HMI node3 → broadcast node31, op0, DLC5. Capture cadence ≈1.02 s.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..3` | LE32 | drive Time = internal 500 ms moving ticks /120; unit full minutes | `CONFIRMED_FIRMWARE+CAPTURE` |
| `4` | u8 constant | 0x0A | `CONFIRMED_FIRMWARE+CAPTURE` |

### `0x3302`

Direction: HMI node3 → broadcast node31, op0, DLC8. Capture cadence ≈220–230 ms.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..7` | same as 6301 | broadcast copy of the same ODO/Trip/MAX source fields and packing as 6301 | `CONFIRMED_FIRMWARE+CAPTURE` |

### `0x3303`

Direction: HMI node3 → broadcast node31, op0, DLC5. Capture cadence ≈220–230 ms.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..4` | same as 6302 | broadcast copy of the same AVG/Service-distance source fields and packing as 6302 | `CONFIRMED_FIRMWARE+CAPTURE` |

### `0x6207`

Firmware-only HMI periodic builder case: HMI → node19, DLC1. Not seen in supplied captures.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0` | u8 constant | HMI sends 0x0A to target node19; physical node role/field semantic not proven | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |

### `0x0000`

Firmware-only HMI periodic builder case: HMI → node19, DLC1. Not seen in supplied captures.

| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0` | u8 | HMI internal state byte +0x19 sent to target node19; physical semantic not proven | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |

### Exact `6300[1]` Assist encoding
| Assist UI level | wire `6300[1]` |
|---:|---:|
|0|`00`|
|1|`0B`|
|2|`0D`|
|3|`15`|
|4|`17`|
|5|`03`|
|Walk Assist active|`06` override|
The same mapping appears in the receive-side decoder, independently confirming the builder mapping.

### Physical key hardware vs `6300[2]`
Stock HMI scans GPIOA: PA4→raw key code1, PA3→raw code2, PA2→raw code4; PA3+PA4→3 and PA2+PA4→8. UI paths map PA3 to UP, PA4 to DOWN, PA2 to center/confirm/escape depending context. **However `6300[2]` carries debounced event/hold/repeat codes, not raw GPIO key codes**, so it is unsafe to label `0x02` or `0x20` globally as a specific key.

## 7. HMI read/write commands relevant to statistics/service
| Command | Operation | Verified behavior |
|---|---|---|
|`6301`|READ|op2 DLC8; ODO/Trip/MAX format identical to periodic 6301|
|`6302`|READ|op2 DLC5; AVG/Service distance|
|`6302`|WRITE/reset|accepts zero service-distance form (`b2..4=0`), sets Service distance=0, ACK|
|`630B`|READ|op2 DLC8; `b0=04`, `b1..3` Service interval; **b4..7 undefined/uninitialized tail**|
|`630B`|WRITE|`b1..3` LE24 raw 10000..50000 → 1000.0..5000.0 km; ACK|
|`6201`|WRITE|`b0..2` LE24 whole km ×100 → internal ODO; zero also resets Trip|
|`6202`|WRITE|3 bytes copied to persistent/config object; semantic name not proven|
|`6203`|WRITE|`b0..2` LE24 ×10 → Trip (0.1 km wire); zero sets Trip-reset flag|
|`6500`|WRITE|handler exists; payload semantic not proven|

## 8. Controller → HMI dashboard family `0x3200..0x3205`
The inspected stock DPC245 broadcast parser (`target=31`, `op=0`) directly consumes `3200..3205`. Fields below are traced from CAN payload into HMI state/dashboard code.

### `0x3200` — DLC 8, stock new-controller period 1980 ms

| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
| `0` | u8 | controller/HMI state byte; consumed by HMI dashboard state logic; exact physical semantic not proven | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |
| `1..2` | LE16 rolling counter | distance-increment source. HMI computes unsigned delta and adds it to ODO, Trip, Service distance and AVG-distance accumulator; unit 0.01 km/count internal delta | `CONFIRMED_FIRMWARE` |
| `3` | u8 | Cadence; unit rpm, 1 rpm/count | `CONFIRMED_FIRMWARE` |
| `4..5` | LE16 | raw input with minimum clamp 750. HMI derives round((raw-750)*cadence*88/2450) into another telemetry field; physical name of raw and derived quantity not proven | `STRUCTURE+FORMULA_CONFIRMED_SEMANTIC_UNKNOWN` |
| `6..7` | LE16 | Range; 0xFFFF treated as special/unavailable; HMI performs km/mile conversion; unit 0.01 km/count | `CONFIRMED_FIRMWARE` |

### `0x3201` — DLC 8, stock new-controller period 247 ms

| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
| `0..1` | LE16 | current speed; HMI converts raw to internal 0.1 km/h and clamps display at 99.9 km/h; unit 0.01 km/h/count | `CONFIRMED_FIRMWARE` |
| `2..3` | signed LE16 | electrical/math factor A. HMI stores it and uses A*B/10000; exact source quantity not independently proven | `STRUCTURE+FORMULA_CONFIRMED_SEMANTIC_UNKNOWN` |
| `4..5` | unsigned LE16 | electrical/math factor B. HMI stores it and uses A*B/10000; exact source quantity not independently proven | `STRUCTURE+FORMULA_CONFIRMED_SEMANTIC_UNKNOWN` |
| `6` | u8 | status byte stored by HMI; exact physical semantic not proven | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |
| `7` | u8 | status byte stored by HMI; exact physical semantic not proven | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |
| `derived` | signed(A)*unsigned(B)/10000, 10-sample rolling average | dashboard Watt value; unit W | `CONFIRMED_FIRMWARE` |

### `0x3202` — DLC 1, stock new-controller period 99 ms

| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
| `0` | u8 bitfield | inspected HMI parser consumes only bit4 and copies it into local state bit4; other bits are not interpreted by this handler | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |

### `0x3203` — DLC 6, stock new-controller period 3960 ms

| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
| `0..1` | LE16 | telemetry field +0x34 | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |
| `2..3` | LE16 | telemetry field +0x36 | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |
| `4..5` | LE16 | telemetry field +0x38 | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |

### `0x3204` — DLC 1, stock new-controller period 50 ms

| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
| `0` | u8 bitfield | inspected HMI parser tests bit0; bit0=0 invokes dedicated handler, bit0=1 does not. Other bits ignored there. | `STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN` |

### `0x3205` — DLC 2, stock new-controller period 3960 ms

| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
| `0..1` | LE16 | Calories displayed as Cal; unit kcal, 1 kcal/count | `CONFIRMED_FIRMWARE` |

### Dashboard names proven by HMI UI data path
`Trip`, `ODO`, `MAX`, `AVG`, `Watt`, `Range`, `Cadence`, `Cal`, `Time`. Units include km/mile, km/h/mph, W, rpm, Kcal and min. Metric/imperial distance/speed conversion uses exact constant **1.60934**.

## 9. Battery Info family `0x3400/0x3401`

### `0x3400`
| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..1` | LE16 | Full Cap; unit mAh | `CONFIRMED_FIRMWARE` |
| `2..3` | LE16 | Res Cap; unit mAh | `CONFIRMED_FIRMWARE` |
| `4` | u8 | RelChargeState; unit % raw | `CONFIRMED_FIRMWARE` |
| `5` | u8 | AbsChargeState; unit % raw | `CONFIRMED_FIRMWARE` |
| `6..7` | raw/if present | not consumed by the inspected Battery Info parser | `NOT_CONSUMED_BY_INSPECTED_HANDLER` |

### `0x3401`
| Byte(s) | Encoding | Meaning | Status |
|---|---|---|---|
| `0..1` | signed LE16 | Current; unit 10 mA/count | `CONFIRMED_FIRMWARE` |
| `2..3` | LE16 | Totalvolt; unit 10 mV/count | `CONFIRMED_FIRMWARE` |
| `4` | signed/raw u8 | Temp = raw - 40; unit degC | `CONFIRMED_FIRMWARE` |
| `5..7` | raw/if present | not consumed by the inspected Battery Info parser | `NOT_CONSUMED_BY_INSPECTED_HANDLER` |

Stock HMI also contains Battery Info strings `Cycle Times`, `M.N.T`, `L.N.T`, `Cell`, `Cell 1..8`. Their exact CAN source is **not yet proven**. Although boot probes node4 with `6400/6401`, this alone is not enough to assign those commands to Battery Info.

## 10. New controller cyclic TX scheduler — exact firmware table
These periods come from the stock firmware descriptor table, not from capture estimation.

| cmd | DLC | period ms | TX data RAM | timer/state RAM | Payload status |
|---|---:|---:|---|---|---|
|`3000`|4|9900|`0x20003B78`|`0x20003B68`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3200`|8|1980|`0x20003B7C`|`0x20003B69`|CONFIRMED_FIRMWARE; STRUCTURE+FORMULA_CONFIRMED_SEMANTIC_UNKNOWN; STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3201`|8|247|`0x20003B84`|`0x20003B6A`|CONFIRMED_FIRMWARE; STRUCTURE+FORMULA_CONFIRMED_SEMANTIC_UNKNOWN; STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3202`|1|99|`0x20003B8C`|`0x20003B6B`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3203`|6|3960|`0x20003C63`|`0x20003B6C`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3204`|1|50|`0x20003B8D`|`0x20003B6D`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3205`|2|3960|`0x20003B8E`|`0x20003B6E`|CONFIRMED_FIRMWARE|
|`3206`|4|99|`0x20003B90`|`0x20003B6F`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`3207`|7|99|`0x20003B94`|`0x20003B70`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`3208`|2|197|`0x20003B9B`|`0x20003B71`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`3209`|2|197|`0x20003B9D`|`0x20003B72`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`320A`|1|99|`0x20003B9F`|`0x20003B73`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`320E`|8|990|`0x2000874A`|`0x20003B74`|CONFIRMED_FIRMWARE; STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`320F`|8|1980|`0x20003BA0`|`0x20003B75`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3210`|8|990|`0x200086EC`|`0x20003B76`|STRUCTURE+CAPTURE_CONFIRMED_SEMANTIC_UNKNOWN; STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`3213`|4|990|`0x200087A0`|`0x20003B77`|CONFIRMED_FIRMWARE+CAPTURE|
|`6200`|3|99|`0x20003BE9`|`0x20003BEC`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`62DB`|6|99|`0x20000B1C`|`0x20003BF5`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`1200`|1|495|`0x20003BA8`|`0x20003BAD`|CLASS_CONFIRMED_SEMANTIC_UNKNOWN|
|`1201`|1|495|`0x20003BA9`|`0x20003BAD`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`1202`|1|495|`0x20003BAA`|`0x20003BAD`|CLASS_CONFIRMED_SEMANTIC_PARTIAL|
|`1203`|1|495|`0x20003BAB`|`0x20003BAD`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`1204`|1|495|`0x20003BAC`|`0x20003BAD`|STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN|
|`A0FA`|5|99|`0x20003C4E`|`0x00000000`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`A0FB`|8|99|`0x20003C53`|`0x00000000`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`A0FC`|3|495|`0x20003642`|`0x00000000`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`A000`|8|12|`0x00000000`|`0x00000000`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|
|`A010`|8|1|`0x00000000`|`0x00000000`|FRAME_CONFIRMED_PAYLOAD_SEMANTIC_UNKNOWN|

### Detailed newly observed/newer diagnostic windows

#### `0x3000`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0..3`|LE32|counter incremented by stock controller code; physical purpose not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x320E`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0..3`|raw 4 bytes|exact same RAM buffer written by controller RX command 0x62DC; therefore direct mirror/rebroadcast of RX62DC payload bytes0..3|`CONFIRMED_FIRMWARE`|
|`4..7`|raw 4 bytes|adjacent bytes of controller object 0x6017 at offsets +0x98..+0x9B; physical fields not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x320F`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0..7`|raw 8 bytes|dedicated controller TX buffer; observed all-zero in supplied captures; writers/physical semantics not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x3210`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0..3`|raw|controller object 0x6017 offsets +0x36..+0x39; physical fields not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|
|`4..5`|LE16|controller object 0x6017 offsets +0x3A..+0x3B; capture proves monotonic +1 progression about once per second during WA; physical counter meaning not proven|`STRUCTURE+CAPTURE_CONFIRMED_SEMANTIC_UNKNOWN`|
|`6..7`|raw|controller object 0x6017 offsets +0x3C..+0x3D; zero in supplied WA capture; physical semantic not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x3213`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0..3`|raw 4 bytes|exact same RAM buffer written by controller RX command 0x62E5; direct mirror/rebroadcast of RX62E5 payload. In supplied capture value happens to equal 320E[0..3], but they are separate RX sources.|`CONFIRMED_FIRMWARE+CAPTURE`|

#### `0x1200`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0`|u8 status/event code|op7 STATUS_EVENT class. HMI has dedicated state/fault logic for low command byte 00; exact physical status represented by command 1200 not proven|`CLASS_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x1201`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0`|u8 status/event code|new-controller cyclic op7 status byte; per-command physical semantic not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x1202`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0`|u8 status/event code|op7 STATUS_EVENT class. HMI low-byte 02 handler treats codes 0,3,6 as inactive/clear and other values as active events; exact physical event family not proven|`CLASS_CONFIRMED_SEMANTIC_PARTIAL`|

#### `0x1203`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0`|u8 status/event code|new-controller cyclic op7 status byte; observed 00; per-command physical semantic not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

#### `0x1204`
| Byte(s) | Encoding | Verified behavior | Status |
|---|---|---|---|
|`0`|u8 status/event code|new-controller cyclic op7 status byte; observed 00; per-command physical semantic not proven|`STRUCTURE_CONFIRMED_SEMANTIC_UNKNOWN`|

`0x6017` object linkage in the newer controller: base `0x200086B6`, next registered object `0x6018` at `0x200087B8`, so object span is `0x102` bytes. `3210` is window `+0x36`, `320E` is `+0x94`, `3213` is `+0xEA`. RX `62DC` writes exactly the first four bytes later broadcast by `320E`; RX `62E5` writes exactly the four-byte buffer broadcast by `3213`.

## 11. Old controller cyclic TX scheduler — exact firmware table
| cmd | DLC | period ms |
|---|---:|---:|
|`3000`|4|9900|
|`3200`|8|1980|
|`3201`|8|247|
|`3202`|1|99|
|`3203`|6|3960|
|`3204`|1|50|
|`3205`|2|3960|
|`3206`|4|99|
|`3207`|7|99|
|`3208`|2|197|
|`3209`|2|197|
|`320A`|1|99|
|`320B`|1|99|
|`6200`|3|99|
|`62DB`|6|99|
|`A0FA`|5|99|
|`A0FB`|8|99|
|`A0FC`|3|495|
|`A000`|8|10|
|`A010`|8|2|
|`1200`|1|495|

Key version delta: old has `320B` and only `1200`; newer lacks `320B` but adds `320E`, `320F`, `3210`, `3213` and `1201..1204`. `A000/A010` periods also differ (old 10/2 ms, new 12/1 ms).

## 12. New controller application RX registry
This table proves which incoming commands have application buffers in the newer supplied controller. Descriptor metadata bytes exist but their symbolic meaning is not assigned.
| cmd | DLC | RX RAM | Known semantic |
|---|---:|---|---|
|`6100`|3|`0x20003BDE`|physical semantic not proven|
|`6208`|8|`0x20003BED`|physical semantic not proven|
|`62D9`|2|`0x20003E21`|physical semantic not proven|
|`62DC`|4|`0x2000874A`|source mirrored by 320E|
|`62DE`|4|`0x20003CDA`|physical semantic not proven|
|`62DF`|1|`0x20003D2A`|physical semantic not proven|
|`62E1`|8|`0x20003BF7`|physical semantic not proven|
|`62E5`|4|`0x200087A0`|source mirrored by 3213|
|`6300`|4|`0x20003BFF`|HMI Assist/state|
|`6303`|1|`0x20003C10`|Auto-Off|
|`6305`|1|`0x20003C11`|physical semantic not proven|
|`6306`|4|`0x20003C12`|physical semantic not proven|
|`6307`|1|`0x20003C16`|physical semantic not proven|
|`630F`|2|`0x20003C17`|physical semantic not proven|
|`6900`|4|`0x20003BFF`|physical semantic not proven|
|`7F00`|2|`0x20003C27`|physical semantic not proven|
|`7F01`|2|`0x20003C29`|physical semantic not proven|
|`7F02`|1|`0x20003C43`|physical semantic not proven|
|`7F03`|1|`0x20003C44`|physical semantic not proven|
|`7F04`|8|`0x20003C45`|physical semantic not proven|
|`F000`|1|`0x20003C41`|physical semantic not proven|
|`F001`|1|`0x20003C42`|physical semantic not proven|
|`F002`|1|`0x20003C4D`|physical semantic not proven|

The absence of `6301`, `6302`, `6304` from this RX registry while `6300` and `6303` are present explains why the supplied capture shows controller ACKs for 6300/6303 but not those statistics/settings frames.

## 13. `op=7` / `0x120x` status-event class
Stock HMI does not process op7 as normal payload data. It has dedicated status/fault/event paths. For source nodes 1/2/4, low command byte `00` treats payload values below4 or equal6 as inactive/clear-like states and other codes as active stored events. Low command byte `02` treats `0,3,6` as inactive/clear and other values as active. This proves the semantic **class**, but not the manufacturer symbolic name or the physical meaning of each `1200..1204` family.

## 14. Confirmed capture examples
- Auto-Off controlled capture: `6303: FF → 0A → FF`, matching OFF → 10 min → OFF.
- Walk Assist capture: `6300[1]` changes into `06` for the WA interval and returns to `00`; `3210[4..5]` runs `47F8,47F9,...47FF,4800,4801...` as a LE16 counter-like field.
- Ride stats capture: `6301 37 00 00 2A 02 00 A8 01` = ODO 55 km, Trip 55.4 km, MAX 42.4 km/h; `6302 71 00 2A 02 00` = AVG 11.3 km/h, Service distance 55.4 km. `3302/3303` are byte-identical broadcast copies by construction.

## 15. Things deliberately still not named

- physical role of node1, node4 and node19.
- exact manufacturer symbolic name for operation 7 (semantic class is status/fault/event).
- physical names of 3200 byte0, 3200 bytes4..5/derived quantity, 3201 factors A/B individually, 3201 bytes6/7, 3202, 3203, 3204.
- payload semantics of controller scheduler-only 3206..320A/old320B, 6200, 62DB and A0xx frames.
- physical meaning of controller 3000 counter.
- physical semantics of 320E bytes4..7, 320F, 3210 fields and 3213/62E5 source object despite proven memory linkage.
- exact physical status family represented by each 1200..1204 command.
- mapping of Battery Info second-page Cycle Times/M.N.T/L.N.T/Cell fields to CAN commands; 6400/6401 remain unassigned.
- exact semantic name of HMI WRITE6202 and WRITE6500.
- exact physical key/event names for every 6300 byte2 bit and special high-bit state.

These are not omissions: they are explicit boundaries of what the supplied stock evidence can prove. The JSON keeps the same distinction per field.

## 16. Implementation invariants for another application
1. Use 29-bit wire ID; strip logger bit31.
2. Configure **250 kbit/s Classic CAN** and extended IDs.
3. Decode/compose command/op/target/source with the exact bit layout above.
4. Long responses require ACK of `LONG_START` using the **original command** before accepting DATA.
5. Reassemble op5/op6 by chunk index in low 16 bits.
6. Do not interpret undefined tail bytes of READ `630B`.
7. Preserve C2/C3 ODO rounding difference if emulating stock behavior exactly.
8. Do not turn structural UNKNOWN fields into physical sensor names without new stock evidence.
## 17. Reproducibility anchors in the stock binaries
These runtime addresses are included so the reverse engineering can be repeated independently. They are **addresses in the mapped Cortex-M image**, not file offsets.

| Firmware | Anchor | Runtime address / relation |
|---|---|---|
| DPC245 C3.0 | periodic HMI CAN payload builder | `0x08032420` |
| DPC245 C3.0 | HMI CAN write/config handler family | `0x0803277C` area |
| DPC245 C3.0 | HMI READ responder (`6301/6302/630B`) | `0x08032964` area |
| DPC245 C3.0 | broadcast parser (`32xx/34xx`) | `0x08035064` |
| DPC245 C3.0 | op7/status-event receive path | `0x080353A8` area |
| DPC245 C2.0 | homologous periodic HMI builder | `0x0803E124` |
| New controller | object `6017` base | RAM `0x200086B6` |
| New controller | object `6018` base | RAM `0x200087B8`; proves `6017` span `0x102` |
| New controller | `3210` TX data | RAM `0x200086EC = 6017+0x36` |
| New controller | `320E` TX / `62DC` RX shared start | RAM `0x2000874A = 6017+0x94` |
| New controller | `3213` TX / `62E5` RX shared buffer | RAM `0x200087A0 = 6017+0xEA` |

For the HMI BINs, file offset `0x20` maps to runtime `0x08020000`; for the controller BINs, file offset `0x20` maps to runtime `0x08004000`. This mapping must be applied before comparing disassembly addresses.

