"""Measurement functions written FOR this task, separated so they can be unit-tested directly.

REVIEW-EVD-AP-01-011 (D011-03) rejected the previous versions for saying more than the data
supports. The two defects and what replaces them:

1. `zero_and_recovery()` called `first_crossing()` once and called the result "reached zero".
   * A single sample within tolerance is not a confirmed zero - the accepted contract already
     carries `min_confirm_s` and it was ignored.
   * If Iq was ALREADY at zero before the event, the first in-window sample is trivially within
     tolerance and the function reported "reached_zero, latency=0". That is not a measured
     extinction time; it is a measurement of the state the window started in. Seen on every
     `*_invalid_end` event, whose Iq was zero throughout the preceding window.
   * There was no recovery measurement at all, despite the name.

2. `observed_state_transitions()` took the first sample that differs from one baseline sample and
   called it the reaction. For a rippling channel like `iq_ref` that is the next sample almost
   always, in the control run just as much as in the disturbed one, so it cannot be evidence that
   the injection caused anything.

The replacements below report a STATUS from a closed set, carry the conditions they were computed
under, and never assert causality. `NOT_OBSERVED`, `ALREADY_ZERO`, `UNCONFIRMED` and `NO_DATA` are
four different answers and none of them is "zero".

Nothing here re-implements production maths; these are descriptions of an exported series.
"""
from __future__ import annotations

from typing import Sequence

# Status vocabulary. A caller must not collapse these.
NO_DATA = "NO_DATA"                    # the window holds no samples
ALREADY_ZERO = "ALREADY_ZERO"          # within tolerance BEFORE the event - no new extinction here
CONFIRMED_ZERO = "CONFIRMED_ZERO"      # first contact, then held within tolerance >= min_confirm_s
UNCONFIRMED_ZERO = "UNCONFIRMED_ZERO"  # touched the tolerance but the hold could not be confirmed
NOT_OBSERVED = "NOT_OBSERVED"          # never came within tolerance in the window
NO_BASELINE = "NO_BASELINE_BEFORE_EVENT"
CHANNEL_ABSENT = "CHANNEL_ABSENT"


def _window(times: Sequence[float], values: Sequence[float], t0: float, t1: float):
    ts, vs = [], []
    for t, v in zip(times, values):
        if t0 <= t < t1:
            ts.append(t)
            vs.append(v)
    return ts, vs


def _last_before(times: Sequence[float], values: Sequence[float], t_event: float):
    out = None
    for t, v in zip(times, values):
        if t < t_event:
            out = (t, v)
        else:
            break
    return out


def zero_assessment(times, values, t_event: float, t_until: float,
                    tolerance: float, min_confirm_s: float) -> dict:
    """Did the channel reach - and HOLD - the zero tolerance after the event?

    Returns one of the statuses above plus the evidence behind it. `latency_*` is only present
    when it is a real latency measured from the event, i.e. never for ALREADY_ZERO.
    """
    ts, vs = _window(times, values, t_event, t_until)
    base = _last_before(times, values, t_event)
    common = dict(
        tolerance=tolerance,
        min_confirm_s=min_confirm_s,
        window_s=[t_event, t_until],
        samples_in_window=len(vs),
        baseline_before_event=(None if base is None else base[1]),
        baseline_t_s=(None if base is None else base[0]),
    )
    if not vs:
        return dict(status=NO_DATA, **common,
                    note="no samples inside the window; nothing was measured")
    if base is None:
        return dict(status=NO_BASELINE, **common,
                    note="no sample before the event, so 'reached zero' cannot be distinguished "
                         "from 'was already there'")
    if abs(base[1]) <= tolerance:
        return dict(status=ALREADY_ZERO, **common,
                    note="the channel was already inside the tolerance before the event; any "
                         "in-window contact is the state the window STARTED in, not a new "
                         "extinction, and no latency is reported")

    t_contact = None
    for t, v in zip(ts, vs):
        if abs(v) <= tolerance:
            t_contact = t
            break
    if t_contact is None:
        return dict(status=NOT_OBSERVED, **common,
                    note="never came within the tolerance inside the window; this is NOT a "
                         "measurement of a value, it is the absence of one")

    # How long did it stay inside the tolerance, without a break, from first contact?
    hold_end = t_contact
    for t, v in zip(ts, vs):
        if t < t_contact:
            continue
        if abs(v) <= tolerance:
            hold_end = t
        else:
            break
    hold = hold_end - t_contact
    truncated = hold_end >= ts[-1]           # the window ended while still inside tolerance
    confirmed = hold >= min_confirm_s
    return dict(
        status=CONFIRMED_ZERO if confirmed else UNCONFIRMED_ZERO,
        **common,
        t_first_contact_s=t_contact,
        latency_first_contact_s=round(t_contact - t_event, 6),
        hold_duration_s=round(hold, 6),
        hold_truncated_by_window=bool(truncated and not confirmed),
        t_confirmed_s=(round(t_contact + min_confirm_s, 6) if confirmed else None),
        latency_confirmed_s=(round(t_contact + min_confirm_s - t_event, 6) if confirmed else None),
        note=("held inside the tolerance for at least min_confirm_s" if confirmed else
              ("the window (or the next event) ended before the hold could be confirmed - "
               "UNCONFIRMED is not a negative result" if truncated else
               "left the tolerance again before min_confirm_s elapsed")),
    )


def recovery_assessment(times, values, t_event: float, t_until: float,
                        tolerance: float, min_confirm_s: float,
                        reference_level: float | None = None) -> dict:
    """After the input returned to normal, did the channel come back - and stay back?

    `reference_level`, when given, is the level recovery is judged against (normally the control
    run's mean). Without it only "left the zero tolerance and stayed out" is reported.
    """
    ts, vs = _window(times, values, t_event, t_until)
    common = dict(tolerance=tolerance, min_confirm_s=min_confirm_s,
                  window_s=[t_event, t_until], samples_in_window=len(vs),
                  reference_level=reference_level)
    if not vs:
        return dict(status=NO_DATA, **common)
    t_back = None
    for i, (t, v) in enumerate(zip(ts, vs)):
        if abs(v) > tolerance:
            # require it to stay out of the tolerance for min_confirm_s
            end = t
            ok = True
            for t2, v2 in zip(ts[i:], vs[i:]):
                if abs(v2) <= tolerance:
                    ok = False
                    break
                end = t2
                if end - t >= min_confirm_s:
                    break
            if ok and end - t >= min_confirm_s:
                t_back = t
                break
    if t_back is None:
        return dict(status=NOT_OBSERVED, **common,
                    note="never left the zero tolerance for min_confirm_s inside this window")
    out = dict(status="CONFIRMED_RECOVERY", **common,
               t_recovered_s=t_back, latency_s=round(t_back - t_event, 6))
    if reference_level:
        after = [v for t, v in zip(ts, vs) if t >= t_back]
        mean_after = sum(after) / len(after)
        out["mean_after_recovery"] = mean_after
        out["ratio_to_reference"] = (mean_after / reference_level) if reference_level else None
    return out


def first_change_descriptive(times, values, t_event: float, t_until: float,
                             pre_from: float, tolerance: float = 0.0) -> dict:
    """First sample after the event that differs from the pre-event sample - DESCRIPTIVE ONLY.

    The same test is run over the pre-event window. If the channel also changes there, its first
    post-event change is NOT evidence that the injection caused anything, and the result says so.
    This function never asserts causality; that is what `causality` records.
    """
    base = _last_before(times, values, t_event)
    if base is None:
        return dict(status=NO_BASELINE, searched_s=[t_event, t_until])

    def first_diff(t0, t1, ref):
        for t, v in zip(times, values):
            if t0 <= t < t1 and abs(v - ref) > tolerance:
                return t
        return None

    pre_base = _last_before(times, values, pre_from)
    changes_before = (first_diff(pre_from, t_event, pre_base[1]) is not None
                      if pre_base is not None else None)
    t_move = first_diff(t_event, t_until, base[1])

    common = dict(baseline_before_event=base[1], baseline_t_s=base[0],
                  searched_s=[t_event, t_until], tolerance=tolerance,
                  pre_window_s=[pre_from, t_event],
                  channel_also_changes_before_event=changes_before)
    if t_move is None:
        return dict(status=NOT_OBSERVED, **common,
                    causality="NOT_ESTABLISHED",
                    note="no change inside the window; this is the absence of an observation, "
                         "not a measurement of 'no reaction'")
    return dict(status="MEASURED", **common,
                t_first_change_s=t_move,
                latency_s=round(t_move - t_event, 6),
                causality="NOT_ESTABLISHED",
                note=("the channel also changes in the pre-event window, so this first change is "
                      "descriptive only and is NOT evidence of a reaction to the injection"
                      if changes_before else
                      "the channel is steady in the pre-event window and changes after the event; "
                      "still descriptive - causality is not established by timing alone"))


def sustained_transition(times, values, t_event: float, t_until: float,
                         predicate, min_hold_s: float) -> dict:
    """First time `predicate(value)` becomes true and STAYS true for min_hold_s."""
    ts, vs = _window(times, values, t_event, t_until)
    if not vs:
        return dict(status=NO_DATA, window_s=[t_event, t_until])
    for i, (t, v) in enumerate(zip(ts, vs)):
        if not predicate(v):
            continue
        end = t
        ok = True
        for t2, v2 in zip(ts[i:], vs[i:]):
            if not predicate(v2):
                ok = False
                break
            end = t2
            if end - t >= min_hold_s:
                break
        if ok and end - t >= min_hold_s:
            return dict(status="MEASURED", t_s=t, latency_s=round(t - t_event, 6),
                        min_hold_s=min_hold_s, window_s=[t_event, t_until])
    return dict(status=NOT_OBSERVED, min_hold_s=min_hold_s, window_s=[t_event, t_until],
                note="the condition was never held for min_hold_s inside the window")

# =============================================================================================
# PAS line edges, acceptance and observation resolution (REVIEW-EVD-AP-01-012, D012-01).
#
# Iteration 012 located the "electrical edge" in two mutually inconsistent ways:
#
#   * evaluate_criteria() scanned pas_transition_index for a decrease IN THE CSV AS SAMPLED. At
#     sample_ms=1 the CSV carries one row per 4 ticks, so the first OBSERVABLE decrease was
#     3.009 s while the real one is 3.00000 s. Inhibit was then searched from 3.009 onward and
#     "found" immediately, yielding "0.00 ms, same tick". Measured at sample_ticks=1 the real
#     latency is 0.75 ms. Worse: on the decimated run the true inhibit (3.001 s) is EARLIER than
#     the observed "edge" (3.009 s) - a negative latency that the search-from-edge silently hid.
#
#   * pas_edge_timing() used (pas_ab != pas_normal_ab), which is an OVERRIDE diagnostic. A clean
#     reverse has no override at all - pas_normal_ab follows the reversing crank - so that test
#     returns NOT_OBSERVED for exactly the case the other path claimed to measure.
#
# What follows is ONE detector, working on the real presented line, plus explicit observation
# resolution so a decimated record can never be reported as an exact instant.
# =============================================================================================

# PAS raw ring for PAS_DIR_SIGN=-1, same order as FWD_AB in controller_lab.c:
#   00 -> 10 -> 11 -> 01 -> 00   i.e. line values 0, 2, 3, 1
FWD_AB_RING = [0, 2, 3, 1]
_RING_POS = {v: i for i, v in enumerate(FWD_AB_RING)}

STEP_FORWARD = "FORWARD"
STEP_REVERSE = "REVERSE"
STEP_ILLEGAL = "ILLEGAL"      # two positions at once - ambiguous in direction


def classify_step(prev_ab: int, ab: int) -> str | None:
    """Direction of one quadrature step on the PRESENTED line, or None if unchanged."""
    if prev_ab == ab:
        return None
    if prev_ab not in _RING_POS or ab not in _RING_POS:
        return STEP_ILLEGAL
    d = (_RING_POS[ab] - _RING_POS[prev_ab]) % 4
    return {1: STEP_FORWARD, 3: STEP_REVERSE, 2: STEP_ILLEGAL}[d]


def sample_interval_s(times: Sequence[float]) -> float | None:
    """The record's sampling period, taken as the smallest gap actually present."""
    if len(times) < 2:
        return None
    gaps = [round(b - a, 9) for a, b in zip(times, times[1:]) if b > a]
    return min(gaps) if gaps else None


CTRL_HZ = 4000
FULL_RATE_S = 1.0 / CTRL_HZ


def is_full_rate(times: Sequence[float], tolerance: float = 1e-9) -> bool:
    """True when the record carries one row per control tick (sample_ticks=1)."""
    dt = sample_interval_s(times)
    return dt is not None and dt <= FULL_RATE_S + tolerance


def _observation(t_obs: float | None, t_prev: float | None, dt: float | None) -> dict:
    """An instant is only known to within one sampling interval: it lies in (t_prev, t_obs]."""
    if t_obs is None:
        return dict(status=NOT_OBSERVED, resolution_s=dt)
    return dict(status="MEASURED", t_observed_s=t_obs, t_prev_sample_s=t_prev,
                resolution_s=dt,
                earliest_possible_s=t_prev, latest_possible_s=t_obs,
                exact=(dt is not None and dt <= FULL_RATE_S + 1e-9))


def line_edge(times, pas_ab, t_from: float, t_to: float, want: str | None = STEP_REVERSE) -> dict:
    """First step of the requested kind on the PRESENTED line inside [t_from, t_to).

    `want=None` means any change. Works for a clean reverse (the line itself steps backwards) and
    for an override such as the bounce (the line is forced one position back) - it never looks at
    pas_normal_ab, which is an override diagnostic, not an edge detector.
    """
    dt = sample_interval_s(times)
    prev_ab = None
    prev_t = None
    for t, ab in zip(times, pas_ab):
        if t < t_from:
            prev_ab, prev_t = int(ab), t
            continue
        if t >= t_to:
            break
        if prev_ab is not None:
            step = classify_step(prev_ab, int(ab))
            if step is not None and (want is None or step == want):
                out = _observation(t, prev_t, dt)
                out["step"] = step
                out["from_ab"] = prev_ab
                out["to_ab"] = int(ab)
                return out
        prev_ab, prev_t = int(ab), t
    out = _observation(None, None, dt)
    out["note"] = (f"no {want or 'line'} step on the presented line inside the window; on a "
                   f"decimated record a step can also be INVISIBLE - see resolution_s")
    return out


def counter_first_increase(times, values, t_from: float, t_to: float) -> dict:
    """First increase of a cumulative counter - used for 'the sampler ACCEPTED the edge'."""
    dt = sample_interval_s(times)
    prev, prev_t = None, None
    for t, v in zip(times, values):
        if t < t_from:
            prev, prev_t = float(v), t
            continue
        if t >= t_to:
            break
        if prev is not None and float(v) > prev:
            out = _observation(t, prev_t, dt)
            out["from_value"] = prev
            out["to_value"] = float(v)
            return out
        prev, prev_t = float(v), t
    return _observation(None, None, dt)


def condition_first_true(times, values, t_from: float, t_to: float, predicate) -> dict:
    """First sample satisfying `predicate` - used for 'inhibit became non-zero'."""
    dt = sample_interval_s(times)
    prev_t = None
    for t, v in zip(times, values):
        if t < t_from:
            prev_t = t
            continue
        if t >= t_to:
            break
        if predicate(v):
            return _observation(t, prev_t, dt)
        prev_t = t
    return _observation(None, None, dt)


def latency_between(first: dict, second: dict) -> dict:
    """Latency from `first` to `second`, carrying the uncertainty of BOTH observations.

    first in (a_prev, a_obs], second in (b_prev, b_obs]  =>  latency in (b_prev - a_obs,
    b_obs - a_prev). A negative lower bound is not hidden: it means the record cannot even
    establish the ORDER of the two events, which is exactly what iteration 012 reported as
    "0.00 ms, same tick".
    """
    if first.get("status") != "MEASURED" or second.get("status") != "MEASURED":
        return dict(status=NOT_OBSERVED,
                    reason="one of the two instants was not observed in this record")
    a_obs, a_prev = first["t_observed_s"], first.get("t_prev_sample_s")
    b_obs, b_prev = second["t_observed_s"], second.get("t_prev_sample_s")
    nominal = b_obs - a_obs
    lower = (b_prev - a_obs) if b_prev is not None else nominal
    upper = (b_obs - a_prev) if a_prev is not None else nominal
    dt = first.get("resolution_s") or second.get("resolution_s")
    exact = bool(first.get("exact") and second.get("exact"))
    return dict(
        status="MEASURED",
        nominal_s=round(nominal, 9),
        lower_bound_s=round(lower, 9),
        upper_bound_s=round(upper, 9),
        resolution_s=dt,
        exact=exact,
        order_established=bool(lower >= 0.0),
        note=("both instants are resolved to a single control tick" if exact else
              "the record is decimated: the true instants lie within one sampling interval of "
              "the observed ones, so only the bounds are meaningful"),
    )

