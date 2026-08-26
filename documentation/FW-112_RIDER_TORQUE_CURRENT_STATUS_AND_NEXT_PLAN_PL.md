# FW-112 RIDER TORQUE CURRENT STATUS

**Last updated**: 22.08.2026
**Branch**: diag/fw112-real-bike-ab

---

## PATCH B0a.1

**STATUS**: IMPLEMENTED / HOST VERIFIED / READY FOR HARDWARE

### Key findings
- gate = sole permission owner (pedal_assist_gate module)
- calculation always running (no pedaling_active gate in prepare_assist_input)
- session = lifecycle only (no permission role)
- recovery = filter only (signal processing, no permission)
- start threshold not rechecked after OPEN (gate KEEP semantics)
- low torque / dead spot does NOT close gate

### What was removed
- rearm_permission_active (declaration + all assignments)
- fwd_run >= start_steps from prepare_assist_input
- session_out.latched as permission variable
- open_now from gate API

### Test results
- B0 gate S1-S10: ALL PASS
- M1-M9 mutations: ALL PASS
- G1-G4 gate API: ALL PASS
- C1-C2 calculation independence: ALL PASS
- Full regression: 30+ suites, 0 failures

### Builds
- NORMAL 0.0406: 100KB, flash=42%, ram=24%
- DIAG 0.0407: 142KB, flash=60%, ram=93%
- Both byte-identical on rebuild

### NEXT STEP
hardware test B0a.1 on real bike

---

## Remaining open cards

### B0b — sensor deadband vs rider effort magnitude
After hardware PASS on B0a.1.

### B1 — per-level MIN_IQ floor
Only if needed based on hardware observation.

### E — output dynamics / peak fall / pedaling pulsation
Deferred. Needs hardware data first.

### D — mode curves / higher mean assist / light rider full power
Deferred. Needs hardware data first.

### FOC
Untouched unless proven necessary by hardware tests.
