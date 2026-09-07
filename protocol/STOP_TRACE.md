# STOP-TRACE schema 1 (2026-09-06)

Passive, explicitly armed NORMAL-build instrumentation. No PI, reference,
angle, PWM, or calibration writes. Full session DIAGNOSTIC builds expose a
disabled status and reject ARM/DUMP because their RAM is already allocated.

Timing extension (backward compatible schema 1): metadata config bit 2 means
the ADC handler body was timed with DWT; bit 3 means that body reached/exceeded
the 62.5 us FOC period. Metadata bytes 34..35 give the maximum body duration in
whole microseconds rounded up (65535 saturates). Without config bit 2 those
bytes remain reserved and timing is unknown, not zero. Status flags bit 5
also reports the over-budget condition. This excludes exception entry/exit
and the final timing-report call; it is not a proof of whole-system WCET.

## Commands (extended CAN, source 5, target 2)

| Request | DLC | Meaning |
|---|---|---|
| WRITE 0x6033 / EFID 0x05106033 | 0 | ARM; replaces previous capture, unless dump busy |
| READ 0x6033 / EFID 0x05116033 | 0 | side-effect-free status |
| WRITE 0x6034 / EFID 0x05106034 | 0 | replay immutable frozen capture |

WRITE uses the existing NORMAL_ACK/ERROR_ACK with DLC 0. READ returns EFID
0x022A6033, DLC 8: schema, state, generation, flags, overview count LE16,
finish reason, complete fast-window mask. State and flags are defined in
`inc/stop_trace.h`; flags bit 2 is feature-present, bit 4 frozen, bit 3 ARM
pending, bit 1 TX failed, bit 0 TX active. Counts are only published when frozen.

## Capture

ADC ISR owns all capture metadata and arrays. Foreground sets a one-byte ARM
request. The ISR clears only small metadata, never bulk-clears the arrays.
Frozen publication uses a barrier; foreground never exports mutable captures.
The generation increments on ARM consumption, modulo 256. Tick is an independent
ADC interrupt counter (16000 Hz), including bridge-off operation.

Overview: 384 slots, up to 16 pre-trigger records, 320 ISR ticks between records
plus explicit QZERO events. Each interval retains Iq minimum/maximum, absolute
Id peak, maximum modular theta step, event OR, and any invalid-sample indication.
No speed gate excludes measurements. Stop: Iq reference zero, QZERO inactive,
Hall age >=500 ms and >=500 ms elapsed after trigger. Timeout: 7 s after trigger
or 30 s waiting for trigger. Capacity exhaustion is reported separately.

Fast streams: three 96-slot rings, up to 32 pre-trigger records and 64 records
starting with trigger (6 ms total). Events: reference nonzero-to-zero or QUIET
rise at zero; QZERO handback/abort; first >=5 degree modular angle jump with
Hall age >=20 ms or ERPS <=10. Untriggered fast streams have count zero.
Partial fast windows at global capture completion are exported with actual
counts, not padded or silently claimed complete.

## Replay

At most one tracked queue frame every 20 control ticks (5 ms). It waits behind
critical traffic and pauses new sends during movement/demand. Each frame is
advanced only after its queue token reports DONE. Failed/unknown outcomes end
the replay with an explicit status flag; the capture remains available.

Data EFIDs 0x00010300..0x00010307 identify fragment 0..7. Every payload is:
generation U8, record index LE16, five bytes of record payload. All records are
40 bytes and explicitly serialized little-endian, independent of C padding.

Record indices: overview 0..383; release 384..479; handback 480..575;
angle 576..671. Only actual counts are sent. Metadata record 0xFFFF precedes
all samples; trailer record 0xFFFE follows them. Trailer begins ASCII `DONE`,
then LE32 CRC-32/ISO-HDLC of metadata followed by each sample record in ascending
record-index order (initial/final xor FFFFFFFF, reflected polynomial EDB88320).
Generation/index/fragment identify duplicates without relying on log timestamps.
Replays are validated separately; a newer incomplete replay is never silently
replaced by an older complete one or repaired from stale fragments.

## Metadata record

| Offset | Type | Meaning |
|---|---|---|
| 0,1,2,3 | U8 each | schema, generation, config bits (QZERO=1, canonical angle=2), finish reason |
| 4 | U32 | tick frequency 16000 |
| 8,10 | U16 each | overview interval 320, record size 40 |
| 12 | U16[4] | stream counts |
| 20 | U32 | overall trigger tick |
| 24 | U16[4] | chronological trigger indices, FFFF if absent |
| 32 | U16 | CAL_I, mA per current count |
| 34 | U16 | max ADC handler body us if config bit 2; otherwise reserved zero |
| 36 | U32 | final tick |

## Sample record

| Offset | Type | Meaning |
|---|---|---|
| 0 | U32 | ADC tick |
| 4,6,8 | I16 | signed physical-axis Iq reference, filtered Iq, filtered Id |
| 10,12 | I16 | commanded Uq/Ud after vector limiting, controller units |
| 14,16 | I16 | PI q/d integral, truncated controller units |
| 18 | U16 | electrical angle (65536 = one turn) |
| 20 | U16 | Hall age in 4 kHz ticks |
| 22 | U16 | raw Hall timer, 500 kHz; wraps at 65536 |
| 24 | U16 | filtered/clamped ERPS |
| 26 | U16 | battery voltage, 10 mV units |
| 28,30 | I16 | interval Iq min/max; instantaneous in fast streams |
| 32,34 | U16 | interval absolute Id peak, maximum modular angle step |
| 36 | U16 | flags from stop_trace.h |
| 38,39 | U8 | Hall state, QZERO state |

CAL_I is the firmware's nominal conversion, not an independently verified
amperes-per-LSB calibration (see FW-128C0). Decoder amp columns are labelled
`*_A_nominal`; signed raw current counts remain the primary evidence.

VALID means FOC ran with fresh ADC completion and a non-INVALID sampling context.
It does not independently prove analog calibration. FOC_RAN distinguishes a
last-valid replacement from a skipped calculation. BAD_SAMPLE in overview is
OR'ed over the interval, so a VALID endpoint does not certify interval peaks.
Bridge-off current/voltage may be retained control values, explicitly flagged;
they must not be interpreted as fresh physical current/voltage measurements.
Uq/Ud describe the command computed from this tick's current, not a simultaneous
oscilloscope measurement. Audio and raw phase-current impulses are not captured.
