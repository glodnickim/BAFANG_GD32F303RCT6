"""AP01-R5 / AP01-V2-03 acceptance checks for the failure paths of the AP-01 tooling.

REV3 (rework after REVIEW-EVD-AP-01-002). The V2 version of this file passed for the WRONG reason:
it copied ap01_runner.py into a temp directory, which broke `from sim.controller_lab import
server`, so the process died on ModuleNotFoundError long before the native binary or the
RequiredCaseFailed path was reached - and the test still reported PASS. Now:

- only the OUTPUT is redirected (AP01_RESULTS_DIR); the runner stays where it is, so its
  dependencies resolve exactly as in a real run;
- the tests assert on the CAUSE (RequiredCaseFailed, the native exit code and the native message),
  and an import/tooling error is an explicit test FAILURE, never a pass;
- the real entry point is exercised (`python ap01_runner.py --abort-selftest`);
- a preparation failure occurring while an OLD COMPLETE manifest exists must not leave that old
  manifest standing as this attempt's result;
- a stale existing metrics file is identified by name when extraction is refused.

Runs in isolated temp OUTPUT directories; never touches the accepted 21-case evidence in
results/, the production tree, or any read-only file.

Run: python test_pipeline_failure_handling.py
"""
from __future__ import annotations

import hashlib
import importlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

AP01 = Path(__file__).resolve().parent
RUNNER = AP01 / "ap01_runner.py"
PYTHON = sys.executable

SYNTHETIC_HEADER = ("time_s,torque_run_native,cadence_control_rpm,iq_mode_request,"
                    "iq_before_profile_limit,iq_requested,iq_allowed,iq_ref\n")


def synthetic_csv(rows: int = 20, value: float = 100.0) -> str:
    out = SYNTHETIC_HEADER
    for i in range(rows):
        out += f"{i*0.5:.3f},10,72,{value:.0f},{value:.0f},{value:.0f},{value:.0f},{value:.0f}\n"
    return out


def write_case_manifest(results: Path, csv_text: str, name: str = "case1",
                         run_complete: bool = True, extra_cases: list | None = None,
                         run_id: str = "test-run-id") -> None:
    (results / f"{name}.csv").write_text(csv_text, encoding="utf-8", newline="")
    cases = [dict(name=name, status="OK",
                  csv_sha256=hashlib.sha256(csv_text.encode()).hexdigest(),
                  ride_start_s=0.5, ride_stop_s=0)]
    cases += (extra_cases or [])
    (results / "ap01-results.json").write_text(json.dumps(
        {"run_complete": run_complete, "run_id": run_id, "cases": cases}), encoding="utf-8")


def load_extractor(results_dir: Path):
    """Import extract_metrics fresh, pointed at an isolated output directory."""
    sys.path.insert(0, str(AP01))
    if "extract_metrics" in sys.modules:
        del sys.modules["extract_metrics"]
    em = importlib.import_module("extract_metrics")
    em.RESULTS = results_dir
    em.MANIFEST_PATH = results_dir / "ap01-results.json"
    return em


def expect_extractor_refusal(em, capsys_note: str = ""):
    try:
        em.main()
    except SystemExit as e:
        assert e.code != 0, f"expected nonzero exit {capsys_note}"
        return
    raise AssertionError(f"expected SystemExit {capsys_note}")


# --- runner: the real entry point, the real native error ------------------------------------

def test_runner_reaches_the_real_native_error_not_an_import_error():
    """AP01-V2-03 core defect: prove the failure path actually gets to the native binary."""
    with tempfile.TemporaryDirectory() as td:
        env = dict(os.environ, AP01_RESULTS_DIR=td)
        p = subprocess.run([PYTHON, str(RUNNER), "--abort-selftest"],
                            capture_output=True, text=True, env=env, cwd=str(AP01))
        stderr = p.stderr

        assert "ModuleNotFoundError" not in stderr, (
            "the runner failed on an import, not on the case under test - this is a TEST error, "
            f"not a pass. stderr:\n{stderr}")
        assert p.returncode != 0, f"expected nonzero exit, got {p.returncode}"
        assert "RequiredCaseFailed" in stderr, f"expected RequiredCaseFailed in stderr:\n{stderr}"
        # the native binary's own diagnosis must be carried through
        assert "invalid/unavailable assist mode 97" in stderr, (
            f"expected the native error message in stderr:\n{stderr}")
        assert "exit=2" in stderr, f"expected the native exit code in stderr:\n{stderr}"

        manifest = json.loads((Path(td) / "ap01-results.json").read_text(encoding="utf-8"))
        assert manifest["run_complete"] is False
        assert manifest["attempt_state"] == "ABORTED"
        assert manifest["abort_type"] == "RequiredCaseFailed"
        case = manifest["cases"][0]
        assert case["status"] == "ERROR"
        assert case["exit_code"] == 2
        assert "invalid/unavailable assist mode 97" in case["error"]


def test_preparation_failure_does_not_leave_an_old_complete_manifest_standing():
    """A failure BEFORE the first case completes, while a previous successful manifest exists in
    the output directory, must invalidate that manifest for this attempt."""
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        # a previous, successful run's manifest is already here
        write_case_manifest(results, synthetic_csv(), run_complete=True, run_id="OLD-COMPLETE-RUN")
        old = json.loads((results / "ap01-results.json").read_text(encoding="utf-8"))
        assert old["run_complete"] is True and old["run_id"] == "OLD-COMPLETE-RUN"

        # break the serializer step (preparation), not the native case
        env = dict(os.environ, AP01_RESULTS_DIR=td,
                   AP01_NODE=str(Path(td) / "definitely-not-node.exe"))
        p = subprocess.run([PYTHON, str(RUNNER), "--abort-selftest"],
                            capture_output=True, text=True, env=env, cwd=str(AP01))
        assert "ModuleNotFoundError" not in p.stderr, p.stderr
        assert p.returncode != 0

        now = json.loads((results / "ap01-results.json").read_text(encoding="utf-8"))
        assert now["run_complete"] is False, "old complete manifest survived a failed attempt"
        assert now["run_id"] != "OLD-COMPLETE-RUN", "the manifest still identifies the OLD run"
        assert now["attempt_state"] == "ABORTED"
        assert now["abort_type"] not in (None, "RequiredCaseFailed"), (
            f"expected a preparation error type, got {now['abort_type']}: {now['abort_reason']}")


def test_required_comparison_false_fails_the_run():
    sys.path.insert(0, str(AP01))
    if "ap01_runner" in sys.modules:
        del sys.modules["ap01_runner"]
    runner = importlib.import_module("ap01_runner")
    # positive: all required comparisons true -> no raise
    runner.check_required_comparisons({"window30_vs_180": True, "window180_vs_360": True})
    # negative: a false required comparison must raise
    try:
        runner.check_required_comparisons({"window30_vs_180": True, "window180_vs_360": False})
        raise AssertionError("a false required comparison did not fail the run")
    except runner.RequiredComparisonFailed as e:
        assert "window180_vs_360" in str(e)
    # negative: a missing required comparison must raise
    try:
        runner.check_required_comparisons({"window30_vs_180": True})
        raise AssertionError("a missing required comparison did not fail the run")
    except runner.RequiredComparisonFailed as e:
        assert "missing" in str(e)


# --- extractor: manifest integrity ----------------------------------------------------------

def test_extractor_rejects_contradictory_manifest_ok_plus_required_error():
    """AP01-V2-03, reproduced by the reviewer: run_complete=true alongside a required ERROR case
    was accepted, the ERROR silently skipped, and metrics written."""
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        write_case_manifest(results, synthetic_csv(), run_complete=True, extra_cases=[
            dict(name="broken", status="ERROR", error="synthetic required failure", exit_code=1)])
        em = load_extractor(results)
        expect_extractor_refusal(em, "for a contradictory manifest")
        assert not (results / "ap01-metrics.json").exists()


def test_extractor_identifies_a_stale_existing_metrics_file():
    """Not merely 'no new file was written in an empty temp': an OLD metrics file must be named
    and marked as not describing the current manifest."""
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        # a metrics file from an earlier, different run
        (results / "ap01-metrics.json").write_text(
            json.dumps({"source_manifest_run_id": "PREVIOUS-RUN", "cases": {}}), encoding="utf-8")
        (results / "ap01-results.json").write_text(json.dumps(
            {"run_complete": False, "run_id": "NEW-RUN", "abort_reason": "synthetic",
             "cases": []}), encoding="utf-8")
        em = load_extractor(results)
        import io
        from contextlib import redirect_stderr
        buf = io.StringIO()
        with redirect_stderr(buf):
            try:
                em.main()
            except SystemExit as e:
                assert e.code != 0
        err = buf.getvalue()
        assert "STALE" in err, f"stale metrics file was not identified:\n{err}"
        assert "PREVIOUS-RUN" in err, f"the stale file's run_id was not reported:\n{err}"
        # and the stale file must not have been overwritten with something that looks current
        still = json.loads((results / "ap01-metrics.json").read_text(encoding="utf-8"))
        assert still["source_manifest_run_id"] == "PREVIOUS-RUN"


def test_extractor_rejects_incomplete_manifest():
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        (results / "ap01-results.json").write_text(json.dumps({
            "run_complete": False, "abort_reason": "synthetic test", "cases": []}), encoding="utf-8")
        expect_extractor_refusal(load_extractor(results), "for run_complete=false")


def test_extractor_rejects_missing_manifest():
    with tempfile.TemporaryDirectory() as td:
        expect_extractor_refusal(load_extractor(Path(td)), "for a missing manifest")


def test_extractor_rejects_tampered_csv():
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        csv_text = synthetic_csv()
        write_case_manifest(results, csv_text)
        em = load_extractor(results)
        em.main()                                   # positive: unmodified input works
        assert (results / "ap01-metrics.json").exists()
        (results / "case1.csv").write_text(csv_text + "999,1,1,1,1,1,1,1\n",
                                            encoding="utf-8", newline="")
        (results / "ap01-metrics.json").unlink()
        expect_extractor_refusal(em, "for a CSV hash mismatch")
        assert not (results / "ap01-metrics.json").exists()


def test_extractor_rejects_missing_csv_for_ok_case():
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        (results / "ap01-results.json").write_text(json.dumps({
            "run_complete": True, "run_id": "x",
            "cases": [dict(name="ghost", status="OK", csv_sha256="deadbeef",
                           ride_start_s=0.5, ride_stop_s=0)]}), encoding="utf-8")
        expect_extractor_refusal(load_extractor(results), "for a missing CSV")


def test_extractor_rejects_csv_missing_a_required_channel():
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        truncated = "time_s,iq_ref\n" + "".join(f"{i*0.5:.3f},100\n" for i in range(20))
        write_case_manifest(results, truncated)
        expect_extractor_refusal(load_extractor(results), "for a CSV missing channels")


def test_extractor_positive_case_still_works():
    with tempfile.TemporaryDirectory() as td:
        results = Path(td)
        write_case_manifest(results, synthetic_csv())
        em = load_extractor(results)
        em.main()
        report = json.loads((results / "ap01-metrics.json").read_text(encoding="utf-8"))
        assert report["source_manifest_run_id"] == "test-run-id"
        assert "case1" in report["cases"]
        steady = report["cases"]["case1"]["steady"]["iq_ref"]
        # a perfectly flat synthetic signal must remain COMPARABLE (AP01-V2-02)
        assert steady["comparable"] is True
        assert steady["limit_status"] == "FLAT_NO_LIMIT_EVIDENCE"


TESTS = [
    test_runner_reaches_the_real_native_error_not_an_import_error,
    test_preparation_failure_does_not_leave_an_old_complete_manifest_standing,
    test_required_comparison_false_fails_the_run,
    test_extractor_rejects_contradictory_manifest_ok_plus_required_error,
    test_extractor_identifies_a_stale_existing_metrics_file,
    test_extractor_rejects_incomplete_manifest,
    test_extractor_rejects_missing_manifest,
    test_extractor_rejects_tampered_csv,
    test_extractor_rejects_missing_csv_for_ok_case,
    test_extractor_rejects_csv_missing_a_required_channel,
    test_extractor_positive_case_still_works,
]


def main():
    failures = []
    for t in TESTS:
        try:
            t()
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failures.append(t.__name__)
            print(f"FAIL {t.__name__}: {e}")
        except Exception as e:  # noqa: BLE001
            failures.append(t.__name__)
            print(f"ERROR {t.__name__}: {e!r}")
    if failures:
        print(f"\n{len(failures)}/{len(TESTS)} FAILED: {failures}")
        sys.exit(1)
    print(f"\nAll {len(TESTS)} pipeline-failure-handling tests PASS.")


if __name__ == "__main__":
    main()
