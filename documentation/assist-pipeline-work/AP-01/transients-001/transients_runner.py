"""AP-01 transients-001 runner: torque step and stop/restart trials on the EXISTING Controller Lab.

Same discipline as the accepted ap01_runner.py (which this file IMPORTS its presets and build/
serializer access from, rather than retyping them):

- the manifest is claimed as IN_PROGRESS before any work, so a failed preparation cannot leave an
  earlier complete manifest standing as this attempt's result;
- a failed required case aborts the whole run with a nonzero exit and run_complete=false;
- a missing required trial closes the manifest as incomplete;
- every case records the effective parameters, the full command, and the source/executable/CSV
  hashes;
- every case is executed TWICE and the two CSV hashes must match, which is what "deterministic
  repetition" means here.

Nothing under ../results/ is written or rewritten: the accepted 21-case series and its manifest are
READ_ONLY for this task. Output goes to ./results/ (or $AP01_TRANSIENTS_RESULTS_DIR).

Qualification (repeated in every report using these numbers): controller_lab.c does not close the
PMSM/FOC loop. u_abs and measured battery current are fixed at zero. Cadence/torque/speed are
forced synthetic inputs. These results characterise REQUEST COMPOSITION for given forced inputs -
not mechanical/electrical performance, not actual current, bridge or hardware behaviour.
"""
from __future__ import annotations

import copy
import csv
import datetime
import hashlib
import io
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent
FW = AP01.parents[2]
sys.path.insert(0, str(AP01))
sys.path.insert(0, str(HERE))

import ap01_runner as accepted  # noqa: E402 - accepted presets/build access, imported not copied
from scenarios import scenarios  # noqa: E402

RESULTS = Path(os.environ.get("AP01_TRANSIENTS_RESULTS_DIR", HERE / "results"))
RESULTS.mkdir(parents=True, exist_ok=True)
MANIFEST_PATH = RESULTS / "transients-results.json"
ACCEPTED_RESULTS = AP01 / "results"

REPEATS = 2


class RequiredCaseFailed(RuntimeError):
    pass


class RequiredTrialMissing(RuntimeError):
    pass


manifest = dict(
    tool="transients_runner.py",
    task="TASK-EVD-AP-01-TRANSIENTS-001",
    run_id=f"{datetime.datetime.now().astimezone().strftime('%Y%m%dT%H%M%S%z')}-{uuid.uuid4().hex[:8]}",
    generated_at=datetime.datetime.now().astimezone().isoformat(),
    qualification=(
        "Forced inputs, production C via the existing Controller Lab. No FOC/PMSM response; "
        "u_abs and battery current fixed at zero. New CLI keys change the SYNTHETIC rider input "
        "only; no assist/limit/ramp math is reimplemented in the harness."
    ),
    repeats_per_case=REPEATS,
    run_complete=False,
    attempt_state="IN_PROGRESS",
    cases=[],
    regression=[],
)


def write_manifest() -> None:
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def build_blobs(tc: dict | None, lc: dict | None) -> tuple[dict, dict]:
    t = {**accepted.TUNING, **(tc or {})}
    lv = {**accepted.LEVEL, **(lc or {})}
    preset = dict(tuning=t, banks=[dict(bank_schema_version=8, bank_index=0,
        active_bank=0, wa_target_rpm=30, cadence_comp_enabled=False,
        levels=[copy.deepcopy(lv) for _ in range(5)])])
    b = subprocess.run([accepted.NODE, str(accepted.BRIDGE)], input=json.dumps(preset), text=True,
                       capture_output=True, check=True, cwd=FW)
    return preset, json.loads(b.stdout)


def run_case(name: str, args: dict, tc: dict | None = None, lc: dict | None = None,
             extra: dict | None = None) -> dict:
    """Run one case REPEATS times; identical CSV hashes are required for it to count."""
    server = accepted.get_server()
    status = server.ensure_built()
    exe = server.EXE
    preset, blobs = build_blobs(tc, lc)
    cfg = {**accepted.BASE, **args}
    cmd = [str(exe), *[f"{k}={v}" for k, v in cfg.items()],
           "tuning_blob=" + blobs["tuning_blob"], "bank_blob=" + blobs["bank_blob"]]

    hashes, stdouts, stderrs = [], [], []
    for _ in range(REPEATS):
        p = subprocess.run(cmd, text=True, capture_output=True, cwd=FW)
        if p.returncode:
            rec = dict(name=name, status="ERROR", exit_code=p.returncode, error=p.stderr,
                       args=cfg, command=cmd)
            manifest["cases"].append(rec)
            raise RequiredCaseFailed(
                f"required case '{name}' failed: exit={p.returncode} stderr={p.stderr.strip()}")
        hashes.append(hashlib.sha256(p.stdout.encode()).hexdigest())
        stdouts.append(p.stdout)
        stderrs.append(p.stderr.strip())

    deterministic = len(set(hashes)) == 1
    csv_path = RESULTS / (name + ".csv")
    csv_path.write_text(stdouts[0], encoding="utf-8", newline="")
    rec = dict(
        name=name, status="OK",
        commanded_event_times_s=dict(extra.get("events", [])) if extra else {},
        commanded_note=("these are the SCRIPTED input times; the observed pipeline reaction is "
                        "measured separately in transients-metrics.json and is not assumed equal"),
        group=(extra or {}).get("group"),
        purpose=(extra or {}).get("purpose"),
        effective_args=cfg, tuning_override=tc, level_override=lc,
        command=cmd, preset=preset, blobs=blobs,
        csv_file=csv_path.name,
        csv_sha256=hashes[0], repeat_csv_sha256=hashes,
        deterministic_repeat=deterministic,
        exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        source_hash=status["source_hash"],
        stderr=stderrs[0],
    )
    manifest["cases"].append(rec)
    if not deterministic:
        raise RequiredCaseFailed(f"case '{name}' is not deterministic across {REPEATS} runs: {hashes}")
    print(f"{name:34s} OK  det={deterministic} sha={hashes[0][:12]}")
    return rec


PRE_EXISTING_COLUMNS = [
    "time_s", "crank_angle_deg", "torque_gen_nm", "torque_load_kg", "torque_fast_native",
    "torque_run_native", "cadence_gen_rpm", "cadence_raw_rpm", "cadence_control_rpm", "speed_kph",
    "battery_v", "soc_pct", "human_power_w", "assist_basis_power_w", "motor_power_w", "support_pct",
    "iq_mode_request", "iq_before_profile_limit", "iq_requested", "iq_allowed", "iq_pre_ramp",
    "iq_ref", "session", "debug_flags", "battery_limit", "gate_steps", "gate_load_ckg", "pedalling",
]
NEW_COLUMNS = ["torque_cmd_mean_nm", "ride_interval"]


def run_regression() -> None:
    """AC-T2: with the new options unset, the pre-existing columns must be identical.

    The accepted CSV in ../results/ is READ only. New columns are compared separately - they are
    appended observations and cannot be part of an "unchanged" claim about the old ones.
    """
    for spec in scenarios.REGRESSION_CASES:
        rec = run_case(spec["name"], spec["args"], tc=spec["tc"], lc=spec["lc"],
                       extra=dict(group="regression_ac_t2",
                                  purpose="new options unset -> pre-existing behaviour must match"))
        acc_path = ACCEPTED_RESULTS / (spec["accepted"] + ".csv")
        new_path = RESULTS / (spec["name"] + ".csv")
        with acc_path.open(encoding="utf-8", newline="") as fh:
            acc = list(csv.DictReader(fh))
        with new_path.open(encoding="utf-8", newline="") as fh:
            new = list(csv.DictReader(fh))
        acc_cols = list(acc[0].keys())
        new_cols = list(new[0].keys())
        same_len = len(acc) == len(new)
        old_identical = (same_len and acc_cols == PRE_EXISTING_COLUMNS
                         and new_cols == PRE_EXISTING_COLUMNS + NEW_COLUMNS
                         and all(all(a[c] == b[c] for c in PRE_EXISTING_COLUMNS)
                                 for a, b in zip(acc, new)))
        diffs = []
        if same_len:
            for i, (a, b) in enumerate(zip(acc, new)):
                for c in PRE_EXISTING_COLUMNS:
                    if a.get(c) != b.get(c):
                        diffs.append(dict(row=i, column=c, accepted=a.get(c), new=b.get(c)))
                        if len(diffs) >= 10:
                            break
                if len(diffs) >= 10:
                    break
        manifest["regression"].append(dict(
            case=spec["name"], accepted_case=spec["accepted"],
            accepted_csv=str(acc_path.relative_to(FW)).replace("\\", "/"),
            accepted_csv_sha256=hashlib.sha256(acc_path.read_bytes()).hexdigest(),
            new_csv_sha256=hashlib.sha256(new_path.read_bytes()).hexdigest(),
            rows_accepted=len(acc), rows_new=len(new),
            accepted_columns=acc_cols, new_columns=new_cols,
            new_columns_appended=NEW_COLUMNS,
            pre_existing_columns_identical=old_identical,
            first_differences=diffs or None,
        ))
        print(f"{spec['name']:34s} regression pre_existing_identical={old_identical}")


REQUIRED_GROUPS = {
    "torque_step_clean": 6,
    "torque_step_ripple": 2,
    "stop_restart_standing": len(scenarios.RESTART_GAPS_S),
    "stop_restart_rolling": len(scenarios.RESTART_GAPS_S),
    "stop_restart_second_stop": 1,
    "regression_ac_t2": len(scenarios.REGRESSION_CASES),
}

# Identity of every required trial/regression NAME, not merely a per-group count. Computed from the
# same scenario definitions run_case() consumes, so it cannot drift from what actually ran.
# REVIEW-EVD-AP-01-006 (T006-02): the four AC-T2 regression trials are runs too, so they belong in
# the by-NAME required set. Previously only a per-group OK count covered them, which happens to
# catch a single ERROR but says nothing about WHICH name failed, and misses a substitution that
# keeps the count intact.
REQUIRED_REGRESSION_NAMES = sorted(r["name"] for r in scenarios.REGRESSION_CASES)
REQUIRED_CASE_NAMES = sorted([c["name"] for c in scenarios.all_cases()]
                             + REQUIRED_REGRESSION_NAMES)
MIN_REPEATS = 2


def check_required() -> None:
    """Reject on group-count success but missing/duplicate/non-OK required NAMES.

    REVIEW-EVD-AP-01-005 (T005-02): the previous version only counted OK cases per group, so an
    empty regression list (or one where every entry silently failed but total ran==0) produced
    `regression_all_identical=True` by vacuous truth (`not []` is True) - a false positive with no
    actual comparison behind it. This version checks the required NAME appears exactly once, is
    OK, and is proven deterministic/hashed, and separately requires the regression list to be
    non-empty and to contain every required regression name before it can claim "identical".
    """
    got = {}
    names_seen = [c["name"] for c in manifest["cases"]]
    dupes = sorted({n for n in names_seen if names_seen.count(n) > 1})
    if dupes:
        raise RequiredCaseFailed(f"duplicate case names in this run's own manifest: {dupes}")
    by_name = {c["name"]: c for c in manifest["cases"]}
    for c in manifest["cases"]:
        if c.get("status") == "OK":
            got[c.get("group")] = got.get(c.get("group"), 0) + 1
    missing = {g: (n, got.get(g, 0)) for g, n in REQUIRED_GROUPS.items() if got.get(g, 0) < n}
    if missing:
        raise RequiredTrialMissing(f"required trials missing (group: expected, got): {missing}")

    missing_names = [n for n in REQUIRED_CASE_NAMES if n not in by_name]
    if missing_names:
        raise RequiredTrialMissing(f"required trial names absent from this manifest: {missing_names}")
    not_ok = [n for n in REQUIRED_CASE_NAMES if by_name[n].get("status") != "OK"]
    if not_ok:
        raise RequiredCaseFailed(f"required trials present but not OK: {not_ok}")

    bad = [c["name"] for c in manifest["cases"] if not c.get("deterministic_repeat")]
    if bad:
        raise RequiredCaseFailed(f"non-deterministic cases: {bad}")

    # Repeat COUNT against the run's own contract, before any "all hashes equal" test - a
    # single-element list is trivially equal to itself and proves nothing was repeated
    # (REVIEW-EVD-AP-01-006, T006-02).
    required_repeats = max(MIN_REPEATS, int(manifest.get("repeats_per_case") or 0))
    thin = [(n, len(by_name[n].get("repeat_csv_sha256") or []))
            for n in REQUIRED_CASE_NAMES
            if len(by_name[n].get("repeat_csv_sha256") or []) < required_repeats]
    if thin:
        raise RequiredCaseFailed(
            f"required trials with fewer repeat hashes than repeats_per_case={required_repeats} "
            f"(name, recorded): {thin}")
    mismatched = [n for n in REQUIRED_CASE_NAMES
                  if len(set(by_name[n]["repeat_csv_sha256"])) != 1
                  or by_name[n].get("csv_sha256") != by_name[n]["repeat_csv_sha256"][0]]
    if mismatched:
        raise RequiredCaseFailed(f"required trials whose repeat hashes disagree: {mismatched}")

    regr = manifest["regression"]
    regr_names_seen = [r["case"] for r in regr]
    regr_dupes = sorted({n for n in regr_names_seen if regr_names_seen.count(n) > 1})
    if regr_dupes:
        raise RequiredCaseFailed(f"duplicate regression case names: {regr_dupes}")
    regr_by_name = {r["case"]: r for r in regr}
    regr_missing = [n for n in REQUIRED_REGRESSION_NAMES if n not in regr_by_name]
    if regr_missing:
        raise RequiredCaseFailed(
            f"required regression cases absent - cannot claim regression_all_identical with an "
            f"incomplete or empty regression list: {regr_missing}")
    unlinked = [(n, regr_by_name[n].get("new_csv_sha256"))
                for n in REQUIRED_REGRESSION_NAMES
                if n not in by_name
                or regr_by_name[n].get("new_csv_sha256") != by_name[n].get("csv_sha256")]
    if unlinked:
        raise RequiredCaseFailed(
            f"regression entries not linked to the run of the same name (entry hash must be the "
            f"CSV that run produced): {unlinked}")
    failed_regr = [n for n in REQUIRED_REGRESSION_NAMES
                   if not regr_by_name[n]["pre_existing_columns_identical"]
                   or not regr_by_name[n].get("accepted_csv_sha256")
                   or not regr_by_name[n].get("new_csv_sha256")]
    manifest["regression_all_identical"] = (not failed_regr) and (not regr_missing) and bool(regr)
    if failed_regr:
        raise RequiredCaseFailed(
            f"AC-T2 regression: pre-existing columns changed in {failed_regr}")


def main() -> None:
    write_manifest()
    run_regression()
    for case in scenarios.all_cases():
        run_case(case["name"], case["args"], extra=case)
    check_required()
    manifest["run_complete"] = True
    manifest["attempt_state"] = "COMPLETE"
    write_manifest()
    print(json.dumps(dict(cases=len(manifest["cases"]),
                          regression=len(manifest["regression"]),
                          regression_all_identical=manifest["regression_all_identical"],
                          run_complete=True), indent=2))


if __name__ == "__main__":
    try:
        main()
    except BaseException as e:  # noqa: BLE001 - any failure must close the manifest as incomplete
        manifest["run_complete"] = False
        manifest["attempt_state"] = "ABORTED"
        manifest["abort_reason"] = str(e)
        manifest["abort_type"] = type(e).__name__
        try:
            write_manifest()
        except Exception as werr:  # noqa: BLE001
            print(f"ABORTED: {type(e).__name__}: {e} (manifest not written: {werr})",
                  file=sys.stderr)
            sys.exit(1)
        print(f"ABORTED: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)
