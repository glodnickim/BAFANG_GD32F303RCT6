"""AP-01 transients-001 analysis: applies event_metrics.py to this directory's own manifest.

Reads ONLY ./results/transients-results.json, verifies run_complete and re-hashes every CSV on
disk before using it (same guard as the accepted extract_metrics.py). Writes
./results/transients-metrics.json. Nothing under ../results/ is touched.

What is deliberately NOT done here: no channel gets a verdict from a stage it does not sit
downstream of, no missing crossing becomes a zero, and no restart is called "during release"
because its scripted gap was short - only iq_state_at_restart, read from the data, decides that.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent
sys.path.insert(0, str(AP01))
sys.path.insert(0, str(HERE))

import metrics  # noqa: E402 - accepted primitives
import event_metrics as em  # noqa: E402
from scenarios import scenarios as scn  # noqa: E402

RESULTS = HERE / "results"
MANIFEST = RESULTS / "transients-results.json"
# The original transients-metrics.json (EXEC-EVD-AP-01-005) carried the circular T005-01 verdicts
# and is kept as historical evidence, untouched. The rework's corrected analysis goes to a NEW file
# under rework-001/ with its own run_id/source-hash provenance (TASK-EVD-AP-01-TRANSIENTS-REWORK-001
# WRITE_SCOPE: "Poprawione metryki zapisz oddzielnie z run_id i hashami źródła").
# rework-002 per TASK-EVD-AP-01-TRANSIENTS-REWORK-002 ("nowe pochodne metryki i dowody w
# rework-002/"). rework-001/ is now READ_ONLY historical evidence for REVIEW-EVD-AP-01-006, exactly
# as results/transients-metrics.json is for EXEC-005.
REWORK_DIR = HERE / "rework-002"
OUT = REWORK_DIR / "transients-metrics-rework-002.json"

# Identity of every trial/regression this analysis REQUIRES, computed from the same scenario
# definitions the runner uses - not retyped, so the two cannot silently drift apart
# (REVIEW-EVD-AP-01-005, T005-02: identity/completeness, not just group counts).
#
# REVIEW-EVD-AP-01-006 (T006-02): scn.all_cases() is step+restart cases ONLY. The four AC-T2
# regression trials are real runs too (group regression_ac_t2) and were therefore never checked by
# name - the reviewer set regr_linear72_baseline's status to ERROR, left every hash intact, and the
# manifest was accepted. A regression run is a required trial like any other.
REQUIRED_REGRESSION_NAMES = sorted(r["name"] for r in scn.REGRESSION_CASES)
REQUIRED_CASE_NAMES = sorted([c["name"] for c in scn.all_cases()] + REQUIRED_REGRESSION_NAMES)
# The manifest declares its own repeat contract in `repeats_per_case`; this is the floor a run must
# meet regardless. A one-element hash list is trivially "all equal" and proves no repetition at all.
MIN_REPEATS = 2

IQ_CHAIN = ["iq_before_profile_limit", "iq_mode_request", "iq_requested", "iq_allowed", "iq_ref"]
REPORTED_CHANNELS = ["torque_run_native", "cadence_control_rpm", *IQ_CHAIN]

# Measurement-contract window lengths (parameters of the measurement, not firmware constants).
PRE_S = 1.0
# 1.0 s settle allowance: the accepted level preset carries iq_fall_slow_ms=525 and
# power_fall_filter_ms=450, so a 0.5 s allowance still leaves the FALLING step mid-transient and the
# "after" window is then not a steady reference. Chosen from the measured fall latency, not tuned to
# flatter a result - it is applied identically to every case, rising and falling alike.
SETTLE_S = 1.0
POST_S = 1.5
# Windows used to look for the restart reaction and the pre-restart stop. Not clipped by hand:
# event_windows() bounds every window with the neighbouring event.


class ManifestRejected(RuntimeError):
    pass


def load_manifest() -> dict:
    """Load and VALIDATE the manifest identity/completeness, not just its run_complete flag.

    REVIEW-EVD-AP-01-005 (T005-02): a reviewer could hand-edit one trial's status to ERROR while
    leaving hashes/flags on every other field intact, and the previous loader accepted the copy
    because it only checked run_complete/attempt_state and re-hashed the CSVs that were still
    listed as cases - it never checked WHICH names were present, whether a required name was
    missing, duplicated, or present but not OK. Every check below is on the trial/regression NAME,
    not merely a group count, and any contradiction is a hard rejection (never a silent downgrade
    or fallback to stale output).
    """
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

    # The repeat contract the run itself declares; MIN_REPEATS is the floor either way.
    declared_repeats = int(m.get("repeats_per_case") or 0)
    required_repeats = max(MIN_REPEATS, declared_repeats)

    not_ok, not_deterministic, no_hash, too_few_repeats = [], [], [], []
    for n in REQUIRED_CASE_NAMES:
        c = by_name[n]
        if c.get("status") != "OK":
            not_ok.append((n, c.get("status")))
            continue
        if not c.get("deterministic_repeat"):
            not_deterministic.append(n)
        if not c.get("csv_sha256") or not c.get("repeat_csv_sha256"):
            no_hash.append(n)
            continue
        # COUNT first, then agreement. A single-element list passes any "all equal" test while
        # proving nothing was repeated (REVIEW-EVD-AP-01-006, T006-02, second counterexample).
        if len(c["repeat_csv_sha256"]) < required_repeats:
            too_few_repeats.append((n, len(c["repeat_csv_sha256"]), required_repeats))
        elif (len(set(c["repeat_csv_sha256"])) != 1
              or c["csv_sha256"] != c["repeat_csv_sha256"][0]):
            no_hash.append(n)
    if too_few_repeats:
        raise ManifestRejected(
            f"required trials record fewer repeat hashes than the run's own repeats_per_case "
            f"contract (name, recorded, required): {too_few_repeats}; a one-element hash list is "
            f"trivially self-consistent and is not evidence of a repeated run")
    if not_ok:
        raise ManifestRejected(
            f"required trials are not status OK (identity/completeness check, not a group count): "
            f"{not_ok}")
    if not_deterministic:
        raise ManifestRejected(
            f"required trials lack a proven repeat-agreement (deterministic_repeat) despite OK "
            f"status - contradictory manifest: {not_deterministic}")
    if no_hash:
        raise ManifestRejected(
            f"required trials are missing or have inconsistent repeat hashes - contradictory "
            f"manifest: {no_hash}")

    # Every listed case (required or not) must still re-hash to the CSV on disk. This is the
    # accepted extract_metrics.py guard, kept for every case, not only the required ones.
    for c in cases:
        p = RESULTS / c["csv_file"]
        actual = hashlib.sha256(p.read_bytes()).hexdigest()
        if actual != c["csv_sha256"]:
            raise ManifestRejected(
                f"{c['csv_file']}: on-disk sha256 {actual} != manifest {c['csv_sha256']}; "
                f"the CSV was modified after the run and is not usable as evidence")

    # Regression identity: required regression names must appear exactly once, OK by the runner's
    # own column comparison, with hashes present. An EMPTY regression list must never read as
    # "all identical" - the previous check_required() only counted len(failed)==0, so an empty list
    # passed vacuously; recompute the verdict here from the actual required rows instead of trusting
    # the manifest's own regression_all_identical flag.
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
        # Tie the regression ENTRY to the RUN of the same name: the comparison must have been made
        # against the CSV this run actually produced (REVIEW-EVD-AP-01-006, T006-02: "powiazanie
        # wpisu regresji z jego uruchomieniem"). An entry quoting some other hash is a claim about
        # a file this manifest does not vouch for.
        run = by_name.get(n)
        if run is None:
            regr_unlinked.append((n, "no run of this name in cases[]"))
        elif r.get("new_csv_sha256") != run.get("csv_sha256"):
            regr_unlinked.append((n, f"entry new_csv_sha256={str(r.get('new_csv_sha256'))[:16]}... "
                                     f"!= run csv_sha256={str(run.get('csv_sha256'))[:16]}..."))
    if regr_unlinked:
        raise ManifestRejected(
            f"regression entries are not linked to their own runs: {regr_unlinked}")
    if regr_bad:
        raise ManifestRejected(
            f"required regression cases failed identity/hash check (contradicts a claimed "
            f"regression_all_identical=True): {sorted(set(regr_bad))}")
    computed_regression_all_identical = not regr_bad and not regr_missing
    if bool(m.get("regression_all_identical")) != computed_regression_all_identical:
        raise ManifestRejected(
            f"manifest's regression_all_identical={m.get('regression_all_identical')} disagrees "
            f"with the recomputed value {computed_regression_all_identical} from the required "
            f"regression rows - contradictory manifest, refusing to use it as evidence")

    return m


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


def channel_block(cols, wins, name) -> dict:
    t = cols["time_s"]
    out = {}
    for ch in REPORTED_CHANNELS:
        v = cols[ch]
        pre = em.window_stats(t, v, wins[name]["pre"])
        post = em.window_stats(t, v, wins[name]["post"])
        block = dict(pre=pre, post=post)
        if pre and post:
            block["mean_delta"] = post["mean"] - pre["mean"]
        stages = steady_stage_analysis(cols, wins[name]["post"])
        block["limit_assessment_post"] = (
            metrics.limit_assessment(
                metrics.window(t, v, *wins[name]["post"])[1], channel=ch, stages=stages)
            if post else None)
        out[ch] = block
    return out


def analyse_step(case, cols, wins) -> dict:
    t = cols["time_s"]
    ev = "torque_step"
    w = wins[ev]
    nxt = None  # a step case has no further event; the record end bounds the search
    res = dict(event="torque_step", windows=w, channels=channel_block(cols, wins, ev))
    res["step_response"] = {
        ch: em.step_response(t, cols[ch], w["event_time_s"], w["pre"], w["post"], search_until=nxt)
        for ch in ("torque_run_native", "iq_ref", "iq_allowed")
    }
    args = case["effective_args"]
    res["commanded_input_check"] = em.commanded_input_check(cols, {
        "before_step": dict(window_s=[args["ride_start_s"] + 0.5, args["torque_step_at_s"]],
                            torque_cmd_mean_nm=float(args["torque"]), ride_interval=1,
                            cadence_gen_rpm_mean=float(args["cadence"]),
                            cadence_tolerance=max(0.5, 0.02 * float(args["cadence"]))),
        "after_step": dict(window_s=[args["torque_step_at_s"] + 0.5, args["duration"]],
                           torque_cmd_mean_nm=float(args["torque_step_nm"]), ride_interval=1,
                           cadence_gen_rpm_mean=float(args["cadence"]),
                           cadence_tolerance=max(0.5, 0.02 * float(args["cadence"]))),
    })
    return res


def analyse_restart(case, cols, wins, events) -> dict:
    t = cols["time_s"]
    times = dict(events)
    stop_t = times["ride_stop"]
    restart_t = times["ride_restart"]
    stop2_t = times.get("ride_stop2")
    end_t = t[-1]

    out = dict(events=times, windows=wins, channels={})
    for ev in wins:
        out["channels"][ev] = channel_block(cols, wins, ev)

    # The stop is judged ONLY on [stop, restart): a later stop cannot confirm it.
    out["stop_before_restart"] = {
        ch: em.stop_between(t, cols[ch], stop_t, restart_t) for ch in ("iq_ref", "iq_allowed")
    }
    # For contrast, the same assessment given the whole remaining record - shown so a reader can
    # see how much of a "confirmed stop" would have been borrowed from later events.
    out["stop_whole_record_for_contrast"] = {
        ch: dict(em.stop_between(t, cols[ch], stop_t, end_t + 1.0),
                 contrast_note=("NOT the reported result: uses samples after the restart and any "
                                "second stop, which is exactly the false confirmation the card "
                                "warns about"))
        for ch in ("iq_ref",)
    }
    out["restart"] = em.restart_analysis(cols, restart_t, stop_t,
                                         stop2_t if stop2_t else end_t + 1.0)
    out["excursions_around_restart"] = {
        ch: em.excursions(t, cols[ch], stop_t, stop2_t if stop2_t else end_t + 1.0)
        for ch in ("iq_ref", "iq_allowed", "torque_run_native", "cadence_control_rpm")
    }
    # Raw Iq trajectory around the restart, exported for the plots and for eyeballing - no smoothing.
    lo, hi = stop_t - 0.2, (stop2_t if stop2_t else min(restart_t + 2.0, end_t))
    ts, vs = metrics.window(t, cols["iq_ref"], lo, hi)
    out["iq_trajectory_raw"] = dict(window_s=[lo, hi], samples=len(ts),
                                    time_s=ts[::4], iq_ref=vs[::4],
                                    decimation="every 4th exported sample (20 ms); no smoothing")

    if stop2_t:
        out["stop2"] = {ch: em.stop_between(t, cols[ch], stop2_t, end_t + 1.0)
                        for ch in ("iq_ref",)}

    args = case["effective_args"]
    expect = {
        "first_interval": dict(window_s=[args["ride_start_s"] + 0.5, stop_t],
                               torque_cmd_mean_nm=float(args["torque"]), ride_interval=1,
                               cadence_gen_rpm_mean=float(args["cadence"]),
                               cadence_tolerance=max(0.5, 0.02 * float(args["cadence"]))),
        "gap": dict(window_s=[stop_t, restart_t], ride_interval=0),
        "second_interval": dict(window_s=[restart_t, stop2_t if stop2_t else args["duration"]],
                                torque_cmd_mean_nm=float(args["torque"]), ride_interval=2),
    }
    if stop2_t:
        expect["after_stop2"] = dict(window_s=[stop2_t, args["duration"]], ride_interval=0)
    out["commanded_input_check"] = em.commanded_input_check(cols, expect)
    return out


# Verdicts, keyed by the pre-restart-only state event_metrics computes. Kept as a module-level map
# so the shipped mapping is one object a test can read, not a chain of literals inside main().
#
# REVIEW-EVD-AP-01-006 (T006-01): the old map turned IQ_STILL_NONZERO_AT_RESTART into
# "RESTART_DURING_IQ_DECAY_*". A single nonzero sample proves neither a decay nor a release, so the
# verdict is now the neutral statement of what was actually observed; anything stronger has to come
# from restart.iq_pre_restart_trend, which is measured separately and reported alongside.
VERDICT_BY_IQ_STATE = {
    "NO_SAMPLE_BEFORE_RESTART": "NO_OBSERVATION_BEFORE_RESTART",
    "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE": "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE",
    "IQ_ZERO_CROSSING_ORDER_UNDETERMINED_AT_EXPORT_RESOLUTION":
        "ORDER_UNDETERMINED_CROSSING_BRACKET_CONTAINS_RESTART",
    "IQ_ZERO_OBSERVED_BEFORE_RESTART": "IQ_ZERO_OBSERVED_BEFORE_RESTART",
}


def restart_classification_row(name: str, case: dict) -> dict:
    """The SHIPPED restart classification for one analysed case.

    Extracted from main() so a test can exercise the real path. REVIEW-EVD-AP-01-006 rejected the
    previous test precisely because it asserted on a helper's field copied into a local variable
    and never ran this mapping at all.

    Every input to `verdict` is computed strictly from samples before the commanded restart:
    `restart.iq_state_at_restart` (zero-crossing bracket vs restart time) and
    `stop_before_restart.iq_ref.status` (window [stop, restart) only). Post-restart material is
    reported in the row for context and is explicitly named as excluded from the verdict.
    """
    r = case["restart"]
    exc = case["excursions_around_restart"]["iq_ref"]
    gap = case["events"]["ride_restart"] - case["events"]["ride_stop"]
    stop_status = case["stop_before_restart"]["iq_ref"]["status"]  # PRE-restart window only
    iq_state = r["iq_state_at_restart"]                            # PRE-restart samples only
    verdict = VERDICT_BY_IQ_STATE.get(iq_state, "IQ_STATE_UNDETERMINED")

    return dict(
        case=name, scripted_gap_s=round(gap, 4), speed_kph=case["effective_args"].get("speed"),
        iq_value_at_restart=r["iq_value_at_restart"],
        iq_state_at_restart=iq_state,
        last_pre_restart_sample_time_s=r["last_pre_restart_sample_time_s"],
        first_sample_at_or_after_restart_time_s=r["first_sample_at_or_after_restart_time_s"],
        iq_zero_crossing_bracket_s=r["iq_zero_crossing_bracket_s"],
        iq_zero_order_vs_restart=r["iq_zero_order_vs_restart"],
        export_dt_s=r["export_dt_s"],
        iq_pre_restart_trend=r["iq_pre_restart_trend"],
        stop_status_before_restart=stop_status,
        stop_status_note=("a separate metric: Iq observed at zero before the restart is NOT the "
                          "same claim as a confirmed stop"),
        session_state_before_restart=r["session_state_before_restart"],
        session_transitions_before_restart=r["session_transitions_before_restart"],
        debug_flags_before_restart_decoded=r["debug_flags_before_restart_decoded"],
        iq_resume_classification=r["iq_resume_classification"],
        cadence_resume_classification=r["cadence_control_resume_classification"],
        iq_resume_latency_s=r["iq_latency_s"],
        cadence_resume_latency_s=r["cadence_control_latency_s"],
        iq_first_positive_sample_after_restart_s=r["iq_first_positive_sample_after_restart_s"],
        cadence_first_positive_sample_after_restart_s=(
            r["cadence_control_first_positive_sample_after_restart_s"]),
        verdict=verdict,
        post_restart_context_not_used_in_verdict=dict(
            session_states_in_window=r["session_states_in_window"],
            session_states_in_window_decoded=r["session_states_in_window_decoded"],
            session_transitions_at_or_after_restart=[x for x in r["session_transitions"]
                                                     if x["side"] == "AT_OR_AFTER_RESTART"],
            debug_flags_in_window_decoded=r["debug_flags_in_window_decoded"],
            iq_min_after_stop=exc["min"], iq_max_after_stop=exc["max"],
            stop_status_whole_record_INVALID_AS_EVIDENCE=(
                case["stop_whole_record_for_contrast"]["iq_ref"]["status"]),
        ),
    )


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
                  "or step; a stop is assessed only on the samples before the next event"),
        ),
        cases={},
    )
    for c in m["cases"]:
        if c.get("group") == "regression_ac_t2":
            continue
        cols = em.read_csv(RESULTS / c["csv_file"])
        events = sorted(c["commanded_event_times_s"].items(), key=lambda kv: kv[1])
        wins = em.event_windows(events, cols["time_s"][-1], PRE_S, SETTLE_S, POST_S)
        if c["group"].startswith("torque_step"):
            body = analyse_step(c, cols, wins)
        else:
            body = analyse_restart(c, cols, wins, events)
        result["cases"][c["name"]] = dict(
            group=c["group"], purpose=c["purpose"],
            effective_args=c["effective_args"], csv_sha256=c["csv_sha256"],
            deterministic_repeat=c["deterministic_repeat"], **body)

    # Which scripted gap actually produced which pipeline state - read from the data, never from
    # the gap length. This is the table the report cites when it names a "fast restart" case.
    #
    # CORRECTED per REVIEW-EVD-AP-01-005 (T005-01). The previous version computed `stop_declared`
    # from `session_states_in_window`/`debug_flags_in_window`, both of which are the state seen
    # AFTER the restart ([restart_time, next_event)) - using that to claim the restart happened
    # "after Iq reached zero" is circular, since the classification never actually looked at any
    # sample before the restart. It also tested flag membership with `16 in debug_flags` (a set/
    # list membership test on the raw int), which misses any bitmask combination such as 144 =
    # 128 + 16.
    #
    # The verdict below is built ONLY from `restart.iq_state_at_restart` and
    # `stop_before_restart.iq_ref.status`, both of which event_metrics.restart_analysis() and
    # stop_between() compute strictly from samples BEFORE the restart. Post-restart fields
    # (session/debug flags in the post window, excursions after the stop) are still reported in
    # the row for context, but are explicitly labelled as NOT part of the verdict.
    classification = [restart_classification_row(name, c)
                      for name, c in result["cases"].items()
                      if not c["group"].startswith("torque_step")]
    result["restart_classification"] = dict(
        note=("verdict is derived ONLY from iq_state_at_restart and stop_before_restart, both "
              "computed strictly from samples BEFORE the commanded restart time - never from "
              "session/debug-flag transitions or Iq excursions observed AFTER the restart (that "
              "was the T005-01 defect: using the post-restart window to argue about pre-restart "
              "order is circular). CORRECTED AGAIN per REVIEW-EVD-AP-01-006 (T006-01): the "
              "uncertainty was previously defined backwards. Order is now decided by the "
              "zero-crossing bracket (t_prev_nonzero, t_first_zero] against the restart instant. A "
              "zero measured at a sample strictly before the restart means the ORDER IS KNOWN "
              "(only the crossing instant inside the bracket and the confirmation length remain "
              "uncertain); 'ORDER_UNDETERMINED_CROSSING_BRACKET_CONTAINS_RESTART' is used only when "
              "that bracket contains the restart instant - which is the real 200 ms case (72 at "
              "5.195, zero first seen at 5.200 = the restart time). A nonzero last pre-restart "
              "sample is now stated neutrally as IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE: it is NOT "
              "evidence of an Iq decay or of a release; iq_pre_restart_trend carries that as a "
              "separate measurement. Zero before the restart is not the same claim as a confirmed "
              "stop. session/debug_flags are decoded from inc/ride_session.h and "
              "inc/ride_control.h (RIDE_SESSION_*, RIDE_DBG_* - not a dedicated stop/release flag), "
              "and debug_flags is decoded bitwise, not by list membership. A short scripted gap on "
              "its own is never evidence that the restart landed during a release. The 72->0 "
              "two-sample observation at 195/200 ms, standing/rolling, is reported per-case in "
              "iq_state_at_restart/iq_value_at_restart; it is NOT generalised into a claim that a "
              "smooth Iq decay is unobtainable or unobservable in general - only this finite set of "
              "scripted gaps at this cadence/torque/config was tested here, not the full ramp or "
              "16 kHz inter-sample behaviour."),
        rows=sorted(classification, key=lambda r: (r["speed_kph"], r["scripted_gap_s"])),
    )

    result["regression_ac_t2"] = m["regression"]
    result["regression_all_identical"] = m["regression_all_identical"]
    result["rework"] = dict(
        task="TASK-EVD-AP-01-TRANSIENTS-REWORK-001",
        corrects="EXEC-EVD-AP-01-005 / T005-01 / T005-02",
        source_manifest_path=str(MANIFEST.relative_to(HERE)).replace("\\", "/"),
        source_manifest_sha256=hashlib.sha256(MANIFEST.read_bytes()).hexdigest(),
        source_run_id=m["run_id"],
        note=("this file supersedes results/transients-metrics.json's restart_classification and "
              "restart-related channel semantics only; results/transients-metrics.json is left on "
              "disk unchanged as historical EXEC-005 evidence. Torque-step results are unaffected "
              "by T005-01 and are recomputed here identically for a single self-consistent file."),
    )
    REWORK_DIR.mkdir(exist_ok=True)
    OUT.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"wrote {OUT.relative_to(HERE)}: {len(result['cases'])} analysed cases")


if __name__ == "__main__":
    main()
