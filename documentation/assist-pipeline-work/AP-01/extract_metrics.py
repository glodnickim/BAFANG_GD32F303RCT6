"""Apply metrics.py to ap01_runner.py CSV outputs; write results/ap01-metrics.json.

REV3 (rework after REVIEW-EVD-AP-01-002):
- AP01-V2-02: limiting is now read from a REAL channel pair (iq_before_profile_limit vs
  iq_requested) instead of being guessed from flatness, and a computed bias against an explicitly
  named reference case is written into the results. A smooth window is no longer excluded from
  comparison for being smooth.
- AP01-V2-01: stop analysis passes an explicit observation contract (min_confirm_s) and a maximum
  transition gap, and stores the transition bracket rather than a median sampling interval.
- AP01-V2-03: a manifest that claims run_complete=true while still containing a non-OK case is
  contradictory and is rejected; when extraction is refused, an existing older metrics file is
  named explicitly so it cannot pass as the current result.

REV2 (after REVIEW-EVD-AP-01-001): windows derived per case from the manifest's ride_start_s/
ride_stop_s (R2); manifest-only inputs with CSV hash verification (R5).
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
import sys
from pathlib import Path

import metrics

AP01 = Path(__file__).resolve().parent
RESULTS = Path(os.environ.get("AP01_RESULTS_DIR", AP01 / "results"))
MANIFEST_PATH = RESULTS / "ap01-results.json"

CHANNELS = ["torque_run_native", "cadence_control_rpm", "iq_before_profile_limit",
            "iq_mode_request", "iq_requested", "iq_allowed", "iq_ref"]

# Channels needed to reconstruct the observed stages of the Iq chain (metrics.IQ_STAGES).
# REV4 (AP01-V3-01): the profile limit sits between iq_before_profile_limit and iq_mode_request,
# NOT between iq_before_profile_limit and iq_requested. REV3 compared the outer pair and so
# reported a single guessed cause for what are three separate stages.
STAGE_CHANNELS = ["iq_before_profile_limit", "iq_mode_request", "iq_requested", "iq_allowed"]

# Explicitly named reference case for the computed bias. Bias is only meaningful against a stated
# reference; this one is the nominal 72 rpm baseline run. For cases whose INPUTS differ from the
# reference (e.g. the cadence sweep) the bias is a diagnostic of the input change, not a defect
# measure - that is recorded alongside the number.
BIAS_REFERENCE_CASE = "linear72_baseline"

STOP_ZERO_TOLERANCE = 1.0          # Iq counts
STOP_MIN_CONFIRM_S = 0.10          # measurement contract, see metrics.DEFAULT_MIN_CONFIRM_S
# The Controller Lab export is uniformly sampled at sample_ms=5, so any bracket wider than 10
# samples means the record has a gap and the stop instant is not localised.
STOP_MAX_TRANSITION_GAP_S = 0.05
STEADY_WARMUP_S = 3.0


class ManifestError(RuntimeError):
    pass


def load_manifest() -> dict:
    if not MANIFEST_PATH.exists():
        raise ManifestError(f"missing manifest: {MANIFEST_PATH} (run ap01_runner.py first)")
    m = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if not m.get("run_complete", False):
        raise ManifestError(
            "manifest run_complete=false (the run did not finish) - refusing to extract metrics "
            "from a partial/aborted run. Re-run ap01_runner.py to completion, or diagnose "
            f"abort_reason={m.get('abort_reason')!r}."
        )
    # AP01-V2-03: a complete run cannot contain a failed required case. Such a manifest is
    # self-contradictory and must not be silently reduced to "just the OK ones".
    bad = [c for c in m.get("cases", []) if c.get("status") != "OK"]
    if bad:
        names = ", ".join(f"{c.get('name')}({c.get('status')})" for c in bad)
        raise ManifestError(
            f"contradictory manifest: run_complete=true but {len(bad)} case(s) are not OK: {names}. "
            "Refusing to extract - a completed run cannot contain a failed required case."
        )
    if not m.get("cases"):
        raise ManifestError("manifest has no cases")
    return m


def load_verified_csv(case: dict) -> list[dict]:
    name = case["name"]
    csv_path = RESULTS / (name + ".csv")
    if not csv_path.exists():
        raise ManifestError(f"case '{name}' listed as OK in manifest but {csv_path} is missing")
    data = csv_path.read_bytes()
    actual_hash = hashlib.sha256(data).hexdigest()
    expected_hash = case.get("csv_sha256")
    if actual_hash != expected_hash:
        raise ManifestError(
            f"case '{name}': CSV on disk does not match manifest hash "
            f"(expected {expected_hash}, got {actual_hash}) - stale or modified file, refusing to use it"
        )
    rows = list(csv.DictReader(data.decode("utf-8").splitlines()))
    if not rows:
        raise ManifestError(f"case '{name}': CSV has no data rows")
    missing = [c for c in CHANNELS + ["time_s"] if c not in rows[0]]
    if missing:
        raise ManifestError(f"case '{name}': CSV is missing required channel(s): {missing}")
    return rows


def channel_summary(vals: list[float], channel: str, stages: dict | None,
                     bias_reference: float | None) -> dict:
    # The verdict is per CHANNEL and per STAGE: a channel is only affected by stages upstream of
    # it, and channels outside the Iq chain get no Iq verdict at all (AP01-V3-01 item 4).
    limit = metrics.limit_assessment(vals, channel=channel, stages=stages)
    out = dict(
        pp=metrics.peak_to_peak(vals),
        mean=metrics.mean(vals),
        rms=metrics.rms(vals),
        ripple_rms=metrics.rms_about_mean(vals),
        relative_ripple=metrics.relative_ripple(vals),
        limit_status=limit["status"],
        comparable=limit["comparable"],
        limit_evidence=limit["evidence"],
        limiting=limit.get("limiting"),
        upstream_stages_changing_this_channel=limit.get("upstream_changes"),
    )
    if bias_reference is not None:
        out["bias_vs_reference"] = metrics.bias(vals, bias_reference)
        out["relative_bias_vs_reference"] = metrics.relative_bias(vals, bias_reference)
        out["bias_reference_case"] = BIAS_REFERENCE_CASE
        out["bias_reference_value"] = bias_reference
    else:
        out["bias_vs_reference"] = None
        out["relative_bias_vs_reference"] = None
        out["bias_reference_case"] = None
    return out


def steady_window_for_case(rows: list[dict], case: dict):
    times = [float(r["time_s"]) for r in rows]
    duration = times[-1] if times else 0.0
    ride_start_s = float(case.get("ride_start_s") or 0.0)
    ride_stop_s = case.get("ride_stop_s")
    ride_stop_s = float(ride_stop_s) if ride_stop_s not in (None, "") else 0.0
    t0 = max(STEADY_WARMUP_S, ride_start_s + STEADY_WARMUP_S - 0.5)
    t1 = ride_stop_s if ride_stop_s > 0 else duration
    if t1 <= t0:
        return None, None
    return t0, t1


def summarize_case(rows: list[dict], case: dict, reference_means: dict | None) -> dict:
    times = [float(r["time_s"]) for r in rows]
    t0, t1 = steady_window_for_case(rows, case)
    entry: dict = dict(rows=len(rows), steady_window_s=[t0, t1])

    if t0 is None:
        entry["steady"] = None
        entry["steady_unavailable_reason"] = "run too short relative to ride_start_s/ride_stop_s"
    else:
        # Stage-by-stage view of the Iq chain over this window. Each stage reports only what the
        # two adjacent channels do; only the profile-limit stage is attributed to a named cause,
        # because production source states that difference is max_iq_pct "and nothing else".
        stage_windows = {}
        for ch in STAGE_CHANNELS:
            vals = [float(r[ch]) for r in rows]
            _, wv = metrics.window(times, vals, t0, t1)
            if wv:
                stage_windows[ch] = wv
        stages = metrics.stage_analysis(stage_windows) if stage_windows else {}
        entry["iq_stage_analysis"] = stages

        steady = {}
        for ch in CHANNELS:
            vals = [float(r[ch]) for r in rows]
            _, wv = metrics.window(times, vals, t0, t1)
            if not wv:
                steady[ch] = None
                continue
            ref = (reference_means or {}).get(ch)
            steady[ch] = channel_summary(wv, ch, stages, ref)
        entry["steady"] = steady

    ride_stop_s = case.get("ride_stop_s")
    ride_stop_s = float(ride_stop_s) if ride_stop_s not in (None, "") else 0.0
    if ride_stop_s > 0 and ride_stop_s < (times[-1] if times else 0.0):
        stop_analysis = {}
        for ch in ("iq_ref", "iq_allowed"):
            vals = [float(r[ch]) for r in rows]
            stop_analysis[ch] = metrics.stop_points(
                times, vals, stop_time=ride_stop_s, zero_tolerance=STOP_ZERO_TOLERANCE,
                min_confirm_s=STOP_MIN_CONFIRM_S,
                max_transition_gap_s=STOP_MAX_TRANSITION_GAP_S)
        entry["stop_analysis"] = stop_analysis
        entry["stop_analysis_contract"] = dict(
            zero_tolerance=STOP_ZERO_TOLERANCE, min_confirm_s=STOP_MIN_CONFIRM_S,
            max_transition_gap_s=STOP_MAX_TRANSITION_GAP_S)
    return entry


def _fail(msg: str) -> None:
    """Refuse extraction, and make sure an older metrics file cannot pass as the current result."""
    print(f"ERROR: {msg}", file=sys.stderr)
    stale = RESULTS / "ap01-metrics.json"
    if stale.exists():
        try:
            old = json.loads(stale.read_text(encoding="utf-8"))
            old_run = old.get("source_manifest_run_id")
        except Exception:  # noqa: BLE001
            old_run = "unreadable"
        print(f"ERROR: STALE metrics file present and NOT updated: {stale} "
              f"(recorded run_id={old_run!r}). It does not describe the current manifest; "
              f"do not use it as this run's result.", file=sys.stderr)
    sys.exit(1)


def main():
    try:
        manifest = load_manifest()
    except ManifestError as e:
        _fail(str(e))
        return

    cases = manifest["cases"]

    # Two passes: the reference case's steady means first, so every case can carry a computed bias
    # against an explicitly named reference.
    reference_means = None
    ref_case = next((c for c in cases if c["name"] == BIAS_REFERENCE_CASE), None)
    if ref_case is not None:
        try:
            ref_rows = load_verified_csv(ref_case)
        except ManifestError as e:
            _fail(str(e))
            return
        ref_entry = summarize_case(ref_rows, ref_case, None)
        if ref_entry.get("steady"):
            reference_means = {ch: (ref_entry["steady"][ch] or {}).get("mean")
                               for ch in CHANNELS if ref_entry["steady"].get(ch)}

    report = dict(
        source_manifest=str(MANIFEST_PATH),
        source_manifest_run_id=manifest.get("run_id"),
        source_manifest_generated_at=manifest.get("generated_at"),
        tool_revision="extract_metrics.py REV3",
        bias_reference_case=BIAS_REFERENCE_CASE,
        bias_reference_note=("bias is computed against the named reference case. For cases whose "
                             "inputs differ from the reference (e.g. the cadence sweep) it "
                             "measures the effect of the input change, not a defect."),
        cases={},
    )
    for case in cases:
        try:
            rows = load_verified_csv(case)
        except ManifestError as e:
            _fail(str(e))
            return
        report["cases"][case["name"]] = summarize_case(rows, case, reference_means)

    (RESULTS / "ap01-metrics.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    for name in sorted(report["cases"]):
        entry = report["cases"][name]
        s = (entry.get("steady") or {}).get("iq_ref")
        if s:
            rr = f"{s['relative_ripple']:.3f}" if s["relative_ripple"] is not None else "N/A"
            bias = (f" bias={s['bias_vs_reference']:+.2f}"
                    if s.get("bias_vs_reference") is not None else "")
            flag = "" if s["comparable"] else f" [{s['limit_status']}]"
            print(f"{name:32s} window={entry['steady_window_s']} iq_ref mean={s['mean']:.2f} "
                  f"pp={s['pp']:.2f} rel_ripple={rr}{bias}{flag}")
        elif entry.get("steady_unavailable_reason"):
            print(f"{name:32s} steady window UNAVAILABLE: {entry['steady_unavailable_reason']}")
        for stage_name, st in (entry.get("iq_stage_analysis") or {}).items():
            if st["status"] != "UNCHANGED":
                cause = st.get("attributed_cause") or "cause not attributable from these columns"
                print(f"  stage {stage_name:20s} {st['status']:9s} "
                      f"{st['upstream_range']} -> {st['downstream_range']}  ({cause})")
        if "stop_analysis" in entry:
            sa = entry["stop_analysis"]["iq_ref"]
            print(f"  stop iq_ref: {sa['status']} confirmed={sa['confirmed_zero_time_s']} "
                  f"bracket={sa['transition_bracket_s']} hold={sa['observed_hold_s']} "
                  f"excursions_before={sa.get('excursions_before_confirmation')}")


if __name__ == "__main__":
    main()
