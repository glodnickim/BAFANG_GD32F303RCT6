#!/usr/bin/env python3
"""Normalize ride/capture CSV into EVistDrive replay schema.

The importer does not reinterpret motor-control behavior. It only renames/normalizes sensor
and recorded-output fields so the native C replay harness can feed the same sensor sequence
back into production EVistDrive modules.
"""
from __future__ import annotations
import argparse, csv, hashlib, json, math, sys
from pathlib import Path

FIELDS = [
    "time_s", "cadence_rpm", "torque_raw_native", "torque_ckg",
    "wheel_speed_kph", "battery_voltage_v", "battery_current_a",
    "assist_level", "brake", "walk", "motor_erps", "iq_actual",
    "pas_ab", "pas_direction", "recorded_iq_request", "recorded_iq_ref",
]
ALIASES = {
    "time_s": ["time_s", "time", "t_s", "timestamp_s", "elapsed_s"],
    "cadence_rpm": ["cadence_rpm", "cadence_input", "cadence", "rpm", "crank_rpm"],
    "torque_raw_native": ["torque_raw_native", "torque_raw", "torque_adc", "torque_native"],
    "torque_ckg": ["torque_ckg", "load_centikg", "torque_load_centikg", "load_ckg"],
    "wheel_speed_kph": ["wheel_speed_kph", "speed_kph", "speed_kmh", "wheel_kph"],
    "battery_voltage_v": ["battery_voltage_v", "battery_v", "voltage_v", "vbat_v"],
    "battery_current_a": ["battery_current_a", "battery_a", "current_a", "ibat_a"],
    "assist_level": ["assist_level", "level", "assist", "assist_level_index"],
    "brake": ["brake", "brake_active", "brake_on"],
    "walk": ["walk", "walk_active", "walk_assist"],
    "motor_erps": ["motor_erps", "erps", "electrical_erps"],
    "iq_actual": ["iq_actual", "current_iq", "i_q", "iq_measured"],
    "pas_ab": ["pas_ab", "pas_state", "pas_raw"],
    "pas_direction": ["pas_direction", "direction", "pedal_direction", "pas_dir"],
    "recorded_iq_request": ["recorded_iq_request", "iq_request", "iq_requested"],
    "recorded_iq_ref": ["recorded_iq_ref", "iq_final", "iq_ref", "i_q_setpoint"],
}

BOOL_TRUE = {"1", "true", "yes", "on", "active"}
BOOL_FALSE = {"0", "false", "no", "off", "inactive"}

def norm_name(s: str) -> str:
    return "".join(ch.lower() if ch.isalnum() else "_" for ch in s.strip()).strip("_")

def parse_map(items: list[str]) -> dict[str, str]:
    out = {}
    for item in items:
        if "=" not in item:
            raise SystemExit(f"bad --map {item!r}; expected canonical=source")
        dst, src = item.split("=", 1)
        dst, src = dst.strip(), src.strip()
        if dst not in FIELDS:
            raise SystemExit(f"unknown canonical field {dst!r}")
        out[dst] = src
    return out

def boolish(v: str) -> str:
    x = v.strip().lower()
    if x in BOOL_TRUE: return "1"
    if x in BOOL_FALSE: return "0"
    try: return "1" if float(v) != 0.0 else "0"
    except ValueError: return "0"

def finite_text(v: str) -> str:
    v = v.strip()
    if not v: return "nan"
    try:
        x = float(v)
        return format(x, ".12g") if math.isfinite(x) else "nan"
    except ValueError:
        return "nan"

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--map", action="append", default=[], metavar="CANONICAL=SOURCE")
    ap.add_argument("--default-assist", type=int, default=3)
    ap.add_argument("--metadata", type=Path, help="write mapping/coverage JSON")
    args = ap.parse_args()
    user_map = parse_map(args.map)

    with args.input.open("r", newline="", encoding="utf-8-sig", errors="replace") as f:
        sample = f.read(8192); f.seek(0)
        try: dialect = csv.Sniffer().sniff(sample, delimiters=",;\t")
        except csv.Error: dialect = csv.excel
        reader = csv.DictReader(f, dialect=dialect)
        if not reader.fieldnames:
            raise SystemExit("input has no CSV header")
        original = {norm_name(n): n for n in reader.fieldnames}
        mapping: dict[str, str | None] = {}
        for dst in FIELDS:
            if dst in user_map:
                src = user_map[dst]
                if src not in reader.fieldnames and norm_name(src) in original:
                    src = original[norm_name(src)]
                if src not in reader.fieldnames:
                    raise SystemExit(f"mapped source column {src!r} not found")
                mapping[dst] = src
                continue
            src = None
            for alias in ALIASES[dst]:
                if norm_name(alias) in original:
                    src = original[norm_name(alias)]; break
            mapping[dst] = src

        args.output.parent.mkdir(parents=True, exist_ok=True)
        rows = 0
        first_t = None
        synth_dt = 0.00025
        with args.output.open("w", newline="", encoding="utf-8") as g:
            w = csv.DictWriter(g, fieldnames=FIELDS); w.writeheader()
            for row in reader:
                out = {}
                for dst in FIELDS:
                    src = mapping[dst]
                    val = row.get(src, "") if src else ""
                    if dst in ("brake", "walk"):
                        out[dst] = boolish(val) if src else "0"
                    elif dst == "assist_level":
                        out[dst] = finite_text(val) if src else str(args.default_assist)
                    else:
                        out[dst] = finite_text(val) if src else "nan"
                if mapping["time_s"] is None:
                    out["time_s"] = format(rows * synth_dt, ".9f")
                else:
                    t = float(out["time_s"])
                    if first_t is None: first_t = t
                    out["time_s"] = format(t - first_t, ".9f")
                w.writerow(out); rows += 1

    found = [f"{k}<-{v}" for k,v in mapping.items() if v]
    missing = [k for k,v in mapping.items() if not v]
    present={k for k,v in mapping.items() if v}
    core={"time_s","cadence_rpm"}
    torque_ok=("torque_raw_native" in present or "torque_ckg" in present)
    coverage=["CORE"] if core <= present and torque_ok else []
    if {"wheel_speed_kph","battery_voltage_v","battery_current_a"} <= present: coverage.append("LIMITS")
    if {"motor_erps","iq_actual"} <= present: coverage.append("MOTOR")
    if "pas_ab" in present or "pas_direction" in present: coverage.append("PAS_RAW")
    meta={"input":str(args.input),"output":str(args.output),"rows":rows,"mapping":mapping,"missing":missing,
          "coverage":coverage,"source_sha256":hashlib.sha256(args.input.read_bytes()).hexdigest()}
    if args.metadata:
        args.metadata.parent.mkdir(parents=True,exist_ok=True); args.metadata.write_text(json.dumps(meta,indent=2)+"\n")
    print(f"IMPORT PASS rows={rows} coverage={'+'.join(coverage) if coverage else 'INSUFFICIENT'} output={args.output}")
    print("mapped: " + ", ".join(found))
    print("missing/defaulted: " + ", ".join(missing))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
