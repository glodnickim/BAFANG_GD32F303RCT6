"""AP-01 disturbances-001 analysis: applies the ACCEPTED metrics to this directory's own manifest.

Reads ONLY ./results/disturbances-results.json, verifies run_complete, re-hashes every CSV on disk
before using it, and writes ./results/disturbances-metrics.json.

What changed after REVIEW-EVD-AP-01-010 (D010-05):

*  The event key is taken from the CASE'S OWN `commanded_event_times_s`. The previous version
   looked up hard-coded keys ("invalid_start", "timebase_start") that no scenario ever emits, so
   every invalid/timebase case would have raised KeyError.
*  Every group is handled, including the control, PAS-edge and hi-res groups. A group that is not
   handled is recorded as such instead of being silently dropped.
*  The three things the card asks to keep apart are kept apart per event:
     INPUT      - when the disturbance was commanded (from the scenario);
     OBSERVED   - when a production state actually moved (searched in the data, per channel);
     RECOVERY   - what happened after the input returned to normal, including peaks.
   A channel that never moves inside its search window is NOT_OBSERVED, which is reported as a
   distinct status from "moved to zero". Search windows end at the next event.

Nothing here re-implements production maths. `metrics` / `event_metrics` are the accepted modules,
loaded by explicit path (accepted_modules.py) so no sys.path entry can redirect them.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent
sys.path.insert(0, str(HERE))

import disturbance_metrics_primitives as metrics  # noqa: E402 - accepted primitives, by path
import disturbance_metrics as em  # noqa: E402 - accepted event primitives, by path
import disturbance_measures as dm  # noqa: E402 - measurement semantics, unit-tested
from scenarios import scenarios as scn  # noqa: E402

RESULTS = HERE / "results"
MANIFEST = RESULTS / "disturbances-results.json"
OUT = RESULTS / "disturbances-metrics.json"

REQUIRED_REGRESSION_NAMES = sorted(r["name"] for r in scn.REGRESSION_CASES)
REQUIRED_CASE_NAMES = sorted([c["name"] for c in scn.all_cases()] + REQUIRED_REGRESSION_NAMES)
MIN_REPEATS = 2

IQ_CHAIN = ["iq_before_profile_limit", "iq_mode_request", "iq_requested", "iq_allowed", "iq_ref"]
REPORTED_CHANNELS = ["torque_run_native", "cadence_control_rpm", *IQ_CHAIN]

# Production state channels whose FIRST movement after an event is the observed reaction.
STATE_CHANNELS = [
    "session", "debug_flags", "gate_steps", "pas_direction_state", "pas_inhibit_reason",
    "pas_fwd_run", "pas_rev_run", "pas_backpedal_confirmed",
]

PRE_S = 1.0
SETTLE_S = 1.0
POST_S = 1.5

ZERO_TOL = em.DEFAULT_ZERO_TOLERANCE
MIN_CONFIRM_S = em.DEFAULT_MIN_CONFIRM_S
# A control case is the reference a disturbed case is judged against: same cadence, same
# speed, same duration family, no disturbance key at all.
CONTROL_GROUPS = ("control_8s", "control_6s", "control_hires")


class ManifestRejected(RuntimeError):
    pass


def load_manifest() -> dict:
    m = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if not m.get("run_complete") or m.get("attempt_state") != "COMPLETE":
        raise ManifestRejected(
            f"manifest is not a complete run (run_id={m.get('run_id')}): "
            f"run_complete={m.get('run_complete')} attempt_state={m.get('attempt_state')}")

    cases = m.get("cases", [])
    names_seen = [c.get("name") for c in cases]
    dupes = sorted({n for n in names_seen if names_seen.count(n) > 1})
    if dupes:
        raise ManifestRejected(f"manifest contains duplicate case names: {dupes}")
    by_name = {c["name"]: c for c in cases}

    missing = [n for n in REQUIRED_CASE_NAMES if n not in by_name]
    if missing:
        raise ManifestRejected(f"required trials missing from manifest: {missing}")

    declared_repeats = int(m.get("repeats_per_case") or 0)
    required_repeats = max(MIN_REPEATS, declared_repeats)

    not_ok, not_deterministic, no_hash, too_few_repeats, bad_contract = [], [], [], [], []
    for n in REQUIRED_CASE_NAMES:
        c = by_name[n]
        if c.get("status") != "OK":
            not_ok.append((n, c.get("status")))
            continue
        if not c.get("deterministic_repeat"):
            not_deterministic.append(n)
        if not c.get("csv_contract_ok"):
            bad_contract.append(n)
        if not c.get("csv_sha256") or not c.get("repeat_csv_sha256"):
            no_hash.append(n)
            continue
        if len(c["repeat_csv_sha256"]) < required_repeats:
            too_few_repeats.append((n, len(c["repeat_csv_sha256"]), required_repeats))
        elif (len(set(c["repeat_csv_sha256"])) != 1
              or c["csv_sha256"] != c["repeat_csv_sha256"][0]):
            no_hash.append(n)
    if too_few_repeats:
        raise ManifestRejected(
            f"required trials record fewer repeat hashes than the run's own repeats_per_case "
            f"contract (name, recorded, required): {too_few_repeats}")
    if not_ok:
        raise ManifestRejected(
            f"required trials are not status OK (identity/completeness check): {not_ok}")
    if not_deterministic:
        raise ManifestRejected(
            f"required trials lack proven repeat-agreement despite OK status: {not_deterministic}")
    if bad_contract:
        raise ManifestRejected(
            f"required trials whose CSV did not pass the column contract: {bad_contract}")
    if no_hash:
        raise ManifestRejected(
            f"required trials missing or have inconsistent repeat hashes: {no_hash}")

    for c in cases:
        p = RESULTS / c["csv_file"]
        actual = hashlib.sha256(p.read_bytes()).hexdigest()
        if actual != c["csv_sha256"]:
            raise ManifestRejected(
                f"{c['csv_file']}: on-disk sha256 {actual} != manifest {c['csv_sha256']}")

    regr = m.get("regression", [])
    regr_names_seen = [r.get("case") for r in regr]
    regr_dupes = sorted({n for n in regr_names_seen if regr_names_seen.count(n) > 1})
    if regr_dupes:
        raise ManifestRejected(f"manifest contains duplicate regression case names: {regr_dupes}")
    regr_by_name = {r["case"]: r for r in regr}
    regr_missing = [n for n in REQUIRED_REGRESSION_NAMES if n not in regr_by_name]
    if regr_missing:
        raise ManifestRejected(f"required regression cases missing from manifest: {regr_missing}")
    regr_bad, regr_unlinked = [], []
    for n in REQUIRED_REGRESSION_NAMES:
        r = regr_by_name[n]
        if not r.get("pre_existing_columns_identical"):
            regr_bad.append(n)
        if not r.get("accepted_csv_sha256") or not r.get("new_csv_sha256"):
            regr_bad.append(n)
        run = by_name.get(n)
        if run is None:
            regr_unlinked.append((n, "no run of this name in cases[]"))
        elif r.get("new_csv_sha256") != run.get("csv_sha256"):
            regr_unlinked.append((n, f"entry new_csv_sha256={str(r.get('new_csv_sha256'))[:16]}... "
                                     f"!= run csv_sha256={str(run.get('csv_sha256'))[:16]}..."))
    if regr_unlinked:
        raise ManifestRejected(f"regression entries not linked to their own runs: {regr_unlinked}")
    if regr_bad:
        raise ManifestRejected(
            f"required regression cases failed identity/hash check: {sorted(set(regr_bad))}")
    computed = not regr_bad and not regr_missing
    if bool(m.get("regression_all_identical")) != computed:
        raise ManifestRejected(
            f"manifest's regression_all_identical={m.get('regression_all_identical')} disagrees "
            f"with recomputed value {computed}")

    return m


# ---------------------------------------------------------------------------------------------

def steady_stage_analysis(cols, bounds) -> dict | None:
    if bounds is None:
        return None
    cw = {}
    for ch in IQ_CHAIN:
        _, vs = metrics.window(cols["time_s"], cols[ch], bounds[0], bounds[1])
        if vs:
            cw[ch] = vs
    if not cw:
        return None
    return metrics.stage_analysis(cw)


def channel_block(cols, win) -> dict:
    t = cols["time_s"]
    out = {}
    stages = steady_stage_analysis(cols, win["post"])
    for ch in REPORTED_CHANNELS:
        v = cols[ch]
        pre = em.window_stats(t, v, win["pre"])
        post = em.window_stats(t, v, win["post"])
        block = dict(pre=pre, post=post)
        if pre and post:
            block["mean_delta"] = post["mean"] - pre["mean"]
        block["limit_assessment_post"] = (
            metrics.limit_assessment(metrics.window(t, v, *win["post"])[1], channel=ch,
                                     stages=stages)
            if post else None)
        out[ch] = block
    return out


def observed_state_transitions(cols, t_event: float, t_until: float, pre_from: float) -> dict:
    """First post-event change per channel - DESCRIPTIVE, never causal (D011-03).

    Each entry also says whether the channel already changes in the pre-event window. For a
    rippling channel such as `iq_ref` it does, so its "first change after the event" is the next
    sample and proves nothing about the injection. That is now stated in the data rather than left
    for the reader to infer.
    """
    out = {}
    for ch in STATE_CHANNELS + ["iq_ref", "iq_allowed", "iq_requested"]:
        if ch not in cols:
            out[ch] = dict(status=dm.CHANNEL_ABSENT)
            continue
        tol = ZERO_TOL if ch.startswith("iq_") else 0.0
        out[ch] = dm.first_change_descriptive(cols["time_s"], cols[ch], t_event, t_until,
                                              pre_from, tolerance=tol)
    return out


def zero_and_recovery(cols, t_event: float, t_until: float) -> dict:
    """Extinction and recovery of iq_ref, with the four answers kept apart (D011-03).

    `ALREADY_ZERO` / `CONFIRMED_ZERO` / `UNCONFIRMED_ZERO` / `NOT_OBSERVED` are different results.
    No latency is reported for a window that started at zero.
    """
    t, v = cols["time_s"], cols["iq_ref"]
    return dict(
        zero_tolerance=ZERO_TOL,
        min_confirm_s=MIN_CONFIRM_S,
        iq_ref_zero=dm.zero_assessment(t, v, t_event, t_until, ZERO_TOL, MIN_CONFIRM_S),
        iq_ref_recovery=dm.recovery_assessment(t, v, t_event, t_until, ZERO_TOL, MIN_CONFIRM_S),
        iq_ref_excursions=em.excursions(t, v, t_event, t_until),
        note=("'excursions' are raw min/max of the exported samples over the window - they are "
              "descriptive and are not a claim about a reaction"),
    )


def pas_edge_timing(cols, t_command: float, t_until: float) -> dict:
    """The FOUR instants of a PAS event, measured once and used everywhere (D012-01).

        COMMAND     - the scenario said so; an input, never a measurement
        LINE        - the presented line actually stepped backwards (dm.line_edge)
        ACCEPTED    - a production sampler counter moved
        INHIBIT     - pas_inhibit_reason became non-zero

    Every instant carries the record's sampling resolution and the interval it is known to lie in.
    Iteration 012 used two different, mutually inconsistent notions of "electrical edge" here and
    in evaluate_criteria(); this function is now the single source for both.
    """
    t = cols["time_s"]
    dt = dm.sample_interval_s(t)
    out = dict(
        command_time_s=t_command,
        window_s=[t_command, t_until],
        sample_interval_s=dt,
        full_rate=dm.is_full_rate(t),
        resolution_note=("one row per control tick - instants are exact to a single 4 kHz tick"
                         if dm.is_full_rate(t) else
                         f"the record is decimated to {dt} s per row, so every instant below is "
                         f"only known to within one sampling interval; an event shorter than that "
                         f"can be invisible entirely"),
    )
    # LINE: a reverse step on the presented line. Deliberately NOT (pas_ab != pas_normal_ab):
    # that is an override diagnostic and is identically zero for a clean reverse.
    out["line_reverse_edge"] = dm.line_edge(t, cols["pas_ab"], t_command, t_until,
                                            want=dm.STEP_REVERSE)
    out["line_any_change"] = dm.line_edge(t, cols["pas_ab"], t_command, t_until, want=None)
    out["line_illegal_step"] = dm.line_edge(t, cols["pas_ab"], t_command, t_until,
                                            want=dm.STEP_ILLEGAL)
    # ACCEPTED: what the production sampler made of it.
    out["accepted"] = {ch: dm.counter_first_increase(t, cols[ch], t_command, t_until)
                       for ch in ("pas_sampler_reverse", "pas_sampler_glitch",
                                  "pas_sampler_invalid", "pas_sampler_forward")
                       if ch in cols}
    # INHIBIT: the supervisory response.
    if "pas_inhibit_reason" in cols:
        out["inhibit"] = dm.condition_first_true(t, cols["pas_inhibit_reason"],
                                                 t_command, t_until, lambda x: x != 0)
        out["line_edge_to_inhibit"] = dm.latency_between(out["line_reverse_edge"], out["inhibit"])
        if "pas_sampler_reverse" in out["accepted"]:
            out["line_edge_to_accepted"] = dm.latency_between(
                out["line_reverse_edge"], out["accepted"]["pas_sampler_reverse"])
    out["note"] = ("command / line / accepted / inhibit are four different instants; P-1 is "
                   "stated against the LINE edge and is only exact on a full-rate record")
    return out


def analyse_event(case, cols, wins, ev_name: str, next_time: float | None) -> dict:
    t = cols["time_s"]
    w = wins[ev_name]
    te = w["event_time_s"]
    until = next_time if next_time is not None else t[-1]
    res = dict(
        event=ev_name,
        commanded_input_time_s=te,
        commanded_note=("INPUT time from the scenario; every 'observed' figure below is searched "
                        "in the data and is not assumed equal to it"),
        search_until_s=until,
        windows=w,
        channels=channel_block(cols, w),
        observed_state_transitions=observed_state_transitions(cols, te, until,
                                                            pre_from=w["pre"][0]),
        iq_zero_and_recovery=zero_and_recovery(cols, te, until),
        pas_edge_timing=pas_edge_timing(cols, te, until),
    )
    res["step_response"] = {
        ch: em.step_response(t, cols[ch], te, w["pre"], w["post"], search_until=until)
        for ch in ("torque_run_native", "iq_ref", "iq_allowed")
    }
    return res


def generator_check(case, cols) -> dict:
    """Check the GENERATOR's own output, not the production reaction to it."""
    args = case["effective_args"]
    start = float(args["ride_start_s"])
    expect = {}
    # A settled stretch after the ride starts and before the first commanded event.
    events = sorted(case["commanded_event_times_s"].values())
    first_event = events[0] if events else cols["time_s"][-1]
    lo, hi = start + 0.5, first_event
    if hi > lo + 0.05:
        expect["settled_before_first_event"] = dict(
            window_s=[lo, hi], torque_cmd_mean_nm=float(args["torque"]), ride_interval=1,
            cadence_gen_rpm_mean=float(args["cadence"]),
            cadence_tolerance=max(0.5, 0.02 * float(args["cadence"])))
    if not expect:
        return dict(status="NO_SETTLED_WINDOW_BEFORE_FIRST_EVENT")
    return em.commanded_input_check(cols, expect)


def timebase_facts(cols) -> dict:
    """What the run actually did to the timebase, read from the latched counters."""
    last = {k: cols[k][-1] for k in ("fg_skips_total", "fg_resume_count", "fg_last_resume_tick",
                                     "fg_last_resume_elapsed", "fast_iq_slew_ticks",
                                     "pas_edges_dropped", "pas_edges_deferred")}
    steps = {round(cols["fast_iq_slew_ticks"][i + 1] - cols["fast_iq_slew_ticks"][i])
             for i in range(len(cols["fast_iq_slew_ticks"]) - 1)}
    max_elapsed = max(cols["elapsed_ticks"])
    return dict(
        foreground_ticks_skipped=int(last["fg_skips_total"]),
        foreground_resumes=int(last["fg_resume_count"]),
        last_resume_tick=int(last["fg_last_resume_tick"]),
        elapsed_handed_to_module_on_last_resume=int(last["fg_last_resume_elapsed"]),
        max_elapsed_ticks_seen=int(max_elapsed),
        pas_edges_dropped=int(last["pas_edges_dropped"]),
        pas_edges_deferred=int(last["pas_edges_deferred"]),
        fast_iq_slew_ticks_total=int(last["fast_iq_slew_ticks"]),
        fast_iq_slew_steps_between_rows=sorted(steps),
        fast_iq_owner_ran_throughout=(0 not in steps),
        note=("the 16 kHz Iq owner is driven from the same loop as the 4 kHz foreground; a stall "
              "of the foreground does not stop it, which is what fast_iq_slew_ticks shows"),
    )


# ---------------------------------------------------------------------------------------------
# Programmatic evaluation of the PROPOSED criteria (REVIEW-EVD-AP-01-011, D011-02).
#
# Iteration 011 asserted "the current code meets P-2" from a glance at the maximum and the absence
# of a zero. It does not: session goes 1 -> 2 -> 1 and Iq dips well below the proposed floor. So
# the criteria are now evaluated HERE, from the data, against the matching control run, and the
# verdict is whatever comes out. A criterion that cannot be evaluated is NOT_EVALUATED, never an
# implied pass, and a FAIL is reported as a FAIL - thresholds are not moved to obtain one.
# ---------------------------------------------------------------------------------------------

P1_INHIBIT_MAX_S = 0.025      # from the electrical edge, not from the scenario command time
P2_IQ_FLOOR_RATIO = 0.80      # of the control mean over the shared window
P6_MEAN_TOLERANCE = 0.10      # +/- of the control mean
P6_PEAK_RATIO = 1.25          # of the control maximum


def _control_name(case: dict, by_name: dict) -> str | None:
    """The control run with the same cadence, speed and duration family."""
    a = case["effective_args"]
    cad, spd, dur = a["cadence"], a["speed"], float(a["duration"])
    tag = {8.0: "control_8s", 6.0: "control_6s", 2.0: "hires_control"}.get(dur)
    if tag is None:
        return None
    name = (f"hires_control_cad{cad}_speed{spd}" if tag == "hires_control"
            else f"{tag}_cad{cad}_speed{spd}")
    return name if name in by_name else None


def _mean(xs):
    return sum(xs) / len(xs) if xs else None


def _win(cols, lo, hi, ch="iq_ref"):
    return [v for t, v in zip(cols["time_s"], cols[ch]) if lo <= t < hi]


def evaluate_criteria(case: dict, cols: dict, ctrl_cols: dict | None, ctrl_name: str | None) -> dict:
    """Evaluate P-1 / P-2 / P-6 for one case. Returns a verdict per criterion."""
    out = {}
    group = case["group"]
    events = dict(case["commanded_event_times_s"])
    args = case["effective_args"]
    t = cols["time_s"]

    # ---- P-1: inhibit within 25 ms of the LINE edge -----------------------------------------
    #
    # D012-01: this used to scan pas_transition_index for a decrease IN THE SAMPLED CSV and then
    # search for inhibit FROM that sample onward, so a decimated record produced "0.00 ms, same
    # tick" - and on reverse_clean_cad72_speed18 the observed "edge" (3.009 s) was actually LATER
    # than the inhibit it was credited with causing (3.001 s). P-1 is now evaluated only where the
    # instants are genuinely resolvable, and uses the same pas_edge_timing() the report shows.
    first_ev = min(events.values()) if events else None
    if group not in ("reverse_clean", "reverse_bounce", "reverse_bounce_hires",
                     "reverse_clean_hires") or first_ev is None:
        out["P-1"] = dict(verdict="NOT_EVALUATED",
                          reason="no reverse line edge is expected in this group; P-1 is stated "
                                 "for a reverse edge only")
    else:
        until = min([v for v in events.values() if v > first_ev] + [t[-1]])
        timing = pas_edge_timing(cols, first_ev, until)
        edge = timing["line_reverse_edge"]
        inhibit = timing.get("inhibit", {})
        lat = timing.get("line_edge_to_inhibit", {})
        common = dict(command_time_s=first_ev,
                      sample_interval_s=timing["sample_interval_s"],
                      full_rate=timing["full_rate"],
                      line_reverse_edge=edge, inhibit=inhibit, latency=lat,
                      threshold_s=P1_INHIBIT_MAX_S)
        if not timing["full_rate"]:
            out["P-1"] = dict(
                verdict="NOT_EVALUATED", **common,
                reason=(f"the record is decimated to {timing['sample_interval_s']} s per row. "
                        f"The line edge and the inhibit are separated by well under that, so this "
                        f"record cannot resolve the latency - and cannot even establish the order "
                        f"of the two events. Use the sample_ticks=1 case of the same scenario."))
        elif edge.get("status") != "MEASURED" or inhibit.get("status") != "MEASURED":
            out["P-1"] = dict(verdict="NOT_EVALUATED", **common,
                              reason="the line edge or the inhibit was not observed in the window")
        else:
            # The verdict answers the THRESHOLD question conservatively, from the upper bound.
            # Sub-tick ordering is reported separately: when the line edge and the inhibit fall in
            # the same 4 kHz tick, the record is at its finest possible resolution (one row per
            # control tick, which is also the rate the production foreground itself runs at), so
            # "which came first inside that tick" has no observable answer here. That is stated,
            # not silently turned into an exact 0.00 ms as in iteration 012.
            same_tick = not lat.get("order_established")
            dt_rec = timing["sample_interval_s"] or 0.0
            if lat["upper_bound_s"] < 0.0:
                # The inhibit DEFINITIVELY precedes the line edge - it cannot be a response to it.
                verdict = "FAIL"
            elif lat["upper_bound_s"] <= P1_INHIBIT_MAX_S:
                verdict = "PASS"
            elif lat["lower_bound_s"] > P1_INHIBIT_MAX_S:
                verdict = "FAIL"
            else:
                verdict = "NOT_EVALUATED"
            out["P-1"] = dict(
                verdict=verdict, **common,
                latency_s=lat["nominal_s"],
                latency_bounds_s=[lat["lower_bound_s"], lat["upper_bound_s"]],
                sub_tick_order_resolved=not same_tick,
                latency_within_one_control_tick=bool(abs(lat["nominal_s"]) <= dt_rec + 1e-12),
                reason=("the inhibit precedes the line edge outright, so it is not a response "
                        "to it" if lat["upper_bound_s"] < 0.0 else
                        "the verdict is taken from the UPPER bound of the latency, so it holds "
                        "whatever the unresolved sub-tick order is"),
                note=("the line edge and the inhibit fall in the SAME 4 kHz control tick: the "
                      f"latency is bounded by one tick ({dt_rec} s) and the order inside that "
                      "tick is not observable at the production timebase - reported as a bound, "
                      "never as an exact 0.00 ms"
                      if same_tick else
                      "latency measured from the LINE edge on a full-rate record, not from the "
                      "scenario command time"))

    # ---- P-2: a single short bounce must not cut assist --------------------------------------
    if group not in ("reverse_bounce", "reverse_bounce_hires"):
        out["P-2"] = dict(verdict="NOT_EVALUATED", reason="P-2 applies to the bounce group only")
    elif ctrl_cols is None:
        out["P-2"] = dict(verdict="NOT_EVALUATED",
                          reason="no matching control run for this cadence/speed/duration")
    else:
        te = events["reverse_bounce"]
        lo, hi = te, min(te + 1.0, t[-1])
        a, b = _win(cols, lo, hi), _win(ctrl_cols, lo, hi)
        sess = _win(cols, lo, hi, "session")
        sess_before = [v for tt, v in zip(t, cols["session"]) if tt < te]
        sess_base = sess_before[-1] if sess_before else None
        sess_changed = any(v != sess_base for v in sess)
        ctrl_mean = _mean(b)
        ratio = (min(a) / ctrl_mean) if (a and ctrl_mean) else None
        ok = bool(a and b and not sess_changed and ratio is not None
                  and ratio >= P2_IQ_FLOOR_RATIO)
        out["P-2"] = dict(
            verdict="PASS" if ok else "FAIL",
            control_case=ctrl_name, window_s=[lo, hi],
            session_before=sess_base,
            session_values_in_window=sorted(set(sess)),
            session_unchanged=not sess_changed,
            iq_ref_min=min(a) if a else None,
            control_mean=ctrl_mean,
            min_over_control_mean=ratio,
            floor_ratio=P2_IQ_FLOOR_RATIO,
            note=("a dip is a dip whether or not it reaches zero; 'iq_ref never hit zero' is not "
                  "evidence that assist was not cut"))

    # ---- P-6: recovery back to the control level, without overshoot --------------------------
    end_events = {k: v for k, v in events.items() if k.endswith("_end")}
    if not end_events:
        out["P-6"] = dict(verdict="NOT_EVALUATED",
                          reason="this case has no recovery event (nothing returns to normal)")
    elif ctrl_cols is None:
        out["P-6"] = dict(verdict="NOT_EVALUATED",
                          reason="no matching control run for this cadence/speed/duration")
    else:
        te = max(end_events.values())
        lo = te + SETTLE_S
        hi = t[-1]
        a, b = _win(cols, lo, hi), _win(ctrl_cols, lo, hi)
        if not a or not b:
            out["P-6"] = dict(verdict="NOT_EVALUATED", control_case=ctrl_name,
                              window_s=[lo, hi],
                              reason="the settled post-recovery window is empty in the case or "
                                     "the control - the run is too short after the event")
        else:
            ma, mb = _mean(a), _mean(b)
            mean_ratio = ma / mb if mb else None
            peak_ratio = max(a) / max(b) if max(b) else None
            ok = (mean_ratio is not None and abs(mean_ratio - 1.0) <= P6_MEAN_TOLERANCE
                  and peak_ratio is not None and peak_ratio <= P6_PEAK_RATIO)
            out["P-6"] = dict(
                verdict="PASS" if ok else "FAIL",
                control_case=ctrl_name, window_s=[lo, hi],
                mean=ma, control_mean=mb, mean_ratio=mean_ratio,
                mean_tolerance=P6_MEAN_TOLERANCE,
                peak=max(a), control_peak=max(b), peak_ratio=peak_ratio,
                peak_ratio_limit=P6_PEAK_RATIO)
    return out


def main() -> None:
    m = load_manifest()
    result = dict(
        tool="analyze.py",
        source_manifest=MANIFEST.name,
        source_run_id=m["run_id"],
        qualification=m["qualification"],
        measurement_contract=dict(
            pre_window_s=PRE_S, settle_s=SETTLE_S, post_window_s=POST_S,
            zero_tolerance=em.DEFAULT_ZERO_TOLERANCE,
            min_confirm_s=em.DEFAULT_MIN_CONFIRM_S,
            max_transition_gap_s=em.DEFAULT_MAX_TRANSITION_GAP_S,
            note=("windows are clipped by the neighbouring event and never cross a stop, restart "
                  "or step; a state that does not move inside its window is NOT_OBSERVED, which "
                  "is reported separately from a measured zero"),
        ),
        groups={},
        cases={},
        unhandled_groups=[],
    )

    by_name = {c["name"]: c for c in m["cases"]}
    cols_cache: dict = {}

    def columns(case_name):
        if case_name not in cols_cache:
            cols_cache[case_name] = em.read_csv(RESULTS / by_name[case_name]["csv_file"])
        return cols_cache[case_name]

    for case in m["cases"]:
        name = case["name"]
        group = case["group"]
        result["groups"].setdefault(group, []).append(name)
        cols = columns(name)
        entry = dict(group=group, purpose=case.get("purpose"),
                     effective_args=case["effective_args"],
                     csv_sha256=case["csv_sha256"],
                     timebase=timebase_facts(cols))

        if group == "regression_ac_t2":
            entry["regression"] = "identity check is in disturbances-results.json"
            result["cases"][name] = entry
            continue

        entry["generator_check"] = generator_check(case, cols)

        events = sorted(case["commanded_event_times_s"].items(), key=lambda kv: kv[1])
        if not events:
            # Control case: no event, so report the settled behaviour of the whole ride.
            t = cols["time_s"]
            lo = float(case["effective_args"]["ride_start_s"]) + 1.0
            entry["control_steady"] = {
                ch: em.window_stats(t, cols[ch], [lo, t[-1]]) for ch in REPORTED_CHANNELS
            }
            entry["events"] = {}
            result["cases"][name] = entry
            continue

        wins = em.event_windows(events, cols["time_s"][-1], PRE_S, SETTLE_S, POST_S)
        per_event = {}
        for i, (ev_name, _te) in enumerate(events):
            nxt = events[i + 1][1] if i + 1 < len(events) else None
            per_event[ev_name] = analyse_event(case, cols, wins, ev_name, nxt)
        entry["events"] = per_event
        ctrl_name = _control_name(case, by_name)
        ctrl_cols = columns(ctrl_name) if ctrl_name else None
        entry["criteria"] = evaluate_criteria(case, cols, ctrl_cols, ctrl_name)
        result["cases"][name] = entry

    handled = {"regression_ac_t2", "control_8s", "control_6s", "reverse_clean", "reverse_bounce",
               "torque_invalid", "pas_invalid", "pas_invalid_seq", "pas_edge_drop",
               "pas_edge_jitter", "miss_tick", "fg_delay", "fg_delay_hires",
               "control_hires", "reverse_bounce_hires", "reverse_clean_hires"}
    result["unhandled_groups"] = sorted(set(result["groups"]) - handled)

    roll: dict = {}
    for name, entry in result["cases"].items():
        for crit, verdict in (entry.get("criteria") or {}).items():
            b = roll.setdefault(crit, dict(PASS=[], FAIL=[], NOT_EVALUATED=[]))
            b[verdict["verdict"]].append(name)
    result["criteria_summary"] = {
        c: dict(PASS=len(b["PASS"]), FAIL=len(b["FAIL"]),
                NOT_EVALUATED=len(b["NOT_EVALUATED"]), failing_cases=sorted(b["FAIL"]))
        for c, b in sorted(roll.items())}

    OUT.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"Analysis written to {OUT}")
    print(f"  cases analysed : {len(result['cases'])}")
    print(f"  groups         : {', '.join(sorted(result['groups']))}")
    print(f"  unhandled      : {result['unhandled_groups'] or 'none'}")
    for c, b in result["criteria_summary"].items():
        print(f"  {c}: PASS={b['PASS']} FAIL={b['FAIL']} "
              f"NOT_EVALUATED={b['NOT_EVALUATED']}"
              + (f"  failing: {b['failing_cases'][:4]}" if b["failing_cases"] else ""))
    if result["unhandled_groups"]:
        raise SystemExit(f"unhandled scenario groups: {result['unhandled_groups']}")


if __name__ == "__main__":
    main()
