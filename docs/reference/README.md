# Reference material

These files preserve the most important reverse-engineering/design context so a new agent does not need access to older chats.

They are references, not automatically-current implementation specifications. In case of conflict, use this precedence:

1. current production source;
2. `AGENTS.md` and `docs/ARCHITECTURE_CURRENT.md`;
3. current tests/SIL contracts;
4. these historical/reference documents.

Key files:

- `G532_CICHY_START_STOP_EVISTDRIVE_AGENT.md` — G532 START/STOP/ARMED_ZERO/Q-slew reference.
- `EVistDrive_G532_FakeTaxi_ROTOR_ANGLE_IMPLEMENTATION_GUIDE_20260903.md` — Hall/theta ownership and Fake Taxi/VESC-derived guidance.
- `EVISTDRIVE_TSDZ2_ASSIST_DYNAMICS_IMPLEMENTATION_GUIDE_PL.md` — rider-assist dynamics/TSDZ2 comparison.
- `EVistDrive_G532_Walk_Assist_reverse_i_plan_wdrozenia.md` — Walk Assist reverse/design context.
- `G532_ACCEL_REVERSE_ANALYSIS_PL.md` — G532 supervisory ACCEL behavior.
- `M820_STOCK_REVERSE_KNOWLEDGE_BASE_EVISTDRIVE.md` — stock M820 current-control/protection knowledge.
- `EVistDrive_ARCHITECTURE_AUDIT_20260906_PL.md` — architecture debt and ownership audit before FW139-FW142 cleanup.
