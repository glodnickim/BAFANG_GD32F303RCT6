#!/usr/bin/env python3
"""Command-line entry point for the eVistDrive trace analyzer."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from trace_analyzer_lib import (
    ProfileStore,
    analyze_trace,
    compare_traces,
    load_trace,
    run_regressions,
    write_outputs,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Explainable eVistDrive Iq/power trace analyzer")
    parser.add_argument("trace_file", nargs="?", help="CSV, JSON, or supported raw CAN log")
    parser.add_argument("--profile", help="transparent JSON baseline profile")
    parser.add_argument("--compare-good", dest="compare_profile", help="profile used for adaptive comparison")
    parser.add_argument("--metadata", help="trace metadata JSON")
    parser.add_argument("--output-dir", help="analysis output directory")
    parser.add_argument("--learn-good", action="store_true", help="confirm this trace GOOD and rebuild its baseline")
    parser.add_argument("--session-status", choices=("GOOD", "BAD", "UNKNOWN"), help="explicit session status")
    parser.add_argument("--firmware-version")
    parser.add_argument("--git-commit")
    parser.add_argument("--motor-type")
    parser.add_argument("--config-hash")
    parser.add_argument("--logger-schema-version")
    parser.add_argument("--remove-trace", help="remove trace SHA/path from --profile and rebuild")
    parser.add_argument("--rebuild-profile", action="store_true")
    parser.add_argument("--good", help="GOOD trace for direct comparison")
    parser.add_argument("--bad", help="BAD trace for direct comparison")
    parser.add_argument("--run-regressions", metavar="DIR")
    parser.add_argument("--no-plots", action="store_true")
    return parser


def _overrides(args: argparse.Namespace, include_status: bool = True) -> dict[str, str | None]:
    return {
        "status": args.session_status if include_status else None,
        "firmware_version": args.firmware_version,
        "git_commit": args.git_commit,
        "motor_type": args.motor_type,
        "config_hash": args.config_hash,
        "logger_schema_version": args.logger_schema_version,
    }


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.run_regressions:
        summary = run_regressions(args.run_regressions)
        for case in summary["cases"]:
            print(f"{Path(case['trace']).stem:32} {case['status']} {case['reason']}")
        print(f"Trace regressions: {summary['passed']} PASS, {summary['failed']} FAIL")
        return 1 if summary["failed"] else 0
    if args.good or args.bad:
        if not (args.good and args.bad):
            raise SystemExit("--good and --bad must be provided together")
        comparison = compare_traces(load_trace(args.good), load_trace(args.bad))
        print(json.dumps(comparison, indent=2, ensure_ascii=False))
        return 0
    profile_path = args.profile or args.compare_profile
    if args.remove_trace or args.rebuild_profile:
        if not profile_path:
            raise SystemExit("--profile is required for profile maintenance")
        store = ProfileStore(profile_path)
        if args.remove_trace:
            removed = store.remove_trace(args.remove_trace)
            print(f"Removed baseline source(s): {removed}")
        if args.rebuild_profile:
            store.rebuild()
            print("Profile aggregates rebuilt from confirmed GOOD sources.")
        store.save()
        if not args.trace_file:
            return 0
    if not args.trace_file:
        raise SystemExit("trace_file is required")
    # During learning, inspect the source label before applying a command-line
    # GOOD confirmation. A persisted BAD label must never be relabelled by one
    # convenient switch and admitted to the baseline.
    trace = load_trace(args.trace_file, args.metadata, _overrides(args, include_status=not args.learn_good))
    store = ProfileStore(profile_path) if profile_path else None
    if args.learn_good:
        if trace.metadata.status == "BAD":
            raise SystemExit("baseline update refused: source metadata marks this trace BAD")
        if trace.metadata.status != "GOOD":
            if args.session_status != "GOOD":
                raise SystemExit(
                    "baseline update refused: mark metadata GOOD or confirm with --session-status GOOD"
                )
            trace.metadata.status = "GOOD"
        if store is None:
            default_profile = Path(__file__).resolve().parents[1] / "diagnostics" / "profiles" / "trace_baseline.json"
            store = ProfileStore(default_profile)
            profile_path = str(default_profile)
        trace_id = store.learn_good(trace)
        store.save()
        print(f"GOOD trace learned: {trace_id}")
        print(f"Profile: {profile_path}")
    result = analyze_trace(trace, store)
    output_dir = Path(args.output_dir) if args.output_dir else Path(args.trace_file).with_suffix(".analysis")
    outputs = write_outputs(result, output_dir, plots=not args.no_plots)
    print(outputs["report"].read_text(encoding="utf-8"))
    print(f"JSON: {outputs['json']}")
    return 1 if result.critical_events else 0


if __name__ == "__main__":
    sys.exit(main())
