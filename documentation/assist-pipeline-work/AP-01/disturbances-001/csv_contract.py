"""Column contract for the Controller Lab CSV used by disturbances-001.

REVIEW-EVD-AP-01-010 / D010-01: the previous attempt emitted a 55-name header and 54 values per
row. Everything from `crank_direction` onwards was read from the WRONG field and the last column
was empty, yet a header-length test passed. So the contract here is by NAME and by MEANING:

  * the header must be exactly the accepted 30 columns, then the appended ones, in order;
  * EVERY row must have exactly as many fields as the header;
  * every field must parse as its declared type;
  * a set of cross-field invariants must hold on every row, which is what actually catches a
    one-field shift (a shifted `ride_interval` breaks `pedalling == (ride_interval != 0)`).

Nothing here re-implements production maths; these are structural facts about the emitted file.
"""
from __future__ import annotations

# The 30 columns of the ACCEPTED AP-01 / transients-001 series, in order. AP-01 CSVs stop after
# column 28 (`pedalling`); transients-001 CSVs carry all 30.
ACCEPTED_28 = [
    "time_s", "crank_angle_deg", "torque_gen_nm", "torque_load_kg", "torque_fast_native",
    "torque_run_native", "cadence_gen_rpm", "cadence_raw_rpm", "cadence_control_rpm", "speed_kph",
    "battery_v", "soc_pct", "human_power_w", "assist_basis_power_w", "motor_power_w", "support_pct",
    "iq_mode_request", "iq_before_profile_limit", "iq_requested", "iq_allowed", "iq_pre_ramp",
    "iq_ref", "session", "debug_flags", "battery_limit", "gate_steps", "gate_load_ckg", "pedalling",
]
ACCEPTED_30 = ACCEPTED_28 + ["torque_cmd_mean_nm", "ride_interval"]

# Columns this task appends. They are OBSERVATIONS of the generator and of production state; no
# accepted column is moved, renamed or reordered.
DISTURBANCE_COLUMNS = [
    "crank_direction", "pas_ab", "pas_normal_ab", "pas_transition_index", "pas_last_step",
    "pas_direction_state", "pas_inhibit_reason", "pas_fwd_run", "pas_rev_run",
    "pas_backpedal_confirmed", "pas_sampler_forward", "pas_sampler_reverse", "pas_sampler_invalid",
    "pas_sampler_glitch", "pas_sampler_overflow", "torque_sensor_valid", "pas_sensor_valid",
    "rider_torque_sensor_valid", "rider_pas_sensor_valid", "elapsed_ticks", "fg_processed",
    "fg_delay_active", "reverse_bounce_active", "pas_invalid_seq_active", "fast_iq_slew_ticks",
    "fg_skips_total", "fg_resume_count", "fg_last_resume_tick", "fg_last_resume_elapsed",
    "pas_edge_drop_active", "pas_edge_jitter_active", "pas_edges_dropped", "pas_edges_deferred",
    "reverse_bounce_ticks",
]

EXPECTED_HEADER = ACCEPTED_30 + DISTURBANCE_COLUMNS

FLOAT_COLUMNS = {
    "time_s", "crank_angle_deg", "torque_gen_nm", "torque_load_kg", "cadence_gen_rpm",
    "speed_kph", "battery_v", "soc_pct", "torque_cmd_mean_nm",
}
# Everything else is an integer field.
BOOL_COLUMNS = {
    "pedalling", "battery_limit", "pas_backpedal_confirmed", "torque_sensor_valid",
    "pas_sensor_valid", "rider_torque_sensor_valid", "rider_pas_sensor_valid", "fg_processed",
    "fg_delay_active", "reverse_bounce_active", "pas_invalid_seq_active", "pas_edge_drop_active",
    "pas_edge_jitter_active",
}
# name -> allowed set of integer values
ENUM_COLUMNS = {
    "ride_interval": {0, 1, 2},
    "crank_direction": {-1, 0, 1},
    "pas_ab": {0, 1, 2, 3},
    "pas_normal_ab": {0, 1, 2, 3},
}
MONOTONIC_COLUMNS = [
    "fast_iq_slew_ticks", "fg_skips_total", "fg_resume_count", "pas_edges_dropped",
    "pas_edges_deferred", "reverse_bounce_ticks", "pas_sampler_forward", "pas_sampler_reverse", "pas_sampler_invalid",
    "pas_sampler_glitch", "pas_sampler_overflow",
]


class CsvContractError(AssertionError):
    pass


def check_header(header: list[str]) -> None:
    if header != EXPECTED_HEADER:
        extra = [c for c in header if c not in EXPECTED_HEADER]
        missing = [c for c in EXPECTED_HEADER if c not in header]
        raise CsvContractError(
            f"header does not match the contract: {len(header)} names vs "
            f"{len(EXPECTED_HEADER)} expected; missing={missing} unexpected={extra}")
    if len(set(header)) != len(header):
        raise CsvContractError("header contains duplicate column names")


def check_rows(header: list[str], rows: list[list[str]]) -> dict:
    """Validate EVERY row: width, type and cross-field meaning. Returns a small summary."""
    if not rows:
        raise CsvContractError("CSV has a header but no data rows")
    idx = {name: i for i, name in enumerate(header)}
    n = len(header)
    prev = {c: None for c in MONOTONIC_COLUMNS}

    for r, row in enumerate(rows, start=2):   # line 1 is the header
        if len(row) != n:
            raise CsvContractError(
                f"line {r}: {len(row)} fields, header declares {n}. A row that is short by one "
                f"field silently shifts every later column.")
        vals = {}
        for name, i in idx.items():
            raw = row[i]
            if raw == "":
                raise CsvContractError(f"line {r}: column '{name}' is empty")
            try:
                vals[name] = float(raw) if name in FLOAT_COLUMNS else int(raw)
            except ValueError as e:
                raise CsvContractError(
                    f"line {r}: column '{name}' = {raw!r} is not a valid "
                    f"{'float' if name in FLOAT_COLUMNS else 'int'}: {e}") from None

        for name in BOOL_COLUMNS:
            if vals[name] not in (0, 1):
                raise CsvContractError(f"line {r}: '{name}' = {vals[name]}, expected 0 or 1")
        for name, allowed in ENUM_COLUMNS.items():
            if vals[name] not in allowed:
                raise CsvContractError(
                    f"line {r}: '{name}' = {vals[name]}, expected one of {sorted(allowed)}")

        # --- cross-field meaning: these are what a one-column shift breaks ---
        if vals["pedalling"] != (1 if vals["ride_interval"] != 0 else 0):
            raise CsvContractError(
                f"line {r}: pedalling={vals['pedalling']} but ride_interval={vals['ride_interval']}; "
                f"pedalling must be exactly (ride_interval != 0)")
        if vals["ride_interval"] == 0 and vals["crank_direction"] != 0:
            raise CsvContractError(
                f"line {r}: crank_direction={vals['crank_direction']} outside any ride interval")
        if vals["fg_processed"] == 0 and vals["elapsed_ticks"] != 0:
            raise CsvContractError(
                f"line {r}: fg_processed=0 but elapsed_ticks={vals['elapsed_ticks']}")
        if vals["fg_processed"] == 1 and vals["elapsed_ticks"] < 1:
            raise CsvContractError(
                f"line {r}: fg_processed=1 but elapsed_ticks={vals['elapsed_ticks']} (< 1)")
        if vals["fg_resume_count"] > vals["fg_skips_total"]:
            raise CsvContractError(
                f"line {r}: fg_resume_count={vals['fg_resume_count']} exceeds "
                f"fg_skips_total={vals['fg_skips_total']}")
        if vals["fg_resume_count"] == 0 and (vals["fg_last_resume_tick"] != 0
                                             or vals["fg_last_resume_elapsed"] != 0):
            raise CsvContractError(f"line {r}: resume latch set before any resume happened")
        if vals["pas_edges_dropped"] and not vals["pas_edge_drop_active"] and r == 2:
            raise CsvContractError(f"line {r}: dropped edges counted before the window opened")
        # A bounce is ONE event of exactly three ticks; a re-arming bounce was a real defect
        # (REVIEW-EVD-AP-01-011, D011-01), so the contract pins the ceiling.
        if vals["reverse_bounce_ticks"] > 3:
            raise CsvContractError(
                f"line {r}: reverse_bounce_ticks={vals['reverse_bounce_ticks']} - a single bounce "
                f"forces exactly 3 ticks, so a higher count means it re-armed")
        if vals["reverse_bounce_active"] and vals["reverse_bounce_ticks"] == 0:
            raise CsvContractError(
                f"line {r}: reverse_bounce_active=1 but no forced tick has been counted")

        for c in MONOTONIC_COLUMNS:
            if prev[c] is not None and vals[c] < prev[c]:
                raise CsvContractError(
                    f"line {r}: '{c}' went backwards ({prev[c]} -> {vals[c]}); it is cumulative")
            prev[c] = vals[c]

    return dict(rows=len(rows), columns=n,
                accepted_columns=len(ACCEPTED_30), appended_columns=len(DISTURBANCE_COLUMNS))


def check_text(text: str) -> dict:
    lines = text.strip("\n").split("\n")
    header = lines[0].rstrip("\r").split(",")
    check_header(header)
    rows = [ln.rstrip("\r").split(",") for ln in lines[1:]]
    return check_rows(header, rows)
