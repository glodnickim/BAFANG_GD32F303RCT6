"""Explainable eVistDrive trace analyzer.

The firmware knowledge layer is deliberately deterministic.  Adaptive profiles
only add observations after that layer and are built solely from explicitly
confirmed GOOD traces.
"""

from __future__ import annotations

import csv
import hashlib
import html
import io
import json
import math
import re
import statistics
import subprocess
import tempfile
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


ANALYZER_VERSION = "1.1.0"
ANALYSIS_SCHEMA_VERSION = 1
PROFILE_SCHEMA_VERSION = 1

PIPELINE = (
    "RIDER_ACTIVE",
    "PERMISSION",
    "LATCHED",
    "MODE_DEMAND",
    "IQ_BEFORE_PU",
    "IQ_REQUEST",
    "IQ_AFTER_LATCH",
    "IQ_PRE_RAMP",
    "FINAL_IQ",
    "BRIDGE_ON",
    "CURRENT_RESPONSE",
    "POWER_RESPONSE",
)

# Consecutive pipeline edges plus the explicit cross-stage timings requested
# by the diagnostic contract. Cross-stage pairs stay measurable when an older
# recorder omits an intermediate field such as LATCHED or IQ_BEFORE_PU.
LATENCY_PAIRS = tuple(zip(PIPELINE, PIPELINE[1:])) + (
    ("PERMISSION", "MODE_DEMAND"),
    ("MODE_DEMAND", "IQ_REQUEST"),
    ("IQ_REQUEST", "FINAL_IQ"),
    ("FINAL_IQ", "BRIDGE_ON"),
    ("BRIDGE_ON", "CURRENT_RESPONSE"),
)

CLASS_INFO = {
    "A0": ("RIDER INPUT INVALID", "rider_input / PAS / torque_input", 72),
    "A1": ("PERMISSION / GATE FAILURE", "ride_session / pedal_assist_gate / ride_control", 88),
    "A2": ("ASSIST DEMAND FAILURE", "assist_modes / ride_control", 90),
    "A3": ("LIMITER / FLOOR FAILURE", "assist_modes limiter / ride_control floor", 92),
    "A4": ("FINAL IQ LOST", "ride_control / recovery / final safety gate", 96),
    "B": ("BRIDGE START FAILURE", "main bridge lifecycle / PWM / MOE", 97),
    "C": ("LOW-LEVEL MOTOR CURRENT RESPONSE FAILURE", "FOC / Hall / bridge / current calibration", 98),
    "D": ("POWER RESPONSE ANOMALY", "motor response / electrical path", 86),
    "X_LATENCY": ("LATENCY ANOMALY", "pipeline stage timing", 76),
    "X_VALUE": ("VALUE RELATION ANOMALY", "pipeline value relation", 70),
    "X_SEQUENCE": ("SEQUENCE ANOMALY", "pipeline state sequence", 80),
    "UNKNOWN": ("INSUFFICIENT DATA", "newer recorder required", 0),
}

REASON_BITS = {
    0x01: "LEVEL_ZERO",
    0x02: "DIRECTION",
    0x04: "START_STEPS",
    0x08: "LOAD_BELOW",
    0x10: "REARM_GRANT",
    0x20: "RECOVERY_WAIT",
    0x40: "MODE_ZERO",
    0x80: "SAFETY",
}

BOOL_FIELDS = {
    "forward",
    "rider_active",
    "permission",
    "latched",
    "pwm_on",
    "moe",
    "wheel_valid",
    "rolling_coast",
    "walk_assist",
    "load_met",
    "no_demand",
    "brake_active",
    "hard_cut",
    "safety_cut",
    "torque_fault",
    "current_cal_foc_allowed",
    "voltage_limit_ok",
    "temperature_limit_ok",
    "legal_enabled",
    "offroad",
    "pwm_cutoff_active",
}


def _header_key(value: str) -> str:
    text = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode("ascii")
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


ALIASES: dict[str, tuple[str, float]] = {}


def _aliases(canonical: str, *names: str, scale: float = 1.0) -> None:
    for name in names:
        ALIASES[_header_key(name)] = (canonical, scale)


_aliases("time_ms", "time_ms", "timestamp_ms", "ticks_ms", "elapsed_ms")
_aliases("time_ms", "time_s", "seconds", scale=1000.0)
_aliases("time_tick", "tick", "timestamp", "tick_abs")
_aliases("torque", "torque", "torque_mv", "torque_raw", "torque_for_assist_mv")
_aliases("torque_filtered", "torque_filtered", "torque_fast", "afilt", "afilt_native")
_aliases("torque_run", "torque_run", "arun", "arun_native")
_aliases("load", "load", "load_centikg", "load_ckg", "load_cg")
_aliases("load_threshold", "load_threshold", "load_threshold_centikg", "thr")
_aliases("cadence", "cadence", "cadence_rpm", "cadence_input", "rpm", "cad")
_aliases("direction", "direction", "pas_direction", "dir", "dir_state")
_aliases("forward", "forward", "pas_forward", "crank_direction_ok", "forward_pedaling")
_aliases("fwd_run", "fwd_run", "forward_steps", "crank_forward_steps")
_aliases("rider_active", "rider_active")
_aliases("permission", "permission", "assist_permission", "permission_present")
_aliases("assist_permission", "assist_permission")
_aliases("permission_bits", "permission_bits")
_aliases("latched", "latched", "gate_open", "rider_latched")
_aliases("mode_demand", "mode_demand", "mode_iq_demand")
_aliases("iq_request", "iq_request", "iq_req", "iq_request_raw")
_aliases("iq_before_pu", "iq_before_pu")
_aliases("iq_after_latch", "iq_after_latch", "iq_after_latch_floor", "iq_latch")
_aliases("iq_pre_ramp", "iq_pre_ramp", "iq_pre")
_aliases("final_iq", "final_iq", "iq_final", "iq_setpoint", "iq_set")
_aliases("iq_actual", "iq_actual_raw", "iq_actual", "iq_meas", "motor_iq_actual")
_aliases("pwm_on", "pwm_on", "pwm")
_aliases("moe", "moe", "hardware_moe", "bridge_on")
_aliases("bridge_lifecycle", "bridge_lifecycle", "bridge_state")
_aliases("hall", "hall", "hall_state")
_aliases("angle_hall", "angle_hall", "hall_angle", "q31_rotorposition_hall")
_aliases("angle_absolute", "angle_absolute", "rotor_angle_absolute", "q31_rotorposition_absolute")
_aliases("rotor_direction", "rotor_direction", "recent_rotor_direction", "i8_recent_rotor_direction")
_aliases("reason_bits", "reason_bits", "reason", "rbits")
_aliases("debug_flags", "debug_flags")
_aliases("status_flags", "status_flags")
_aliases("neutral_dwell_active", "neutral_dwell_active")
_aliases("neutral_dwell_counter", "neutral_dwell_counter")
# FW-122.1 (schema v3): D2 raw evidence - see FW-124 section 6 and the wire-format comment in
# inc/rolling_no_assist_diag.h. Absent (all None) for schema 1/2 traces.
_aliases("pwm_cutoff_active", "pwm_cutoff_active")
_aliases("pwm_cutoff_progress", "pwm_cutoff_progress")
_aliases("hall_timeout_progress", "hall_timeout_progress")
_aliases("flags2", "flags2")
_aliases("session_state", "session_state", "session")
_aliases("recovery_state", "recovery_state", "recovery", "recov")
_aliases("wheel_valid", "wheel_valid")
_aliases("rolling_coast", "rolling_coast")
_aliases("motor_voltage_utilization", "motor_voltage_utilization", "mvu")
_aliases("assist_level", "assist_level", "level")
_aliases("walk_assist", "walk_assist", "walk_active")
_aliases("speed", "speed", "speed_km_h", "speed_kmh")
_aliases("speed_limit", "speed_limit", "speed_limit_km_h", "speed_limit_kmh")
_aliases("speed_limit", "speed_limit_x100", scale=0.01)
_aliases("brake_active", "brake_active", "brake", "brake_active_flag")
_aliases("hard_cut", "hard_cut", "hard_cut_active")
_aliases("safety_cut", "safety_cut", "safety_cut_non_direction")
_aliases("torque_fault", "torque_fault", "torque_sensor_fault")
_aliases("current_cal_foc_allowed", "current_cal_foc_allowed", "foc_allowed")
_aliases("voltage_limit_ok", "voltage_limit_ok", "undervoltage_ok")
_aliases("temperature_limit_ok", "temperature_limit_ok", "thermal_limit_ok", "temperature_ok")
_aliases("legal_enabled", "legal_enabled", "legal_mode")
_aliases("offroad", "offroad", "offroad_enabled")
_aliases("current", "current", "current_a", "battery_current", "battery_current_a")
_aliases("voltage", "voltage", "voltage_v", "battery_voltage")
_aliases("controller_temperature_c", "controller_temperature_c", "controller_temp_c", "controller_temp")
_aliases("power", "power", "power_w", "electrical_power", "electrical_power_w")
_aliases("motor_power", "motor_power", "motor_power_w")
_aliases("human_power", "human_power", "human_power_w")
_aliases("erps", "erps")
_aliases("pi_q_int", "pi_q_int", "pi_q")
_aliases("pi_d_int", "pi_d_int", "pi_d")
_aliases("assist_hold_ticks", "assist_hold_ticks", "assist_hold", "hold")
_aliases("required_steps", "required_steps", "req_st")
_aliases("start_steps", "start_steps", "start_st")


@dataclass
class TraceMetadata:
    status: str = "UNKNOWN"
    firmware_version: str = "UNKNOWN"
    git_commit: str = "UNKNOWN"
    motor_type: str = "UNKNOWN"
    config_hash: str = "UNKNOWN"
    logger_schema_version: str = "UNKNOWN"
    extra: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_mapping(cls, data: Mapping[str, Any] | None) -> "TraceMetadata":
        src = dict(data or {})
        known = {}
        for name in (
            "status",
            "firmware_version",
            "git_commit",
            "motor_type",
            "config_hash",
            "logger_schema_version",
        ):
            value = src.pop(name, "UNKNOWN")
            known[name] = str(value if value not in (None, "") else "UNKNOWN")
        known["status"] = known["status"].upper()
        return cls(**known, extra=src)

    def as_dict(self) -> dict[str, Any]:
        result = {
            "status": self.status,
            "firmware_version": self.firmware_version,
            "git_commit": self.git_commit,
            "motor_type": self.motor_type,
            "config_hash": self.config_hash,
            "logger_schema_version": self.logger_schema_version,
        }
        result.update(self.extra)
        return result

    def key(self) -> str:
        raw = "|".join(
            (
                self.firmware_version,
                self.git_commit,
                self.motor_type,
                self.config_hash,
                self.logger_schema_version,
            )
        )
        return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:16]

    def complete_for_learning(self) -> bool:
        return all(
            getattr(self, name) != "UNKNOWN"
            for name in (
                "firmware_version",
                "git_commit",
                "motor_type",
                "config_hash",
                "logger_schema_version",
            )
        )


@dataclass
class TraceData:
    path: Path
    samples: list[dict[str, Any]]
    fields: set[str]
    field_sources: dict[str, str]
    metadata: TraceMetadata
    adapter: str
    warnings: list[str] = field(default_factory=list)

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.path.read_bytes()).hexdigest()


@dataclass
class AnomalyEvent:
    event_time: float
    end_time: float
    duration_ms: float
    event_type: str
    first_divergence: str
    class_code: str
    class_name: str
    reason_bits: list[str]
    anomaly_score: int
    confidence: str
    evidence: dict[str, Any]
    likely_code_area: str
    latency_ms: dict[str, float] = field(default_factory=dict)
    cannot_distinguish: list[str] = field(default_factory=list)
    expected_next_state: str | None = None
    adaptive: bool = False

    def as_dict(self) -> dict[str, Any]:
        return {
            "event_time": round(self.event_time / 1000.0, 6),
            "event_time_ms": round(self.event_time, 3),
            "event_end_ms": round(self.end_time, 3),
            "duration_ms": round(self.duration_ms, 3),
            "event_type": self.event_type,
            "first_divergence": self.first_divergence,
            "class": self.class_code,
            "class_name": self.class_name,
            "reason_bits": self.reason_bits,
            "anomaly_score": self.anomaly_score,
            "confidence": self.confidence,
            "evidence": self.evidence,
            "likely_code_area": self.likely_code_area,
            "latency_ms": self.latency_ms,
            "cannot_distinguish": self.cannot_distinguish,
            "expected_next_state": self.expected_next_state,
            "adaptive": self.adaptive,
        }


@dataclass
class AnalysisResult:
    trace: TraceData
    events: list[AnomalyEvent]
    segments: list[dict[str, Any]]
    missing_signals: list[str]
    adaptive_profile_key: str | None = None
    adaptive_notes: list[str] = field(default_factory=list)

    @property
    def critical_events(self) -> list[AnomalyEvent]:
        return [event for event in self.events if event.class_code not in {"UNKNOWN", "X_VALUE"}]

    def as_dict(self) -> dict[str, Any]:
        return {
            "schema_version": ANALYSIS_SCHEMA_VERSION,
            "analyzer_version": ANALYZER_VERSION,
            "analysis_kind": "TRACE_ANALYSIS",
            "trace": str(self.trace.path),
            "trace_sha256": self.trace.sha256,
            "input_adapter": self.trace.adapter,
            "metadata": self.trace.metadata.as_dict(),
            "available_signals": sorted(self.trace.fields),
            "missing_signals": self.missing_signals,
            "warnings": self.trace.warnings,
            "adaptive_profile_key": self.adaptive_profile_key,
            "adaptive_notes": self.adaptive_notes,
            "segments": self.segments,
            "events": [event.as_dict() for event in self.events],
            "summary": {
                "event_count": len(self.events),
                "critical_event_count": len(self.critical_events),
            },
        }


def _read_text(path: Path) -> str:
    raw = path.read_bytes()
    for encoding in ("utf-8-sig", "cp1250", "latin-1"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            pass
    return raw.decode("utf-8", errors="replace")


def _parse_scalar(value: Any, boolean: bool = False) -> Any:
    if value is None:
        return None
    if isinstance(value, (int, float, bool)):
        return int(value) if boolean else value
    text = str(value).strip()
    if text == "":
        return None
    low = text.lower()
    if boolean:
        if low in {"true", "yes", "y", "on", "open", "active"}:
            return 1
        if low in {"false", "no", "n", "off", "closed", "inactive"}:
            return 0
    if re.fullmatch(r"[-+]?0x[0-9a-f]+", low):
        return int(low, 16)
    candidate = text.replace(",", ".") if "," in text and "." not in text else text
    try:
        number = float(candidate)
        return int(number) if number.is_integer() else number
    except ValueError:
        return text


def _metadata_candidates(path: Path) -> Iterable[Path]:
    yield path.with_suffix(path.suffix + ".meta.json")
    yield path.with_suffix(".meta.json")
    yield path.parent / "metadata.json"


def _load_metadata(path: Path, explicit: Path | None, overrides: Mapping[str, Any] | None) -> TraceMetadata:
    data: dict[str, Any] = {}
    candidates = [explicit] if explicit else list(_metadata_candidates(path))
    for candidate in candidates:
        if candidate and candidate.exists():
            loaded = json.loads(_read_text(candidate))
            if isinstance(loaded, dict):
                data.update(loaded)
            break
    data.update({key: value for key, value in (overrides or {}).items() if value is not None})
    return TraceMetadata.from_mapping(data)


def _normalize_rows(
    path: Path,
    rows: Sequence[Mapping[str, Any]],
    metadata: TraceMetadata,
    adapter: str,
    warnings: list[str] | None = None,
) -> TraceData:
    if not rows:
        return TraceData(path, [], set(), {}, metadata, adapter, list(warnings or []))
    header_map: dict[str, tuple[str, float]] = {}
    field_sources: dict[str, str] = {}
    for header in rows[0].keys():
        key = _header_key(str(header))
        canonical, scale = ALIASES.get(key, (key, 1.0))
        header_map[str(header)] = (canonical, scale)
        # Prefer explicit millisecond time over a raw tick/timestamp alias.
        if canonical not in field_sources or canonical == "time_ms":
            field_sources[canonical] = str(header)
    samples: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        sample: dict[str, Any] = {"_index": index}
        for header, raw_value in row.items():
            canonical, scale = header_map[str(header)]
            value = _parse_scalar(raw_value, canonical in BOOL_FIELDS)
            if value is None:
                continue
            if isinstance(value, (int, float)) and scale != 1.0:
                value *= scale
            # A dedicated time_ms column wins over a raw timestamp/tick.
            if canonical in sample and canonical == "time_ms":
                continue
            sample[canonical] = value
        samples.append(sample)
    explicit_times = [sample.get("time_ms") for sample in samples if isinstance(sample.get("time_ms"), (int, float))]
    tick_source = field_sources.get("time_tick", "")
    tick_scale = 0.25 if _header_key(tick_source) in {"tick", "tick_abs", "timestamp"} else 1.0
    if not explicit_times:
        for sample in samples:
            if isinstance(sample.get("time_tick"), (int, float)):
                sample["time_ms"] = float(sample["time_tick"]) * tick_scale
        if not any("time_ms" in sample for sample in samples):
            period = float(metadata.extra.get("sample_period_ms", 1.0))
            for index, sample in enumerate(samples):
                sample["time_ms"] = index * period
    samples.sort(key=lambda item: float(item.get("time_ms", 0.0)))
    fields = {key for sample in samples for key in sample if not key.startswith("_")}
    return TraceData(path, samples, fields, field_sources, metadata, adapter, list(warnings or []))


def _load_csv(path: Path, metadata: TraceMetadata, adapter: str = "normalized_csv") -> TraceData:
    text = _read_text(path)
    first = text.splitlines()[0] if text.splitlines() else ""
    delimiter = ";" if first.count(";") > first.count(",") else ","
    rows = list(csv.DictReader(io.StringIO(text), delimiter=delimiter))
    return _normalize_rows(path, rows, metadata, adapter)


def _run_ps_json(script: Path, log: Path) -> list[dict[str, Any]]:
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(script),
        "-Log",
        str(log),
        "-OutputJson",
    ]
    completed = subprocess.run(command, capture_output=True, check=False)
    if completed.returncode != 0:
        stderr = completed.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(f"decoder failed ({script.name}): {stderr.strip()}")
    output = completed.stdout.decode("utf-8-sig", errors="replace").strip()
    parsed = json.loads(output or "[]")
    return parsed if isinstance(parsed, list) else [parsed]


def _flags(text: Any) -> set[str]:
    if text is None:
        return set()
    return {part.strip().upper() for part in re.split(r"[+|,; ]+", str(text)) if part.strip()}


def _load_fw112_diag(path: Path, metadata: TraceMetadata, tools_dir: Path) -> TraceData:
    decoded = _run_ps_json(tools_dir / "decode_fw112_diag.ps1", path)
    rows: list[dict[str, Any]] = []
    cumulative_ticks = 0.0
    for item in decoded:
        elapsed = _parse_scalar(item.get("El"))
        cumulative_ticks += float(elapsed or 0.0)
        names = _flags(item.get("Flags"))
        schema = int(_parse_scalar(item.get("Schema")) or 0)
        permission_bits = _parse_scalar(item.get("PermBits"))
        permission = (
            int(bool(int(permission_bits) & 0x01))
            if isinstance(permission_bits, (int, float))
            else _parse_scalar(item.get("Permission"), boolean=True)
        )
        latched = _parse_scalar(item.get("Latched"), boolean=True)
        # Schema 4 deliberately no longer carries the old flags byte; do not
        # turn an unavailable PWM/wheel observation into a false zero.
        flag_observation_available = bool(names) or schema < 4
        row = {
            "time_ms": cumulative_ticks / 4.0,
            "cadence": item.get("Cad"),
            "fwd_run": item.get("FwdRun"),
            "load": item.get("Load"),
            "load_threshold": item.get("Thr"),
            "permission": permission,
            "assist_permission": item.get("Permission"),
            "permission_bits": permission_bits,
            "mode_demand": item.get("IqBeforePu"),
            "iq_request": item.get("IqReq"),
            "iq_before_pu": item.get("IqBeforePu"),
            "iq_pre_ramp": item.get("IqPre"),
            "final_iq": item.get("IqSet"),
            "iq_actual": item.get("IqAct"),
            "latched": latched if latched is not None else (int("LATCHED" in names) if flag_observation_available else None),
            "pwm_on": int("PWM_ON" in names) if flag_observation_available else None,
            "wheel_valid": int("WHEEL_VALID" in names) if flag_observation_available else None,
            "rolling_coast": int("ROLLING_COAST" in names) if flag_observation_available else None,
            "direction": item.get("Dir"),
            "session_state": item.get("SesSt"),
            "recovery_state": item.get("Recov"),
            "reason_bits": item.get("Rbits"),
            "flags2": item.get("Flags2"),
            "motor_voltage_utilization": item.get("MVU"),
            "logger_schema_version": item.get("Schema"),
        }
        rows.append(row)
    if decoded and metadata.logger_schema_version == "UNKNOWN":
        metadata.logger_schema_version = str(decoded[0].get("Schema", "UNKNOWN"))
    return _normalize_rows(path, rows, metadata, "existing_decoder:decode_fw112_diag.ps1")


def _load_fw112_ab(path: Path, metadata: TraceMetadata, tools_dir: Path) -> TraceData:
    decoded = _run_ps_json(tools_dir / "decode_fw112_ab.ps1", path)
    rows: list[dict[str, Any]] = []
    for item in decoded:
        sample = item.get("Sample")
        if not isinstance(sample, dict):
            continue
        names = _flags(sample.get("Flags"))
        rows.append(
            {
                "time_ms": float(sample.get("TickOffset", 0)) / 4.0,
                "torque": sample.get("TorqueMv"),
                "torque_filtered": sample.get("AfiltNative"),
                "torque_run": sample.get("ArunNative"),
                "load": sample.get("LoadCkg"),
                "cadence": sample.get("Cadence"),
                "latched": int("LATCHED" in names),
                "pwm_on": int("PWM_ON" in names),
                "wheel_valid": int("WHEEL_VALID" in names),
                "rolling_coast": int("ROLLING_COAST" in names),
                "session_state": sample.get("SessionState"),
                "recovery_state": sample.get("Recovery"),
                "direction": sample.get("Dir"),
                "iq_request": sample.get("IqRequest"),
                "iq_after_latch": sample.get("IqAfterLatch"),
                "iq_pre_ramp": sample.get("IqPreRamp"),
                "final_iq": sample.get("IqSetpoint"),
                "assist_level": sample.get("AssistLevel"),
                "assist_hold_ticks": sample.get("AssistHold"),
                "milestone": item.get("Milestone"),
                "episode": item.get("Episode"),
            }
        )
    if decoded and metadata.logger_schema_version == "UNKNOWN":
        metadata.logger_schema_version = str(decoded[0].get("Schema", "UNKNOWN"))
    return _normalize_rows(path, rows, metadata, "existing_decoder:decode_fw112_ab.ps1")


def _load_fw117(path: Path, metadata: TraceMetadata, tools_dir: Path) -> TraceData:
    decoded = _run_ps_json(tools_dir / "decode_fw117_trace.ps1", path)
    rows: list[dict[str, Any]] = []
    for item in decoded:
        sample = item.get("S")
        if not isinstance(sample, dict):
            continue
        flag_value = int(sample.get("Flags", 0))
        rows.append(
            {
                "time_ms": float(sample.get("Tick", 0)) / 4.0,
                "iq_request": sample.get("IqReq"),
                "iq_pre_ramp": sample.get("IqPre"),
                "final_iq": sample.get("IqSet"),
                "iq_actual": sample.get("IqMeas"),
                "pwm_on": int(bool(flag_value & 0x04)),
                "moe": int(bool(flag_value & 0x01)),
                "bridge_lifecycle": sample.get("State"),
                "hall": sample.get("Hall"),
                "pi_q_int": sample.get("PiQ"),
                "pi_d_int": sample.get("PiD"),
                "vq": sample.get("Vq"),
                "vd": sample.get("Vd"),
            }
        )
    if decoded and metadata.logger_schema_version == "UNKNOWN":
        metadata.logger_schema_version = str(decoded[0].get("Schema", "UNKNOWN"))
    return _normalize_rows(path, rows, metadata, "existing_decoder:decode_fw117_trace.ps1")


def _load_rolling(path: Path, metadata: TraceMetadata, tools_dir: Path) -> TraceData:
    with tempfile.TemporaryDirectory(prefix="evd_trace_") as directory:
        output = Path(directory) / "rolling.csv"
        command = [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(tools_dir / "decode_rolling_no_assist.ps1"),
            "-InputFile",
            str(path),
            "-OutputFile",
            str(output),
        ]
        completed = subprocess.run(command, capture_output=True, check=False)
        if completed.returncode != 0 or not output.exists():
            stderr = completed.stderr.decode("utf-8", errors="replace")
            raise RuntimeError(f"rolling decoder failed: {stderr.strip()}")
        trace = _load_csv(output, metadata, "existing_decoder:decode_rolling_no_assist.ps1")
        if trace.samples and metadata.logger_schema_version == "UNKNOWN":
            schema = _number(trace.samples[0], "schema_version")
            if schema is not None:
                metadata.logger_schema_version = f"rna-{int(schema)}"
        trace.path = path
        return trace


def load_trace(
    path: str | Path,
    metadata_path: str | Path | None = None,
    metadata_overrides: Mapping[str, Any] | None = None,
) -> TraceData:
    trace_path = Path(path).resolve()
    if not trace_path.exists():
        raise FileNotFoundError(trace_path)
    metadata = _load_metadata(trace_path, Path(metadata_path).resolve() if metadata_path else None, metadata_overrides)
    tools_dir = Path(__file__).resolve().parents[1]
    head = _read_text(trace_path)[:2_000_000]
    def contains_efid(value: str) -> bool:
        escaped = re.escape(value)
        sniffer = rf"(?im)\bID:800{escaped}\b[^\r\n]*\bData:\s*(?:[0-9A-F]{{2}}\s*){{8}}"
        compact = (
            rf"(?im)^\s*(?:0x)?(?:000)?{escaped}\s+"
            rf"(?:[0-9A-F]{{8}}\s+)?(?:[0-9A-F]{{2}}(?:\s+|$)){{8}}"
        )
        return bool(re.search(sniffer, head) or re.search(compact, head))

    if trace_path.suffix.lower() == ".json":
        # The prefix is sufficient for CAN format detection, but JSON must be
        # read in full; real captures can easily exceed 2 MB.
        loaded = json.loads(_read_text(trace_path))
        rows = loaded.get("samples", []) if isinstance(loaded, dict) else loaded
        if not isinstance(rows, list):
            raise ValueError("JSON trace must be a list or an object with a samples list")
        return _normalize_rows(trace_path, rows, metadata, "normalized_json")
    if contains_efid("10248") and (contains_efid("10249") or contains_efid("1024A")):
        return _load_rolling(trace_path, metadata, tools_dir)
    if contains_efid("1022A"):
        return _load_fw112_diag(trace_path, metadata, tools_dir)
    if contains_efid("1022F"):
        return _load_fw112_ab(trace_path, metadata, tools_dir)
    if contains_efid("10234"):
        return _load_fw117(trace_path, metadata, tools_dir)
    return _load_csv(trace_path, metadata)


def _number(sample: Mapping[str, Any], field: str) -> float | None:
    value = sample.get(field)
    if isinstance(value, bool):
        return float(value)
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return None


def _truth(sample: Mapping[str, Any], field: str) -> bool | None:
    value = sample.get(field)
    if value is None:
        return None
    if isinstance(value, str):
        low = value.lower()
        if any(token in low for token in ("false", "off", "no", "invalid", "inhibit", "reverse")):
            return False
        if any(token in low for token in ("true", "on", "yes", "valid", "forward", "fwd", "safe", "active")):
            return True
        return None
    if isinstance(value, (int, float, bool)):
        return bool(value)
    return None


def _percentile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(float(value) for value in values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentile / 100.0
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    return ordered[low] * (high - position) + ordered[high] * (position - low)


def _sample_period(samples: Sequence[Mapping[str, Any]]) -> float:
    gaps = []
    for left, right in zip(samples, samples[1:]):
        a, b = _number(left, "time_ms"), _number(right, "time_ms")
        if a is not None and b is not None and 0 < b - a < 10_000:
            gaps.append(b - a)
    return max(0.25, statistics.median(gaps)) if gaps else 1.0


@dataclass
class _Context:
    torque_threshold: float
    torque_is_offset_sensor: bool
    idle_current: float | None
    idle_power: float | None
    period_ms: float
    has_low_level: bool


def _build_context(trace: TraceData) -> _Context:
    torque_values = [_number(sample, "torque") for sample in trace.samples]
    torque_values = [value for value in torque_values if value is not None]
    source = _header_key(trace.field_sources.get("torque", ""))
    if "mv" in source and torque_values:
        low = _percentile(torque_values, 10)
        span = max(torque_values) - min(torque_values)
        torque_threshold = low + max(10.0, span * 0.05)
    else:
        torque_threshold = 0.0
    current_values = [abs(value) for sample in trace.samples if (value := _number(sample, "current")) is not None]
    power_values = [abs(value) for sample in trace.samples if (value := _number(sample, "power")) is not None]
    return _Context(
        torque_threshold=torque_threshold,
        torque_is_offset_sensor="mv" in source,
        idle_current=_percentile(current_values, 10) if current_values else None,
        idle_power=_percentile(power_values, 10) if power_values else None,
        period_ms=_sample_period(trace.samples),
        has_low_level=bool({"final_iq", "iq_actual", "pwm_on"} & trace.fields),
    )


def _torque_active(sample: Mapping[str, Any], context: _Context) -> bool | None:
    load = _number(sample, "load")
    if load is not None:
        threshold = _number(sample, "load_threshold")
        return load >= max(1.0, threshold or 1.0)
    torque = _number(sample, "torque_filtered")
    if torque is None:
        torque = _number(sample, "torque")
    return torque > context.torque_threshold if torque is not None else None


def _rider_active(sample: Mapping[str, Any], context: _Context) -> bool | None:
    explicit = _truth(sample, "rider_active")
    if explicit is not None:
        return explicit
    torque = _torque_active(sample, context)
    cadence = _number(sample, "cadence")
    forward = _truth(sample, "forward")
    direction = _truth(sample, "direction")
    cadence_ok = cadence > 1.0 if cadence is not None else forward
    direction_ok = direction if direction is not None else (forward if forward is not None else True)
    if torque is None or cadence_ok is None:
        return None
    return bool(torque and cadence_ok and direction_ok)


def _permission(sample: Mapping[str, Any]) -> bool | None:
    explicit = _truth(sample, "permission")
    if explicit is not None:
        return explicit
    bits = _number(sample, "permission_bits")
    if bits is not None:
        # The deterministic permission stage is the earlier gate-open fact
        # (bit 0). Aggregate ASSIST_PERMISSION (bit 7) already includes a
        # positive Iq request and would collapse A1 and A2 into one verdict.
        return bool(int(bits) & 0x01)
    aggregate = _truth(sample, "assist_permission")
    if aggregate is not None:
        return aggregate
    return _truth(sample, "latched")


def _positive(sample: Mapping[str, Any], field: str, epsilon: float = 0.5) -> bool | None:
    value = _number(sample, field)
    return value > epsilon if value is not None else None


def _reasons(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, (int, float)):
        number = int(value)
        return [name for bit, name in REASON_BITS.items() if number & bit]
    text = str(value).strip()
    if re.fullmatch(r"(?:0x)?[0-9a-fA-F]{1,2}", text):
        number = int(text, 16)
        return [name for bit, name in REASON_BITS.items() if number & bit]
    return sorted(_flags(text))


def _event_type(sample: Mapping[str, Any], rider: bool | None) -> str:
    if _truth(sample, "walk_assist"):
        return "WALK_ASSIST"
    recovery = str(sample.get("recovery_state", "")).upper()
    if recovery and recovery not in {"IDLE", "0", "NONE"}:
        return "ROLLING_REENABLE"
    if _truth(sample, "rolling_coast"):
        return "ROLLING_REENABLE"
    moving = _truth(sample, "wheel_valid")
    if moving is None:
        speed = _number(sample, "speed")
        moving = speed > 0.5 if speed is not None else None
    if rider:
        if moving:
            iq_actual = _number(sample, "iq_actual")
            current = _number(sample, "current")
            power = _number(sample, "power")
            response_present = (
                (iq_actual is not None and abs(iq_actual) >= 1.0)
                or (current is not None and abs(current) >= 0.5)
                or (power is not None and abs(power) >= 25.0)
            )
            return "NORMAL_RUN" if response_present else "ROLLING_REENABLE"
        return "STANDSTILL_START"
    return "COAST" if moving else "STOP"


def _first_divergence(
    sample: Mapping[str, Any],
    context: _Context,
    active_since_ms: float | None,
) -> tuple[str, str] | None:
    rider = _rider_active(sample, context)
    torque = _torque_active(sample, context)
    cadence = _number(sample, "cadence")
    direction = _truth(sample, "direction")
    cadence_active = cadence > 1.0 if cadence is not None else None
    explicit_rider = _truth(sample, "rider_active")
    cadence_without_torque = (
        cadence_active is True
        and torque is False
        and (explicit_rider is True or not context.torque_is_offset_sensor)
    )
    if (
        (torque is True and cadence_active is False)
        or cadence_without_torque
        or (direction is False and (torque is True or cadence_active is True))
    ):
        return "A0", "RIDER_INPUT"
    if rider is not True:
        return None
    permission = _permission(sample)
    if permission is False:
        return "A1", "PERMISSION"
    latched = _truth(sample, "latched")
    if permission is True and latched is False:
        return "A1", "LATCHED"
    demand = _positive(sample, "mode_demand")
    iq_request = _positive(sample, "iq_request")
    iq_before_pu = _positive(sample, "iq_before_pu")
    if demand is None:
        demand = iq_before_pu if iq_before_pu is not None else iq_request
    if permission is True and demand is False:
        return "A2", "MODE_DEMAND"
    if iq_before_pu is True and iq_request is False:
        return "A3", "IQ_REQUEST"
    if iq_request is True:
        after_latch = _positive(sample, "iq_after_latch")
        if after_latch is False:
            return "A3", "IQ_AFTER_LATCH"
        pre_ramp = _positive(sample, "iq_pre_ramp")
        if pre_ramp is False:
            return "A3", "IQ_PRE_RAMP"
    pre_ramp = _positive(sample, "iq_pre_ramp")
    final_iq = _positive(sample, "final_iq")
    if pre_ramp is True and final_iq is False:
        return "A4", "FINAL_IQ"
    pwm = _truth(sample, "pwm_on")
    moe = _truth(sample, "moe")
    lifecycle = _number(sample, "bridge_lifecycle")
    current_cal_allowed = _truth(sample, "current_cal_foc_allowed")
    if final_iq is True and (
        current_cal_allowed is False
        or pwm is False
        or (pwm is True and moe is False)
        or (pwm is True and lifecycle is not None and lifecycle < 2.0)
    ):
        return "B", "BRIDGE_ON"
    if final_iq is True and pwm is True:
        final_value = abs(_number(sample, "final_iq") or 0.0)
        actual = _number(sample, "iq_actual")
        grace = 24.0
        if active_since_ms is not None and (_number(sample, "time_ms") or 0.0) - active_since_ms >= grace:
            if actual is not None and abs(actual) < max(1.0, final_value * 0.25):
                return "C", "CURRENT_RESPONSE"
    return None


def _evidence(sample: Mapping[str, Any]) -> dict[str, Any]:
    fields = (
        "torque",
        "torque_filtered",
        "load",
        "cadence",
        "direction",
        "fwd_run",
        "permission",
        "assist_permission",
        "permission_bits",
        "latched",
        "mode_demand",
        "iq_request",
        "iq_before_pu",
        "iq_after_latch",
        "iq_pre_ramp",
        "final_iq",
        "pwm_on",
        "moe",
        "iq_actual",
        "bridge_lifecycle",
        "hall",
        "angle_hall",
        "angle_absolute",
        "rotor_direction",
        "erps",
        "pi_q_int",
        "pi_d_int",
        "current",
        "power",
        "assist_level",
        "speed",
        "speed_limit",
        "brake_active",
        "hard_cut",
        "safety_cut",
        "torque_fault",
        "current_cal_foc_allowed",
        "voltage_limit_ok",
        "temperature_limit_ok",
        "controller_temperature_c",
        "recovery_state",
        "session_state",
        "reason_bits",
        "debug_flags",
        "status_flags",
        "neutral_dwell_active",
        "neutral_dwell_counter",
        "flags2",
        "motor_voltage_utilization",
        "pwm_cutoff_active",
        "pwm_cutoff_progress",
        "hall_timeout_progress",
    )
    result = {name: sample[name] for name in fields if name in sample}
    iq_actual = _number(sample, "iq_actual")
    if iq_actual is not None:
        # The forward-drive Park convention is signed (normally negative).
        # Preserve that raw diagnostic fact and expose magnitude separately;
        # only the response comparison is sign-independent.
        result["iq_actual_raw"] = iq_actual
        result["iq_actual_magnitude"] = abs(iq_actual)
    return result


def _cannot_distinguish(code: str, trace: TraceData) -> list[str]:
    if code not in {"B", "C"}:
        return []
    candidates = {
        "moe": "MOE hardware state",
        "bridge_lifecycle": "bridge lifecycle",
        "current_cal_foc_allowed": "current-calibration FOC allow state",
    }
    if code == "B":
        candidates["pwm_cutoff_active"] = "soft-cutoff-in-progress state (FW-122.1 D2 evidence)"
    if code == "C":
        candidates.update({
        "hall": "Hall state",
        "angle_hall": "Hall-sector angle",
        "angle_absolute": "absolute/interpolated rotor angle",
        "rotor_direction": "recent rotor direction",
        "erps": "motor electrical speed (ERPS)",
        "pi_q_int": "current PI state",
        "pi_d_int": "flux PI state",
        })
    return [description for field_name, description in candidates.items() if field_name not in trace.fields]


def _cutoff_aborted_by_redemand(trace: TraceData, sample: Mapping[str, Any]) -> bool:
    """FW-122.1 D2 transition evidence: the PREVIOUS sampled tick was mid-soft-cutoff
    (pwm_cutoff_active=1) and THIS tick already shows it cleared while hardware MOE stayed on
    and a positive final_iq is present - i.e. the cutoff was cancelled by a fresh demand rather
    than completing to a real bridge-off (main.c:1292-1330 vs 1368-1399; see FW-124 section 6).
    Requires two consecutive DECODED samples - at 250 Hz sampling this can miss a cutoff that
    starts and gets cancelled between two 4 ms samples, so a False here is not proof the race
    did not happen; a True here is never a guess, only a directly observed transition."""
    if "pwm_cutoff_active" not in trace.fields:
        return False
    index = None
    for i, candidate in enumerate(trace.samples):
        if candidate is sample:
            index = i
            break
    if index is None or index == 0:
        return False
    previous = trace.samples[index - 1]
    return bool(
        _truth(previous, "pwm_cutoff_active") is True
        and _truth(sample, "pwm_cutoff_active") is False
        and _truth(sample, "moe") is True
        and _positive(sample, "final_iq") is True
    )


def _bridge_failure_subreason(sample: Mapping[str, Any], trace: TraceData | None = None) -> str:
    """Return the earliest recorder-backed B subreason without inventing state.

    FW-122.1: two subreasons below are pure evidence, not a new firmware-computed verdict -
    both are derived off-device from raw facts (pwm_cutoff_active, moe, final_iq) that the
    recorder already copies unchanged. See FW-124 section 6 for the "D2" finding these exist
    to make visible on a captured trace.
    """
    if _truth(sample, "current_cal_foc_allowed") is False:
        return "CURRENT_CAL_FOC_NOT_ALLOWED"
    if _truth(sample, "pwm_cutoff_active") is True:
        # More specific than the generic PWM_ON_NOT_SET below: software PWM is 0 here
        # precisely BECAUSE main.c's soft-cutoff owns CCR, with hardware MOE still on - a
        # legal transient (FW-124 section 4), not a stuck/failed bridge.
        return "REDEMAND_DURING_SOFT_CUTOFF"
    if trace is not None and _cutoff_aborted_by_redemand(trace, sample):
        return "CUTOFF_ABORTED_BY_REDEMAND"
    if _truth(sample, "pwm_on") is False:
        return "PWM_ON_NOT_SET"
    if _truth(sample, "moe") is False:
        return "HARDWARE_MOE_NOT_SET"
    lifecycle = _number(sample, "bridge_lifecycle")
    if lifecycle is not None and lifecycle < 2.0:
        return "BRIDGE_LIFECYCLE_BEFORE_MOE"
    return "BRIDGE_FAILURE_SUBREASON_NOT_RECORDED"


def _event_latencies(trace: TraceData, event_time: float) -> dict[str, float]:
    """Return observed consecutive-stage latencies in the active episode."""
    context = _build_context(trace)
    episode: list[Mapping[str, Any]] = []
    for sample in trace.samples:
        now = float(sample.get("time_ms", 0.0))
        if now > event_time:
            break
        if _rider_active(sample, context) is True:
            episode.append(sample)
        else:
            episode = []
    if not episode:
        return {}
    stage_times: dict[str, float] = {}
    for stage in PIPELINE:
        for sample in episode:
            if _stage_active(sample, stage, context) is True:
                stage_times[stage] = float(sample.get("time_ms", 0.0))
                break
    result: dict[str, float] = {}
    for left, right in LATENCY_PAIRS:
        if left in stage_times and right in stage_times and stage_times[right] >= stage_times[left]:
            result[f"{left}->{right}"] = round(stage_times[right] - stage_times[left], 3)
    return result


def _make_event(
    trace: TraceData,
    code: str,
    divergence: str,
    first: Mapping[str, Any],
    last: Mapping[str, Any],
    duration: float,
    adaptive: bool = False,
    expected_next: str | None = None,
    detail: Mapping[str, Any] | None = None,
    confidence_override: str | None = None,
) -> AnomalyEvent:
    class_name, area, base_score = CLASS_INFO[code]
    data = _evidence(first)
    if code == "B":
        data["diagnostic_subreason"] = _bridge_failure_subreason(first, trace)
    if detail:
        data.update(detail)
    completeness = len(data)
    confidence = confidence_override or (
        "HIGH" if completeness >= 5 and duration >= 24 else "MEDIUM" if completeness >= 2 else "LOW"
    )
    score = min(100, int(base_score + min(4.0, duration / 250.0)))
    return AnomalyEvent(
        event_time=float(first.get("time_ms", 0.0)),
        end_time=float(last.get("time_ms", first.get("time_ms", 0.0))),
        duration_ms=duration,
        event_type=_event_type(first, _rider_active(first, _build_context(trace))),
        first_divergence=divergence,
        class_code=code,
        class_name=class_name,
        reason_bits=_reasons(first.get("reason_bits")),
        anomaly_score=score,
        confidence=confidence,
        evidence=data,
        likely_code_area=area,
        latency_ms=(
            {}
            if code in {"A0", "UNKNOWN"}
            else _event_latencies(trace, float(first.get("time_ms", 0.0)))
        ),
        cannot_distinguish=_cannot_distinguish(code, trace),
        expected_next_state=expected_next,
        adaptive=adaptive,
    )


def _segments(trace: TraceData, context: _Context) -> list[dict[str, Any]]:
    if not trace.samples:
        return []
    runs: list[dict[str, Any]] = []
    current_name = None
    first = trace.samples[0]
    last = first
    for sample in trace.samples:
        rider = _rider_active(sample, context)
        # Segmentation follows the riding situation, not the instantaneous
        # torque half-wave. Cadence with a non-invalid direction is continuous
        # pedalling even when the torque sample briefly crosses its threshold.
        cadence = _number(sample, "cadence")
        direction = _truth(sample, "direction")
        segment_rider = rider is True or (
            cadence is not None and cadence > 1.0 and direction is not False
        )
        name = _event_type(sample, segment_rider)
        if current_name is None:
            current_name, first = name, sample
        elif name != current_name:
            runs.append(
                {
                    "type": current_name,
                    "start_ms": round(float(first.get("time_ms", 0.0)), 3),
                    "end_ms": round(float(last.get("time_ms", 0.0)), 3),
                }
            )
            current_name, first = name, sample
        last = sample
    runs.append(
        {
            "type": current_name,
            "start_ms": round(float(first.get("time_ms", 0.0)), 3),
            "end_ms": round(float(last.get("time_ms", 0.0)), 3),
        }
    )
    # Remove only sampling-scale state islands. This is display segmentation,
    # not CLASS D persistence; the latter is evaluated from consecutive samples
    # and cadence below.
    segment_island_ms = context.period_ms * 3.0
    changed = True
    while changed and len(runs) >= 3:
        changed = False
        merged: list[dict[str, Any]] = []
        index = 0
        while index < len(runs):
            if (
                0 < index < len(runs) - 1
                and runs[index - 1]["type"] == runs[index + 1]["type"]
                and runs[index]["end_ms"] - runs[index]["start_ms"] + context.period_ms < segment_island_ms
            ):
                if merged:
                    merged[-1]["end_ms"] = runs[index + 1]["end_ms"]
                index += 2
                changed = True
                continue
            merged.append(dict(runs[index]))
            index += 1
        runs = merged
    return runs


def _deterministic_events(trace: TraceData, context: _Context) -> list[AnomalyEvent]:
    events: list[AnomalyEvent] = []
    active_since: float | None = None
    groups: list[tuple[str, str, dict[str, Any], dict[str, Any]]] = []
    current: tuple[str, str, dict[str, Any], dict[str, Any]] | None = None
    for sample in trace.samples:
        now = float(sample.get("time_ms", 0.0))
        if _rider_active(sample, context) is True:
            if active_since is None:
                active_since = now
        else:
            active_since = None
        finding = _first_divergence(sample, context, active_since)
        if finding is None:
            if current:
                groups.append(current)
                current = None
            continue
        code, divergence = finding
        if current and current[0:2] == (code, divergence) and now - float(current[3].get("time_ms", now)) <= max(20.0, context.period_ms * 2.5):
            current = (code, divergence, current[2], sample)
        else:
            if current:
                groups.append(current)
            current = (code, divergence, sample, sample)
    if current:
        groups.append(current)
    minimums = {"A0": 60.0, "A1": 24.0, "A2": 24.0, "A3": 20.0, "A4": 20.0, "B": 20.0, "C": 24.0}
    for code, divergence, first, last in groups:
        duration = float(last.get("time_ms", 0.0)) - float(first.get("time_ms", 0.0)) + context.period_ms
        if duration + 1e-6 < max(minimums[code], context.period_ms):
            continue
        events.append(_make_event(trace, code, divergence, first, last, duration))
    return events


@dataclass
class _MotorExpectation:
    status: str
    support: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    inhibits: list[str] = field(default_factory=list)


def _motor_response_expectation(sample: Mapping[str, Any]) -> _MotorExpectation:
    """Conservatively decide whether telemetry proves that assist should flow.

    CLASS D sits below the command/FOC recorders, so rider motion alone is not
    proof of expected motor response. The checks mirror the real ride-control
    ordering: level/gate and hard cuts first, then speed, voltage and thermal
    limiting. Any observed inhibit suppresses D; missing proof produces UNKNOWN.
    """
    support: list[str] = []
    missing: list[str] = []
    inhibits: list[str] = []

    assist_level = _number(sample, "assist_level")
    if assist_level is None:
        missing.append("assist_level")
    elif assist_level <= 0:
        inhibits.append("assist_level=0")
    else:
        support.append(f"assist_level={assist_level:g}")

    direction = _truth(sample, "direction")
    if direction is None:
        direction = _truth(sample, "forward")
    if direction is False:
        inhibits.append("forward_direction_not_confirmed")
    elif direction is True:
        support.append("forward_direction_confirmed")

    bits_number = _number(sample, "permission_bits")
    permission_bits = int(bits_number) if bits_number is not None else None
    aggregate = _truth(sample, "assist_permission")
    if aggregate is None and permission_bits is not None:
        aggregate = bool(permission_bits & 0x80)
    if aggregate is False:
        inhibits.append("assist_permission=false")
    elif aggregate is True:
        support.append("assist_permission=true")
    else:
        missing.append("assist_permission_or_permission_bits[7]")

    # These are all terminal or downstream-validity inhibits when present.
    for field_name, label in (
        ("brake_active", "brake_active"),
        ("hard_cut", "hard_cut"),
        ("safety_cut", "safety_cut"),
        ("torque_fault", "torque_sensor_fault"),
    ):
        state = _truth(sample, field_name)
        if state is True:
            inhibits.append(label)
    foc_allowed = _truth(sample, "current_cal_foc_allowed")
    if foc_allowed is False:
        inhibits.append("current_cal_foc_not_allowed")
    if permission_bits is not None and permission_bits & 0x08:
        inhibits.append("permission_bits:HARD_CUT")

    speed = _number(sample, "speed")
    speed_limit = _number(sample, "speed_limit")
    legal_enabled = _truth(sample, "legal_enabled")
    offroad = _truth(sample, "offroad")
    if legal_enabled is False or offroad is True:
        support.append("pedal_speed_limiter_bypassed")
    elif speed is not None and speed_limit is not None:
        # assist_limits_apply() is still full at speed_limit, tapers until
        # speed_limit+2 km/h, and can legally be zero at/above that endpoint.
        if speed <= speed_limit:
            support.append(f"speed={speed:g}<=limit={speed_limit:g}km/h")
        elif speed >= speed_limit + 2.0:
            inhibits.append(f"speed={speed:g}>=zero_endpoint={speed_limit + 2.0:g}km/h")
        else:
            inhibits.append(
                f"speed_limiter_taper={speed:g}km/h_in_{speed_limit:g}..{speed_limit + 2.0:g}"
            )
    elif permission_bits is not None and permission_bits & 0x20:
        support.append("permission_bits:SPEED_LIMIT_OK")
    else:
        missing.append("speed_and_speed_limit_semantics")

    temperature = _number(sample, "controller_temperature_c")
    temperature_ok = _truth(sample, "temperature_limit_ok")
    if temperature_ok is False or (temperature is not None and temperature >= 90.0):
        inhibits.append("temperature_limiter_can_zero_request")
    elif temperature_ok is True or (temperature is not None and temperature < 75.0):
        support.append("temperature_limit_ok")
    elif temperature is not None:
        inhibits.append("temperature_limiter_taper")
    else:
        missing.append("temperature_limit_ok")

    voltage_ok = _truth(sample, "voltage_limit_ok")
    if voltage_ok is False:
        inhibits.append("undervoltage_limiter")
    elif voltage_ok is True:
        support.append("voltage_limit_ok")
    else:
        # Battery voltage without the configured/raw undervoltage threshold is
        # not sufficient to reproduce assist_limits_apply().
        missing.append("voltage_limit_ok_or_configured_threshold")

    if inhibits:
        return _MotorExpectation("INHIBITED", support, sorted(set(missing)), sorted(set(inhibits)))
    if missing:
        return _MotorExpectation("INSUFFICIENT_DATA", support, sorted(set(missing)), [])
    return _MotorExpectation("CONFIRMED", support, [], [])


def _power_response_present(sample: Mapping[str, Any], context: _Context) -> bool | None:
    current = _number(sample, "current")
    power = _number(sample, "power")
    if current is None and power is None:
        return None
    current_ok = current is not None and abs(current) > (context.idle_current or 0.0) + 0.5
    power_ok = power is not None and abs(power) > max(25.0, (context.idle_power or 0.0) + 20.0)
    return bool(current_ok or power_ok)


def _telemetry_response_events(trace: TraceData, context: _Context) -> list[AnomalyEvent]:
    """Detect a telemetry-only power failure using cadence-scaled persistence."""
    if context.has_low_level:
        return []

    groups: list[list[tuple[dict[str, Any], _MotorExpectation]]] = []
    current: list[tuple[dict[str, Any], _MotorExpectation]] = []
    current_status: str | None = None

    def flush() -> None:
        nonlocal current, current_status
        if current:
            groups.append(current)
        current = []
        current_status = None

    for sample in trace.samples:
        response = _power_response_present(sample, context)
        expectation = _motor_response_expectation(sample)
        candidate = (
            _rider_active(sample, context) is True
            and response is False
            and expectation.status != "INHIBITED"
        )
        if not candidate:
            flush()
            continue
        now = float(sample.get("time_ms", 0.0))
        if current:
            previous_time = float(current[-1][0].get("time_ms", now))
            contiguous = now - previous_time <= max(20.0, context.period_ms * 2.5)
            if not contiguous or expectation.status != current_status:
                flush()
        if not current:
            current_status = expectation.status
        current.append((sample, expectation))
    flush()

    events: list[AnomalyEvent] = []
    for group in groups:
        samples = [item[0] for item in group]
        cadences = [value for sample in samples if (value := _number(sample, "cadence")) is not None and value > 1.0]
        if not cadences:
            # Without cadence there is no physical crank-cycle persistence
            # basis, so this can never be promoted to a D event.
            continue
        cadence_median = statistics.median(cadences)
        revolution_ms = 60_000.0 / cadence_median
        gaps = [
            float(right.get("time_ms", 0.0)) - float(left.get("time_ms", 0.0))
            for left, right in zip(samples, samples[1:])
            if 0.0 < float(right.get("time_ms", 0.0)) - float(left.get("time_ms", 0.0)) < 10_000.0
        ]
        sample_period = statistics.median(gaps) if gaps else context.period_ms
        required_samples = int(math.ceil(revolution_ms / sample_period)) + 1
        confirmation_span = float(samples[-1].get("time_ms", 0.0)) - float(samples[0].get("time_ms", 0.0))
        if len(samples) < required_samples or confirmation_span + 1e-6 < revolution_ms:
            continue

        expectations = [item[1] for item in group]
        status = expectations[0].status
        support = sorted({value for item in expectations for value in item.support})
        missing = sorted({value for item in expectations for value in item.missing})
        detail = {
            "motor_response_expectation": status,
            "expectation_support": support,
            "missing_expectation_signals": missing,
            "confirmation_samples": len(samples),
            "required_confirmation_samples": required_samples,
            "confirmation_span_ms": round(confirmation_span, 3),
            "sampling_period_ms": round(sample_period, 3),
            "sampling_rate_hz": round(1000.0 / sample_period, 3),
            "cadence_median_rpm": round(cadence_median, 3),
            "required_crank_revolution_ms": round(revolution_ms, 3),
            "persistence_basis": "at_least_one_crank_revolution",
        }
        code = "D" if status == "CONFIRMED" else "UNKNOWN"
        events.append(
            _make_event(
                trace,
                code,
                "POWER_RESPONSE",
                samples[0],
                samples[-1],
                confirmation_span,
                detail=detail,
                confidence_override="HIGH" if code == "D" else "LOW",
            )
        )
    return events


def _stage_active(sample: Mapping[str, Any], stage: str, context: _Context) -> bool | None:
    if stage == "RIDER_ACTIVE":
        return _rider_active(sample, context)
    if stage == "PERMISSION":
        return _permission(sample)
    if stage == "LATCHED":
        return _truth(sample, "latched")
    if stage == "MODE_DEMAND":
        value = _positive(sample, "mode_demand")
        if value is None:
            value = _positive(sample, "iq_before_pu")
        return _positive(sample, "iq_request") if value is None else value
    if stage == "IQ_BEFORE_PU":
        return _positive(sample, "iq_before_pu")
    if stage == "IQ_REQUEST":
        return _positive(sample, "iq_request")
    if stage == "IQ_AFTER_LATCH":
        return _positive(sample, "iq_after_latch")
    if stage == "IQ_PRE_RAMP":
        return _positive(sample, "iq_pre_ramp")
    if stage == "FINAL_IQ":
        return _positive(sample, "final_iq")
    if stage == "BRIDGE_ON":
        return _truth(sample, "pwm_on")
    if stage == "CURRENT_RESPONSE":
        actual = _number(sample, "iq_actual")
        final_value = abs(_number(sample, "final_iq") or 0.0)
        return abs(actual) >= max(1.0, final_value * 0.25) if actual is not None else None
    if stage == "POWER_RESPONSE":
        current = _number(sample, "current")
        power = _number(sample, "power")
        current_ok = current is not None and abs(current) > (context.idle_current or 0.0) + 0.5
        power_ok = power is not None and abs(power) > max(25.0, (context.idle_power or 0.0) + 20.0)
        return (current_ok or power_ok) if (current is not None or power is not None) else None
    return None


def extract_features(trace: TraceData) -> dict[str, Any]:
    context = _build_context(trace)
    episodes: list[list[dict[str, Any]]] = []
    current: list[dict[str, Any]] = []
    for sample in trace.samples:
        active = _rider_active(sample, context)
        if active:
            current.append(sample)
        elif current:
            episodes.append(current)
            current = []
    if current:
        episodes.append(current)
    latencies: dict[str, list[float]] = {}
    sequences: list[list[str]] = []
    for episode in episodes:
        stage_times: dict[str, float] = {}
        for stage in PIPELINE:
            for sample in episode:
                if _stage_active(sample, stage, context) is True:
                    stage_times[stage] = float(sample.get("time_ms", 0.0))
                    break
        sequence = [stage for stage in PIPELINE if stage in stage_times]
        if sequence:
            sequences.append(sequence)
        for left, right in LATENCY_PAIRS:
            if left in stage_times and right in stage_times and stage_times[right] >= stage_times[left]:
                latencies.setdefault(f"{left}->{right}", []).append(stage_times[right] - stage_times[left])
    relations: dict[str, list[float]] = {}
    relation_pairs = (
        ("iq_before_pu", "iq_request"),
        ("iq_request", "iq_pre_ramp"),
        ("iq_pre_ramp", "final_iq"),
        ("final_iq", "iq_actual"),
    )
    stride = max(1, len(trace.samples) // 500)
    for sample in trace.samples[::stride]:
        cadence = _number(sample, "cadence")
        cadence_band = "unknown" if cadence is None else f"{int(cadence // 20) * 20}-{int(cadence // 20) * 20 + 19}rpm"
        level = _number(sample, "assist_level")
        torque_basis = _number(sample, "load")
        if torque_basis is None:
            torque_basis = _number(sample, "torque_filtered")
        if torque_basis is None:
            torque_basis = _number(sample, "torque")
        torque_band = "unknown" if torque_basis is None else f"{int(torque_basis // 50) * 50}-{int(torque_basis // 50) * 50 + 49}"
        situation = _event_type(sample, _rider_active(sample, context))
        for upstream, downstream in relation_pairs:
            a, b = _number(sample, upstream), _number(sample, downstream)
            if a is None or b is None or abs(a) < 0.5:
                continue
            key = (
                f"{upstream}->{downstream}|level={int(level) if level is not None else '?'}|"
                f"{cadence_band}|torque={torque_band}|{situation}"
            )
            relations.setdefault(key, []).append(abs(b) / abs(a))
        iq_request = _number(sample, "iq_request")
        if torque_basis is not None and iq_request is not None:
            key = (
                f"torque_context->iq_request|level={int(level) if level is not None else '?'}|"
                f"{cadence_band}|torque={torque_band}|{situation}"
            )
            # This is an observed output range within a context band, not a
            # claim that torque and demand have a linear relationship.
            relations.setdefault(key, []).append(abs(iq_request))
    return {"latencies_ms": latencies, "sequences": sequences, "relations": relations}


def _stats(values: Sequence[float]) -> dict[str, Any]:
    clean = [float(value) for value in values if math.isfinite(float(value))]
    return {
        "count": len(clean),
        "min": round(min(clean), 6) if clean else None,
        "max": round(max(clean), 6) if clean else None,
        "median": round(_percentile(clean, 50), 6) if clean else None,
        "p90": round(_percentile(clean, 90), 6) if clean else None,
        "p95": round(_percentile(clean, 95), 6) if clean else None,
        "p99": round(_percentile(clean, 99), 6) if clean else None,
    }


def _aggregate_sources(sources: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    latency_values: dict[str, list[float]] = {}
    relation_values: dict[str, list[float]] = {}
    transitions: dict[str, int] = {}
    for source in sources:
        features = source.get("features", {})
        for key, values in features.get("latencies_ms", {}).items():
            latency_values.setdefault(key, []).extend(values)
        for key, values in features.get("relations", {}).items():
            relation_values.setdefault(key, []).extend(values)
        for sequence in features.get("sequences", []):
            for left, right in zip(sequence, sequence[1:]):
                transition = f"{left}->{right}"
                transitions[transition] = transitions.get(transition, 0) + 1
    return {
        "sample_count": len(sources),
        "latencies_ms": {key: _stats(values) for key, values in sorted(latency_values.items())},
        "value_relations": {key: _stats(values) for key, values in sorted(relation_values.items())},
        "known_state_transitions": dict(sorted(transitions.items())),
    }


class ProfileStore:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        if self.path.exists():
            self.data = json.loads(_read_text(self.path))
        else:
            self.data = {
                "schema_version": PROFILE_SCHEMA_VERSION,
                "analyzer_version": ANALYZER_VERSION,
                "profiles": {},
            }
        if self.data.get("schema_version") != PROFILE_SCHEMA_VERSION:
            raise ValueError("unsupported profile schema")

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self.data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    def learn_good(self, trace: TraceData) -> str:
        if trace.metadata.status != "GOOD":
            raise ValueError("baseline update refused: only status GOOD may learn")
        if not trace.metadata.complete_for_learning():
            raise ValueError("baseline update refused: version/commit/motor/config/logger metadata must be complete")
        key = trace.metadata.key()
        profile = self.data["profiles"].setdefault(
            key,
            {"metadata": trace.metadata.as_dict(), "confirmed_good_sources": [], "aggregates": {}},
        )
        source_id = trace.sha256
        source = {
            "trace_id": source_id,
            "path": str(trace.path),
            "sha256": source_id,
            "metadata": trace.metadata.as_dict(),
            "features": extract_features(trace),
        }
        profile["confirmed_good_sources"] = [
            item for item in profile["confirmed_good_sources"] if item.get("trace_id") != source_id
        ] + [source]
        profile["aggregates"] = _aggregate_sources(profile["confirmed_good_sources"])
        return source_id

    def remove_trace(self, trace_id_or_path: str) -> int:
        removed = 0
        for profile in self.data.get("profiles", {}).values():
            before = list(profile.get("confirmed_good_sources", []))
            after = [
                source
                for source in before
                if source.get("trace_id") != trace_id_or_path and source.get("path") != trace_id_or_path
            ]
            removed += len(before) - len(after)
            profile["confirmed_good_sources"] = after
            profile["aggregates"] = _aggregate_sources(after)
        return removed

    def rebuild(self) -> None:
        for profile in self.data.get("profiles", {}).values():
            profile["aggregates"] = _aggregate_sources(profile.get("confirmed_good_sources", []))

    def matching(self, metadata: TraceMetadata) -> tuple[str | None, Mapping[str, Any] | None]:
        key = metadata.key()
        profile = self.data.get("profiles", {}).get(key)
        return (key, profile) if profile else (None, None)


def _adaptive_events(trace: TraceData, profile: Mapping[str, Any]) -> list[AnomalyEvent]:
    events: list[AnomalyEvent] = []
    features = extract_features(trace)
    aggregates = profile.get("aggregates", {})
    first_sample = trace.samples[0] if trace.samples else {"time_ms": 0.0}
    for key, values in features.get("latencies_ms", {}).items():
        baseline = aggregates.get("latencies_ms", {}).get(key)
        if not baseline or baseline.get("count", 0) < 2:
            continue
        limit = max(float(baseline.get("p99") or 0.0) * 1.25, float(baseline.get("p99") or 0.0) + 4.0)
        for value in values:
            if value > limit:
                right = key.split("->", 1)[1]
                events.append(
                    _make_event(
                        trace,
                        "X_LATENCY",
                        right,
                        first_sample,
                        first_sample,
                        value,
                        adaptive=True,
                        detail={"latency": key, "observed_ms": value, "baseline_p99_ms": baseline.get("p99")},
                    )
                )
                break
    for key, values in features.get("relations", {}).items():
        baseline = aggregates.get("value_relations", {}).get(key)
        if not baseline or baseline.get("count", 0) < 5:
            continue
        low, high = float(baseline["min"]), float(baseline["max"])
        margin = max(0.05, (high - low) * 0.2)
        outliers = [value for value in values if value < low - margin or value > high + margin]
        if outliers:
            right = key.split("->", 1)[1].split("|", 1)[0].upper()
            events.append(
                _make_event(
                    trace,
                    "X_VALUE",
                    right,
                    first_sample,
                    first_sample,
                    0.0,
                    adaptive=True,
                    detail={"relation": key, "observed": outliers[0], "baseline_min": low, "baseline_max": high},
                )
            )
    known = aggregates.get("known_state_transitions", {})
    for sequence in features.get("sequences", []):
        for left, right in zip(sequence, sequence[1:]):
            transition = f"{left}->{right}"
            if known and transition not in known:
                events.append(
                    _make_event(
                        trace,
                        "X_SEQUENCE",
                        right,
                        first_sample,
                        first_sample,
                        0.0,
                        adaptive=True,
                        expected_next=next((key.split("->", 1)[1] for key in known if key.startswith(left + "->")), None),
                        detail={"unseen_transition": transition},
                    )
                )
                break
    return events


def analyze_trace(trace: TraceData, profile_store: ProfileStore | None = None) -> AnalysisResult:
    context = _build_context(trace)
    deterministic = _deterministic_events(trace, context)
    deterministic.extend(_telemetry_response_events(trace, context))
    adaptive: list[AnomalyEvent] = []
    notes: list[str] = []
    profile_key = None
    if profile_store:
        profile_key, profile = profile_store.matching(trace.metadata)
        if profile:
            adaptive = _adaptive_events(trace, profile)
        else:
            notes.append("No exact version/config/logger profile match; adaptive comparison skipped.")
    # Deterministic diagnosis wins. Suppress an adaptive event at the same stage
    # and time instead of reporting the same failure twice.
    for event in adaptive:
        duplicate = any(
            abs(event.event_time - known.event_time) <= max(20.0, context.period_ms * 2)
            and event.first_divergence == known.first_divergence
            for known in deterministic
        )
        if not duplicate:
            deterministic.append(event)
    downstream = {"permission", "latched", "iq_request", "iq_pre_ramp", "final_iq", "pwm_on", "iq_actual", "current", "power"}
    rider_evidence = [
        sample
        for sample in trace.samples
        if _rider_active(sample, context) is True
        or _torque_active(sample, context) is True
        or ((_number(sample, "cadence") or 0.0) > 1.0)
    ]
    if rider_evidence and not deterministic and not (trace.fields & downstream):
        first = rider_evidence[0]
        deterministic.append(
            _make_event(
                trace,
                "UNKNOWN",
                "UNKNOWN",
                first,
                first,
                0.0,
                detail={"required_newer_recorder_fields": sorted(downstream)},
            )
        )
    deterministic.sort(key=lambda event: (event.event_time, PIPELINE.index(event.first_divergence) if event.first_divergence in PIPELINE else 99))
    important = [
        "torque",
        "cadence",
        "direction",
        "permission",
        "latched",
        "iq_request",
        "iq_pre_ramp",
        "final_iq",
        "pwm_on",
        "iq_actual",
        "power",
    ]
    return AnalysisResult(
        trace=trace,
        events=deterministic,
        segments=_segments(trace, context),
        missing_signals=[field_name for field_name in important if field_name not in trace.fields],
        adaptive_profile_key=profile_key,
        adaptive_notes=notes,
    )


def compare_traces(good: TraceData, bad: TraceData) -> dict[str, Any]:
    good_features = extract_features(good)
    bad_features = extract_features(bad)
    latency_rows = []
    first_divergence = None
    for left, right in LATENCY_PAIRS:
        key = f"{left}->{right}"
        good_values = good_features["latencies_ms"].get(key, [])
        bad_values = bad_features["latencies_ms"].get(key, [])
        good_median = statistics.median(good_values) if good_values else None
        bad_median = statistics.median(bad_values) if bad_values else None
        status = "COMPARABLE"
        if good_values and not bad_values:
            status = "MISSING_IN_BAD"
            first_divergence = first_divergence or right
        elif good_median is not None and bad_median is not None and bad_median > max(good_median + 10.0, good_median * 2.0):
            status = "SLOW_IN_BAD"
            first_divergence = first_divergence or right
        latency_rows.append(
            {"transition": key, "good_median_ms": good_median, "bad_median_ms": bad_median, "status": status}
        )
    return {
        "schema_version": 1,
        "analysis_kind": "TRACE_COMPARISON",
        "good_trace": str(good.path),
        "bad_trace": str(bad.path),
        "first_statistical_divergence": first_divergence or "NONE_DETECTED",
        "latencies": latency_rows,
        "note": "This compares recorded traces; it is not firmware replay.",
    }


def _report_text(result: AnalysisResult) -> str:
    metadata = result.trace.metadata
    lines = [
        "=" * 50,
        "eVistDrive Trace Analyzer",
        "=" * 50,
        "",
        f"TRACE: {result.trace.path}",
        f"Adapter: {result.trace.adapter}",
        f"Firmware: {metadata.firmware_version}",
        f"Git commit: {metadata.git_commit}",
        f"Motor/config/logger: {metadata.motor_type} / {metadata.config_hash} / {metadata.logger_schema_version}",
        f"Session status: {metadata.status}",
        "",
        f"Detected anomalous events: {len(result.events)}",
        f"Critical events: {len(result.critical_events)}",
        f"Available signals: {', '.join(sorted(result.trace.fields))}",
        f"Missing important signals: {', '.join(result.missing_signals) or 'none'}",
    ]
    for index, event in enumerate(result.events, 1):
        lines.extend(
            [
                "",
                "-" * 50,
                f"EVENT {index}",
                f"time: {event.event_time / 1000.0:.3f} s (duration {event.duration_ms:.1f} ms)",
                f"type: {event.event_type}",
                "",
                f"FIRST DIVERGENCE: {event.first_divergence}",
                f"CLASS: {event.class_code} — {event.class_name}",
                f"confidence: {event.confidence}",
                f"anomaly score: {event.anomaly_score}/100",
                f"reason bits: {', '.join(event.reason_bits) or 'none/unknown'}",
                f"LIKELY CODE AREA: {event.likely_code_area}",
                "evidence:",
            ]
        )
        for key, value in event.evidence.items():
            lines.append(f"  {key}: {value}")
        if event.latency_ms:
            lines.append("latency_ms:")
            lines.extend(f"  {key}: {value}" for key, value in event.latency_ms.items())
        if event.expected_next_state:
            lines.append(f"expected next state: {event.expected_next_state}")
        if event.cannot_distinguish:
            lines.append("Cannot distinguish with this recorder:")
            lines.extend(f"  - {item}" for item in event.cannot_distinguish)
    if result.adaptive_notes:
        lines.extend(["", "Adaptive layer:"] + [f"  - {note}" for note in result.adaptive_notes])
    lines.extend(["", "Analysis mode: TRACE ANALYSIS (not firmware replay).", ""])
    return "\n".join(lines)


def _svg_plot(trace: TraceData, event: AnomalyEvent, path: Path) -> bool:
    window = [
        sample
        for sample in trace.samples
        if event.event_time - 500.0 <= float(sample.get("time_ms", 0.0)) <= event.event_time + 1000.0
    ]
    requested = ("torque", "cadence", "iq_request", "iq_pre_ramp", "final_iq", "iq_actual", "pwm_on")
    fields = [field_name for field_name in requested if any(_number(sample, field_name) is not None for sample in window)]
    if len(window) < 2 or not fields:
        return False
    width, lane_height, margin = 1100, 95, 75
    height = margin * 2 + lane_height * len(fields)
    t_min = float(window[0]["time_ms"])
    t_max = float(window[-1]["time_ms"])
    span = max(1.0, t_max - t_min)
    colors = ("#c44e52", "#4c72b0", "#55a868", "#8172b3", "#ccb974", "#64b5cd", "#dd8452")
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<style>text{font-family:Segoe UI,Arial,sans-serif;font-size:13px;fill:#222}.grid{stroke:#ddd;stroke-width:1}</style>',
        f'<text x="{margin}" y="28" font-size="18">{html.escape(trace.path.name)} — {html.escape(event.class_code)} first divergence</text>',
    ]
    plot_width = width - margin - 30
    for lane, (field_name, color) in enumerate(zip(fields, colors)):
        top = margin + lane * lane_height
        values = [_number(sample, field_name) for sample in window]
        numeric = [value for value in values if value is not None]
        low, high = min(numeric), max(numeric)
        if math.isclose(low, high):
            high = low + 1.0
        points = []
        for sample, value in zip(window, values):
            if value is None:
                continue
            x = margin + (float(sample["time_ms"]) - t_min) / span * plot_width
            y = top + lane_height - 18 - (value - low) / (high - low) * (lane_height - 35)
            points.append(f"{x:.1f},{y:.1f}")
        parts.extend(
            [
                f'<line class="grid" x1="{margin}" y1="{top + lane_height - 18}" x2="{width - 30}" y2="{top + lane_height - 18}"/>',
                f'<text x="8" y="{top + 25}">{html.escape(field_name)}</text>',
                f'<text x="8" y="{top + 44}">{low:.1f}..{high:.1f}</text>',
                f'<polyline fill="none" stroke="{color}" stroke-width="2" points="{" ".join(points)}"/>',
            ]
        )
    marker_x = margin + (event.event_time - t_min) / span * plot_width
    parts.extend(
        [
            f'<line x1="{marker_x:.1f}" y1="50" x2="{marker_x:.1f}" y2="{height - 35}" stroke="#d62728" stroke-width="3" stroke-dasharray="8 5"/>',
            f'<text x="{min(marker_x + 7, width - 260):.1f}" y="55" fill="#d62728">FIRST DIVERGENCE: {html.escape(event.first_divergence)}</text>',
            f'<text x="{margin}" y="{height - 12}">window {t_min - event.event_time:.0f}..+{t_max - event.event_time:.0f} ms relative to event</text>',
            "</svg>",
        ]
    )
    path.write_text("\n".join(parts), encoding="utf-8")
    return True


def write_outputs(result: AnalysisResult, output_dir: str | Path, plots: bool = True) -> dict[str, Path]:
    directory = Path(output_dir)
    directory.mkdir(parents=True, exist_ok=True)
    for stale_plot in directory.glob("event_*.svg"):
        stale_plot.unlink()
    json_path = directory / "analysis.json"
    report_path = directory / "report.txt"
    json_path.write_text(json.dumps(result.as_dict(), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    report_path.write_text(_report_text(result), encoding="utf-8")
    outputs = {"json": json_path, "report": report_path}
    if plots:
        for index, event in enumerate(result.events, 1):
            plot_path = directory / f"event_{index:03d}.svg"
            if _svg_plot(result.trace, event, plot_path):
                outputs[f"plot_{index}"] = plot_path
    return outputs


def run_regressions(root: str | Path) -> dict[str, Any]:
    base = Path(root)
    cases = []
    for trace_path in sorted(base.rglob("*.csv")):
        metadata = _load_metadata(trace_path, None, None)
        trace = load_trace(trace_path)
        result = analyze_trace(trace)
        expected = metadata.extra.get("expected", {}) if isinstance(metadata.extra.get("expected", {}), dict) else {}
        expected_class = expected.get("class")
        expected_divergence = expected.get("first_divergence")
        max_critical = expected.get("max_critical")
        actual = result.events[0] if result.events else None
        passed = True
        reasons = []
        if expected_class and (actual is None or actual.class_code != expected_class):
            passed = False
            reasons.append(f"expected class {expected_class}, got {actual.class_code if actual else 'NONE'}")
        if expected_divergence and (actual is None or actual.first_divergence != expected_divergence):
            passed = False
            reasons.append(
                f"expected divergence {expected_divergence}, got {actual.first_divergence if actual else 'NONE'}"
            )
        if max_critical is not None and len(result.critical_events) > int(max_critical):
            passed = False
            reasons.append(f"critical events {len(result.critical_events)} > {max_critical}")
        cases.append(
            {
                "trace": str(trace_path),
                "status": "PASS" if passed else "FAIL",
                "reason": "; ".join(reasons),
                "actual_class": actual.class_code if actual else None,
                "actual_first_divergence": actual.first_divergence if actual else None,
            }
        )
    return {
        "schema_version": 1,
        "analysis_kind": "TRACE_ANALYSIS_REGRESSION",
        "cases": cases,
        "passed": sum(case["status"] == "PASS" for case in cases),
        "failed": sum(case["status"] == "FAIL" for case in cases),
        "note": "Recorded-output analysis only; no firmware replay is claimed.",
    }
