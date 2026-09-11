"""AP-01 transients-001: before/after manifest of everything this task could have touched.

Two jobs:

1. Re-check the 258-file base captured in integration/tasks/TASK-EVD-AP-01-BASELINE.json. That base
   is HISTORICAL IDENTIFICATION and is never rewritten here - only compared against.
2. Record the ALLOWED harness difference explicitly: this task's WRITE_SCOPE is
   sim/controller_lab/controller_lab.c, sim/controller_lab/README.md and this new directory. A
   difference in any of those is expected and is listed by name with its before/after hash. A
   difference ANYWHERE ELSE is reported as OUT_OF_SCOPE_DIFFERENCE - it is a finding to raise, not
   something this script resets.

Usage:
    python baseline_manifest.py before
    python baseline_manifest.py after
"""
from __future__ import annotations

import datetime
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent
FW = AP01.parents[2]
ROOT = FW.parent
BASELINE = ROOT / "integration" / "tasks" / "TASK-EVD-AP-01-BASELINE.json"

# Files this task is allowed to change, relative to the eVistDrive workspace root.
ALLOWED_CHANGED = {
    "motor-controller-firmware/sim/controller_lab/controller_lab.c",
    "motor-controller-firmware/sim/controller_lab/README.md",
}
# Everything created under here is new work product, not a modification of the accepted base.
ALLOWED_NEW_PREFIX = "motor-controller-firmware/documentation/assist-pipeline-work/AP-01/transients-001/"


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def check_base() -> dict:
    base = json.loads(BASELINE.read_text(encoding="utf-8"))
    files = base["files"]
    missing, changed, unchanged = [], [], 0
    for rel, expected in files.items():
        p = ROOT / rel
        if not p.exists():
            missing.append(rel)
            continue
        actual = sha256(p)
        if actual == expected:
            unchanged += 1
        else:
            changed.append(dict(path=rel, baseline_sha256=expected, current_sha256=actual,
                                in_write_scope=rel in ALLOWED_CHANGED))
    out_of_scope = [c for c in changed if not c["in_write_scope"]]
    return dict(
        baseline_file=str(BASELINE.relative_to(ROOT)).replace("\\", "/"),
        baseline_captured_at=base["captured_at"],
        total_files=len(files), unchanged=unchanged,
        missing=missing, changed=changed,
        allowed_harness_changes=[c["path"] for c in changed if c["in_write_scope"]],
        out_of_scope_differences=out_of_scope,
        verdict=("OK_ONLY_ALLOWED_HARNESS_DIFFERENCE" if not out_of_scope and not missing
                 else "OUT_OF_SCOPE_DIFFERENCE"),
        verdict_note=("An out-of-scope difference is REPORTED, not reset: resetting someone else's "
                      "working tree is outside this assignment."),
    )


# The BEFORE state of the two write-scope files is taken from the byte copies stored in
# snapshot/ at the start of this task, not from the live files: by the time a manifest is written
# the live files are already extended. The snapshot is verifiable - its controller_lab.c hash is
# the one the task card states as the starting SHA256.
SNAPSHOT = {
    "motor-controller-firmware/sim/controller_lab/controller_lab.c":
        HERE / "snapshot" / "controller_lab.c.input",
    "motor-controller-firmware/sim/controller_lab/README.md":
        HERE / "snapshot" / "README.md.input",
}


def scope_files(phase: str) -> dict:
    """Hashes of the write-scope files: from the snapshot for 'before', live for 'after'."""
    out = {}
    for rel in sorted(ALLOWED_CHANGED):
        p = SNAPSHOT[rel] if phase == "before" else ROOT / rel
        out[rel] = sha256(p) if p.exists() else None
    if phase == "after":
        for p in sorted(HERE.rglob("*")):
            if p.is_file() and "__pycache__" not in p.parts:
                out[str(p.relative_to(ROOT)).replace("\\", "/")] = sha256(p)
    return out


def accepted_artifacts() -> dict:
    """The accepted AP-01 outputs this task must leave alone (evidence, not a target)."""
    out = {}
    for p in sorted((AP01 / "results").glob("*")):
        if p.is_file():
            out[str(p.relative_to(ROOT)).replace("\\", "/")] = sha256(p)
    return out


def main() -> int:
    phase = sys.argv[1] if len(sys.argv) > 1 else "after"
    if phase not in ("before", "after"):
        print("usage: baseline_manifest.py [before|after]", file=sys.stderr)
        return 2
    doc = dict(
        phase=phase,
        generated_at=datetime.datetime.now().astimezone().isoformat(),
        task="TASK-EVD-AP-01-TRANSIENTS-001",
        write_scope=sorted(ALLOWED_CHANGED) + [ALLOWED_NEW_PREFIX],
        base_check=(check_base() if phase == "after" else dict(
            note=("'before' re-states the accepted base as captured: the two write-scope files are "
                  "hashed from snapshot/, which is byte-identical to the task card's stated "
                  "starting SHA256. The drift check itself is run in the 'after' phase."),
            baseline_file=str(BASELINE.relative_to(ROOT)).replace("\\", "/"),
            baseline_sha256=sha256(BASELINE))),
        write_scope_file_hashes=scope_files(phase),
        accepted_ap01_results_hashes=accepted_artifacts(),
    )
    out = HERE / f"manifest_{phase}.json"
    out.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    bc = doc["base_check"]
    if phase == "before":
        print(f"before: write-scope input hashes recorded from snapshot/ "
              f"({len(doc['write_scope_file_hashes'])} files); drift check runs in the after phase")
    else:
        print(f"after: base {bc['unchanged']}/{bc['total_files']} unchanged, "
              f"allowed harness changes={bc['allowed_harness_changes']}, "
              f"out-of-scope={len(bc['out_of_scope_differences'])}, verdict={bc['verdict']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
