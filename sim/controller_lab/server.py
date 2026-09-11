#!/usr/bin/env python3
"""EVistDrive source-linked Controller Lab local web server.

Every /api/run fingerprints current src/ + inc/ + harness inputs and rebuilds the native host
executable when that fingerprint changes. The browser therefore observes the checked-out source,
not a Python reimplementation of the control algorithm.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import subprocess
import sys
import threading
import time
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]
LAB = ROOT / "sim" / "controller_lab"
WEB = LAB / "web"
OUT = ROOT / ".build" / "controller_lab"
EXE = OUT / ("controller_lab.exe" if os.name == "nt" else "controller_lab")
STAMP = OUT / "source.sha256"
CC = os.environ.get("CC", "gcc")

# --- Canable settings source -------------------------------------------------------------
# The rider's real settings come from Canable, not from a second parameter UI here. Whatever
# JSON is obtained below (live push or exported file) is converted to the controller's own
# wire blobs by canable-web's OWN serializers (canable_bridge.js -> canbus.js) and handed to
# the production apply_blob functions. Nothing about a tuning field is redefined on this side.
#
# Two sources, in priority order:
#   1. LIVE - canable-web's own server (ui/js/evistdrive/live-bridge.js) pushes state.lastTuning
#      / state.lastBanks to its own /api/evistdrive-live whenever they change; this process
#      polls that endpoint. Works only while that canable-web tab is open and has read/written
#      something from/to the bike at least once this session.
#   2. FILE - a preset exported once via canable-web's "Export preset" button. Always available,
#      never disappears when the canable-web tab is closed - the deliberate fallback.
PRESET = Path(os.environ.get("EVIST_CANABLE_PRESET", LAB / "canable_preset.json"))
BRIDGE = LAB / "canable_bridge.js"
NODE = os.environ.get("NODE", "node")
CANABLE_LIVE_URL = os.environ.get("EVIST_CANABLE_URL", "http://127.0.0.1:8080/api/evistdrive-live")
CANABLE_LIVE_TIMEOUT_S = 1.5

# Reuse the exact Level-4 production module list instead of maintaining another controller list.
sys.path.insert(0, str(ROOT))
from tools.run_level4 import PROD  # noqa: E402

BUILD_LOCK = threading.Lock()


def source_files() -> list[Path]:
    files = [ROOT / p for p in PROD]
    files += sorted((ROOT / "inc").glob("*.h"))
    files += [LAB / "controller_lab.c", ROOT / "tools" / "run_level4.py"]
    return [p for p in files if p.exists()]


def fingerprint() -> str:
    h = hashlib.sha256()
    for p in source_files():
        rel = p.relative_to(ROOT).as_posix().encode()
        h.update(rel + b"\0" + p.read_bytes() + b"\0")
    return h.hexdigest()


def git_info() -> dict:
    def run(*args: str) -> str:
        try:
            return subprocess.check_output(["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            return "unknown"
    head = run("rev-parse", "--short=12", "HEAD")
    dirty_text = run("status", "--porcelain", "--", "src", "inc", "sim/controller_lab", "tools/run_level4.py")
    return {"head": head, "dirty": bool(dirty_text and dirty_text != "unknown")}


_SETTINGS_CACHE: dict = {"key": None, "value": None}


def fetch_live_preset() -> tuple[bytes | None, str | None]:
    """Poll canable-web's own live endpoint. Returns (json_bytes, error) - never raises.

    Silent-ish by design: canable-web simply not running is the common case (nobody has it
    open right now), not a fault to alarm about every 1.5 s.
    """
    import urllib.error
    import urllib.request
    try:
        with urllib.request.urlopen(CANABLE_LIVE_URL, timeout=CANABLE_LIVE_TIMEOUT_S) as r:
            body = json.loads(r.read())
    except (urllib.error.URLError, OSError, TimeoutError, ValueError) as e:
        return None, f"canable-web live endpoint not reachable at {CANABLE_LIVE_URL} ({e})"
    if not body.get("available"):
        return None, body.get("reason") or "canable-web has nothing to push yet"
    payload = body.get("payload")
    if not payload:
        return None, "canable-web live endpoint returned no payload"
    return json.dumps(payload).encode("utf-8"), None


def run_bridge(preset_bytes: bytes) -> dict:
    p = subprocess.run([NODE, str(BRIDGE)], cwd=ROOT, input=preset_bytes,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    out = json.loads(p.stdout) if p.stdout.strip().startswith(b"{") else {}
    if p.returncode or out.get("error"):
        raise RuntimeError((out.get("error") or p.stdout.decode(errors="replace") or "bridge failed").strip()[:400])
    return out


def settings_state() -> dict:
    """Canable-sourced settings for the next run, or why there are none.

    Deliberately NOT part of the build fingerprint: changing a setting changes the RUN, not
    the executable, so it must never trigger a recompile. It does change the result, so the
    UI can re-run on it.

    Tries LIVE first (canable-web's own /api/evistdrive-live), falls back to the exported
    FILE. Whichever wins, the blobs come from canable-web's own serializers - see the module
    docstring above PRESET/CANABLE_LIVE_URL.
    """
    live_bytes, live_reason = fetch_live_preset()
    if live_bytes is not None:
        key = ("live", hashlib.sha256(live_bytes).hexdigest())
        if _SETTINGS_CACHE["key"] == key:
            return _SETTINGS_CACHE["value"]
        state = {"available": False, "source": "live", "hash": key[1][:12]}
        try:
            out = run_bridge(live_bytes)
            state["available"] = True
            state["tuning_blob"] = out.get("tuning_blob", "")
            state["bank_blob"] = out.get("bank_blob", "")
            state["meta"] = out.get("meta", {})
        except Exception as e:
            state["reason"] = str(e)
        _SETTINGS_CACHE["key"] = key
        _SETTINGS_CACHE["value"] = state
        return state

    if not PRESET.exists():
        return {"available": False, "source": "none", "path": str(PRESET),
                "reason": f"{live_reason}; no fallback file at {PRESET.name} either "
                          f"- open canable-web or export a preset"}
    stat = PRESET.stat()
    key = ("file", stat.st_mtime_ns, stat.st_size)
    if _SETTINGS_CACHE["key"] == key:
        return _SETTINGS_CACHE["value"]

    file_bytes = PRESET.read_bytes()
    state = {
        "available": False,
        "source": "file",
        "path": str(PRESET),
        "hash": hashlib.sha256(file_bytes).hexdigest()[:12],
        "live_reason": live_reason,
    }
    try:
        out = run_bridge(file_bytes)
        state["available"] = True
        state["tuning_blob"] = out.get("tuning_blob", "")
        state["bank_blob"] = out.get("bank_blob", "")
        state["meta"] = out.get("meta", {})
    except Exception as e:
        state["reason"] = str(e)

    _SETTINGS_CACHE["key"] = key
    _SETTINGS_CACHE["value"] = state
    return state


def build_status() -> dict:
    fp = fingerprint()
    built = STAMP.read_text(encoding="ascii").strip() if STAMP.exists() else ""
    return {
        "source_hash": fp,
        "source_short": fp[:12],
        "built_hash": built,
        "built_short": built[:12] if built else None,
        "stale": (not EXE.exists()) or built != fp,
        "git": git_info(),
        "production_modules": len(PROD),
        "settings": settings_state(),
    }


def ensure_built() -> dict:
    with BUILD_LOCK:
        status = build_status()
        if not status["stale"]:
            status["rebuilt"] = False
            return status
        OUT.mkdir(parents=True, exist_ok=True)
        flags = [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=type-limits",
            "-Wno-error=unused-parameter", "-O2",
        ]
        cmd = [
            CC, *flags,
            "-Isim/full_host_stubs", "-Isim/l4", "-Iinc", "-Itests/host/common",
            "-o", str(EXE), "sim/controller_lab/controller_lab.c", *PROD, "-lm",
        ]
        p = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if p.returncode:
            raise RuntimeError("Native Controller Lab build failed:\n" + p.stdout)
        fp = fingerprint()
        STAMP.write_text(fp, encoding="ascii")
        status = build_status()
        status["rebuilt"] = True
        status["build_output"] = p.stdout
        return status


def clamp_num(value, lo, hi, default):
    try:
        x = float(value)
    except (TypeError, ValueError):
        x = default
    return min(hi, max(lo, x))


def normalize_cfg(raw: dict) -> dict:
    modes = {"power", "progressive", "emtb", "torque", "curve", "keep"}
    mode = str(raw.get("mode", "power"))
    if mode not in modes:
        mode = "power"
    return {
        "duration": clamp_num(raw.get("duration"), 0.25, 30.0, 6.0),
        "cadence": clamp_num(raw.get("cadence"), 0.0, 180.0, 72.0),
        "cadence_ripple": clamp_num(raw.get("cadence_ripple"), 0.0, 80.0, 0.0),
        "cadence_ripple_hz": clamp_num(raw.get("cadence_ripple_hz"), 0.05, 5.0, 0.6),
        "torque": clamp_num(raw.get("torque"), 0.0, 180.0, 28.0),
        "torque_ripple": clamp_num(raw.get("torque_ripple"), 0.0, 150.0, 45.0),
        "asymmetry": clamp_num(raw.get("asymmetry"), -90.0, 90.0, 8.0),
        "speed": clamp_num(raw.get("speed"), 0.0, 100.0, 18.0),
        "voltage": clamp_num(raw.get("voltage"), 20.0, 65.0, 39.0),
        "soc": clamp_num(raw.get("soc"), 0.0, 100.0, 55.0),
        "assist": int(clamp_num(raw.get("assist"), 0, 5, 3)),
        "mode": mode,
        "sample_ms": int(clamp_num(raw.get("sample_ms"), 2, 50, 5)),
        # START -> STOP: when the legs start and when they stop. 0 = never stop.
        "ride_start_s": clamp_num(raw.get("ride_start_s"), 0.0, 30.0, 0.0),
        "ride_stop_s": clamp_num(raw.get("ride_stop_s"), 0.0, 30.0, 0.0),
    }


def release_timings(rows: list) -> dict:
    """What happens between the legs stopping and the motor going quiet, and when.

    Measured off the produced trace - nothing here models or predicts the behaviour.
    """
    stop_t = None
    for r in rows:
        if r.get("pedalling") == 1.0:
            stop_t = r.get("time_s")
    if stop_t is None:
        return {"available": False, "reason": "rider never stops in this scenario"}

    after = [r for r in rows if (r.get("time_s") or 0) > stop_t]
    iq_at_stop = next((r.get("iq_ref") for r in reversed(rows)
                       if (r.get("time_s") or 0) <= stop_t), None)

    def first_ms(pred):
        for r in after:
            try:
                if pred(r):
                    return round((r["time_s"] - stop_t) * 1000.0, 1)
            except (TypeError, KeyError):
                continue
        return None

    return {
        "available": True,
        "legs_stop_s": round(stop_t, 4),
        "iq_at_stop": iq_at_stop,
        "iq_zero_ms": first_ms(lambda r: r.get("iq_ref") == 0),
        "cadence_zero_ms": first_ms(lambda r: r.get("cadence_control_rpm") == 0),
        "session_change_ms": first_ms(lambda r: r.get("session") != rows[0].get("session")),
    }


def run_lab(raw: dict) -> dict:
    status = ensure_built()
    cfg = normalize_cfg(raw)
    args = [str(EXE)] + [f"{k}={v}" for k, v in cfg.items()]

    # Canable settings, when present, are applied through the production apply_blob path.
    settings = settings_state()
    use_canable = bool(raw.get("use_canable", True)) and settings.get("available")
    if use_canable:
        if settings.get("tuning_blob"):
            args.append(f"tuning_blob={settings['tuning_blob']}")
        if settings.get("bank_blob"):
            args.append(f"bank_blob={settings['bank_blob']}")
    p = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError(p.stderr or p.stdout or f"Controller Lab exited {p.returncode}")
    rows = list(csv.DictReader(io.StringIO(p.stdout)))
    numeric = []
    for row in rows:
        out = {}
        for k, v in row.items():
            try:
                out[k] = float(v)
            except (TypeError, ValueError):
                out[k] = v
        numeric.append(out)
    return {
        "config": cfg,
        "source": status,
        "settings": settings,
        "settings_applied": use_canable,
        "release": release_timings(numeric),
        "rows": numeric,
        "stderr": p.stderr.strip(),
        "notes": [
            "Control and Iq stages come from production C linked from the current checkout.",
            "Speed, battery voltage and SOC are forced test inputs.",
            "Battery current, FOC voltage utilization and measured phase currents are not invented in Controller Lab; use Level 4/replay for closed-loop electrical behavior.",
        ],
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "EVistControllerLab/0.1"

    def log_message(self, fmt, *args):
        print("[web] " + (fmt % args))

    def send_json(self, payload, code=HTTPStatus.OK):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/status":
            try:
                self.send_json(build_status())
            except Exception as e:
                self.send_json({"error": str(e)}, HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        if path in ("/", "/index.html"):
            data = (WEB / "index.html").read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if urlparse(self.path).path != "/api/run":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            n = min(int(self.headers.get("Content-Length", "0")), 64 * 1024)
            raw = json.loads(self.rfile.read(n) or b"{}")
            self.send_json(run_lab(raw))
        except Exception as e:
            self.send_json({"error": str(e)}, HTTPStatus.INTERNAL_SERVER_ERROR)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--open", action="store_true")
    ap.add_argument("--build-only", action="store_true")
    args = ap.parse_args()
    status = ensure_built()
    print(f"Controller Lab source={status['source_short']} git={status['git']['head']} modules={status['production_modules']}")
    if args.build_only:
        return
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"EVistDrive Controller Lab: {url}")
    if args.open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
