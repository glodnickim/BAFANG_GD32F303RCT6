"""AP-01 metric primitives: p-p/mean, RMS, mean/bias, 10-90% rise/fall latency, stop confirmation,
limit (cap/floor) evidence, and baseline-vs-candidate comparison.

REV3 (rework after REVIEW-EVD-AP-01-002, findings AP01-V2-01 / AP01-V2-02):

- stop confirmation now needs an OBSERVED hold, not just "the last sample happened to be zero".
  A zero that appears only in the final sample yields INSUFFICIENT_OBSERVATION, not CONFIRMED.
- the uncertainty of the reported stop instant is the pair of ADJACENT timestamps that bracket the
  transition, not a median sampling interval taken over the whole record. With irregular sampling
  those are wildly different numbers, and only the bracket is true.
- rise_time_10_90 no longer returns a latency when one of the two crossings is outside the record
  (e.g. the series already starts above the 10% level): that is an insufficient record, not a
  measurement of 0 s or of the 90%-only interval.
- limiting (cap/floor) is decided from REAL evidence - a pre-limit vs post-limit channel pair, or a
  detected plateau at an extreme - never from flatness alone. A perfectly smooth signal stays
  comparable; that is the whole point of the comparison.
- compare_channel() implements the baseline-vs-candidate comparison directionally: a LOWER relative
  ripple is better and is never penalised for being "too much better".

REV2 (after REVIEW-EVD-AP-01-001, AP01-R1): stop_points reports NO_DATA / NOT_REACHED / CONFIRMED
instead of a "time to zero" derived from the last nonzero sample; input validation added.

Independent of run_probe.py / probe-results.json (original audit evidence, read-only). Operates on
the same Controller Lab CSV column names (see controller_lab.c print_header). No production math is
duplicated here: these are statistics over columns the production code already emitted.
"""
from __future__ import annotations

import math
from typing import Sequence

# --- measurement contract parameters --------------------------------------------------------
# These are parameters of the MEASUREMENT, not firmware constants. They do not describe what the
# controller must do; they describe how long this tool must watch before it is willing to call
# something "observed". Callers may override them per scenario.
#
# DEFAULT_MIN_CONFIRM_S: how long the channel must be observed to REMAIN within tolerance after
# first entering it before the stop instant counts as measured. 0.10 s is 20 sampling intervals of
# the 5 ms Controller Lab export - long enough that a single trailing sample cannot "confirm" a
# stop, short enough to leave every reviewer-accepted V1 case (which hold for >= 1 s) confirmed.
DEFAULT_MIN_CONFIRM_S = 0.10


def _validate_series(times: Sequence[float], values: Sequence[float]) -> None:
    if len(times) != len(values):
        raise ValueError(f"times/values length mismatch: {len(times)} vs {len(values)}")
    for t in times:
        if not math.isfinite(t):
            raise ValueError(f"non-finite timestamp: {t!r}")
    for v in values:
        if not math.isfinite(v):
            raise ValueError(f"non-finite value: {v!r}")
    for i in range(1, len(times)):
        if times[i] <= times[i - 1]:
            raise ValueError(
                f"timestamps must be strictly increasing (no duplicates, no reordering): "
                f"index {i-1}={times[i-1]!r} -> index {i}={times[i]!r}"
            )


def peak_to_peak(values: Sequence[float]) -> float:
    if not values:
        raise ValueError("empty series")
    return max(values) - min(values)


def mean(values: Sequence[float]) -> float:
    if not values:
        raise ValueError("empty series")
    return sum(values) / len(values)


def rms(values: Sequence[float]) -> float:
    if not values:
        raise ValueError("empty series")
    return math.sqrt(sum(v * v for v in values) / len(values))


def rms_about_mean(values: Sequence[float]) -> float:
    """RMS of the AC component only (ripple), i.e. RMS(values - mean(values))."""
    m = mean(values)
    return rms([v - m for v in values])


def relative_ripple(values: Sequence[float], reference: float | None = None) -> float | None:
    """Ripple RMS as a fraction of a reference magnitude (mean(values) by default).

    Returns None when the reference is at/near zero, since a relative measure is meaningless there.
    """
    denom = mean(values) if reference is None else reference
    if abs(denom) < 1e-9:
        return None
    return rms_about_mean(values) / abs(denom)


def bias(values: Sequence[float], reference: float) -> float:
    """Mean signed deviation from an external reference/target value."""
    return mean(values) - reference


def relative_bias(values: Sequence[float], reference: float) -> float | None:
    """bias() expressed as a fraction of the reference magnitude; None if reference ~ 0."""
    if abs(reference) < 1e-9:
        return None
    return bias(values, reference) / abs(reference)


def window(times: Sequence[float], values: Sequence[float], t0: float, t1: float):
    """Return the (times, values) restricted to [t0, t1).

    Raises on a length mismatch instead of silently truncating via zip (AP01-R1), and on
    non-finite or inverted bounds (AP01-V2-01: "walidowac stop_time/window bounds").
    """
    _validate_series(times, values)
    if not math.isfinite(t0) or not math.isfinite(t1):
        raise ValueError(f"window bounds must be finite, got t0={t0!r}, t1={t1!r}")
    if t1 <= t0:
        raise ValueError(f"window bounds must satisfy t0 < t1, got t0={t0!r}, t1={t1!r}")
    pairs = [(t, v) for t, v in zip(times, values) if t0 <= t < t1]
    if not pairs:
        return [], []
    ts, vs = zip(*pairs)
    return list(ts), list(vs)


# --- 10-90% latency -------------------------------------------------------------------------

def rise_time_10_90_detail(times: Sequence[float], values: Sequence[float],
                            start_level: float, end_level: float) -> dict:
    """10-90% latency of a start->end step, with an explicit status.

    BOTH crossings must be visible inside the record. A series that already starts at or beyond the
    10% level crossed it before the record began: the elapsed time to the 90% level is then not a
    10-90% latency, and reporting it as one would invent a measurement (AP01-V2-01, reviewer's
    [50,90,100] counterexample for a 0->100 step).

    status:
      MEASURED                    - both crossings observed; rise_time_s is valid.
      NO_STEP_DEFINED             - start_level == end_level.
      INSUFFICIENT_DATA           - fewer than 2 samples.
      LO_CROSSING_BEFORE_RECORD   - the series already satisfies the 10% threshold at its first
                                    sample; the crossing lies outside the record.
      LO_CROSSING_NOT_REACHED     - the 10% threshold is never reached within the record.
      HI_CROSSING_NOT_REACHED     - the 90% threshold is never reached within the record.
    """
    _validate_series(times, values)
    span = end_level - start_level
    if span == 0:
        return dict(status="NO_STEP_DEFINED", rise_time_s=None, t_lo_s=None, t_hi_s=None)
    if len(times) < 2:
        return dict(status="INSUFFICIENT_DATA", rise_time_s=None, t_lo_s=None, t_hi_s=None)

    lo = start_level + 0.10 * span
    hi = start_level + 0.90 * span
    rising = span > 0

    def crossed(v, threshold):
        return v >= threshold if rising else v <= threshold

    if crossed(values[0], lo):
        return dict(status="LO_CROSSING_BEFORE_RECORD", rise_time_s=None,
                    t_lo_s=None, t_hi_s=None,
                    first_sample=values[0], lo_threshold=lo, hi_threshold=hi)

    t_lo = None
    t_hi = None
    for t, v in zip(times, values):
        if t_lo is None and crossed(v, lo):
            t_lo = t
        if t_lo is not None and t_hi is None and crossed(v, hi):
            t_hi = t
        if t_lo is not None and t_hi is not None:
            break
    if t_lo is None:
        return dict(status="LO_CROSSING_NOT_REACHED", rise_time_s=None, t_lo_s=None, t_hi_s=None,
                    lo_threshold=lo, hi_threshold=hi)
    if t_hi is None:
        return dict(status="HI_CROSSING_NOT_REACHED", rise_time_s=None, t_lo_s=t_lo, t_hi_s=None,
                    lo_threshold=lo, hi_threshold=hi)
    return dict(status="MEASURED", rise_time_s=t_hi - t_lo, t_lo_s=t_lo, t_hi_s=t_hi,
                lo_threshold=lo, hi_threshold=hi)


def rise_time_10_90(times: Sequence[float], values: Sequence[float],
                     start_level: float, end_level: float) -> float | None:
    """Scalar form of rise_time_10_90_detail: the latency, or None when it was not measured."""
    return rise_time_10_90_detail(times, values, start_level, end_level)["rise_time_s"]


# --- stop confirmation ----------------------------------------------------------------------

def stop_points(times: Sequence[float], values: Sequence[float],
                 stop_time: float, zero_tolerance: float,
                 min_confirm_s: float = DEFAULT_MIN_CONFIRM_S,
                 max_transition_gap_s: float | None = None) -> dict:
    """Report what is actually observed around a scripted stop event, not an extrapolation.

    status:
      NO_DATA                   - no sample at or after stop_time exists in this record.
      NOT_REACHED               - samples exist at/after stop_time, but the channel never stays
                                  within zero_tolerance for the remainder of the record. A
                                  transient dip that later rises again does NOT count.
      INSUFFICIENT_OBSERVATION  - the channel does enter tolerance and hold to the end of the
                                  record, but it was observed there for less than min_confirm_s.
                                  The record ends too early to call the stop measured (AP01-V2-01:
                                  a single trailing zero is not a confirmed stop).
      OBSERVATION_GAP_TOO_LARGE - confirmed in principle, but the sampling gap bracketing the
                                  transition exceeds max_transition_gap_s, so the instant is not
                                  localised well enough to be reported as measured.
      CONFIRMED                 - entered tolerance, held for at least min_confirm_s to the end of
                                  the record, and (if a limit was given) the transition is
                                  localised within max_transition_gap_s.

    Uncertainty of the instant is `transition_bracket_s` = [last sample still OUT of tolerance,
    first sample of the confirmed in-tolerance run]. The true crossing lies in that half-open
    interval. `median_sample_dt_s` is reported for information only and is explicitly NOT the
    uncertainty: with irregular sampling the two differ by orders of magnitude.
    """
    _validate_series(times, values)
    if not math.isfinite(zero_tolerance) or zero_tolerance <= 0:
        raise ValueError(f"zero_tolerance must be a finite positive number, got {zero_tolerance!r}")
    if not math.isfinite(stop_time):
        raise ValueError(f"stop_time must be finite, got {stop_time!r}")
    if not math.isfinite(min_confirm_s) or min_confirm_s < 0:
        raise ValueError(f"min_confirm_s must be a finite non-negative number, got {min_confirm_s!r}")
    if max_transition_gap_s is not None and (
            not math.isfinite(max_transition_gap_s) or max_transition_gap_s <= 0):
        raise ValueError(
            f"max_transition_gap_s must be a finite positive number or None, got {max_transition_gap_s!r}")

    before = [(t, v) for t, v in zip(times, values) if t < stop_time]
    at_after = [(t, v) for t, v in zip(times, values) if t >= stop_time]
    value_at_stop = before[-1][1] if before else None
    last_sample_before_stop_s = before[-1][0] if before else None

    base = dict(
        value_at_stop=value_at_stop,
        last_sample_before_stop_s=last_sample_before_stop_s,
        candidate_zero_time_s=None,
        confirmed_zero_time_s=None,
        time_to_confirmed_zero_s=None,
        observed_hold_s=None,
        min_confirm_s=min_confirm_s,
        transition_bracket_s=None,
        transition_bracket_width_s=None,
        last_observed_time_s=None,
        median_sample_dt_s=None,
        median_sample_dt_note="information only - NOT the uncertainty of the stop instant; use transition_bracket_s",
    )

    if not at_after:
        return dict(base, status="NO_DATA")

    last_observed_time_s = at_after[-1][0]
    dts = [at_after[i][0] - at_after[i - 1][0] for i in range(1, len(at_after))]
    median_dt = sorted(dts)[len(dts) // 2] if dts else None
    base = dict(base, last_observed_time_s=last_observed_time_s, median_sample_dt_s=median_dt)

    confirmed_idx = None
    n = len(at_after)
    for i in range(n):
        if all(abs(v) <= zero_tolerance for _, v in at_after[i:]):
            confirmed_idx = i
            break

    if confirmed_idx is None:
        return dict(base, status="NOT_REACHED")

    candidate_time = at_after[confirmed_idx][0]
    observed_hold_s = last_observed_time_s - candidate_time

    # The transition is bracketed by the last out-of-tolerance sample anywhere in the record and
    # the first sample of the confirmed in-tolerance run.
    out_before_candidate = [t for t, v in zip(times, values)
                            if t < candidate_time and abs(v) > zero_tolerance]
    bracket_lo = max(out_before_candidate) if out_before_candidate else None
    bracket = [bracket_lo, candidate_time]
    bracket_width = (candidate_time - bracket_lo) if bracket_lo is not None else None

    # A CONFIRMED status means "the LAST stretch of the record stays in tolerance". It does NOT
    # mean the channel never came back up earlier: an excursion before the confirmed stretch is
    # explicitly tolerated by this definition. Count those separately so a caller can judge the
    # trajectory instead of reading a guarantee into CONFIRMED that it does not carry
    # (AP01-V3, "Korekty powiazanego opisu").
    excursions = 0
    inside = False
    for _, v in at_after[:confirmed_idx]:
        if abs(v) <= zero_tolerance:
            inside = True
        elif inside:
            excursions += 1
            inside = False

    base = dict(base,
                candidate_zero_time_s=candidate_time,
                observed_hold_s=observed_hold_s,
                transition_bracket_s=bracket,
                transition_bracket_width_s=bracket_width,
                excursions_before_confirmation=excursions,
                excursions_note=("number of times the channel entered tolerance and left it again "
                                 "BEFORE the confirmed stretch; CONFIRMED does not imply this is 0"))

    if observed_hold_s < min_confirm_s:
        return dict(base, status="INSUFFICIENT_OBSERVATION")

    if (max_transition_gap_s is not None and bracket_width is not None
            and bracket_width > max_transition_gap_s):
        return dict(base, status="OBSERVATION_GAP_TOO_LARGE",
                    max_transition_gap_s=max_transition_gap_s)

    return dict(base, status="CONFIRMED",
                confirmed_zero_time_s=candidate_time,
                time_to_confirmed_zero_s=candidate_time - stop_time)


# --- limiting (cap / floor) evidence --------------------------------------------------------

# The Iq chain as the export actually orders it. Verified against production source, not assumed:
#   src/assist_modes.c   `output->iq_before_pu = phase_iq_request;` is taken BEFORE the profile
#                        limit, and the very next statements clamp phase_iq_request to
#                        profile_iq_pct_limit(). The file's own comment states that a difference
#                        between iq_before_pu and iq_request "means max_iq_pct clamped the result
#                        and nothing else".
#   sim/controller_lab/controller_lab.c  prints mo->iq_request, mo->iq_before_pu,
#                        chain->requested, chain->allowed into the columns named
#                        iq_mode_request, iq_before_profile_limit, iq_requested, iq_allowed.
# So the profile limit sits between iq_before_profile_limit and iq_mode_request - NOT between
# iq_before_profile_limit and iq_requested, which is what REV3 wrongly compared (AP01-V3-01).
# Everything between iq_mode_request and iq_requested happens later, in ride_control.c; this
# module does not name which mechanism it was without evidence.
IQ_STAGES = (
    ("profile_limit", "iq_before_profile_limit", "iq_mode_request"),
    ("post_mode_request", "iq_mode_request", "iq_requested"),
    ("request_to_allowed", "iq_requested", "iq_allowed"),
)

# Which observed stages sit UPSTREAM of each channel. A channel can only be affected by the stages
# above it. Channels outside the Iq chain get no Iq limit verdict at all (AP01-V3-01 item 4:
# the REV3 extractor stamped the same label onto torque and cadence).
CHANNEL_UPSTREAM_STAGES = {
    "iq_before_profile_limit": (),
    "iq_mode_request": ("profile_limit",),
    "iq_requested": ("profile_limit", "post_mode_request"),
    "iq_allowed": ("profile_limit", "post_mode_request", "request_to_allowed"),
    "iq_ref": ("profile_limit", "post_mode_request", "request_to_allowed"),
    "torque_run_native": None,
    "cadence_control_rpm": None,
}

NON_IQ_CHANNEL_STATUS = "NOT_APPLICABLE_NON_IQ_CHANNEL"


def stage_transition(upstream: Sequence[float], downstream: Sequence[float],
                      majority_threshold: float = 0.5, tolerance: float = 1e-9) -> dict:
    """Compare one observed stage of the chain, WITHOUT naming a cause.

    Reports only what the two channels do relative to each other: REDUCED / RAISED / UNCHANGED /
    MIXED. Attributing a change to a specific mechanism (floor, gate, battery limiter, ...) needs
    evidence beyond these two columns, and is deliberately not done here.
    """
    if len(upstream) != len(downstream):
        raise ValueError(f"channel length mismatch: {len(upstream)} vs {len(downstream)}")
    if not upstream:
        raise ValueError("empty series")
    n = len(upstream)
    reduced = sum(1 for a, b in zip(upstream, downstream) if b < a - tolerance)
    raised = sum(1 for a, b in zip(upstream, downstream) if b > a + tolerance)
    unchanged = n - reduced - raised
    f_red, f_raise, f_same = reduced / n, raised / n, unchanged / n
    if f_same == 1.0:
        status = "UNCHANGED"
    elif f_red > majority_threshold:
        status = "REDUCED"
    elif f_raise > majority_threshold:
        status = "RAISED"
    else:
        status = "MIXED"
    return dict(status=status, samples=n,
                fraction_reduced=f_red, fraction_raised=f_raise, fraction_unchanged=f_same,
                upstream_range=[min(upstream), max(upstream)],
                downstream_range=[min(downstream), max(downstream)])


def stage_analysis(channel_windows: dict) -> dict:
    """Run stage_transition over every stage whose two channels are present.

    Returns {stage_name: transition_dict}. The profile_limit stage is the only one this module
    attributes to a named mechanism, and only when it REDUCES, because production source states
    that a difference there is max_iq_pct "and nothing else".
    """
    out = {}
    for name, up, down in IQ_STAGES:
        if up in channel_windows and down in channel_windows:
            t = stage_transition(channel_windows[up], channel_windows[down])
            if name == "profile_limit" and t["status"] == "REDUCED":
                t = dict(t, attributed_cause="profile Iq limit (max_iq_pct) per assist_modes.c")
            else:
                t = dict(t, attributed_cause=None,
                         attribution_note="cause not attributable from these two columns alone")
            out[name] = t
    return out


def limit_assessment(values: Sequence[float], channel: str | None = None,
                      stages: dict | None = None,
                      plateau_fraction_threshold: float = 0.5,
                      flat_pp_threshold: float = 1e-9) -> dict:
    """Assess a window's limiting evidence and whether it stays COMPARABLE.

    Shape NEVER excludes a result on its own (AP01-V3-02). A plateau, a flat line or a stepped
    signal can equally come from quantisation, a constant input or a genuine change in effort;
    without evidence from an observed stage it is information, not a limit. Exclusion requires
    either (a) an observed upstream stage that actually changed this channel's value, or (b) the
    channel being constant zero, i.e. carrying no assist to compare at all.

    status:
      NOT_APPLICABLE_NON_IQ_CHANNEL - not part of the Iq chain; no Iq limit verdict is stamped on it
      LIMITED_UPSTREAM              - an observed upstream stage REDUCED/RAISED/MIXED this value
      ZERO_NO_ASSIST                - constant zero window
      PLATEAU_NO_LIMIT_EVIDENCE     - a majority of samples at the maximum, but no upstream stage
                                      changed the value: informational, limiting UNKNOWN
      FLAT_NO_LIMIT_EVIDENCE        - constant non-zero, no upstream change: limiting UNKNOWN
      NO_LIMIT_OBSERVED             - no observed stage changed this value. NOT a claim that no
                                      limiter acted anywhere: battery/thermal/speed limiters are
                                      not separated by these columns.
    """
    if not values:
        raise ValueError("empty series")

    if channel is not None and CHANNEL_UPSTREAM_STAGES.get(channel, "missing") is None:
        return dict(status=NON_IQ_CHANNEL_STATUS, comparable=True, limiting="NOT_APPLICABLE",
                    evidence=f"'{channel}' is not part of the Iq chain; Iq stage verdicts do not apply",
                    upstream_stages=None, peak_to_peak=peak_to_peak(values))

    pp = peak_to_peak(values)
    n = len(values)
    vmax = max(values)
    at_max = sum(1 for v in values if abs(v - vmax) <= flat_pp_threshold)
    fraction_at_max = at_max / n

    upstream_names = CHANNEL_UPSTREAM_STAGES.get(channel, ()) if channel is not None else ()
    changed = {}
    if stages:
        for s in (upstream_names or ()):
            st = stages.get(s)
            if st and st["status"] != "UNCHANGED":
                changed[s] = st["status"]

    common = dict(peak_to_peak=pp, fraction_at_max=fraction_at_max,
                  upstream_stages=list(upstream_names or []),
                  upstream_changes=changed or None)

    if changed:
        return dict(common, status="LIMITED_UPSTREAM", comparable=False,
                    limiting="OBSERVED",
                    evidence=f"observed upstream stage(s) changed this value: {changed}")

    if pp <= flat_pp_threshold and abs(vmax) <= flat_pp_threshold:
        return dict(common, status="ZERO_NO_ASSIST", comparable=False, limiting="NOT_APPLICABLE",
                    evidence="constant zero window - no assist present to compare")

    if pp <= flat_pp_threshold:
        return dict(common, status="FLAT_NO_LIMIT_EVIDENCE", comparable=True, limiting="UNKNOWN",
                    evidence="constant non-zero value and no observed upstream change; "
                             "flatness alone is not evidence of a limit")

    if fraction_at_max > plateau_fraction_threshold:
        return dict(common, status="PLATEAU_NO_LIMIT_EVIDENCE", comparable=True, limiting="UNKNOWN",
                    evidence=f"{at_max}/{n} samples at max={vmax}, but no observed upstream stage "
                             f"changed this value; a plateau can also come from quantisation, a "
                             f"constant input or a change in effort - not evidence of clipping")

    return dict(common, status="NO_LIMIT_OBSERVED", comparable=True, limiting="NONE_OBSERVED",
                evidence="no observed stage changed this value; other limiters "
                         "(battery/thermal/speed) are not separated by these columns")


# --- baseline vs candidate comparison -------------------------------------------------------

def compare_channel(baseline_values: Sequence[float], candidate_values: Sequence[float],
                     ripple_reduction_target: float = 0.50,
                     mean_budget: float = 0.05) -> dict:
    """Compare one channel of a candidate run against the same channel of the baseline run.

    Two SEPARATE questions, per AP01-V2-02:
      preservation - did the assist magnitude stay put?  |relative mean change| <= mean_budget
      improvement  - did the ripple go DOWN?             reduction >= ripple_reduction_target

    The improvement side is DIRECTIONAL: a larger reduction is always better and is never rejected
    for exceeding the target. `ripple_reduction_fraction` is 1 - candidate/baseline, so 0.9 means
    the candidate's relative ripple is 10% of the baseline's (a 90% reduction).

    A baseline with ZERO ripple is a boundary case, not an ordinary "no improvement": a percentage
    reduction is undefined, but ripple appearing where there was none is a REGRESSION and must be
    reported as one (AP01-V3-02). The absolute ripple RMS of both sides is therefore always
    returned, so an increase is visible even when the ratio is undefined.

    verdict:
      IMPROVED_WITHIN_BUDGET     - ripple target met and mean preserved
      PRESERVED_NOT_IMPROVED     - mean preserved, ripple target not met (ripple unchanged or
                                   reduced by less than the target)
      RIPPLE_REGRESSED           - ripple got worse
      RIPPLE_REGRESSED_FROM_ZERO - baseline had no ripple and the candidate introduces some; the
                                   percentage reduction is undefined, the regression is not
      UNCHANGED_BOTH_FLAT        - neither side has ripple; nothing to compare on this axis
      MEAN_OUT_OF_BUDGET         - mean moved more than the budget (whatever the ripple did)
      NOT_COMPARABLE             - a relative measure is undefined (mean at/near zero)
    """
    b_rel = relative_ripple(baseline_values)
    c_rel = relative_ripple(candidate_values)
    b_mean = mean(baseline_values)
    c_mean = mean(candidate_values)
    b_ripple_rms = rms_about_mean(baseline_values)
    c_ripple_rms = rms_about_mean(candidate_values)
    mean_bias_abs = c_mean - b_mean
    mean_bias_relative = relative_bias(candidate_values, b_mean)

    result = dict(
        baseline_mean=b_mean, candidate_mean=c_mean,
        baseline_relative_ripple=b_rel, candidate_relative_ripple=c_rel,
        baseline_ripple_rms=b_ripple_rms, candidate_ripple_rms=c_ripple_rms,
        mean_bias_abs=mean_bias_abs, mean_bias_relative=mean_bias_relative,
        ripple_reduction_target=ripple_reduction_target, mean_budget=mean_budget,
    )

    if b_rel is None or c_rel is None or mean_bias_relative is None:
        return dict(result, ripple_reduction_fraction=None, meets_ripple_target=None,
                    within_mean_budget=None, verdict="NOT_COMPARABLE",
                    reason="relative measure undefined (reference mean at/near zero)")

    within_budget = abs(mean_bias_relative) <= mean_budget

    # Zero-ripple baseline: the ratio is undefined, so decide on the absolute ripple RMS instead.
    if b_rel == 0:
        if c_ripple_rms > b_ripple_rms:
            verdict = "MEAN_OUT_OF_BUDGET" if not within_budget else "RIPPLE_REGRESSED_FROM_ZERO"
            return dict(result, ripple_reduction_fraction=None, meets_ripple_target=False,
                        within_mean_budget=within_budget, verdict=verdict,
                        reason=("baseline ripple is zero so a percentage reduction is undefined; "
                                f"candidate ripple RMS rose from {b_ripple_rms:.6g} to "
                                f"{c_ripple_rms:.6g}, which is a regression"))
        return dict(result, ripple_reduction_fraction=None, meets_ripple_target=False,
                    within_mean_budget=within_budget,
                    verdict="MEAN_OUT_OF_BUDGET" if not within_budget else "UNCHANGED_BOTH_FLAT",
                    reason="neither baseline nor candidate carries ripple on this channel/window")

    reduction = 1.0 - (c_rel / b_rel)
    meets_target = reduction >= ripple_reduction_target

    if not within_budget:
        verdict = "MEAN_OUT_OF_BUDGET"
    elif reduction < 0:
        verdict = "RIPPLE_REGRESSED"
    elif meets_target:
        verdict = "IMPROVED_WITHIN_BUDGET"
    else:
        verdict = "PRESERVED_NOT_IMPROVED"

    return dict(result, ripple_reduction_fraction=reduction, meets_ripple_target=meets_target,
                within_mean_budget=within_budget, verdict=verdict)
