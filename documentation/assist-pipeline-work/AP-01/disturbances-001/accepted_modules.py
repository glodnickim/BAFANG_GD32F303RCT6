"""Explicit, path-independent loading of the ACCEPTED AP-01 measurement modules.

REVIEW-EVD-AP-01-010 (D010-05) asked for an explicit import of the accepted module instead of
relying on whatever happens to be importable. The earlier shims did
`sys.path.insert(0, ".../transients-001")`, which is worse than implicit: AP-01, transients-001 and
disturbances-001 each contain a package called `scenarios`, so importing the metrics adapter
silently moved `import scenarios` onto ANOTHER task's scenario set. That is how a test can end up
validating 35 transients cases while reporting on disturbances-001.

So: load each accepted module from its FILE, register it under a task-unique module name, and never
touch sys.path. Nothing here copies or redefines the accepted maths.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent

ACCEPTED_METRICS_PY = AP01 / "metrics.py"
ACCEPTED_EVENT_METRICS_PY = AP01 / "transients-001" / "event_metrics.py"


def load_by_path(path: Path, module_name: str):
    """Import `path` under `module_name`, without adding its directory to sys.path."""
    if module_name in sys.modules:
        return sys.modules[module_name]
    if not path.is_file():
        raise ImportError(f"accepted module not found on disk: {path}")
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot build an import spec for {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(module_name, None)
        raise
    return module


def accepted_metrics():
    """AP-01 metrics.py - the accepted primitives (mean, window, stage_analysis, ...)."""
    return load_by_path(ACCEPTED_METRICS_PY, "ap01_accepted_metrics")


def accepted_event_metrics():
    """transients-001 event_metrics.py - the accepted event/window primitives."""
    return load_by_path(ACCEPTED_EVENT_METRICS_PY, "ap01_accepted_event_metrics")


SOURCES = {
    "ap01_accepted_metrics": str(ACCEPTED_METRICS_PY),
    "ap01_accepted_event_metrics": str(ACCEPTED_EVENT_METRICS_PY),
}
