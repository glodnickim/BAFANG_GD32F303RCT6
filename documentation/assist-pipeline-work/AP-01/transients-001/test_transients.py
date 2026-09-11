"""AP-01 transients-001 tests: event windows, event metrics, and the new CLI contract.

Two halves:

  A. metric/window tests on SYNTHETIC signals with analytically known answers. They must fail if
     a window is allowed to cross the next event, if a stop is confirmed on evidence belonging to a
     later stop, or if a missing crossing is reported as a number.
  B. CLI tests against the REAL native binary: event time boundaries, the rejection of every kind
     of bad argument, and the presence of the actually-commanded torque mean and cadence in the CSV.

Part B needs the native Controller Lab (gcc + node, same prerequisites as the accepted suites).
A missing toolchain is a test FAILURE here, not a silent skip - a suite that quietly does nothing
proves nothing.

Run:  python documentation/assist-pipeline-work/AP-01/transients-001/test_transients.py
"""
from __future__ import annotations

import copy
import csv
import hashlib
import io
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent
FW = AP01.parents[2]
sys.path.insert(0, str(AP01))
sys.path.insert(0, str(HERE))

import ap01_runner as accepted  # noqa: E402
import event_metrics as em  # noqa: E402
import make_plots  # noqa: E402
import analyze  # noqa: E402
import transients_runner as runner  # noqa: E402

FAILED: list[str] = []
PASSED = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global PASSED
    if condition:
        PASSED += 1
        print(f"  PASS  {name}")
    else:
        FAILED.append(f"{name}: {detail}")
        print(f"  FAIL  {name}  {detail}")


def raises(name: str, fn, exc=Exception) -> None:
    try:
        fn()
    except exc:
        check(name, True)
        return
    except Exception as e:  # noqa: BLE001
        check(name, False, f"raised {type(e).__name__}, expected {exc.__name__}")
        return
    check(name, False, "did not raise")


# ============================ A. windows =====================================================

def test_event_windows() -> None:
    print("event_windows")
    w = em.event_windows([("stop", 5.0), ("restart", 5.2), ("stop2", 8.0)], 12.0,
                         pre_s=1.0, settle_s=0.5, post_s=1.5)
    check("stop pre window starts 1.0 s before the stop",
          w["stop"]["pre"] == [4.0, 5.0], str(w["stop"]["pre"]))
    check("stop post window is empty - the restart follows before the settle allowance",
          w["stop"]["post"] is None, str(w["stop"]["post"]))
    check("restart pre window is clipped by the stop, not 1.0 s long",
          w["restart"]["pre"] == [5.0, 5.2] and w["restart"]["pre_clipped_by"] == "stop",
          str(w["restart"]))
    check("a clipped window is reported as not full length",
          w["restart"]["pre_full_length"] is False)
    check("restart post window ends before the second stop",
          w["restart"]["post"][1] <= 8.0, str(w["restart"]["post"]))
    check("stop2 post window is bounded by the record end",
          w["stop2"]["post"][1] <= 12.0, str(w["stop2"]["post"]))

    # A steady window must never span an event.
    for name, win in w.items():
        for bounds in (win["pre"], win["post"]):
            if bounds is None:
                continue
            crossed = [n for n, t in (("stop", 5.0), ("restart", 5.2), ("stop2", 8.0))
                       if bounds[0] < t < bounds[1]]
            check(f"{name} window {bounds} does not span another event", not crossed, str(crossed))

    raises("non-increasing event times are rejected",
           lambda: em.event_windows([("a", 5.0), ("b", 5.0)], 10.0, 1.0, 0.5, 1.5), ValueError)
    raises("non-positive pre_s is rejected",
           lambda: em.event_windows([("a", 5.0)], 10.0, 0.0, 0.5, 1.5), ValueError)


# ============================ A. step latency ================================================

def test_step_response_known() -> None:
    print("step_response on an analytically known step")
    # 0 -> 100 linear ramp of exactly 1.0 s starting at t=2.0. 10% at 2.1 s, 90% at 2.9 s,
    # so the true 10-90% latency is 0.8 s. Sampling at 10 ms hits both levels exactly.
    times = [round(i * 0.01, 4) for i in range(0, 601)]
    values = []
    for t in times:
        if t < 2.0:
            values.append(0.0)
        elif t < 3.0:
            values.append(100.0 * (t - 2.0))
        else:
            values.append(100.0)
    r = em.step_response(times, values, 2.0, [1.0, 2.0], [3.5, 5.0])
    check("status MEASURED", r["status"] == "MEASURED", r["status"])
    check("reference levels are the window means, reported explicitly",
          abs(r["start_level"]) < 1e-9 and abs(r["end_level"] - 100.0) < 1e-9,
          f"{r['start_level']} -> {r['end_level']}")
    # A crossing is located at a SAMPLE, so the analytic 0.80 s is resolvable only to the 10 ms
    # sampling interval (the 90% level falls between two samples here). Asserting sub-sample
    # accuracy would be asserting something the measurement cannot deliver.
    check("10-90% latency matches the analytic 0.80 s within one sampling interval",
          abs(r["rise_time_s"] - 0.80) <= 0.0101, str(r["rise_time_s"]))
    check("the located crossings are the samples the thresholds fall on",
          abs(r["t_lo_s"] - 2.10) <= 0.0101 and abs(r["t_hi_s"] - 2.90) <= 0.0101,
          f"{r['t_lo_s']} {r['t_hi_s']}")
    check("a clean step is not flagged as ripple-ambiguous",
          r["ripple_spans_thresholds"] is False)

    # Falling step, mirrored: 100 -> 0 over 1 s. Same 0.8 s.
    fall = [100.0 - v for v in values]
    rf = em.step_response(times, fall, 2.0, [1.0, 2.0], [3.5, 5.0])
    check("falling step measured with the same analytic latency",
          rf["status"] == "MEASURED" and abs(rf["rise_time_s"] - 0.80) <= 0.0101,
          f"{rf['status']} {rf['rise_time_s']}")

    # A search bounded by the next event must not borrow a crossing from after it.
    rb = em.step_response(times, values, 2.0, [1.0, 2.0], [3.5, 5.0], search_until=2.5)
    check("crossing after the next event is not borrowed -> HI_CROSSING_NOT_REACHED",
          rb["status"] == "HI_CROSSING_NOT_REACHED" and rb["rise_time_s"] is None,
          f"{rb['status']} {rb['rise_time_s']}")
    check("an unreached threshold gives no number, not zero", rb["rise_time_s"] is None)


def test_step_response_ripple_flag() -> None:
    print("step_response ripple limitation flag")
    # Same step, but both plateaus carry +-30 ripple, which reaches across the 10%/90% levels.
    times = [round(i * 0.01, 4) for i in range(0, 601)]
    vals = []
    for i, t in enumerate(times):
        base = 0.0 if t < 2.0 else (100.0 * (t - 2.0) if t < 3.0 else 100.0)
        vals.append(base + (30.0 if i % 2 == 0 else -30.0))
    r = em.step_response(times, vals, 2.0, [1.0, 2.0], [3.5, 5.0])
    check("pulsating plateaus are flagged as ripple-ambiguous",
          r["ripple_spans_thresholds"] is True, str(r.get("ripple_spans_thresholds")))
    # With ripple this large the signal is already above the 10% level at the event instant, so
    # the accepted primitive declines to measure rather than timing a ripple peak. That refusal is
    # the correct answer, and it must not be silently replaced by a number.
    check("a ripple peak is not timed as if it were the mean crossing",
          r["status"] == "LO_CROSSING_BEFORE_RECORD" and r["rise_time_s"] is None, r["status"])
    check("the limitation is stated in the record", bool(r["ripple_note"]))

    # Smaller ripple that does not reach the thresholds still measures, and is not flagged.
    small = []
    for i, t in enumerate(times):
        base = 0.0 if t < 2.0 else (100.0 * (t - 2.0) if t < 3.0 else 100.0)
        small.append(base + (2.0 if i % 2 == 0 else -2.0))
    rs = em.step_response(times, small, 2.0, [1.0, 2.0], [3.5, 5.0])
    check("ripple below the thresholds still yields a measured latency",
          rs["status"] == "MEASURED" and rs["rise_time_s"] is not None, rs["status"])


# ============================ A. T005-01: session/flag decode + restart order ================

def test_session_and_debug_flag_decode() -> None:
    print("session/debug_flags decode (T005-01: names from the C headers, bitwise not membership)")
    check("session 2 decodes to the real enum name, not a guessed stop/release name",
          em.decode_session(2) == "RIDE_SESSION_SUSPENDED_BY_DIRECTION", em.decode_session(2))
    check("session 1 decodes to ACTIVE", em.decode_session(1) == "RIDE_SESSION_ACTIVE")
    check("an unknown session value is reported as unknown, not guessed",
          em.decode_session(9) == "UNKNOWN_SESSION_9")

    # 144 = 128 (RIDE_DBG_COAST_RELEASE) + 16 (RIDE_DBG_NOT_LATCHED). The old code's `16 in flags`
    # was a list-membership test against the raw int and would silently miss this bit combination
    # (16 in [144] is False even though bit 0x10 IS set in 144).
    decoded = em.decode_debug_flags(144)
    check("144 = 128+16 decodes to BOTH bits, unlike `16 in [144]` (False)",
          "RIDE_DBG_NOT_LATCHED" in decoded and "RIDE_DBG_COAST_RELEASE" in decoded, decoded)
    check("`16 in [144]` really is the false negative this replaces", 16 not in [144])
    check("debug_flags=0 decodes to NONE_SET, not an empty/ambiguous list",
          em.decode_debug_flags(0) == ["RIDE_DBG_NONE_SET"])
    check("a single known bit decodes to exactly one name",
          em.decode_debug_flags(0x10) == ["RIDE_DBG_NOT_LATCHED"])


def test_restart_order_same_sample_vs_margin() -> None:
    """Corrected per REVIEW-EVD-AP-01-006 (T006-01).

    This test previously ASSERTED the inverted rule: it required a zero measured at the last
    sample strictly before the restart to be reported as 'order undetermined'. That is wrong - the
    order is known there (the zero was measured before the restart was commanded); only the exact
    crossing instant inside the bracket, and the confirmation length, are uncertain. The order is
    undetermined only when the crossing bracket CONTAINS the restart instant.
    """
    print("restart_analysis: zero-crossing bracket vs restart instant (T006-01, corrected)")
    times = [round(i * 0.01, 4) for i in range(0, 400)]  # 10 ms export interval
    n = len(times)

    # Zero appears at 2.49, the last sample strictly before the restart at 2.50. The bracket is
    # (2.48, 2.49] - entirely before the restart, so the ORDER IS KNOWN.
    cols_same = dict(time_s=times, ride_interval=[1.0] * n, pedalling=[1.0] * n,
                     cadence_control_rpm=[70.0] * n, gate_steps=[4.0] * n, session=[1.0] * n,
                     debug_flags=[0.0] * n, iq_allowed=[0.0] * n,
                     iq_ref=[100.0 if t < 2.49 else 0.0 for t in times])
    r_same = em.restart_analysis(cols_same, 2.50, 2.0, 5.0)
    check("a zero at the last pre-restart sample means the ORDER IS KNOWN, not undetermined",
          r_same["iq_state_at_restart"] == "IQ_ZERO_OBSERVED_BEFORE_RESTART",
          r_same["iq_state_at_restart"])
    check("...and the crossing bracket that bounds the remaining uncertainty is reported",
          r_same["iq_zero_crossing_bracket_s"] == [2.48, 2.49],
          r_same["iq_zero_crossing_bracket_s"])

    # The genuinely undetermined case: the first zero sample IS the restart sample, so the crossing
    # bracket (2.49, 2.50] contains the restart instant.
    cols_bracket = copy.deepcopy(cols_same)
    cols_bracket["iq_ref"] = [100.0 if t < 2.50 else 0.0 for t in times]
    r_bracket = em.restart_analysis(cols_bracket, 2.50, 2.0, 5.0)
    check("a crossing bracket containing the restart instant IS order-undetermined",
          r_bracket["iq_state_at_restart"] ==
          "IQ_ZERO_CROSSING_ORDER_UNDETERMINED_AT_EXPORT_RESOLUTION",
          r_bracket["iq_state_at_restart"])
    check("...and that bracket really straddles the restart time",
          r_bracket["iq_zero_crossing_bracket_s"][0] < 2.50
          <= r_bracket["iq_zero_crossing_bracket_s"][1],
          r_bracket["iq_zero_crossing_bracket_s"])

    # Zero-crossing several samples earlier: same known order, wider margin.
    cols_margin = copy.deepcopy(cols_same)
    cols_margin["iq_ref"] = [100.0 if t < 2.20 else 0.0 for t in times]
    r_margin = em.restart_analysis(cols_margin, 2.50, 2.0, 5.0)
    check("zero reached with several samples of margin is also reported as before the restart",
          r_margin["iq_state_at_restart"] == "IQ_ZERO_OBSERVED_BEFORE_RESTART",
          r_margin["iq_state_at_restart"])
    check("its bracket ends well before the restart instant",
          r_margin["iq_zero_crossing_bracket_s"][1] < 2.50,
          r_margin["iq_zero_crossing_bracket_s"])

    # Still nonzero at the restart instant, and never zero at all: neutral statement only.
    cols_nonzero = copy.deepcopy(cols_same)
    cols_nonzero["iq_ref"] = [100.0] * n
    r_nz = em.restart_analysis(cols_nonzero, 2.50, 2.0, 5.0)
    check("a nonzero last pre-restart sample is stated neutrally, with no decay/release claim",
          r_nz["iq_state_at_restart"] == "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE",
          r_nz["iq_state_at_restart"])
    check("...and the pre-restart trend is reported separately as a measurement",
          r_nz["iq_pre_restart_trend"]["delta"] == 0.0,
          r_nz["iq_pre_restart_trend"].get("delta"))
    check("no zero anywhere in the record is reported as such, not as an order claim",
          r_nz["iq_zero_order_vs_restart"] == "NO_ZERO_OBSERVED_IN_RECORD",
          r_nz["iq_zero_order_vs_restart"])

    # resume classification: already positive vs resumed vs no observation.
    check("Iq already positive at restart -> not counted as a 'resume'",
          r_nz["iq_resume_classification"] == "ALREADY_POSITIVE_AT_RESTART_NOT_A_RESUME",
          r_nz["iq_resume_classification"])
    cols_resume = copy.deepcopy(cols_margin)
    cols_resume["iq_ref"] = [100.0 if t < 2.2 else (0.0 if t < 2.7 else 55.0) for t in times]
    r_resume = em.restart_analysis(cols_resume, 2.50, 2.0, 5.0)
    check("Iq zero at restart then nonzero later in the window -> RESUMED_AFTER_RESTART",
          r_resume["iq_resume_classification"] == "RESUMED_AFTER_RESTART",
          r_resume["iq_resume_classification"])
    cols_no_obs = copy.deepcopy(cols_margin)  # stays zero for the whole post-restart window
    r_no_obs = em.restart_analysis(cols_no_obs, 2.50, 2.0, 5.0)
    check("Iq zero at restart and never seen positive in the window -> NO_OBSERVATION",
          r_no_obs["iq_resume_classification"] == "NO_OBSERVATION_IN_POST_RESTART_WINDOW",
          r_no_obs["iq_resume_classification"])


def test_analyze_classification_uses_only_pre_restart_evidence() -> None:
    """Regression test for the exact T005-01 defect: post-restart session/debug-flag transitions
    and post-restart Iq excursions must never flip the verdict away from what iq_state_at_restart
    (pre-restart only) says. This reproduces the review's own restart_gap0195ms_speed18 shape:
    Iq is still nonzero at the last pre-restart sample, session/debug flags only change AFTER the
    restart - the old code read `stop_declared` from that post-restart change and wrongly concluded
    a confirmed stop; the fix must not."""
    print("analyze.py classification does not use post-restart state as pre-restart evidence")
    times = [round(i * 0.005, 4) for i in range(0, 2000)]  # 5 ms export, like the real CSVs
    n = len(times)
    stop_t, restart_t = 5.0, 5.195
    cols = dict(
        time_s=times,
        ride_interval=[1.0 if t < stop_t else (0.0 if t < restart_t else 2.0) for t in times],
        pedalling=[1.0 if t < stop_t else (0.0 if t < restart_t else 1.0) for t in times],
        cadence_control_rpm=[70.0] * n,
        gate_steps=[4.0] * n,
        # session/debug_flags only change strictly AFTER the restart - exactly the review example.
        session=[1.0 if t < restart_t + 0.005 else 2.0 for t in times],
        debug_flags=[0.0] * n,
        # Iq decays slowly and is STILL 72 at the last pre-restart sample - never reaches zero
        # before the restart in this scenario.
        iq_ref=[max(0.0, 100.0 - 40.0 * max(0.0, t - stop_t)) for t in times],
        iq_allowed=[max(0.0, 100.0 - 40.0 * max(0.0, t - stop_t)) for t in times],
    )
    row = _analyzer_row("synthetic_nonzero_at_restart", cols, stop_t, restart_t, 8.0)
    check("setup sanity: session only changes in the POST-restart window (the old evidence)",
          row["post_restart_context_not_used_in_verdict"]["session_states_in_window"] == [1, 2],
          row["post_restart_context_not_used_in_verdict"]["session_states_in_window"])
    check("the SHIPPED analyzer verdict is the neutral pre-restart statement, not a stop/decay "
          "claim borrowed from the post-restart session transition",
          row["verdict"] == "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE", row["verdict"])


def _analyzer_row(name: str, cols: dict, stop_t: float, restart_t: float, end_t: float,
                  speed: float = 0.0) -> dict:
    """Build one analysed case and run the SHIPPED classification over it.

    REVIEW-EVD-AP-01-006 rejected the previous version of this test because it asserted on
    em.restart_analysis()'s field copied into a local variable and never executed the mapping
    analyze.py actually ships. Everything below therefore goes through
    analyze.restart_classification_row().
    """
    body = dict(
        group="stop_restart_standing",
        events={"ride_stop": stop_t, "ride_restart": restart_t},
        effective_args={"speed": speed},
        restart=em.restart_analysis(cols, restart_t, stop_t, end_t),
        stop_before_restart={ch: em.stop_between(cols["time_s"], cols[ch], stop_t, restart_t)
                             for ch in ("iq_ref", "iq_allowed")},
        excursions_around_restart={"iq_ref": em.excursions(cols["time_s"], cols["iq_ref"],
                                                           stop_t, end_t)},
        stop_whole_record_for_contrast={"iq_ref": em.stop_between(
            cols["time_s"], cols["iq_ref"], stop_t, cols["time_s"][-1] + 1.0)},
    )
    return analyze.restart_classification_row(name, body)


def _restart_cols(times, iq, session=None) -> dict:
    n = len(times)
    return dict(time_s=list(times), iq_ref=list(iq), iq_allowed=list(iq),
                session=list(session) if session else [1.0] * n,
                cadence_control_rpm=[70.0] * n, gate_steps=[4.0] * n,
                ride_interval=[1.0] * n, debug_flags=[0.0] * n, pedalling=[1.0] * n)


def _real_restart_row(case_name: str) -> dict:
    """Run the shipped classification over one of the 39 preserved CSVs (read-only)."""
    m = json.loads((analyze.RESULTS / "transients-results.json").read_text(encoding="utf-8"))
    c = {x["name"]: x for x in m["cases"]}[case_name]
    cols = em.read_csv(analyze.RESULTS / c["csv_file"])
    ev = c["commanded_event_times_s"]
    return _analyzer_row(case_name, cols, ev["ride_stop"], ev["ride_restart"],
                         cols["time_s"][-1] + 1.0, c["effective_args"]["speed"])


def test_restart_order_uses_crossing_bracket() -> None:
    """REVIEW-EVD-AP-01-006 T006-01, both counterexamples, through the SHIPPED analyzer.

    The uncertainty was previously defined backwards: a zero measured at a sample strictly BEFORE
    the restart was called 'order undetermined' (it is not - the order is known, only the crossing
    instant inside the bracket is uncertain), while the genuinely undetermined case, where the
    crossing bracket CONTAINS the restart instant, went unflagged.
    """
    print("restart ordering: zero-crossing bracket vs restart instant (shipped classification)")

    # Counterexample A, verbatim from the review: t=[0,1,2,3], Iq=[10,0,0,10], restart=2.
    row = _analyzer_row("review_counterexample_zero_before",
                        _restart_cols([0.0, 1.0, 2.0, 3.0], [10.0, 0.0, 0.0, 10.0]), 0.0, 2.0, 4.0)
    check("zero measured at t=1 with the restart at t=2 is reported as observed BEFORE the "
          "restart, not as undetermined order",
          row["verdict"] == "IQ_ZERO_OBSERVED_BEFORE_RESTART", row["verdict"])
    check("the crossing bracket is reported and lies entirely before the restart",
          row["iq_zero_crossing_bracket_s"] == [0.0, 1.0], row["iq_zero_crossing_bracket_s"])

    # Counterexample B, the real 200 ms trial: 72 at 5.195, first zero at 5.200 = the restart time.
    r200 = _real_restart_row("restart_gap0200ms_speed0")
    check("real 200 ms: the crossing bracket contains the restart instant, so the order is "
          "reported as undetermined at export resolution",
          r200["verdict"] == "ORDER_UNDETERMINED_CROSSING_BRACKET_CONTAINS_RESTART",
          f"{r200['verdict']} bracket={r200['iq_zero_crossing_bracket_s']}")
    lo, hi = r200["iq_zero_crossing_bracket_s"]
    check("the reported bracket really brackets the commanded restart time",
          lo < r200["scripted_gap_s"] + 5.0 <= hi + 1e-9,
          f"bracket={[lo, hi]} restart={r200['last_pre_restart_sample_time_s']}")

    # The real 195 ms trial: Iq is still 72 before the restart and does not reach zero until after.
    r195 = _real_restart_row("restart_gap0195ms_speed18")
    check("real 195 ms: neutral verdict, no decay/release claim from a single nonzero sample",
          r195["verdict"] == "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE",
          f"{r195['verdict']} iq={r195['iq_value_at_restart']}")

    # A constantly-positive Iq: nothing decays, nothing resumes.
    n = 400
    times = [round(i * 0.005, 4) for i in range(n)]
    flat = _analyzer_row("flat_positive", _restart_cols(times, [80.0] * n), 1.0, 1.2, 2.0)
    check("a constantly-positive Iq never yields a decay verdict",
          "DECAY" not in flat["verdict"] and flat["verdict"] ==
          "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE", flat["verdict"])
    check("an already-positive channel reports no resume latency, only a diagnostic first-positive "
          "sample",
          flat["iq_resume_latency_s"] is None
          and flat["iq_first_positive_sample_after_restart_s"] is not None,
          f"latency={flat['iq_resume_latency_s']} "
          f"diag={flat['iq_first_positive_sample_after_restart_s']}")
    check("session transitions carry times, not just a set of names",
          isinstance(flat["session_transitions_before_restart"], list),
          type(flat["session_transitions_before_restart"]).__name__)

    # A zero that is genuinely established well before the restart, with margin.
    early = _analyzer_row("zero_with_margin",
                          _restart_cols(times, [80.0 if t < 1.0 else 0.0 for t in times]),
                          1.0, 1.5, 2.0)
    check("a zero established with margin before the restart is reported as observed before it",
          early["verdict"] == "IQ_ZERO_OBSERVED_BEFORE_RESTART", early["verdict"])
    check("zero-before-restart is NOT reported as a confirmed stop (separate metric)",
          "stop_status_before_restart" in early and early["verdict"] != early[
              "stop_status_before_restart"], early["stop_status_before_restart"])


def test_footer_lines_do_not_collide_or_overflow() -> None:
    """T006-03: the bitmask legend and the limitations note were drawn 1 px apart (legend_y+27 vs
    total_h-6) and both ran past the 1320 px image width. Footer text must wrap, sit on separate
    rows, and stay inside the image."""
    print("make_plots footer: wrapped, separate rows, inside the image width")
    avail = make_plots.W - 2 * make_plots.PAD_L
    lines = [ln for b in make_plots.FOOTER_BLOCKS for ln in make_plots.wrap_text(b, avail)]
    ys = [i * make_plots.FOOTER_LINE_H for i in range(len(lines))]
    gaps = [b - a for a, b in zip(ys, ys[1:])]
    check("every footer line sits on its own row (no two within 10 px)",
          all(g >= 10 for g in gaps), f"{len(lines)} lines, gaps={sorted(set(gaps))}")
    widths = [make_plots.PAD_L + len(ln) * make_plots.FOOTER_CHAR_W for ln in lines]
    check("no footer line runs past the image width",
          all(w <= make_plots.W for w in widths),
          f"max={round(max(widths))}px vs W={make_plots.W}px")
    check("the bitmask legend is still present in full after wrapping",
          "RIDE_DBG_COAST_RELEASE" in " ".join(lines) and "0x10" in " ".join(lines))
    labels = {n: lab for _, series in make_plots.PANELS for n, _, lab in series}
    check("torque_run_native is not labelled Nm (it is assist_delta_run_native, native counts)",
          "[Nm]" not in labels["torque_run_native"], labels["torque_run_native"])
    check("torque_run_native's label names the native-count unit explicitly",
          "native counts" in labels["torque_run_native"], labels["torque_run_native"])


def test_series_legend_labels_fit() -> None:
    """The series legend sits in the right margin; a label longer than the margin is clipped by the
    image edge. Found while viewing the re-rendered restart zoom after the T006-03 unit fix: the
    corrected torque_run_native label was long enough to be cut off."""
    print("make_plots series legend: every label fits inside the right margin")
    too_long = [(n, lab, len(lab)) for _, series in make_plots.PANELS for n, _, lab in series
                if len(lab) > make_plots.LEGEND_MAX_CHARS]
    check(f"no series label exceeds the {make_plots.LEGEND_MAX_CHARS}-char right margin",
          not too_long, too_long)


def test_nice_ticks_uses_requested_scale() -> None:
    """T005-03: nice_ticks() must pick mag from the ACTUAL requested step, not hardcode 1e-3 for
    any raw step below 1.0. A 4.6-6.2 s panel wanting ~8 divisions (raw ~= 0.2) must get an ~0.2 s
    step, not the ~0.01 s grid the bug produced."""
    print("make_plots.nice_ticks: 1-2-5 stepping picked from the actual range, not floored at 1e-3")
    ticks = make_plots.nice_ticks(4.6, 6.2, 8)
    steps = sorted({round(b - a, 6) for a, b in zip(ticks, ticks[1:])})
    check("tick step for a 1.6 s / 8-division panel is 0.2 s, not ~0.01 s",
          steps == [0.2], f"ticks={ticks} steps={steps}")
    check("tick count is sane for the panel width (not dozens of overlapping labels)",
          3 <= len(ticks) <= 12, str(ticks))

    # A magnitude-1 case must still work (regression: the old code's >=1 branch is untouched).
    ticks_big = make_plots.nice_ticks(0, 1000, 5)
    steps_big = sorted({round(b - a, 6) for a, b in zip(ticks_big, ticks_big[1:])})
    check("large-range tick step is unaffected by the small-step fix",
          steps_big and steps_big[0] in (200, 250), f"ticks={ticks_big}")

    # A very small range (well below 0.01) must not be floored to 0.01 either.
    ticks_small = make_plots.nice_ticks(0.001, 0.009, 5)
    steps_small = sorted({round(b - a, 6) for a, b in zip(ticks_small, ticks_small[1:])})
    check("a sub-0.01 range gets a sub-0.01 step, not the old hardcoded 1e-3 floor artefact",
          steps_small and steps_small[0] <= 0.002, f"ticks={ticks_small}")


# ============================ A. T005-02: manifest integrity ==================================

def _write_min_manifest(tmp: Path, mutate=None, drop_case=None, empty_regression=False,
                        break_regression_flag=False, mutate_manifest=None) -> Path:
    """A minimal, otherwise-valid manifest/CSV set covering every REQUIRED_CASE_NAMES /
    REQUIRED_REGRESSION_NAMES entry, in an ISOLATED temp directory (never the real results base -
    REVIEW-EVD-AP-01-005 T005-02 explicitly requires this)."""
    cases = []
    # REQUIRED_CASE_NAMES now includes the four AC-T2 regression RUNS (REVIEW-EVD-AP-01-006,
    # T006-02); their real group must be used, or the runner's group quota fails for an unrelated
    # reason and the identity gap is never actually exercised.
    regr_runs = set(analyze.REQUIRED_REGRESSION_NAMES)
    for name in analyze.REQUIRED_CASE_NAMES:
        csv_bytes = f"time_s,iq_ref\n0,0\n{name}\n".encode()
        (tmp / f"{name}.csv").write_bytes(csv_bytes)
        h = hashlib.sha256(csv_bytes).hexdigest()
        cases.append(dict(name=name, status="OK", csv_file=f"{name}.csv", csv_sha256=h,
                          repeat_csv_sha256=[h, h], deterministic_repeat=True,
                          group=("regression_ac_t2" if name in regr_runs else "torque_step_clean"),
                          commanded_event_times_s={}, effective_args={}, purpose="test"))
    by = {c["name"]: c for c in cases}
    # A regression ENTRY must quote the CSV hash of the RUN of the same name.
    regression = []
    for name in analyze.REQUIRED_REGRESSION_NAMES:
        regression.append(dict(case=name, pre_existing_columns_identical=True,
                               accepted_csv_sha256="a" * 64,
                               new_csv_sha256=by[name]["csv_sha256"]))
    if drop_case:
        cases = [c for c in cases if c["name"] != drop_case]
    if mutate:
        mutate(cases)
    m = dict(run_id="TEST-RUN", qualification="test", run_complete=True,
             attempt_state="COMPLETE", repeats_per_case=2, cases=cases,
             regression=[] if empty_regression else regression,
             regression_all_identical=(not empty_regression) and not break_regression_flag)
    if mutate_manifest:
        mutate_manifest(m)
    if break_regression_flag:
        m["regression_all_identical"] = True  # contradicts the actually-empty/failing regression
    p = tmp / "transients-results.json"
    p.write_text(json.dumps(m), encoding="utf-8")
    return p


def test_analyze_load_manifest_integrity() -> None:
    print("analyze.load_manifest(): identity/completeness + contradiction rejection, isolated dir")
    orig_results, orig_manifest = analyze.RESULTS, analyze.MANIFEST
    try:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            analyze.RESULTS = tmp

            # --- positive: a complete, consistent manifest loads cleanly.
            analyze.MANIFEST = _write_min_manifest(tmp)
            m = analyze.load_manifest()
            check("a complete, consistent manifest in an isolated dir loads",
                  m["run_id"] == "TEST-RUN")

            # --- negative: reviewer hand-edits ONE required trial's status to ERROR, hashes intact
            # (the exact REVIEW-EVD-AP-01-005 T005-02 counterexample).
            def to_error(cases):
                cases[0]["status"] = "ERROR"
            analyze.MANIFEST = _write_min_manifest(tmp, mutate=to_error)
            raises("a required trial hand-edited to ERROR (hashes/flags intact) is rejected",
                  analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: REVIEW-EVD-AP-01-006 T006-02, counterexample 1 verbatim. The FIRST
            # regression RUN's status is set to ERROR with every hash and flag left intact. This
            # passed before, because REQUIRED_CASE_NAMES covered only scn.all_cases().
            def regr_to_error(cases):
                for c in cases:
                    if c["name"] == "regr_linear72_baseline":
                        c["status"] = "ERROR"
            analyze.MANIFEST = _write_min_manifest(tmp, mutate=regr_to_error)
            raises("regr_linear72_baseline hand-edited to ERROR (hashes intact) is rejected: a "
                   "regression run is a required trial too",
                   analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: T006-02 counterexample 2. Two repeats declared, one hash recorded; a
            # one-element set is trivially 'all equal' and proves no repetition.
            def one_repeat_hash(cases):
                for c in cases:
                    if c["name"] == "step_up_clean_cad72":
                        c["repeat_csv_sha256"] = c["repeat_csv_sha256"][:1]
            analyze.MANIFEST = _write_min_manifest(tmp, mutate=one_repeat_hash)
            raises("a single repeat hash against repeats_per_case=2 is rejected",
                   analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: a regression entry not linked to the run of the same name.
            def unlink_regr(m):
                m["regression"][0]["new_csv_sha256"] = "c" * 64
            analyze.MANIFEST = _write_min_manifest(tmp, mutate_manifest=unlink_regr)
            raises("a regression entry whose new_csv_sha256 is not the run's CSV is rejected",
                   analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: a required trial is simply missing from the manifest.
            analyze.MANIFEST = _write_min_manifest(tmp, drop_case=analyze.REQUIRED_CASE_NAMES[0])
            raises("a manifest missing a required trial name is rejected",
                  analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: duplicate required trial name.
            def dupe(cases):
                cases.append(dict(cases[0]))
            analyze.MANIFEST = _write_min_manifest(tmp, mutate=dupe)
            raises("a manifest with a duplicated case name is rejected",
                  analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: EMPTY regression list must not silently pass as "all identical".
            analyze.MANIFEST = _write_min_manifest(tmp, empty_regression=True)
            raises("an empty regression list is rejected, not accepted as vacuously identical",
                  analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: manifest's own regression_all_identical flag contradicts the rows.
            analyze.MANIFEST = _write_min_manifest(tmp, empty_regression=True,
                                                    break_regression_flag=True)
            raises("a regression_all_identical=True flag contradicting an empty/failing "
                  "regression list is rejected outright",
                  analyze.load_manifest, analyze.ManifestRejected)

            # --- negative: CSV modified on disk after the manifest was written.
            analyze.MANIFEST = _write_min_manifest(tmp)
            (tmp / f"{analyze.REQUIRED_CASE_NAMES[0]}.csv").write_bytes(b"tampered")
            raises("a CSV that no longer matches its manifest hash is rejected",
                  analyze.load_manifest, analyze.ManifestRejected)
    finally:
        analyze.RESULTS, analyze.MANIFEST = orig_results, orig_manifest


def test_runner_check_required_rejects_identity_gaps() -> None:
    print("transients_runner.check_required(): identity, not just group counts, isolated state")
    orig_manifest = runner.manifest
    try:
        # Positive: every required case + regression present, OK, deterministic.
        # Each case carries a real repeat-hash set (count >= repeats_per_case, all equal, matching
        # csv_sha256) and each regression entry quotes its own run's hash - the contract the
        # runner now enforces (REVIEW-EVD-AP-01-006, T006-02).
        def _h(n):
            return hashlib.sha256(n.encode()).hexdigest()

        good_cases = [dict(name=n, status="OK", group="torque_step_clean",
                           deterministic_repeat=True, csv_sha256=_h(n),
                           repeat_csv_sha256=[_h(n), _h(n)])
                     for n in runner.REQUIRED_CASE_NAMES]
        good_regr = [dict(case=n, pre_existing_columns_identical=True,
                          accepted_csv_sha256="a", new_csv_sha256=_h(n))
                    for n in runner.REQUIRED_REGRESSION_NAMES]
        # REQUIRED_GROUPS counts by group; give every case the group name check_required() expects
        # to see enough of, by overriding group per REQUIRED_GROUPS keys proportionally is overkill
        # for this unit test - instead directly validate the NAME-identity logic in isolation by
        # monkeypatching REQUIRED_GROUPS to require nothing beyond what's supplied here.
        orig_groups = dict(runner.REQUIRED_GROUPS)
        runner.REQUIRED_GROUPS.clear()
        runner.manifest = dict(cases=good_cases, regression=good_regr,
                               regression_all_identical=False)
        try:
            runner.check_required()
            ok = runner.manifest.get("regression_all_identical") is True
            check("a complete, identity-matching manifest recomputes regression_all_identical=True",
                  ok, runner.manifest.get("regression_all_identical"))

            # Negative: empty regression list must not pass as identical.
            runner.manifest = dict(cases=good_cases, regression=[], regression_all_identical=True)
            raises("empty regression list with required names is rejected (no vacuous pass)",
                  runner.check_required, runner.RequiredCaseFailed)

            # Negative: a required case name is simply absent (group counts alone would not catch
            # this if another, non-required, OK case padded the same group).
            padded = [dict(name="not_a_real_required_case", status="OK",
                           group="torque_step_clean", deterministic_repeat=True)] + good_cases[1:]
            runner.manifest = dict(cases=padded, regression=good_regr,
                                   regression_all_identical=False)
            raises("a required trial name missing (even with the group count satisfied by a "
                  "substitute) is rejected",
                  runner.check_required, runner.RequiredTrialMissing)

            # Negative: required case present but not OK.
            bad = copy.deepcopy(good_cases)
            bad[0]["status"] = "ERROR"
            runner.manifest = dict(cases=bad, regression=good_regr, regression_all_identical=False)
            raises("a required trial present but not OK is rejected",
                  runner.check_required, runner.RequiredCaseFailed)

            # Negative (T006-02, counterexample 1): a REGRESSION run set to ERROR. It is a
            # required trial by name now, not merely one unit of a group quota.
            bad_regr_run = copy.deepcopy(good_cases)
            for c in bad_regr_run:
                if c["name"] == "regr_linear72_baseline":
                    c["status"] = "ERROR"
            runner.manifest = dict(cases=bad_regr_run, regression=good_regr,
                                   regression_all_identical=False)
            raises("a regression RUN set to ERROR is rejected by name, not only by group count",
                  runner.check_required, runner.RequiredCaseFailed)

            # Negative (T006-02, counterexample 2): one repeat hash against repeats_per_case=2.
            thin = copy.deepcopy(good_cases)
            thin[0]["repeat_csv_sha256"] = thin[0]["repeat_csv_sha256"][:1]
            runner.manifest = dict(cases=thin, regression=good_regr,
                                   regression_all_identical=False, repeats_per_case=2)
            raises("a single repeat hash against repeats_per_case=2 is rejected by the runner",
                  runner.check_required, runner.RequiredCaseFailed)

            # Negative: a regression entry not tied to the run of the same name.
            unlinked = copy.deepcopy(good_regr)
            unlinked[0]["new_csv_sha256"] = "c" * 64
            runner.manifest = dict(cases=good_cases, regression=unlinked,
                                   regression_all_identical=False)
            raises("a regression entry whose hash is not its run's CSV is rejected",
                  runner.check_required, runner.RequiredCaseFailed)
        finally:
            runner.REQUIRED_GROUPS.clear()
            runner.REQUIRED_GROUPS.update(orig_groups)
    finally:
        runner.manifest = orig_manifest


# ============================ A. interrupted stop ============================================

def test_stop_not_confirmed_by_later_stop() -> None:
    print("stop assessment on an interrupted stop")
    # Assist is 100, stops at t=2.0, reaches 0 at 2.1, RESTARTS at 2.3 back to 100, then stops for
    # good at 5.0 and stays at 0 to the end. Judged over the whole record, the trailing zeros of the
    # SECOND stop would confirm the first one. That is the defect this window bound exists for.
    times = [round(i * 0.01, 4) for i in range(0, 801)]
    vals = []
    for t in times:
        if t < 2.0:
            vals.append(100.0)
        elif t < 2.25:
            vals.append(0.0 if t >= 2.25 else 100.0 * (1.0 - (t - 2.0) / 0.25))
        elif t < 2.3:
            vals.append(0.0)          # reaches zero only 50 ms before the restart
        elif t < 5.0:
            vals.append(100.0)        # pedalling again
        else:
            vals.append(0.0)          # the SECOND stop, held to the end of the record
    bounded = em.stop_between(times, vals, 2.0, 2.3)
    whole = em.stop_between(times, vals, 2.0, 8.1)
    check("whole-record assessment WOULD confirm the first stop (the counterexample)",
          whole["status"] == "CONFIRMED", whole["status"])
    check("bounded assessment does not confirm it on the later stop's evidence",
          bounded["status"] != "CONFIRMED", bounded["status"])
    check("bounded assessment says the observation is too short",
          bounded["status"] == "INSUFFICIENT_OBSERVATION", bounded["status"])
    check("the bounded window is recorded with the result",
          bounded["window_s"] == [2.0, 2.3], str(bounded.get("window_s")))

    # A hold long enough for the contract does confirm, so the bound is not always-negative.
    longer = em.stop_between(times, vals, 2.0, 2.3, min_confirm_s=0.02)
    check("a hold longer than min_confirm_s inside the bounded window still confirms",
          longer["status"] == "CONFIRMED", longer["status"])


def test_restart_analysis_separates_stages() -> None:
    print("restart_analysis separates commanded / pipeline / Iq")
    times = [round(i * 0.01, 4) for i in range(0, 801)]
    n = len(times)
    cols = dict(
        time_s=times,
        ride_interval=[1.0 if t < 2.0 else (0.0 if t < 2.5 else 2.0) for t in times],
        pedalling=[1.0 if t < 2.0 else (0.0 if t < 2.5 else 1.0) for t in times],
        cadence_control_rpm=[70.0 if t < 2.2 else (0.0 if t < 2.6 else 70.0) for t in times],
        gate_steps=[4.0] * n,
        iq_ref=[100.0 if t < 2.2 else (0.0 if t < 2.7 else 100.0) for t in times],
        session=[1.0 if t < 2.2 else 2.0 for t in times],
        debug_flags=[0.0] * n,
    )
    r = em.restart_analysis(cols, 2.5, 2.0, 8.1)
    check("commanded resume is at the scripted restart", abs(r["commanded_resume_latency_s"]) < 1e-9,
          str(r["commanded_resume_latency_s"]))
    check("cadence reaction is LATER than the commanded resume and is measured separately",
          abs(r["cadence_control_latency_s"] - 0.10) < 1e-6, str(r["cadence_control_latency_s"]))
    check("Iq reaction is later still and is its own number",
          abs(r["iq_latency_s"] - 0.20) < 1e-6, str(r["iq_latency_s"]))
    # Renamed per REVIEW-EVD-AP-01-006 (T006-01): zero measured before the restart is an ORDER
    # statement bounded by the crossing bracket, not a "with margin" special case.
    check("Iq zero measured before the restart instant -> reported as observed before it",
          r["iq_state_at_restart"] == "IQ_ZERO_OBSERVED_BEFORE_RESTART",
          r["iq_state_at_restart"])

    # Restart while Iq is still nonzero must be classified differently - and neutrally.
    cols2 = copy.deepcopy(cols)
    cols2["iq_ref"] = [100.0 if t < 2.6 else 50.0 for t in times]
    r2 = em.restart_analysis(cols2, 2.5, 2.0, 8.1)
    check("restart while Iq is still nonzero is stated neutrally, with no decay/release claim",
          r2["iq_state_at_restart"] == "IQ_NONZERO_AT_LAST_PRE_RESTART_SAMPLE",
          r2["iq_state_at_restart"])

    # A reaction that never happens is None, not 0.
    cols3 = copy.deepcopy(cols)
    cols3["iq_ref"] = [100.0 if t < 2.2 else 0.0 for t in times]
    r3 = em.restart_analysis(cols3, 2.5, 2.0, 8.1)
    check("an Iq resume that never happens is None, not 0",
          r3["iq_latency_s"] is None, str(r3["iq_latency_s"]))


def test_commanded_input_check_detects_wrong_input() -> None:
    print("commanded_input_check")
    times = [round(i * 0.01, 4) for i in range(0, 401)]
    cols = dict(time_s=times,
                torque_cmd_mean_nm=[15.0 if t < 2.0 else 40.0 for t in times],
                ride_interval=[1.0] * len(times),
                cadence_gen_rpm=[72.0] * len(times))
    ok = em.commanded_input_check(cols, {
        "before": dict(window_s=[0.5, 2.0], torque_cmd_mean_nm=15.0, ride_interval=1,
                       cadence_gen_rpm_mean=72.0),
        "after": dict(window_s=[2.0, 4.0], torque_cmd_mean_nm=40.0, ride_interval=1),
    })
    check("a correct commanded input passes", ok["all_ok"] is True, json.dumps(ok)[:200])
    bad = em.commanded_input_check(cols, {
        "after": dict(window_s=[2.0, 4.0], torque_cmd_mean_nm=15.0, ride_interval=1),
    })
    check("a wrong expected torque mean FAILS the check", bad["all_ok"] is False)


# ============================ B. native CLI ==================================================

def build_native():
    server = accepted.get_server()
    server.ensure_built()
    return server.EXE


def blobs():
    preset = dict(tuning=accepted.TUNING,
                  banks=[dict(bank_schema_version=8, bank_index=0, active_bank=0,
                              wa_target_rpm=30, cadence_comp_enabled=False,
                              levels=[copy.deepcopy(accepted.LEVEL) for _ in range(5)])])
    b = subprocess.run([accepted.NODE, str(accepted.BRIDGE)], input=json.dumps(preset), text=True,
                       capture_output=True, check=True, cwd=FW)
    return json.loads(b.stdout)


def run_native(exe, bl, **args):
    cfg = {**accepted.BASE, **args}
    cmd = [str(exe), *[f"{k}={v}" for k, v in cfg.items()],
           "tuning_blob=" + bl["tuning_blob"], "bank_blob=" + bl["bank_blob"]]
    return subprocess.run(cmd, text=True, capture_output=True, cwd=FW)


def rows_of(p):
    return list(csv.DictReader(io.StringIO(p.stdout)))


def test_cli_invalid_arguments(exe, bl) -> None:
    print("CLI: invalid arguments are rejected with exit code 2")
    cases = [
        ("lone torque_step_at_s", dict(torque_step_at_s=3), "together"),
        ("lone torque_step_nm", dict(torque_step_nm=40), "together"),
        ("NaN step time", dict(torque_step_at_s="nan", torque_step_nm=40), "invalid argument"),
        ("inf step time", dict(torque_step_at_s="inf", torque_step_nm=40), "invalid argument"),
        ("trailing garbage", dict(torque_step_at_s="3x", torque_step_nm=40), "invalid argument"),
        ("empty value", dict(torque_step_at_s="", torque_step_nm=40), "invalid argument"),
        ("NaN torque", dict(torque_step_at_s=3, torque_step_nm="nan"), "invalid argument"),
        ("step at t=0", dict(torque_step_at_s=0, torque_step_nm=40), "0 < t <= duration"),
        ("step beyond duration", dict(duration=6, torque_step_at_s=7, torque_step_nm=40),
         "0 < t <= duration"),
        ("negative step torque", dict(torque_step_at_s=3, torque_step_nm=-5), "out of range"),
        ("step torque above range", dict(torque_step_at_s=3, torque_step_nm=200), "out of range"),
        ("stop2 without restart", dict(ride_stop_s=5, ride_stop2_s=7), "requires ride_restart_s"),
        ("restart without a stop", dict(ride_stop_s=0, ride_restart_s=6),
         "requires a first stop"),
        ("restart before the stop", dict(duration=10, ride_stop_s=5, ride_restart_s=4),
         "must be > ride_stop_s"),
        ("restart equal to the stop", dict(duration=10, ride_stop_s=5, ride_restart_s=5),
         "must be > ride_stop_s"),
        ("restart beyond duration", dict(duration=8, ride_stop_s=5, ride_restart_s=9),
         "exceeds duration"),
        ("stop2 before the restart", dict(duration=10, ride_stop_s=5, ride_restart_s=6,
                                          ride_stop2_s=5.5), "must be > ride_restart_s"),
        ("stop2 beyond duration", dict(duration=8, ride_stop_s=5, ride_restart_s=6,
                                       ride_stop2_s=9), "exceeds duration"),
        ("NaN restart", dict(ride_stop_s=5, ride_restart_s="nan"), "invalid argument"),
    ]
    for name, args, expect in cases:
        p = run_native(exe, bl, **args)
        check(f"rejected: {name}",
              p.returncode == 2 and expect in p.stderr,
              f"exit={p.returncode} stderr={p.stderr.strip()[:120]}")


def test_cli_step_boundary(exe, bl) -> None:
    print("CLI: torque step boundary and commanded value in the CSV")
    p = run_native(exe, bl, duration=8, cadence=72, torque=15, torque_ripple=0, asymmetry=0,
                   ride_start_s=0.5, ride_stop_s=0, torque_step_at_s=4.0, torque_step_nm=40,
                   sample_ms=5)
    check("step case runs", p.returncode == 0, p.stderr.strip()[:200])
    rows = rows_of(p)
    before = [r for r in rows if abs(float(r["time_s"]) - 3.995) < 1e-9]
    at = [r for r in rows if abs(float(r["time_s"]) - 4.000) < 1e-9]
    check("the sample BEFORE the step still carries the old mean",
          before and abs(float(before[0]["torque_cmd_mean_nm"]) - 15.0) < 1e-6,
          str(before[:1]))
    check("the sample AT the step already carries the new mean (half-open [t, ...))",
          at and abs(float(at[0]["torque_cmd_mean_nm"]) - 40.0) < 1e-6, str(at[:1]))
    check("the generated torque follows the commanded mean without ripple",
          abs(float(at[0]["torque_gen_nm"]) - 40.0) < 1e-3, at[0]["torque_gen_nm"])
    check("crank cadence really is the commanded 72 rpm in the CSV",
          abs(float(rows[400]["cadence_gen_rpm"]) - 72.0) < 1e-6, rows[400]["cadence_gen_rpm"])
    check("ride_interval is 1 throughout a run with no stop",
          {r["ride_interval"] for r in rows if float(r["time_s"]) >= 0.5} == {"1"})


def test_cli_ride_intervals(exe, bl) -> None:
    print("CLI: pedalling interval boundaries and state continuity")
    p = run_native(exe, bl, duration=10, cadence=72, torque=28, ride_start_s=0.5, ride_stop_s=5.0,
                   ride_restart_s=5.5, ride_stop2_s=8.0, sample_ms=5)
    check("two-interval case runs", p.returncode == 0, p.stderr.strip()[:200])
    rows = rows_of(p)
    by_t = {round(float(r["time_s"]), 4): r for r in rows}

    def interval(t):
        return int(by_t[round(t, 4)]["ride_interval"])

    check("interval 1 up to but excluding the stop", interval(4.995) == 1)
    check("the stop instant itself is already outside interval 1", interval(5.0) == 0)
    check("gap is interval 0", interval(5.25) == 0)
    check("the restart instant is already inside interval 2", interval(5.5) == 2)
    check("interval 2 up to but excluding the second stop", interval(7.995) == 2)
    check("the second stop instant is already outside interval 2", interval(8.0) == 0)
    check("after the second stop the rider stays stopped", interval(9.5) == 0)

    # Nothing is reset across the gap: the crank angle resumes from where it froze.
    a_stop = float(by_t[5.0]["crank_angle_deg"])
    a_last_gap = float(by_t[5.495]["crank_angle_deg"])
    check("crank angle does not move during the gap (legs stopped)",
          abs(a_last_gap - a_stop) < 1e-6, f"{a_stop} -> {a_last_gap}")
    a_after = float(by_t[5.6]["crank_angle_deg"])
    check("crank angle resumes from the frozen value, it is not reset to 0",
          a_after > a_stop, f"{a_stop} -> {a_after}")
    check("generated torque is zero during the gap",
          all(abs(float(r["torque_gen_nm"])) < 1e-9
              for r in rows if 5.0 <= float(r["time_s"]) < 5.5))
    check("commanded mean torque is reported even while not pedalling (it is a command, not a load)",
          abs(float(by_t[5.25]["torque_cmd_mean_nm"]) - 28.0) < 1e-6,
          by_t[5.25]["torque_cmd_mean_nm"])

    # Wheel speed is independent of the pedalling intervals.
    check("wheel speed is unaffected by the pedalling gap",
          len({r["speed_kph"] for r in rows}) == 1, "speed changed across the run")


def test_cli_defaults_unchanged(exe, bl) -> None:
    print("CLI: with no new key set, the pre-existing columns are unchanged")
    p = run_native(exe, bl, cadence=72, ride_start_s=0.5, ride_stop_s=5.0)
    check("baseline case runs", p.returncode == 0, p.stderr.strip()[:200])
    rows = rows_of(p)
    accepted_csv = AP01 / "results" / "stop_release_cad72.csv"
    with accepted_csv.open(encoding="utf-8", newline="") as fh:
        old = list(csv.DictReader(fh))
    old_cols = list(old[0].keys())
    check("the accepted CSV's columns are all still present, in order",
          list(rows[0].keys())[:len(old_cols)] == old_cols)
    check("exactly two observation columns are appended",
          list(rows[0].keys())[len(old_cols):] == ["torque_cmd_mean_nm", "ride_interval"],
          str(list(rows[0].keys())[len(old_cols):]))
    check("every pre-existing column is byte-identical to the accepted run",
          len(old) == len(rows) and all(all(a[c] == b[c] for c in old_cols)
                                        for a, b in zip(old, rows)))
    check("the pedalling column still means what it meant (generator diagnostic)",
          all(a["pedalling"] == b["pedalling"] for a, b in zip(old, rows)))


def main() -> int:
    test_event_windows()
    test_step_response_known()
    test_step_response_ripple_flag()
    test_session_and_debug_flag_decode()
    test_restart_order_same_sample_vs_margin()
    test_analyze_classification_uses_only_pre_restart_evidence()
    test_restart_order_uses_crossing_bracket()
    test_footer_lines_do_not_collide_or_overflow()
    test_series_legend_labels_fit()
    test_nice_ticks_uses_requested_scale()
    test_analyze_load_manifest_integrity()
    test_runner_check_required_rejects_identity_gaps()
    test_stop_not_confirmed_by_later_stop()
    test_restart_analysis_separates_stages()
    test_commanded_input_check_detects_wrong_input()

    print("native Controller Lab (a missing toolchain is a FAILURE, not a skip)")
    try:
        exe = build_native()
        bl = blobs()
        check("native harness available and built", exe.exists(), str(exe))
    except Exception as e:  # noqa: BLE001
        check("native harness available and built", False, f"{type(e).__name__}: {e}")
        print(f"\n{PASSED} passed, {len(FAILED)} failed")
        for f in FAILED:
            print("  FAILED:", f)
        return 1

    test_cli_invalid_arguments(exe, bl)
    test_cli_step_boundary(exe, bl)
    test_cli_ride_intervals(exe, bl)
    test_cli_defaults_unchanged(exe, bl)

    print(f"\n{PASSED} passed, {len(FAILED)} failed")
    for f in FAILED:
        print("  FAILED:", f)
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
