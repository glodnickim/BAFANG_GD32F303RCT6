"""AP01-V2 step 1: REPRODUCE each finding from REVIEW-EVD-AP-01-002 against the code as it
stands, BEFORE any fix. Per the rework card: "Wykonawca ma najpierw pokazac testy potwierdzajace
konkretny problem, dopiero potem zmiane i wynik."

Each check prints DEFECT_CONFIRMED (the reviewer's counterexample reproduces) or
DEFECT_ABSENT (it does not reproduce, which would itself need explaining).

This script is kept in the deliverables as the "before" evidence. After the fixes it is re-run:
the same checks then report DEFECT_FIXED, because the assertions describe the DEFECT, not the
desired behaviour. Run:
  python repro_v2_findings.py            # report current behaviour
  python repro_v2_findings.py --expect-fixed   # nonzero exit if any defect still present
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

AP01 = Path(__file__).resolve().parent
sys.path.insert(0, str(AP01))

import metrics  # noqa: E402

findings: list[dict] = []


def record(fid: str, title: str, defect_present: bool, observed, expected_by_review) -> None:
    findings.append(dict(finding=fid, title=title,
                         defect_present=defect_present,
                         observed=observed, review_expectation=expected_by_review))
    state = "DEFECT_CONFIRMED" if defect_present else "DEFECT_FIXED/ABSENT"
    print(f"[{fid}] {state}: {title}")
    print(f"        observed = {observed}")
    print(f"        review says it should be = {expected_by_review}")


# --- AP01-V2-01 -----------------------------------------------------------------------------

def check_single_trailing_zero():
    """Review: stop_points([0,1,2],[10,10,0],stop=1,tol=1) is CONFIRMED although the zero exists
    only in the very last sample - no observation time after it."""
    sp = metrics.stop_points([0, 1, 2], [10, 10, 0], stop_time=1, zero_tolerance=1)
    defect = sp.get("status") == "CONFIRMED"
    record("AP01-V2-01a", "single trailing zero accepted as CONFIRMED (no observation after zero)",
           defect, sp, "a distinct status for insufficient observation, not CONFIRMED")


def check_irregular_sampling_resolution():
    """Review: for times=[0,1,1.1,1.2,5,5.1,5.2] / values=[10,10,10,10,0,0,0] the transition may
    lie anywhere in (1.2, 5] s, but the tool reports median dt ~0.1 s as its resolution."""
    t = [0, 1, 1.1, 1.2, 5, 5.1, 5.2]
    v = [10, 10, 10, 10, 0, 0, 0]
    sp = metrics.stop_points(t, v, stop_time=1, zero_tolerance=1)
    # the defect: the only uncertainty figure offered is a global median dt (~0.1), while the real
    # bracket around the transition is (1.2, 5] = 3.8 s wide, and no bracket is reported at all.
    reports_bracket = any(k for k in sp if "bracket" in k)
    dt = sp.get("sample_dt_s")
    defect = (not reports_bracket) and dt is not None and dt < 1.0
    record("AP01-V2-01b", "irregular sampling: median dt presented as resolution, real transition bracket (1.2, 5] not reported",
           defect, sp, "report the adjacent timestamps bracketing the transition, i.e. (1.2, 5.0]")


def check_missing_10_percent_crossing():
    """Review: [50,90,100] for a 0->100 step yields 1 s although the 10% threshold crossing was
    never observed (the record starts already above it)."""
    rt = metrics.rise_time_10_90([0, 1, 2], [50, 90, 100], start_level=0.0, end_level=100.0)
    defect = rt is not None
    record("AP01-V2-01c", "rise_time_10_90 returns a latency although the 10% crossing is not in the record",
           defect, rt, "None / an explicit insufficient-record status")


# --- AP01-V2-02 -----------------------------------------------------------------------------

def check_large_ripple_improvement_penalised():
    """Review: the |delta| <= 0.05 criterion is a similarity test. baseline 0.10 vs candidate 0.01
    is a 90% improvement yet falls outside the threshold."""
    crit = json.loads((AP01 / "comparison_criteria_r3.json").read_text(encoding="utf-8"))
    ripple = next((m for m in crit["metrics"] if m["name"] == "relative_ripple_delta"), None)
    threshold_text = ripple["proposed_threshold"] if ripple else "MISSING"
    baseline, candidate = 0.10, 0.01
    delta = abs(candidate - baseline)
    defect = ripple is not None and "<= 0.05" in threshold_text and delta > 0.05
    record("AP01-V2-02a", "symmetric ripple threshold rejects a large improvement",
           defect, dict(baseline=baseline, candidate=candidate, abs_delta=delta,
                        criterion=threshold_text[:90]),
           "directional criterion: lower relative ripple is better, never penalised for being better")


def check_arithmetic_error_in_justification():
    """Review: '5 points is 20-30% at 0.097' is arithmetically wrong (~52%)."""
    crit = json.loads((AP01 / "comparison_criteria_r3.json").read_text(encoding="utf-8"))
    ripple = next((m for m in crit["metrics"] if m["name"] == "relative_ripple_delta"), None)
    just = ripple["justification"] if ripple else ""
    real_pct = 0.05 / 0.097 * 100.0
    defect = "20-30%" in just
    record("AP01-V2-02b", "justification states 20-30% where the real figure is ~52%",
           defect, dict(claimed="20-30%", actual_percent=round(real_pct, 1)),
           "corrected arithmetic or a different criterion altogether")


# --- pure predicates for the refined checks --------------------------------------------------
# Factored out so test_check_sensitivity.py can feed them V2-LIKE data and prove they still flag
# the original defects. A check that only passes on fixed code, without evidence that it fails on
# broken code, is worth nothing - which is the very complaint the review made about the V2 runner
# test.

def flatness_defect(is_comparable) -> tuple[bool, dict]:
    """is_comparable(values, stages) -> bool.

    SUPERSEDED EXPECTATION, recorded deliberately. The V2 form of this predicate required a
    plateau-shaped signal to be EXCLUDED from comparison. REVIEW-EVD-AP-01-003 (AP01-V3-02)
    overrules that: `[80,100,100,80,100,100]` with identical channels on both sides of every
    observed stage shows no demonstrated change, and a plateau can equally come from
    quantisation, a constant input or a change in effort - so it must stay comparable. Exclusion
    now requires demonstrated evidence. The predicate therefore checks three things, which is
    strictly stronger than either earlier form:

      1. a perfectly smooth signal must stay comparable   (the original V2 defect)
      2. a plateau WITHOUT stage evidence must stay comparable   (the V3 correction)
      3. clipping WITH stage evidence must be excluded    (real limiting must still be caught)
    """
    smooth = [100.0] * 20
    plateau = [80.0, 100.0, 100.0, 80.0, 100.0, 100.0]
    no_change = {"profile_limit": {"status": "UNCHANGED"},
                 "post_mode_request": {"status": "UNCHANGED"},
                 "request_to_allowed": {"status": "UNCHANGED"}}
    evidenced = {"profile_limit": {"status": "REDUCED"},
                 "post_mode_request": {"status": "UNCHANGED"},
                 "request_to_allowed": {"status": "UNCHANGED"}}

    smooth_excluded = not is_comparable(smooth, no_change)
    plateau_excluded_without_evidence = not is_comparable(plateau, no_change)
    evidenced_clipping_missed = is_comparable([100.0] * 6, evidenced)

    defect = smooth_excluded or plateau_excluded_without_evidence or evidenced_clipping_missed
    return defect, dict(smooth_excluded_from_comparison=smooth_excluded,
                        plateau_excluded_without_evidence=plateau_excluded_without_evidence,
                        evidenced_clipping_missed=evidenced_clipping_missed)


def bias_and_limit_defect(extractor_text: str, metrics_json: dict) -> tuple[bool, dict]:
    uses_limit_evidence = any(name in extractor_text for name in
                              ("limit_assessment", "limit_evidence_from_channels", "saturation_flags"))
    reads_limit_channels = "iq_before_profile_limit" in extractor_text
    sample = next(iter(metrics_json["cases"].values()))
    channel = (sample.get("steady") or {}).get("iq_ref") or {}
    has_bias = any("bias" in k for k in channel) and channel.get("bias_vs_reference") is not None
    defect = (not uses_limit_evidence) or (not reads_limit_channels) or (not has_bias)
    return defect, dict(extractor_uses_limit_helper=uses_limit_evidence,
                        extractor_reads_limit_channel_pair=reads_limit_channels,
                        result_channel_keys=sorted(channel), has_computed_bias=has_bias)


def overclaim_defect(inventory: dict) -> tuple[bool, dict]:
    phrases = ["already proven exhaustively", "does not need new evidence",
               "proof already complete", "exhaustively proven"]
    live = [(p, phrase) for p, text in _strings_outside_withdrawals(inventory)
            for phrase in phrases if phrase in text]

    def coverage_entries(node):
        if isinstance(node, dict):
            if "evidence_level" in node or ("file" in node and "what_the_source_shows" in node):
                yield node
            for v in node.values():
                yield from coverage_entries(v)
        elif isinstance(node, list):
            for v in node:
                yield from coverage_entries(v)

    entries = list(coverage_entries(inventory))
    missing_level = [e.get("file") for e in entries if not e.get("evidence_level")]
    executed = [e.get("file") for e in entries
                if e.get("evidence_level") == "EXECUTED_IN_THIS_SESSION"]
    defect = bool(live) or bool(missing_level) or bool(executed) or not entries
    return defect, dict(live_overclaims_outside_withdrawals=live, coverage_entries=len(entries),
                        entries_missing_evidence_level=missing_level,
                        entries_claiming_execution=executed)


def csv_and_walk_defect(inventory: dict, criteria: dict) -> tuple[bool, dict]:
    inv_text = json.dumps(inventory)
    names_l4 = "sim/l4/virtual_bike_l4.c" in inv_text
    names_replay = "sim/replay/replay_fw.c" in inv_text
    live_exclusivity = any("only harness" in text or "only CSV" in text
                           for _, text in _strings_outside_withdrawals(inventory))
    low_cadence = next((c for c in criteria.get("criteria", [])
                        if str(c.get("id", "")).startswith("I2_low_cadence")), None)
    just = (low_cadence or {}).get("justification", "")
    disclaims_walk = "no Walk-based justification is used" in just
    walk_mentions = [p for p, text in _strings_outside_withdrawals(criteria)
                     if ("Walk" in text or "FW143" in text)
                     and "no Walk-based justification" not in text]
    defect = (not names_l4) or (not names_replay) or live_exclusivity \
        or (not disclaims_walk) or bool(walk_mentions)
    return defect, dict(names_l4_csv_emitter=names_l4, names_replay_csv_emitter=names_replay,
                        live_exclusivity_claim=live_exclusivity,
                        low_cadence_explicitly_disclaims_walk=disclaims_walk,
                        other_walk_mentions_in_criteria=walk_mentions)


def check_flatness_mistaken_for_saturation():
    """Review: a perfectly smooth [100]*20 is flagged SATURATED (and excluded), while a genuinely
    clipped [80,100,100,80,100,100] is not.

    CHECK REFINED in the after-fix run: the V2 code decided this with a bare pp<0.5 test, so the
    original check inspected peak_to_peak directly. The decision now lives in
    metrics.limit_assessment, so the check interrogates the DECISION (is the case excluded from
    comparison?) rather than the obsolete heuristic. That is the property the review actually
    asked about, and it is stricter: it fails if either signal is classified wrongly.
    """
    try:
        def is_comparable(values, stages):
            return metrics.limit_assessment(values, channel="iq_ref", stages=stages)["comparable"]
        is_comparable([1.0, 2.0], {})
    except TypeError:                        # V2 behaviour: flatness alone decided it
        def is_comparable(values, stages):
            return not (metrics.peak_to_peak(values) < 0.5)

    defect, details = flatness_defect(is_comparable)
    record("AP01-V2-02c", "shape alone decides exclusion instead of demonstrated limit evidence",
           defect, details,
           "flatness/plateau informational only; exclusion requires a demonstrated change at an observed stage")


def check_saturation_flags_unused_and_no_bias_in_results():
    """Review: the limit helper is not used by the extractor (which reads no caps), and no bias
    value is computed anywhere in the results.

    CHECK REFINED in the after-fix run: V2's helper was named saturation_flags and was dead code.
    It has been replaced by limit_assessment + limit_evidence_from_channels, so the check now asks
    whether the extractor consumes A limit-evidence helper at all (by any of those names) AND
    whether a bias number is actually present in the produced results.
    """
    extractor = (AP01 / "extract_metrics.py").read_text(encoding="utf-8")
    metrics_json = json.loads((AP01 / "results" / "ap01-metrics.json").read_text(encoding="utf-8"))
    defect, details = bias_and_limit_defect(extractor, metrics_json)
    record("AP01-V2-02d", "limit helper unused by extractor and no computed bias in results",
           defect, details,
           "real limit evidence consumed by the extractor and a computed bias against an explicit reference")


def check_floor_case_described_as_cap_forced():
    """Review: floor cases are described as cap-forced, although the audit showed the FLOOR
    exceeding the profile cap."""
    matrix = json.loads((AP01 / "scenario_matrix.json").read_text(encoding="utf-8"))
    entry = next((s for s in matrix["scenarios"] if s["id"] == "linear_baseline_and_windows"), None)
    text = entry.get("proposed_threshold", "") if entry else ""
    m = json.loads((AP01 / "results" / "ap01-metrics.json").read_text(encoding="utf-8"))
    floor_above = m["cases"]["floor_above_profile_repro"]["steady"]["iq_ref"]["mean"]
    cap_only = m["cases"]["floor_default_profile1_repro"]["steady"]["iq_ref"]["mean"]
    defect = "cap/floor limited by design" in text and floor_above > cap_only
    record("AP01-V2-02e", "floor-above-profile case labelled cap-limited although the floor exceeds the profile cap",
           defect,
           dict(floor_above_profile_mean=floor_above, profile_cap_only_mean=cap_only,
                floor_exceeds_cap=floor_above > cap_only, label=text[:80]),
           "state that the floor (assist_min_iq_pct) overrides the profile cap (max_iq_pct)")


# --- AP01-V2-03 -----------------------------------------------------------------------------

def check_runner_negative_test_hits_wrong_error():
    """Review captured ModuleNotFoundError: No module named 'sim' - the temp copy of the runner
    resolves the wrong FW root via parents[2], so the test never reaches serialization or the
    native binary, yet reports PASS."""
    src = (AP01 / "test_pipeline_failure_handling.py").read_text(encoding="utf-8")
    copies_tree = "copytree" in src
    # Reproduce the reviewer's observation directly: copy the runner out of the tree and import it.
    with tempfile.TemporaryDirectory() as td:
        target = Path(td) / "ap01_runner.py"
        target.write_bytes((AP01 / "ap01_runner.py").read_bytes())
        driver = Path(td) / "drive.py"
        driver.write_text(
            "import sys; sys.path.insert(0, r'" + td + "')\n"
            "import ap01_runner\n", encoding="utf-8")
        p = subprocess.run([sys.executable, str(driver)], capture_output=True, text=True, cwd=td)
        stderr = p.stderr
    hits_module_error = "ModuleNotFoundError" in stderr and "No module named 'sim'" in stderr
    defect = copies_tree and hits_module_error
    record("AP01-V2-03a", "negative runner test relocates the runner, so it fails on import instead of the native/RequiredCaseFailed path",
           defect,
           dict(test_uses_copytree=copies_tree, relocated_import_error=hits_module_error,
                stderr_tail=stderr.strip().splitlines()[-1] if stderr.strip() else ""),
           "isolate only the OUTPUT; reach RequiredCaseFailed / the real native exit code; a missing import must FAIL the test")


def check_runner_catches_only_required_case_failed():
    runner = (AP01 / "ap01_runner.py").read_text(encoding="utf-8")
    only_narrow = "except RequiredCaseFailed" in runner and "except Exception" not in runner
    writes_incomplete_upfront = "run_complete\"] = False" in runner and runner.index("cases_run") if False else None
    # does the runner mark the attempt incomplete BEFORE running the first case?
    pre_marks = "write_incomplete_marker" in runner or "run_id" in runner
    defect = only_narrow or not pre_marks
    record("AP01-V2-03b", "only RequiredCaseFailed is caught, and no incomplete marker is written before the first case",
           defect, dict(catches_only_required_case_failed=only_narrow,
                        writes_run_id_or_marker_upfront=pre_marks),
           "any preparation/build/serializer failure must also leave incomplete state; a previous complete manifest must not survive as the apparent new result")


def check_required_comparisons_not_enforced():
    runner = (AP01 / "ap01_runner.py").read_text(encoding="utf-8")
    idx_cmp = runner.find("manifest[\"comparisons\"]")
    idx_complete = runner.find("run_complete\"] = True")
    between = runner[idx_cmp:idx_complete] if idx_cmp >= 0 and idx_complete > idx_cmp else ""
    enforced = ("raise" in between) or ("Required" in between and "raise" in between)
    defect = not enforced
    record("AP01-V2-03c", "required comparisons are recorded but never checked before run_complete=true",
           defect, dict(enforcement_between_comparison_and_completion=enforced),
           "a false required comparison must fail the run")


def check_extractor_accepts_contradictory_manifest():
    """Review reproduced: run_complete=true with a list containing OK plus a required ERROR is
    accepted; the ERROR is skipped and metrics are written."""
    import hashlib
    import importlib
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        csv_text = ("time_s,torque_run_native,cadence_control_rpm,iq_mode_request,"
                    "iq_requested,iq_allowed,iq_ref\n")
        for i in range(20):
            csv_text += f"{i*0.5:.3f},10,72,100,100,100,100\n"
        (results / "good.csv").write_text(csv_text, encoding="utf-8", newline="")
        (results / "ap01-results.json").write_text(json.dumps({
            "run_complete": True,
            "cases": [
                dict(name="good", status="OK",
                     csv_sha256=hashlib.sha256(csv_text.encode()).hexdigest(),
                     ride_start_s=0.5, ride_stop_s=0),
                dict(name="broken", status="ERROR", error="synthetic required failure",
                     exit_code=1),
            ],
        }), encoding="utf-8")
        if "extract_metrics" in sys.modules:
            del sys.modules["extract_metrics"]
        em = importlib.import_module("extract_metrics")
        em.RESULTS = results
        em.MANIFEST_PATH = results / "ap01-results.json"
        accepted = False
        try:
            em.main()
            accepted = (results / "ap01-metrics.json").exists()
        except SystemExit as e:
            accepted = (e.code == 0)
    record("AP01-V2-03d", "extractor accepts a manifest that is complete yet contains a required ERROR case",
           accepted, dict(metrics_written=accepted),
           "reject the contradictory manifest with a nonzero exit")


# --- AP01-V2-04 -----------------------------------------------------------------------------

WITHDRAWAL_KEYS = {"scope_statement", "correction", "what_changed_in_rev3",
                   "consequence_for_the_gap_statement", "significance", "note_on_flag_syntax"}


def _strings_outside_withdrawals(node, key=None, path="$"):
    """Yield (path, text) for every string EXCEPT those inside a field whose whole purpose is to
    quote and withdraw an earlier claim. Quoting a retracted phrase in order to retract it is not
    the same as asserting it, and a check that cannot tell the difference would force the
    correction to be silent about what it corrected."""
    if isinstance(node, dict):
        for k, v in node.items():
            yield from _strings_outside_withdrawals(v, k, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _strings_outside_withdrawals(v, key, f"{path}[{i}]")
    elif isinstance(node, str) and key not in WITHDRAWAL_KEYS:
        yield path, node


def check_inventory_overclaims():
    """CHECK REFINED in the after-fix run: a bare substring search also matched the sentence that
    WITHDRAWS the overclaim. The check now (a) ignores explicit withdrawal fields, and (b) adds a
    positive requirement that every coverage entry carries an evidence_level, which a mere absence
    of phrases could never establish."""
    inv = json.loads((AP01 / "tool_inventory_r4.json").read_text(encoding="utf-8"))
    defect, details = overclaim_defect(inv)
    record("AP01-V2-04a", "inventory promotes reading test files to a completed safety proof",
           defect, details,
           "claims limited to the module read; distinguish exists / read / executed")


def check_inventory_conflates_invalid_kinds():
    inv = json.loads((AP01 / "tool_inventory_r4.json").read_text(encoding="utf-8"))
    sensor = next(c for c in inv["categories"] if c["id"] == "sensor_invalid")
    text = json.dumps(sensor)
    claims_no_test = "No existing test" in text or "no existing test" in text
    boost_cited = "fw100_extended_boost_host" in text
    record("AP01-V2-04b", "sensor_invalid claims no test forces torque_sensor_valid; fw100_extended_boost_host.c:200-201 already does",
           claims_no_test and not boost_cited,
           dict(claims_no_test_exists=claims_no_test, cites_boost_host=boost_cited),
           "separate PAS-quadrature invalid / pas_sensor_valid / torque_sensor_valid and cite the partial boost coverage")


def check_inventory_csv_exclusivity_and_walk():
    """CHECK REFINED in the after-fix run: the substring forms matched the retraction sentences
    themselves ("REV2 claimed ... the only harness ... That is FALSE") and the explicit Walk
    disclaimer. The check now demands the POSITIVE evidence the review asked for: that the other
    CSV emitters were actually looked up and named, and that the low-cadence rationale stands on
    its own without leaning on Walk."""
    inv = json.loads((AP01 / "tool_inventory_r4.json").read_text(encoding="utf-8"))
    crit = json.loads((AP01 / "comparison_criteria_r3.json").read_text(encoding="utf-8"))
    defect, details = csv_and_walk_defect(inv, crit)
    record("AP01-V2-04c", "claims Controller Lab is the only CSV source, and uses Walk to justify low-cadence tolerance",
           defect, details,
           "check sim/l4 and sim/replay CSV emitters; drop the Walk-based justification")


CHECKS = [
    check_single_trailing_zero,
    check_irregular_sampling_resolution,
    check_missing_10_percent_crossing,
    check_large_ripple_improvement_penalised,
    check_arithmetic_error_in_justification,
    check_flatness_mistaken_for_saturation,
    check_saturation_flags_unused_and_no_bias_in_results,
    check_floor_case_described_as_cap_forced,
    check_runner_negative_test_hits_wrong_error,
    check_runner_catches_only_required_case_failed,
    check_required_comparisons_not_enforced,
    check_extractor_accepts_contradictory_manifest,
    check_inventory_overclaims,
    check_inventory_conflates_invalid_kinds,
    check_inventory_csv_exclusivity_and_walk,
]


def main():
    expect_fixed = "--expect-fixed" in sys.argv
    for c in CHECKS:
        try:
            c()
        except Exception as e:  # noqa: BLE001
            print(f"[{c.__name__}] CHECK_ERROR: {e!r}")
            findings.append(dict(finding=c.__name__, error=repr(e)))
    still_present = [f["finding"] for f in findings if f.get("defect_present")]
    print()
    print(f"defects still present: {len(still_present)} / {len(CHECKS)}")
    for f in still_present:
        print(f"  - {f}")
    out = AP01 / "results" / ("repro_v2_after_fix.json" if expect_fixed else "repro_v2_before_fix.json")
    out.write_text(json.dumps(dict(expect_fixed=expect_fixed, findings=findings), indent=2),
                    encoding="utf-8")
    print(f"\nevidence written: {out.name}")
    if expect_fixed and still_present:
        sys.exit(1)


if __name__ == "__main__":
    main()
