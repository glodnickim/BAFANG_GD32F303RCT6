# Registered real-bike regressions

Each case lives in `sim/replay/cases/<case>/` with a canonical `input.csv` and `manifest.json`.
A case without `accepted_output_sha256` is replayed for invariants/determinism but does not freeze
the old output. After a fix is reviewed, an accepted output hash may be stored so future firmware
changes cannot silently alter that ride.

Do **not** accept a baseline from a known-bug firmware merely to make the gate green.
