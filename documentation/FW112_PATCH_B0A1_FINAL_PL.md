# FW-112 PATCH B0a.1 - FINAL CLOSEOUT

**Data**: 22.08.2026
**Branch**: diag/fw112-real-bike-ab
**HEAD**: ba794be + local changes
**EBICS_BUILD_VERSION NORMAL**: 0.0406
**EBICS_BUILD_VERSION DIAG**: 0.0407
**MCU**: GD32F303RCT6, 120 MHz
**BOOTLOADER**: 820 (ORIGIN = 0x08005000)

---

## 1. Problem historyczny

FW-112 wymagal uprawnienia do przeplywu assistu. Stary kod mial kilka nakladajacych sie mechanizmow:
fwd_run >= start_steps, rearm_permission_active, session_out.latched, WAIT_FRESH_LOAD, TRACK_FAST.
Nakladajace sie ownerzy powodowaly nierepetowalne progi startowe i martwe strefy.

## 2. Cel B0a.1

Uproszczenie architektury uprawnien do jednego ownera (gate = sole permission owner).

## 3. Authorization graph

PERMISSION (gate = SOLE owner): forward_valid AND load_met -> SET; stop_or_reverse -> RESET
SESSION: lifecycle only (cold_arm, fast_rearm, direction_inhibit, real_stop)
MODE: always runs on valid sensor data, no permission gate
OUTPUT: iq_target -> hard_cut -> dynamics -> ramp -> MS.i_q_setpoint

## 4. Gate SET / KEEP / RESET

SET: forward_valid (crank_direction_ok) AND load_met (torque >= threshold or threshold == 0)
KEEP: once open, stays open regardless of forward_valid/load_met drops
RESET: real_stop OR direction_inhibit OR !session_out.latched

## 5. Brake behavior

Brake does NOT reset the gate. hard_cut zeros iq_target same-tick. Gate stays OPEN.

## 6. Test results

S1-S10: ALL PASS | M1-M9: ALL PASS | G1-G4: ALL PASS | C1-C2: ALL PASS
Full regression: 30+ suites, 0 failures

## 7. Production diff

NEW: src/pedal_assist_gate.c, inc/pedal_assist_gate.h
MODIFIED: src/ride_control.c (160 lines), src/assist_modes.c (20 lines)
No unrelated functional changes.

## 8. Builds

NORMAL 0.0406: 100016 bytes, text=99712 data=268 bss=11772, flash=42%, ram=24%
DIAG 0.0407: 141912 bytes, text=141608 data=268 bss=45332, flash=60%, ram=93%

SHA256 NORMAL: 1DFE32EB78A74EC3F4C3DDBCA7958D8310F4602187362788D7096748431FE6CD
SHA256 DIAG: 6D9C1950246F22BD5D48F1C61130775F0A68E2B11E352B936CA7540CB5105C56

## 9. Reproducibility

Both variants byte-identical on independent rebuild from frozen source.

## 10. Permission cleanup proof

rearm_permission_active: 0 assignments, 0 declarations (only comments)
pedaling_active in prepare_assist_input: REMOVED from code
session_out.latched as latched: REPLACED by gate_open
No remaining path: gate OPEN + iq>0 + forward + brake clear + fault clear -> output=0 from stale permission.

## 11. Hardware test procedure

eMTB mode, one fixed level:
A: stop -> forward -> press -> verify START_LOAD opens assist repeatably
B: gate OPEN -> torque dip -> torque returns -> no re-press needed (dead spot)
C: gate OPEN -> low torque for several rotations -> gate stays OPEN
D: real_stop -> gate CLOSED -> next start requires START_LOAD
E: brake during pedaling -> iq=0 -> brake release -> gate still OPEN
F: reverse -> assist 0, gate reset -> forward works per gate rules

Subjective: the inconsistent start threshold behavior should be gone.

## 12. Remaining open cards

B0b: sensor deadband vs rider effort magnitude
B1: per-level MIN_IQ floor
E: output dynamics / peak fall / pedaling pulsation
D: mode curves / higher mean assist / light rider full power

## 13. Verdict

**READY FOR HARDWARE**
