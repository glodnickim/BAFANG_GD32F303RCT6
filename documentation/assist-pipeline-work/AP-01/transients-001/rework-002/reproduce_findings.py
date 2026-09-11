"""AP-01 transients-001 / rework-002: reproduce REVIEW-EVD-AP-01-006's counterexamples.

TASK-EVD-AP-01-TRANSIENTS-REWORK-002 requires the counterexamples to be REPRODUCED first, against
whatever code is currently in place, before anything is changed. This script is therefore run twice:

    BEFORE the fix -> rework-002/counterexamples-before.json   (every finding must REPRODUCE)
    AFTER  the fix -> rework-002/counterexamples-after.json    (every finding must be FIXED)

It exercises the REAL paths (event_metrics.restart_analysis, analyze's own classification,
analyze.load_manifest, transients_runner.check_required, make_plots' label/footer geometry), never a
local copy of a helper's field - that shortcut is exactly what REVIEW-EVD-AP-01-006 rejected in
test_analyze_classification_uses_only_pre_restart_evidence.

Nothing here writes to results/, rework-001/ or the 39 CSVs: manifest work happens in a
tempfile.TemporaryDirectory(), and the real CSVs are only ever READ.
"""
from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
AP01 = BASE.parent
sys.path.insert(0, str(AP01))
sys.path.insert(0, str(BASE))

import event_metrics as em  # noqa: E402
import analyze  # noqa: E402
import make_plots  # noqa: E402
import transients_runner as runner  # noqa: E402
from scenarios import scenarios as scn  # noqa: E402

RESULTS = BASE / "results"
ZTOL = em.DEFAULT_ZERO_TOLERANCE

findings: list[dict] = []


def record(fid: str, title: str, reproduced: bool, expected: str, observed) -> None:
    findings.append(dict(finding=fid, title=title, reproduced=bool(reproduced),
                         expected_after_fix=expected, observed=observed))
    print(f"  [{'REPRODUCED' if reproduced else 'fixed     '}] {fid}: {title}")
    print(f"       observed: {observed}")


# --------------------------------------------------------------------------------------------
# helpers

def synth_cols(times, iq, session=None, cadence=None, gate=None, ride_interval=None,
               debug_flags=None) -> dict:
    """Minimal Controller Lab-shaped column set; only the channels restart_analysis reads."""
    n = len(times)
    return dict(
        time_s=list(times), iq_ref=list(iq), iq_allowed=list(iq),
        session=list(session) if session else [1.0] * n,
        cadence_control_rpm=list(cadence) if cadence else [70.0] * n,
        gate_steps=list(gate) if gate else [4.0] * n,
        ride_interval=list(ride_interval) if ride_interval else [1.0] * n,
        debug_flags=list(debug_flags) if debug_flags else [0.0] * n,
        pedalling=[1.0] * n,
    )


def real_case(name: str):
    """Read one of the 39 preserved CSVs (READ-ONLY) plus its commanded event times."""
    m = json.loads((RESULTS / "transients-results.json").read_text(encoding="utf-8"))
    c = {x["name"]: x for x in m["cases"]}[name]
    return c, em.read_csv(RESULTS / c["csv_file"])


def analyzer_verdict(cols, case_name, stop_t, restart_t, end_t) -> dict:
    """Run the analyzer's REAL restart classification, not a helper's field.

    Before the fix analyze.py has no callable classification (it is inline in main()), so this
    reproduces what main() would compute; after the fix it must call the extracted function so the
    test exercises the shipped path.
    """
    body = dict(
        group="stop_restart_standing",
        events={"ride_stop": stop_t, "ride_restart": restart_t},
        effective_args={"speed": 0},
        restart=em.restart_analysis(cols, restart_t, stop_t, end_t),
        stop_before_restart={ch: em.stop_between(cols["time_s"], cols[ch], stop_t, restart_t)
                             for ch in ("iq_ref", "iq_allowed")},
        excursions_around_restart={"iq_ref": em.excursions(cols["time_s"], cols["iq_ref"],
                                                           stop_t, end_t)},
        stop_whole_record_for_contrast={"iq_ref": em.stop_between(
            cols["time_s"], cols["iq_ref"], stop_t, cols["time_s"][-1] + 1.0)},
    )
    fn = getattr(analyze, "restart_classification_row", None)
    if fn is None:
        return dict(verdict="<analyze.restart_classification_row does not exist - "
                            "classification is inline in main() and cannot be exercised>",
                    callable_path=False, restart=body["restart"])
    row = fn(case_name, body)
    row["callable_path"] = True
    row["restart"] = body["restart"]
    return row


# --------------------------------------------------------------------------------------------
# Finding 1 - restart ordering / uncertainty defined backwards

def finding_1() -> None:
    print("\nT006-01  restart ordering and uncertainty")

    # 1a. REVIEW-EVD-AP-01-006's literal counterexample: zero is measured at t=1, strictly BEFORE
    #     the restart at t=2. The ORDER is therefore known; only the exact crossing instant inside
    #     (0, 1] and the confirmation length are uncertain.
    cols = synth_cols([0.0, 1.0, 2.0, 3.0], [10.0, 0.0, 0.0, 10.0])
    r = em.restart_analysis(cols, 2.0, 0.0, 4.0)
    state = r["iq_state_at_restart"]
    bad = "ORDER_UNDETERMINED" in state
    record("T006-01a",
           "zero at t=1 with restart at t=2: order is KNOWN (zero observed before restart)",
           bad, "a state meaning 'zero observed before the restart', not ORDER_UNDETERMINED",
           dict(iq_state_at_restart=state,
                reported_bracket_s=r.get("iq_zero_crossing_bracket_s"),
                reported_order=r.get("iq_zero_order_vs_restart")))

    # 1b. The real 200 ms case: 72 at 5.195, zero first seen at 5.200 which IS the restart time.
    #     The crossing bracket (5.195, 5.200] contains the restart instant -> the order of
    #     "Iq reached zero" vs "restart issued" genuinely cannot be resolved at export resolution.
    c, cols200 = real_case("restart_gap0200ms_speed0")
    ev = c["commanded_event_times_s"]
    r200 = em.restart_analysis(cols200, ev["ride_restart"], ev["ride_stop"],
                               cols200["time_s"][-1] + 1.0)
    t = cols200["time_s"]
    iq = cols200["iq_ref"]
    restart_t = ev["ride_restart"]
    first_zero_i = next((i for i, v in enumerate(iq)
                         if abs(v) <= ZTOL and t[i] >= ev["ride_stop"]), None)
    bracket = ([t[first_zero_i - 1], t[first_zero_i]] if first_zero_i else None)
    straddles = bool(bracket and bracket[0] < restart_t <= bracket[1])
    # The defect is present whenever the bracket contains the restart instant but the reported
    # state does NOT say the order is undetermined.
    flagged = "ORDER_UNDETERMINED" in r200["iq_state_at_restart"]
    record("T006-01b",
           "real 200 ms: zero-crossing bracket contains the restart instant but is NOT reported as "
           "undetermined order",
           straddles and not flagged,
           "the crossing bracket that contains the restart time is reported as undetermined order",
           dict(restart_time_s=restart_t, last_pre_sample=r200["last_pre_restart_sample_time_s"],
                iq_at_restart=r200["iq_value_at_restart"],
                independently_computed_bracket_s=bracket,
                bracket_contains_restart=straddles,
                reported_bracket_s=r200.get("iq_zero_crossing_bracket_s"),
                reported_order=r200.get("iq_zero_order_vs_restart"),
                iq_state_at_restart=r200["iq_state_at_restart"]))

    # 1c. A constantly-positive Iq must not yield a "during the Iq decay" verdict: a positive
    #     sample proves neither a decay nor a release.
    n = 400
    times = [round(i * 0.005, 4) for i in range(n)]
    flat = synth_cols(times, [80.0] * n)
    row = analyzer_verdict(flat, "flat_positive", 1.0, 1.2, 2.0)
    v = str(row.get("verdict"))
    if row.get("callable_path"):
        overreaches, how = ("DECAY" in v), "shipped classification called directly"
    else:
        # Before the fix the classification is inline in main() and cannot be called (that is
        # T006-01d). Judge it on the shipped source's own mapping instead of re-implementing it,
        # so this is still a statement about the shipped code and not a local copy.
        src = (BASE / "analyze.py").read_text(encoding="utf-8")
        state = em.restart_analysis(flat, 1.2, 1.0, 2.0)["iq_state_at_restart"]
        overreaches = (state == "IQ_STILL_NONZERO_AT_RESTART"
                       and 'verdict = "RESTART_DURING_IQ_DECAY' in src)
        v = f"<inline in main(); source maps {state} -> RESTART_DURING_IQ_DECAY_*>"
        how = "read from the shipped analyze.py source (not callable yet - see T006-01d)"
    record("T006-01c",
           "constantly-positive Iq (never decaying) still gets a DECAY verdict",
           overreaches,
           "a neutral verdict ('last pre-restart sample nonzero'), with any decay/trend claim "
           "measured separately",
           dict(verdict=v, evaluated_via=how))

    # 1d. The classification the analyzer actually ships must be callable by a test.
    record("T006-01d",
           "analyzer classification is not callable/testable (inline in main())",
           not row.get("callable_path"),
           "analyze exposes the shipped classification so tests exercise the real path",
           dict(callable_path=row.get("callable_path")))

    # 1e. For an already-positive channel the first positive sample after the restart must not be
    #     presented as a resume time.
    r_flat = flat and em.restart_analysis(flat, 1.2, 1.0, 2.0)
    record("T006-01e",
           "already-positive Iq: first positive sample after restart is reported as a resume time",
           (r_flat["iq_resume_classification"].startswith("ALREADY_POSITIVE")
            and r_flat["iq_resume_s"] is not None),
           "resume time/latency is None for an already-positive channel; the observation is kept "
           "under a diagnostic name",
           dict(classification=r_flat["iq_resume_classification"],
                iq_resume_s=r_flat["iq_resume_s"], iq_latency_s=r_flat["iq_latency_s"]))

    # 1f. Session transitions must carry TIMES, not just a set of names.
    sess = [1.0] * 200 + [2.0] * 200
    cols_s = synth_cols(times, [80.0] * n, session=sess)
    r_s = em.restart_analysis(cols_s, 0.5, 0.2, 2.0)
    record("T006-01f",
           "session transitions are reported as a name set with no times",
           "session_transitions" not in r_s,
           "session transitions carry the time of each change",
           dict(has_session_transitions="session_transitions" in r_s,
                fields=[k for k in r_s if "session" in k]))


# --------------------------------------------------------------------------------------------
# Finding 2 - manifest identity: regression runs and repeat count

def _min_manifest(tmp: Path, mutate=None) -> Path:
    """Isolated manifest whose NAME SET matches the real run: required trials AND the four
    regression runs (group regression_ac_t2), which is precisely what the shipped
    REQUIRED_CASE_NAMES omits."""
    cases = []
    # Real group per case, so the runner's own REQUIRED_GROUPS counts are satisfied and its gate is
    # tested on the identity gap alone rather than tripping over a stub's wrong group labels.
    groups = {c["name"]: c["group"] for c in scn.all_cases()}
    regr_runs = sorted(r["name"] for r in scn.REGRESSION_CASES)
    for name in regr_runs:
        groups[name] = "regression_ac_t2"
    for name in sorted(groups):
        b = f"time_s,iq_ref\n0,0\n{name}\n".encode()
        (tmp / f"{name}.csv").write_bytes(b)
        h = hashlib.sha256(b).hexdigest()
        cases.append(dict(name=name, status="OK", csv_file=f"{name}.csv", csv_sha256=h,
                          repeat_csv_sha256=[h, h], deterministic_repeat=True,
                          group=groups[name],
                          commanded_event_times_s={}, effective_args={}, purpose="test"))
    by = {c["name"]: c for c in cases}
    regression = [dict(case=n, pre_existing_columns_identical=True,
                       accepted_csv_sha256="a" * 64, new_csv_sha256=by[n]["csv_sha256"])
                  for n in regr_runs]
    m = dict(run_id="TEST-RUN", qualification="test", run_complete=True, attempt_state="COMPLETE",
             repeats_per_case=2, cases=cases, regression=regression,
             regression_all_identical=True)
    if mutate:
        mutate(m)
    p = tmp / "transients-results.json"
    p.write_text(json.dumps(m), encoding="utf-8")
    return p


def _accepts(manifest_path: Path, results_dir: Path) -> tuple[bool, str]:
    o_r, o_m = analyze.RESULTS, analyze.MANIFEST
    try:
        analyze.RESULTS, analyze.MANIFEST = results_dir, manifest_path
        analyze.load_manifest()
        return True, "ACCEPTED"
    except analyze.ManifestRejected as e:
        return False, f"rejected: {e}"
    except Exception as e:  # noqa: BLE001
        return False, f"rejected ({type(e).__name__}): {e}"
    finally:
        analyze.RESULTS, analyze.MANIFEST = o_r, o_m


def finding_2() -> None:
    print("\nT006-02  manifest identity: regression runs and repeat count")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        ok, msg = _accepts(_min_manifest(tmp), tmp)
        record("T006-02-positive", "a complete, consistent manifest still loads", not ok,
               "ACCEPTED", msg)

        # 2a. The exact counterexample: the FIRST regression run's status -> ERROR, every hash and
        #     flag left intact. REQUIRED_CASE_NAMES covers only scn.all_cases(), so no check ever
        #     looks at this name.
        def err_regr(m):
            for c in m["cases"]:
                if c["name"] == "regr_linear72_baseline":
                    c["status"] = "ERROR"

        ok, msg = _accepts(_min_manifest(tmp, mutate=err_regr), tmp)
        record("T006-02a",
               "regr_linear72_baseline status=ERROR (hashes intact) is accepted",
               ok, "rejected: a regression RUN is a required trial too", msg)

        # 2b. Two repeats declared (repeats_per_case=2) but only ONE hash recorded: a one-element
        #     set is trivially 'all equal', so the shipped len(set(...)) != 1 check passes.
        def one_hash(m):
            for c in m["cases"]:
                if c["name"] == "step_up_clean_cad72":
                    c["repeat_csv_sha256"] = c["repeat_csv_sha256"][:1]

        ok, msg = _accepts(_min_manifest(tmp, mutate=one_hash), tmp)
        record("T006-02b",
               "step_up_clean_cad72 with a single repeat hash vs repeats_per_case=2 is accepted",
               ok, "rejected: repeat count must match the manifest's own contract", msg)

        # 2c. A regression ENTRY that is not tied to its actual RUN.
        def unlink_regr(m):
            m["regression"][0]["new_csv_sha256"] = "c" * 64

        ok, msg = _accepts(_min_manifest(tmp, mutate=unlink_regr), tmp)
        record("T006-02c",
               "regression entry whose new_csv_sha256 does not match its run is accepted",
               ok, "rejected: the regression entry must be linked to its run's CSV hash", msg)

        # 2d/2e. The same gaps in the runner's own gate.
        def runner_verdict(mutate) -> tuple[bool, str]:
            o = runner.manifest
            try:
                runner.manifest = json.loads(
                    _min_manifest(tmp, mutate=mutate).read_text(encoding="utf-8"))
                runner.check_required()
                return True, "ACCEPTED"
            except Exception as e:  # noqa: BLE001
                return False, f"rejected ({type(e).__name__}): {e}"
            finally:
                runner.manifest = o

        ok, msg = runner_verdict(err_regr)
        record("T006-02d",
               "runner.check_required() and the ERROR regression run",
               ok,
               "rejected by the runner too - and by NAME, not only because an OK-per-group count "
               "happened to drop below its quota",
               msg)

        ok, msg = runner_verdict(one_hash)
        record("T006-02e",
               "runner.check_required() accepts a single repeat hash against repeats_per_case=2",
               ok, "rejected: the runner enforces the repeat-count contract as well", msg)


# --------------------------------------------------------------------------------------------
# Finding 3 - wrong unit label and colliding footer

def finding_3() -> None:
    print("\nT006-03  plot unit label and footer geometry")

    labels = {n: lab for _, series in make_plots.PANELS for n, _, lab in series}
    trn = labels["torque_run_native"]
    record("T006-03a",
           "torque_run_native is labelled [Nm] although it exports assist_delta_run_native "
           "(uint16 native sensor counts; controller_lab.c line 486, inc/torque_input.h line 194)",
           "[Nm]" in trn, "a native-counts unit, data unchanged", dict(label=trn))

    # Footer geometry, derived from the module's own constants in both the old (fixed LEGEND_H,
    # hand-placed rows) and the new (wrapped, height computed from the line count) layouts.
    legend_y = 44 + make_plots.H * len(make_plots.PANELS)
    if hasattr(make_plots, "FOOTER_BLOCKS"):
        avail = make_plots.W - 2 * make_plots.PAD_L
        lines = [ln for b in make_plots.FOOTER_BLOCKS for ln in make_plots.wrap_text(b, avail)]
        ys = [legend_y + 12 + i * make_plots.FOOTER_LINE_H for i in range(len(lines))]
        total_h = legend_y + 12 + make_plots.FOOTER_LINE_H * len(lines)
        widths = [make_plots.PAD_L + len(ln) * make_plots.FOOTER_CHAR_W for ln in lines]
        gaps = [b - a for a, b in zip(ys, ys[1:])]
        collide = any(g < 10 for g in gaps)
        overflow = any(w > make_plots.W for w in widths)
        detail = dict(footer_lines=len(lines), line_ys=ys, min_gap_px=(min(gaps) if gaps else None),
                      max_line_width_px=round(max(widths)), image_width_px=make_plots.W,
                      bottom_line_y=ys[-1], image_height_px=total_h, overflows=overflow)
    else:
        total_h = legend_y + make_plots.LEGEND_H
        y_flags, y_limits = legend_y + 27, total_h - 6
        collide = abs(y_flags - y_limits) < 10
        # 9.5px monospace advance is ~0.6em; the legend starts at PAD_L.
        flags_text = f"debug_flags (bitmask): {make_plots.DEBUG_FLAG_LEGEND}"
        est_w = make_plots.PAD_L + len(flags_text) * 9.5 * 0.6
        overflow = est_w > make_plots.W
        detail = dict(y_debug_flags=y_flags, y_limitations=y_limits,
                      delta_px=y_limits - y_flags, estimated_text_width_px=round(est_w),
                      image_width_px=make_plots.W, overflows=overflow)
    record("T006-03b",
           "bitmask legend and limitations note are drawn at the same height and overflow the "
           "image width",
           collide or overflow,
           "wrapped footer text, separate rows, everything inside the image width",
           detail)


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else "before"
    print(f"=== reproduce REVIEW-EVD-AP-01-006 counterexamples ({which} the fix) ===")
    finding_1()
    finding_2()
    finding_3()

    positives = [f for f in findings if f["finding"].endswith("positive")]
    defects = [f for f in findings if not f["finding"].endswith("positive")]
    n_repro = sum(1 for f in defects if f["reproduced"])
    out = HERE / f"counterexamples-{which}.json"
    me = Path(__file__).resolve()
    out.write_text(json.dumps(dict(
        phase=which,
        source_review="integration/task-reports/REVIEW-EVD-AP-01-006.md",
        task="TASK-EVD-AP-01-TRANSIENTS-REWORK-002",
        script_sha256=hashlib.sha256(me.read_bytes()).hexdigest(),
        analysed_code_sha256={p.name: hashlib.sha256((BASE / p.name).read_bytes()).hexdigest()
                              for p in [BASE / "event_metrics.py", BASE / "analyze.py",
                                        BASE / "transients_runner.py", BASE / "make_plots.py"]},
        defects_total=len(defects), defects_reproduced=n_repro,
        positive_controls=[dict(finding=f["finding"], still_accepted=not f["reproduced"])
                           for f in positives],
        findings=findings), indent=2), encoding="utf-8")
    print(f"\n{n_repro}/{len(defects)} defect counterexamples reproduce; wrote {out.name}")
    for f in positives:
        print(f"positive control {f['finding']}: "
              f"{'still accepted (good)' if not f['reproduced'] else 'BROKEN - now rejected'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
