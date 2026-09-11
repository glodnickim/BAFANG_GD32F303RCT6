"""AP-01 transients-001: measurement adapter for records that contain SEVERAL events.

Why a new module instead of extending extract_metrics.py: the accepted extractor assumes one
ride_start_s/ride_stop_s pair per record and derives a single steady window from it. A record with
a torque step, a stop, a restart and possibly a second stop needs windows that are bounded by the
NEXT event, and a stop assessment that cannot borrow evidence from a later stop. Both accepted
files stay untouched (READ_ONLY for this task); the primitives in ../metrics.py are IMPORTED, never
copied, so no statistic is redefined here.

Measurement contract of this module (parameters of the MEASUREMENT, not firmware constants):

- A window never crosses the next event. `event_windows()` clips every window against the
  neighbouring event times and reports the clipping, so a "steady" mean is never taken across a
  stop, a restart or a step.
- A stop is assessed ONLY on the samples between that stop and the next event. A second stop later
  in the record can therefore never confirm the first one. When the gap is too short to observe a
  hold, the status is INSUFFICIENT_OBSERVATION - that is the answer, not a defect to work around.
- An absent threshold crossing is reported as "not measured", never as 0.
- 10-90% latency is reported against EXPLICIT reference levels taken from the pre and post windows.
  On a pulsating channel a single crossing of a threshold is not the same thing as the underlying
  mean crossing it; `ripple_spans_thresholds` marks exactly that case instead of hiding it.
"""
from __future__ import annotations

import csv
import math
import sys
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import metrics  # noqa: E402  - accepted AP-01 primitives, imported not copied

# Measurement-contract defaults, aligned with the accepted AP-01 stop contract so the two are
# comparable (see ../comparison_criteria_r3.json S1 and REVIEW-EVD-AP-01-004).
DEFAULT_ZERO_TOLERANCE = 1.0
DEFAULT_MIN_CONFIRM_S = 0.10
DEFAULT_MAX_TRANSITION_GAP_S = 0.05

# Names decoded from the C sources, not guessed (REVIEW-EVD-AP-01-005, T005-01).
# inc/ride_session.h - session is a small enum, not a boolean stop/release flag.
SESSION_NAMES = {
    0: "RIDE_SESSION_COLD",
    1: "RIDE_SESSION_ACTIVE",
    2: "RIDE_SESSION_SUSPENDED_BY_DIRECTION",
    3: "RIDE_SESSION_WAIT_REARM_LOAD",
}

# inc/ride_control.h - debug_flags is a BITMASK. `16 in flags` (list membership on the raw int) is
# wrong: it silently misses any combination such as 144 = 128 + 16. Bit 0x10 means "ride latch not
# armed -> assist target forced 0", not a dedicated "stop declared" flag.
DEBUG_FLAG_BITS = [
    (0x01, "RIDE_DBG_WALK"),
    (0x02, "RIDE_DBG_CALIBRATION"),
    (0x04, "RIDE_DBG_HARD_CUT"),
    (0x08, "RIDE_DBG_LEVEL_ZERO"),
    (0x10, "RIDE_DBG_NOT_LATCHED"),
    (0x20, "RIDE_DBG_MODE_UNSUPPORTED"),
    (0x40, "RIDE_DBG_LIMITER_ZEROED"),
    (0x80, "RIDE_DBG_COAST_RELEASE"),
]


def decode_session(v: int) -> str:
    return SESSION_NAMES.get(int(v), f"UNKNOWN_SESSION_{int(v)}")


def decode_debug_flags(v: int) -> list[str]:
    """Bitwise decode (not `in`/list-membership - that misses any combined value)."""
    iv = int(v)
    names = [name for bit, name in DEBUG_FLAG_BITS if iv & bit]
    unknown = iv & ~sum(bit for bit, _ in DEBUG_FLAG_BITS)
    if unknown:
        names.append(f"UNKNOWN_BITS_0x{unknown:02x}")
    return names or (["RIDE_DBG_NONE_SET"] if iv == 0 else [f"UNKNOWN_0x{iv:02x}"])


def read_csv(path: Path) -> dict[str, list[float]]:
    """Controller Lab CSV -> {column: [float, ...]}. Columns are kept exactly as exported."""
    with Path(path).open(encoding="utf-8", newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        raise ValueError(f"{path}: no data rows")
    cols = {k: [] for k in rows[0]}
    for r in rows:
        for k, v in r.items():
            cols[k].append(float(v))
    return cols


# --- windows --------------------------------------------------------------------------------

def event_windows(events: Sequence[tuple[str, float]], t_end: float,
                  pre_s: float, settle_s: float, post_s: float) -> dict:
    """Build a pre and a post window around every event, clipped by the neighbouring events.

    For event i at time te:
      pre  = [max(te - pre_s, previous_event_time), te)
      post = [te + settle_s, min(te + settle_s + post_s, next_event_time))

    `settle_s` is the transient allowance: the post window starts only after it, so a "steady after"
    figure is not contaminated by the transition it is supposed to follow. Every window carries
    `clipped_by` naming the neighbouring event that shortened it, and `full_length` saying whether
    the requested length survived. A caller must not read a shortened window as an equally strong
    observation - that is the whole point of reporting it.
    """
    if pre_s <= 0 or post_s <= 0 or settle_s < 0:
        raise ValueError("pre_s/post_s must be > 0 and settle_s >= 0")
    ev = sorted(events, key=lambda e: e[1])
    for i in range(1, len(ev)):
        if ev[i][1] <= ev[i - 1][1]:
            raise ValueError(f"event times must be strictly increasing: {ev}")
    out = {}
    for i, (name, te) in enumerate(ev):
        prev_t = ev[i - 1][1] if i > 0 else 0.0
        next_t = ev[i + 1][1] if i + 1 < len(ev) else t_end
        pre_lo = max(te - pre_s, prev_t)
        post_lo = te + settle_s
        post_hi = min(post_lo + post_s, next_t)
        out[name] = dict(
            event_time_s=te,
            pre=[pre_lo, te],
            post=[post_lo, post_hi] if post_hi > post_lo else None,
            pre_clipped_by=(ev[i - 1][0] if pre_lo > te - pre_s else None),
            post_clipped_by=(ev[i + 1][0] if i + 1 < len(ev) and post_hi < post_lo + post_s else
                             ("record_end" if post_hi < post_lo + post_s else None)),
            pre_full_length=(pre_lo <= te - pre_s + 1e-12),
            post_full_length=(post_hi is not None and post_hi >= post_lo + post_s - 1e-12),
            settle_s=settle_s,
        )
    return out


def window_stats(times, values, bounds) -> dict | None:
    """mean / p-p / ripple RMS / relative ripple over one window, or None if the window is empty."""
    if bounds is None:
        return None
    ts, vs = metrics.window(times, values, bounds[0], bounds[1])
    if not vs:
        return None
    return dict(
        window_s=[bounds[0], bounds[1]], samples=len(vs), t_first_s=ts[0], t_last_s=ts[-1],
        mean=metrics.mean(vs), peak_to_peak=metrics.peak_to_peak(vs),
        ripple_rms=metrics.rms_about_mean(vs),
        relative_ripple=metrics.relative_ripple(vs),
        min=min(vs), max=max(vs),
    )


# --- step / release latency -----------------------------------------------------------------

def step_response(times, values, event_time: float, pre_bounds, post_bounds,
                  search_until: float | None = None) -> dict:
    """10-90% latency of one step, against reference levels measured in the two windows.

    The reference levels are NOT assumed: `start_level` is the mean of the pre window and
    `end_level` the mean of the post (settled) window, both reported in the result so a reader can
    check what the percentages were taken of.

    The search segment starts at `event_time` (so a crossing that happened before the event cannot
    be picked up) and ends at `search_until` (the next event) so a later event cannot supply the
    crossing.

    `ripple_spans_thresholds` is the honest limitation on a pulsating channel: when the pre or post
    window's own peak-to-peak already reaches across the 10% or 90% threshold, an individual sample
    crossing that threshold is a crossing of the RIPPLE, not of the underlying mean. The latency is
    still reported - suppressing it would hide data - but it must be read as the first excursion
    past the level, not as the settling time of the mean.
    """
    pre = window_stats(times, values, pre_bounds)
    post = window_stats(times, values, post_bounds)
    base = dict(pre_window=pre, post_window=post)
    if pre is None or post is None:
        return dict(base, status="NO_REFERENCE_WINDOW", rise_time_s=None,
                    note="a reference level is missing; no latency is measured (not zero)")

    start_level, end_level = pre["mean"], post["mean"]
    hi = search_until if search_until is not None else times[-1] + 1.0
    ts, vs = metrics.window(times, values, event_time, hi)
    if len(ts) < 2:
        return dict(base, status="INSUFFICIENT_DATA", rise_time_s=None,
                    start_level=start_level, end_level=end_level)

    detail = metrics.rise_time_10_90_detail(ts, vs, start_level, end_level)
    span = end_level - start_level
    lo_thr = start_level + 0.10 * span
    hi_thr = start_level + 0.90 * span
    spans = False
    for w in (pre, post):
        if w["min"] <= lo_thr <= w["max"] or w["min"] <= hi_thr <= w["max"]:
            spans = True
    return dict(base, start_level=start_level, end_level=end_level,
                step_magnitude=span, lo_threshold=lo_thr, hi_threshold=hi_thr,
                search_window_s=[event_time, ts[-1]],
                ripple_spans_thresholds=spans,
                ripple_note=("pre/post ripple reaches across a 10-90% threshold: a single sample "
                             "crossing is a crossing of the RIPPLE, not of the mean. Read the "
                             "latency as first excursion past the level, not as settling of the "
                             "mean." if spans else None),
                status=detail["status"], rise_time_s=detail["rise_time_s"],
                t_lo_s=detail["t_lo_s"], t_hi_s=detail["t_hi_s"],
                latency_detail=detail)


# --- stop / restart -------------------------------------------------------------------------

def stop_between(times, values, stop_time: float, next_event_time: float,
                 zero_tolerance: float = DEFAULT_ZERO_TOLERANCE,
                 min_confirm_s: float = DEFAULT_MIN_CONFIRM_S,
                 max_transition_gap_s: float | None = DEFAULT_MAX_TRANSITION_GAP_S) -> dict:
    """Assess a stop using ONLY [stop_time, next_event_time).

    Truncating the series here is the point: with the whole record, a second stop later on would
    let stop_points() find a trailing in-tolerance stretch and report the FIRST stop as confirmed
    on evidence that belongs to the second one (card: "Zbyt krotkie okno przed restartem nie moze
    'potwierdzic' stopu"). A short gap therefore yields INSUFFICIENT_OBSERVATION / NOT_REACHED, and
    that is the correct answer.
    """
    ts, vs = metrics.window(times, values, min(stop_time, times[0]), next_event_time)
    if not ts:
        return dict(status="NO_DATA", window_s=[stop_time, next_event_time])
    r = metrics.stop_points(ts, vs, stop_time, zero_tolerance,
                            min_confirm_s=min_confirm_s,
                            max_transition_gap_s=max_transition_gap_s)
    return dict(r, window_s=[stop_time, next_event_time],
                observation_window_note=("assessed only on samples before the next event; a later "
                                         "stop in the same record cannot confirm this one"))


def first_crossing(times, values, t_from: float, t_to: float, predicate) -> float | None:
    """First timestamp in [t_from, t_to) whose value satisfies `predicate`, or None."""
    for t, v in zip(times, values):
        if t_from <= t < t_to and predicate(v):
            return t
    return None


def restart_analysis(cols: dict, restart_time: float, prev_event_time: float,
                     next_event_time: float,
                     iq_channel: str = "iq_ref",
                     zero_tolerance: float = DEFAULT_ZERO_TOLERANCE) -> dict:
    """Separate the three things a restart consists of, instead of collapsing them into one number.

      commanded  - the generator resumes crank motion (ride_interval leaves 0). This is an INPUT.
      pipeline   - PAS/session/gate react: cadence_control_rpm leaves zero, gate_steps rises,
                   session state changes. These are production OBSERVATIONS.
      iq         - the Iq channel leaves zero again.

    Also reported: the state of the Iq channel AT the restart instant, which is what decides whether
    the restart actually landed during the release / Iq decay or after it. A short scripted gap is
    NOT evidence of that by itself - PAS may still consider pedalling active.
    """
    t = cols["time_s"]
    iq = cols[iq_channel]

    def after(name, pred):
        return first_crossing(t, cols[name], restart_time, next_event_time, pred)

    # State at the restart instant: the last sample STRICTLY BEFORE it. Nothing here or below in
    # this block may read a sample at or after restart_time - that is the whole point of T005-01.
    idx = None
    for i, tt in enumerate(t):
        if tt < restart_time:
            idx = i
        else:
            break
    iq_at_restart = iq[idx] if idx is not None else None
    last_pre_sample_time = t[idx] if idx is not None else None

    # First export sample at or after the commanded restart. Together with the last sample strictly
    # before it, this is the pair the ordering argument is actually allowed to use.
    idx_after = idx + 1 if (idx is not None and idx + 1 < len(t)) else (0 if idx is None and t else
                                                                       None)
    first_at_or_after_time = t[idx_after] if idx_after is not None else None

    # Export sampling interval, measured from the data itself (not assumed).
    export_dt_s = None
    if idx is not None and idx + 1 < len(t):
        export_dt_s = t[idx + 1] - t[idx]
    elif idx is not None and idx > 0:
        export_dt_s = t[idx] - t[idx - 1]

    # Was Iq observed nonzero anywhere in [prev_event_time, restart_time)? This window ends BEFORE
    # restart_time (metrics.window is documented half-open [lo, hi)) so it cannot include the
    # restart sample itself.
    ts_gap, iq_gap = metrics.window(t, iq, prev_event_time, restart_time)
    reached_zero_in_gap = any(abs(v) <= zero_tolerance for v in iq_gap) if iq_gap else None
    nonzero_in_gap = [v for v in iq_gap if abs(v) > zero_tolerance] if iq_gap else []

    # --- zero-crossing bracket and its order relative to the restart -------------------------
    #
    # REVIEW-EVD-AP-01-006 (T006-01): the previous version had this backwards. It called the case
    # "order undetermined" when the last sample STRICTLY BEFORE the restart already read zero - but
    # there the order IS known (the zero was measured before the restart was commanded); only the
    # exact crossing instant inside the bracket, and the confirmation length, are uncertain. And it
    # left the genuinely undetermined case unflagged: when the first zero sample IS the restart
    # sample, the crossing happened somewhere in (t_prev, t_zero] - an interval that contains the
    # restart instant, so this export cannot order the two.
    #
    # The rule: use the last pre-restart sample time, the first at-or-after sample time, and the
    # crossing bracket. Observations from both sides may BOUND the uncertainty, but a later
    # observation is never moved into the past.
    i_zero = None
    for i, tt in enumerate(t):
        if tt >= prev_event_time and abs(iq[i]) <= zero_tolerance:
            i_zero = i
            break
    t_zero = t[i_zero] if i_zero is not None else None
    t_zero_prev = t[i_zero - 1] if (i_zero is not None and i_zero > 0) else None
    crossing_bracket = ([t_zero_prev, t_zero] if (t_zero is not None and t_zero_prev is not None)
                        else ([None, t_zero] if t_zero is not None else None))

    if t_zero is None:
        zero_order = "NO_ZERO_OBSERVED_IN_RECORD"
    elif t_zero < restart_time:
        zero_order = "ZERO_OBSERVED_BEFORE_RESTART"
    elif t_zero_prev is not None and t_zero_prev < restart_time <= t_zero:
        zero_order = "ORDER_UNDETERMINED_CROSSING_BRACKET_CONTAINS_RESTART"
    else:
        zero_order = "ZERO_FIRST_OBSERVED_AFTER_RESTART"

    if iq_at_restart is None:
        iq_state = "NO_SAMPLE_BEFORE_RESTART"
    elif zero_order == "ZERO_OBSERVED_BEFORE_RESTART":
        iq_state = "IQ_ZERO_OBSERVED_BEFORE_RESTART"
    elif zero_order == "ORDER_UNDETERMINED_CROSSING_BRACKET_CONTAINS_RESTART":
        iq_state = "IQ_ZERO_CROSSING_ORDER_UNDETERMINED_AT_EXPORT_RESOLUTION"
    else:
        # Deliberately neutral: a nonzero sample proves neither a decay nor a release. Any trend
        # claim must come from iq_pre_restart_trend, which is measured separately and named as a
        # measurement, not a verdict.
        iq_state = "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE"

    # Measured pre-restart trend - reported so a decay CAN be argued, but only from evidence, and
    # never folded into iq_state_at_restart.
    if len(iq_gap) >= 2:
        trend = dict(samples=len(iq_gap), window_s=[prev_event_time, restart_time],
                     first=iq_gap[0], last=iq_gap[-1], delta=iq_gap[-1] - iq_gap[0],
                     max=max(iq_gap), min=min(iq_gap),
                     monotonic_non_increasing=all(b <= a + 1e-9 for a, b in
                                                  zip(iq_gap, iq_gap[1:])),
                     note=("a MEASUREMENT of the pre-restart window, not a verdict. Use this to "
                           "argue a decay; never infer one from a single nonzero sample."))
    else:
        trend = dict(samples=len(iq_gap), status="INSUFFICIENT_SAMPLES_FOR_A_TREND",
                     window_s=[prev_event_time, restart_time])

    t_cmd = after("ride_interval", lambda v: v != 0)
    t_cad = after("cadence_control_rpm", lambda v: v > 0)
    t_gate = after("gate_steps", lambda v: v > 0)
    t_iq = after(iq_channel, lambda v: abs(v) > zero_tolerance)
    t_ped = after("pedalling", lambda v: v != 0)

    def lat(x):
        return None if x is None else x - restart_time

    def resume_classification(value_at_restart, resume_time_after) -> str:
        """already positive / resumed / no observation - never inferred from a step-count alone."""
        if value_at_restart is None:
            return "NO_OBSERVATION_BEFORE_RESTART"
        if abs(value_at_restart) > zero_tolerance:
            return "ALREADY_POSITIVE_AT_RESTART_NOT_A_RESUME"
        if resume_time_after is not None:
            return "RESUMED_AFTER_RESTART"
        return "NO_OBSERVATION_IN_POST_RESTART_WINDOW"

    cad_at_restart = cols["cadence_control_rpm"][idx] if idx is not None else None
    gate_at_restart = cols["gate_steps"][idx] if idx is not None else None

    def resume_fields(prefix, value_at_restart, t_first_after) -> dict:
        """Split 'resume' from 'first positive sample after the restart'.

        REVIEW-EVD-AP-01-006 (T006-01): for a channel that was ALREADY positive at the restart, the
        first positive sample afterwards is not a resume - nothing resumed. The observation is kept
        under a diagnostic name; the resume time/latency stay None so no reader can quote them as a
        reaction time.
        """
        cls = resume_classification(value_at_restart, t_first_after)
        already = cls == "ALREADY_POSITIVE_AT_RESTART_NOT_A_RESUME"
        return {
            f"{prefix}_resume_classification": cls,
            f"{prefix}_value_at_last_pre_restart_sample": value_at_restart,
            f"{prefix}_resume_s": None if already else t_first_after,
            f"{prefix}_latency_s": None if already else lat(t_first_after),
            f"{prefix}_first_positive_sample_after_restart_s": t_first_after,
            f"{prefix}_resume_note": (
                "channel was already positive at the restart: *_resume_s/_latency_s are None on "
                "purpose; *_first_positive_sample_after_restart_s is a diagnostic observation, NOT "
                "a reaction time" if already else None),
        }

    # Session transitions WITH TIMES (T006-01): a set of names cannot say when anything changed.
    session_transitions = []
    prev_sv = None
    for tt, sv in zip(t, cols["session"]):
        if prev_sv is None:
            prev_sv = int(sv)
            continue
        if int(sv) != prev_sv:
            session_transitions.append(dict(
                time_s=tt, from_value=prev_sv, to_value=int(sv),
                from_state=decode_session(prev_sv), to_state=decode_session(sv),
                relative_to_restart_s=tt - restart_time,
                side=("BEFORE_RESTART" if tt < restart_time else "AT_OR_AFTER_RESTART")))
            prev_sv = int(sv)

    return dict(
        restart_time_s=restart_time,
        window_s=[restart_time, next_event_time],
        iq_channel=iq_channel,
        export_dt_s=export_dt_s,
        last_pre_restart_sample_time_s=last_pre_sample_time,
        first_sample_at_or_after_restart_time_s=first_at_or_after_time,
        iq_zero_crossing_bracket_s=crossing_bracket,
        iq_zero_order_vs_restart=zero_order,
        iq_zero_first_observed_s=t_zero,
        iq_value_at_restart=iq_at_restart,
        iq_state_at_restart=iq_state,
        iq_pre_restart_trend=trend,
        iq_state_note=("iq_state_at_restart is decided by the zero-crossing BRACKET "
                       "(t_prev_nonzero, t_first_zero] compared with restart_time_s. A zero "
                       "measured at a sample strictly before the restart means the ORDER is known "
                       "- only the exact crossing instant inside the bracket and the confirmation "
                       "length are uncertain. The order is undetermined only when the bracket "
                       "CONTAINS the restart instant. A nonzero last pre-restart sample is stated "
                       "neutrally: it is not evidence of a decay or a release - see "
                       "iq_pre_restart_trend for that, measured separately. Zero before the "
                       "restart is NOT the same claim as a confirmed stop; that stays in "
                       "stop_between()."),
        iq_reached_zero_before_restart=reached_zero_in_gap,
        iq_nonzero_samples_in_gap=len(nonzero_in_gap),
        **resume_fields("iq", iq_at_restart, t_iq),
        **resume_fields("cadence_control", cad_at_restart, t_cad),
        **resume_fields("gate_steps", gate_at_restart, t_gate),
        commanded_resume_s=t_cmd, commanded_resume_latency_s=lat(t_cmd),
        generator_pedalling_flag_s=t_ped,
        session_transitions=session_transitions,
        session_transitions_before_restart=[x for x in session_transitions
                                            if x["side"] == "BEFORE_RESTART"],
        session_states_in_window=sorted({int(v) for tt, v in zip(t, cols["session"])
                                         if restart_time <= tt < next_event_time}),
        session_states_in_window_decoded=sorted({decode_session(v) for tt, v in
                                                  zip(t, cols["session"])
                                                  if restart_time <= tt < next_event_time}),
        session_state_before_restart=(decode_session(cols["session"][idx])
                                      if idx is not None else None),
        debug_flags_in_window=sorted({int(v) for tt, v in zip(t, cols["debug_flags"])
                                      if restart_time <= tt < next_event_time}),
        debug_flags_in_window_decoded=sorted({n for tt, v in zip(t, cols["debug_flags"])
                                              if restart_time <= tt < next_event_time
                                              for n in decode_debug_flags(v)}),
        debug_flags_before_restart_decoded=(decode_debug_flags(cols["debug_flags"][idx])
                                            if idx is not None else None),
        note=("commanded_* is the scripted INPUT; cadence/gate/session and iq_* are production "
              "observations. A latency that is None was not observed in this window - it is not 0. "
              "session/debug_flags are decoded from inc/ride_session.h and inc/ride_control.h, not "
              "guessed; debug_flags is a bitmask, decoded with bitwise AND, not list membership. "
              "*_in_window fields (post-restart) must never be used to argue about pre-restart "
              "ordering - use iq_state_at_restart / *_resume_classification for that."),
    )


def excursions(times, values, t_from: float, t_to: float) -> dict:
    """Peaks/dips and the raw trajectory of a channel over a window, without any smoothing."""
    ts, vs = metrics.window(times, values, t_from, t_to)
    if not vs:
        return dict(samples=0, note="empty window")
    i_max = max(range(len(vs)), key=lambda i: vs[i])
    i_min = min(range(len(vs)), key=lambda i: vs[i])
    return dict(samples=len(vs), window_s=[t_from, t_to],
                max=vs[i_max], t_max_s=ts[i_max], min=vs[i_min], t_min_s=ts[i_min],
                first=vs[0], last=vs[-1], mean=metrics.mean(vs),
                note="raw exported samples; no smoothing is applied anywhere in this module")


def commanded_input_check(cols: dict, expect: dict, tolerance: float = 1e-6) -> dict:
    """Verify the CSV really carries the commanded torque mean and cadence per interval.

    A generator test must check the generator's OWN output, not the production reaction to it.
    `expect` maps a [t0, t1) window to {"torque_cmd_mean_nm": x, "cadence_gen_rpm_mean": y}.
    """
    t = cols["time_s"]
    out = {}
    for label, spec in expect.items():
        lo, hi = spec["window_s"]
        checks = {}
        if "torque_cmd_mean_nm" in spec:
            _, vs = metrics.window(t, cols["torque_cmd_mean_nm"], lo, hi)
            ok = bool(vs) and all(abs(v - spec["torque_cmd_mean_nm"]) <= tolerance for v in vs)
            checks["torque_cmd_mean_nm"] = dict(expected=spec["torque_cmd_mean_nm"],
                                                observed_unique=sorted(set(vs))[:5],
                                                samples=len(vs), ok=ok)
        if "ride_interval" in spec:
            _, vs = metrics.window(t, cols["ride_interval"], lo, hi)
            ok = bool(vs) and all(int(v) == spec["ride_interval"] for v in vs)
            checks["ride_interval"] = dict(expected=spec["ride_interval"],
                                           observed_unique=sorted({int(v) for v in vs}),
                                           samples=len(vs), ok=ok)
        if "cadence_gen_rpm_mean" in spec:
            _, vs = metrics.window(t, cols["cadence_gen_rpm"], lo, hi)
            m = metrics.mean(vs) if vs else None
            ok = m is not None and abs(m - spec["cadence_gen_rpm_mean"]) <= spec.get(
                "cadence_tolerance", 0.5)
            checks["cadence_gen_rpm_mean"] = dict(expected=spec["cadence_gen_rpm_mean"],
                                                  observed=m, samples=len(vs), ok=ok)
        out[label] = dict(window_s=[lo, hi], checks=checks,
                          ok=all(c["ok"] for c in checks.values()) if checks else False)
    out["all_ok"] = all(v["ok"] for k, v in out.items() if k != "all_ok")
    return out


def finite(x):
    return x is not None and isinstance(x, float) and math.isfinite(x)
