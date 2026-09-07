# Regression run results

Generated: 2026-08-21 11:20:21

## Build

All harnesses built and linked with -Wall -Wextra -Werror (host gcc), with the
documented -Wno-type-limits exception for torque_input.c and assist_modes.c only.

## Determinism smoke-test (RUN_100, power_pipeline, two independent runs)

Result: **PASS - identical**

## Metrics summary

See metrics_summary.csv in this directory for the full table (all scenarios x
layers x columns). No PASS/FAIL thresholds are declared yet (card section 12) -
these are baseline measurements of the CURRENT firmware behaviour.

## MISSED_TICK_BURST

See missed_tick_burst_summary.csv and the console output above for the three
category demonstrations (A elapsed-time / B control-update / C lost sample).

