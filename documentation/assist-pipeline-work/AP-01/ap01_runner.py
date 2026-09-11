"""AP-01 runner: reproduces baseline steady-state cases and extends the cadence sweep, using the
EXISTING native Controller Lab (sim/controller_lab/controller_lab.c) and the EXISTING CANable
serializer (sim/controller_lab/canable_bridge.js -> canable-web's own canbus.js).

Read-only w.r.t. production src/inc, canable-web, and original audit evidence
(documentation/assist-pipeline-evidence/*). Writes only under this AP-01 directory and the
shared native-lab build cache .build/controller_lab/ (rebuild-if-fingerprint-changed, same as
run_probe.py and the Controller Lab web UI already do).

Qualification of the generator (must be repeated in every report referencing these numbers):
controller_lab.c does not close the PMSM/FOC loop. u_abs and measured battery current are fixed
at zero. cadence/torque/speed are forced synthetic inputs, not derived from bike dynamics. These
results characterise REQUEST COMPOSITION for given forced inputs (assist_modes -> limits ->
fast_iq_slew as seen in iq_ref), not mechanical/electrical performance and not "actual" motor
behaviour. AC4 boundary: forced-input / electrical / Level4 / HW are different evidence classes;
u_abs=0 here does not describe a real ride.

REV3 (rework after REVIEW-EVD-AP-01-002, finding AP01-V2-03):
- The results directory can be redirected with AP01_RESULTS_DIR so a negative test can isolate the
  OUTPUT without relocating this file. Relocating the script breaks the sim.controller_lab import
  and makes a failure test pass for the wrong reason (the V2 test failed on ModuleNotFoundError
  and never reached the native binary).
- An "in progress" manifest carrying a fresh run_id and run_complete=false is written BEFORE the
  first case, so a failure during preparation (build, serializer, import) cannot leave a previous
  complete manifest standing as the apparent result of this attempt.
- Any exception - not only RequiredCaseFailed - marks the attempt incomplete and exits nonzero.
- Required comparisons are CHECKED before run_complete=true is written.

REV2 (rework after REVIEW-EVD-AP-01-001, finding AP01-R5): a failed REQUIRED case aborts the whole
run with a nonzero exit code instead of silently recording an error and continuing.
extract_metrics.py reads ONLY this run's own results/ap01-results.json (hash-checked against the
CSVs on disk), never a directory listing.
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

AP01 = Path(__file__).resolve().parent
FW = AP01.parents[2]          # motor-controller-firmware/
ROOT = FW.parent              # eVistDrive/
# Output location only. The code, and therefore its dependency resolution, stays in place.
RESULTS = Path(os.environ.get("AP01_RESULTS_DIR", AP01 / "results"))
RESULTS.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(FW))


def get_server():
    """Import the existing Controller Lab server lazily.

    Deliberately NOT a module-level import: a module-level failure here would abort before
    mark_attempt_in_progress() could claim the manifest, leaving a previous complete manifest
    standing as the apparent result of this attempt (AP01-V2-03). Imported inside main()'s
    try-block instead, so an unavailable harness is recorded as an aborted attempt.
    """
    from sim.controller_lab import server  # noqa: PLC0415 - intentionally deferred
    return server


NODE = os.environ.get("AP01_NODE", r"C:\Program Files\nodejs\node.exe")
BRIDGE = FW / "sim/controller_lab/canable_bridge.js"

# Comparisons that must hold for the run to count as complete (AP01-V2-03).
REQUIRED_COMPARISONS = ("window30_vs_180", "window180_vs_360")

# Same tuning/level/base defaults as documentation/assist-pipeline-evidence/run_probe.py,
# duplicated here (not imported) because that file is original audit evidence and READ_ONLY;
# this is an independent reproduction, not a shared-code dependency on it.
TUNING = dict(tuning_schema_version=8, startup_boost_cadence_step=20,
    assist_run_deadband_mv=5, assist_hold_ms=1400, assist_min_iq_pct=2,
    assist_torque_run_window_deg=180, assist_start_steps=4,
    assist_torque_full_scale_centikg=6000, crank_length_mm=165)
LEVEL = dict(mode_type=1, support_ratio_pct=160, support_min_pct=80,
    support_max_pct=260, reference_power_w=200, progression_pct=50,
    curve_exponent_x10=15, curve_exponent_high_x10=15,
    emtb_parameter=140, emtb_based_on_power=True, emtb_reference_voltage_mv=36000,
    torque_assist_factor=120, max_motor_power_w=0, max_iq_pct=100,
    assist_without_rotation=False, minimum_pedal_load_kg=0.7,
    riding_minimum_pedal_load_kg=0.3, startup_boost_enabled=True,
    startup_boost_mode=0, startup_boost_strength_pct=100, startup_boost_end_rpm=27,
    smooth_start_enabled=False, smooth_start_ms=300, release_ms=100,
    power_rise_filter_ms=190, power_fall_filter_ms=450,
    iq_rise_slow_ms=325, iq_rise_fast_ms=190, iq_fall_slow_ms=525,
    iq_fall_fast_ms=125, extended_boost_trigger_load_kg=8,
    extended_boost_strength_pct=100, extended_boost_duration_ms=0)
BASE = dict(duration=10, cadence=72, torque=28, torque_ripple=45, asymmetry=8,
    speed=18, voltage=39, soc=55, assist=3, mode="keep", sample_ms=5,
    ride_start_s=0.5, ride_stop_s=8)

manifest = dict(
    tool="ap01_runner.py",
    tool_revision="REV3 (post REVIEW-EVD-AP-01-002 rework)",
    run_id=f"{datetime.datetime.now().astimezone().strftime('%Y%m%dT%H%M%S%z')}-{uuid.uuid4().hex[:8]}",
    generated_at=datetime.datetime.now().astimezone().isoformat(),
    qualification=(
        "Forced inputs, production C via existing Controller Lab. No FOC/PMSM response; "
        "u_abs and battery current fixed at zero. Not a re-implementation of assist_modes/"
        "limits/fast_iq_slew: those run as the real production objects."
    ),
    run_complete=False,
    cases=[],
)
allrows: dict[str, list[dict]] = {}

MANIFEST_PATH = RESULTS / "ap01-results.json"


class RequiredCaseFailed(RuntimeError):
    pass


class RequiredComparisonFailed(RuntimeError):
    pass


def write_manifest() -> None:
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def mark_attempt_in_progress() -> None:
    """Claim the manifest for THIS attempt before doing any work.

    Without this, a failure during preparation (native build, node serializer, import) leaves a
    previously successful manifest untouched, and it then reads as the result of this attempt
    (AP01-V2-03).
    """
    manifest["run_complete"] = False
    manifest["attempt_state"] = "IN_PROGRESS"
    write_manifest()


def run(name: str, tc: dict | None = None, lc: dict | None = None, args: dict | None = None,
        required: bool = True):
    """Run one Controller Lab case. required=True (default): a failure aborts the whole run
    (AP01-R5) instead of being recorded and skipped, so a partial manifest is never mistaken
    for a complete one. required=False is reserved for genuinely optional/exploratory cases and
    is not used by main() below - every case this script runs today is required."""
    server = get_server()
    status = server.ensure_built()
    exe = server.EXE
    t = {**TUNING, **(tc or {})}
    lv = {**LEVEL, **(lc or {})}
    preset = dict(tuning=t, banks=[dict(bank_schema_version=8, bank_index=0,
        active_bank=0, wa_target_rpm=30, cadence_comp_enabled=False,
        levels=[copy.deepcopy(lv) for _ in range(5)])])
    b = subprocess.run([NODE, str(BRIDGE)], input=json.dumps(preset), text=True,
        capture_output=True, check=True, cwd=FW)
    blobs = json.loads(b.stdout)
    cfg = {**BASE, **(args or {})}
    cmd = [str(exe), *[f"{k}={v}" for k, v in cfg.items()],
        "tuning_blob=" + blobs["tuning_blob"], "bank_blob=" + blobs["bank_blob"]]
    p = subprocess.run(cmd, text=True, capture_output=True, cwd=FW)
    if p.returncode:
        manifest["cases"].append(dict(name=name, status="ERROR", error=p.stderr, exit_code=p.returncode))
        print(name, "ERROR", p.stderr.strip())
        if required:
            raise RequiredCaseFailed(f"required case '{name}' failed: exit={p.returncode} stderr={p.stderr.strip()}")
        return None
    rows = list(csv.DictReader(io.StringIO(p.stdout)))
    (RESULTS / (name + ".csv")).write_text(p.stdout, encoding="utf-8", newline="")
    manifest["cases"].append(dict(
        name=name, status="OK", preset=preset, args=cfg, blobs=blobs,
        csv_sha256=hashlib.sha256(p.stdout.encode()).hexdigest(),
        exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        source_hash=status["source_hash"],
        ride_start_s=cfg.get("ride_start_s"), ride_stop_s=cfg.get("ride_stop_s"),
    ))
    allrows[name] = rows
    return rows


def check_required_comparisons(comparisons: dict, required=REQUIRED_COMPARISONS) -> None:
    """A recorded comparison is not a checked comparison (AP01-V2-03)."""
    missing = [k for k in required if k not in comparisons]
    failed = [k for k in required if comparisons.get(k) is False]
    if missing or failed:
        raise RequiredComparisonFailed(
            f"required comparisons not satisfied: missing={missing} failed={failed}")


def main(abort_selftest: bool = False):
    mark_attempt_in_progress()

    if abort_selftest:
        # Deliberate failure path, exercised through the REAL entry point so the negative test
        # proves that a failing case reaches RequiredCaseFailed and the __main__ handler, rather
        # than dying earlier for an unrelated reason (AP01-V2-03). mode=97 is rejected by the
        # native binary itself ("invalid/unavailable assist mode 97", exit 2).
        run("selftest_invalid_native_mode", args={"mode": 97})
        raise AssertionError("--abort-selftest: the invalid case did not raise")

    # Reproduction of the original 3-cadence baseline (mode 1) plus the AC-required sweep
    # 20/30/40/60/72/90/110/120 rpm (card section "Wejscia i zakres" item 2).
    for cadence in [20, 30, 40, 60, 72, 90, 110, 120]:
        run(f"sweep_mode1_cad{cadence}", lc={"mode_type": 1}, args={"cadence": cadence})

    # Baseline linear-window reproduction: linear 72 rpm, RUN 30/180/360 deg window, floor vs
    # profile, without rotation (card item 1).
    run("linear72_baseline", args={"cadence": 72})
    for window in [30, 180, 360]:
        run(f"window{window}_repro", tc={"assist_torque_run_window_deg": window}, args={"cadence": 72})
    run("floor_above_profile_repro", tc={"assist_min_iq_pct": 25}, lc={"max_iq_pct": 1}, args={"cadence": 72})
    run("floor_default_profile1_repro", lc={"max_iq_pct": 1}, args={"cadence": 72})
    run("without_rotation_repro", lc={"assist_without_rotation": True},
        args={"cadence": 0, "torque_ripple": 0, "speed": 0})
    run("without_rotation_off_repro", args={"cadence": 0, "torque_ripple": 0, "speed": 0})

    # Uneven legs: asymmetry sweep (harness DOES support this via asymmetry_pct).
    for asym in [0, 8, 30, 60]:
        run(f"uneven_legs_asym{asym}", args={"cadence": 72, "asymmetry": asym})

    # Stop / restart-adjacent case supported by the harness's single ride_start_s/ride_stop_s
    # event: pedal, stop mid-run, observe release/decay. This is NOT "restart during release"
    # (harness has only one stop event, no re-start after it) - see scenario_matrix.json for the
    # gap and the proposed extension.
    run("stop_release_cad72", args={"cadence": 72, "ride_start_s": 0.5, "ride_stop_s": 5.0})

    def identical(a, b, keys):
        ra, rb = allrows.get(a), allrows.get(b)
        if ra is None or rb is None or len(ra) != len(rb):
            return False
        return all(all(x[k] == y[k] for k in keys) for x, y in zip(ra, rb))

    keys = ["torque_run_native", "iq_mode_request", "iq_requested", "iq_allowed", "iq_ref"]
    manifest["comparisons"] = {
        "window30_vs_180": identical("window30_repro", "window180_repro", keys),
        "window180_vs_360": identical("window180_repro", "window360_repro", keys),
    }
    manifest["required_comparisons"] = list(REQUIRED_COMPARISONS)
    # Checked, not merely recorded: a false required comparison must fail the run.
    check_required_comparisons(manifest["comparisons"])

    failed_cases = [c["name"] for c in manifest["cases"] if c.get("status") != "OK"]
    if failed_cases:
        raise RequiredCaseFailed(f"cases not OK at completion: {failed_cases}")

    manifest["run_complete"] = True
    manifest["attempt_state"] = "COMPLETE"
    write_manifest()
    print(json.dumps({
        "cases_run": len(manifest["cases"]),
        "cases_error": sum(1 for c in manifest["cases"] if c.get("status") == "ERROR"),
        "comparisons": manifest.get("comparisons"),
    }, indent=2))


if __name__ == "__main__":
    try:
        main(abort_selftest="--abort-selftest" in sys.argv)
    except BaseException as e:  # noqa: BLE001 - see below
        # ANY failure - a required case, a failed required comparison, a native build error, a
        # missing serializer, an import error, an interrupt - must leave the attempt marked
        # incomplete and exit nonzero. Catching only RequiredCaseFailed (REV2) left preparation
        # failures able to abort while a previous complete manifest still stood as the apparent
        # result of this attempt (AP01-V2-03).
        manifest["run_complete"] = False
        manifest["attempt_state"] = "ABORTED"
        manifest["abort_reason"] = str(e)
        manifest["abort_type"] = type(e).__name__
        try:
            write_manifest()
        except Exception as write_err:  # noqa: BLE001
            print(f"ABORTED: {type(e).__name__}: {e} "
                  f"(and the incomplete manifest could not be written: {write_err})",
                  file=sys.stderr)
            sys.exit(1)
        print(f"ABORTED: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)
