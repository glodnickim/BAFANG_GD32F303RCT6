"""AP-01 transients-001 / rework-002: input identity and protected-artefact verification.

TASK-EVD-AP-01-TRANSIENTS-REWORK-002: "Nie odtwarzaj fikcyjnego pre-hash historycznej sesji: zapisz
rzeczywiste hashe obecnego wejscia przed rozpoczeciem tej poprawki." and "Przed praca porownaj
hashe z AP-01-review-005/review-evidence.json i sprawdz WIP" (rework-001 wording; for this round the
authoritative baseline is AP-01-review-006/review-evidence.json, the state the reviewer actually
examined).

Two questions, answered against the reviewer's own evidence file rather than against anything this
session wrote:

  1. INPUT IDENTITY - did this rework start from exactly the files REVIEW-EVD-AP-01-006 reviewed?
     The pre-edit hashes recorded at 2026-09-09T13:13:25+02:00 are compared with the reviewer's.
  2. PROTECTED ARTEFACTS - are the 39 CSVs, results/, rework-001/, the snapshot and the before/after
     manifests still byte-identical now, after the rework?

Run:  python rework-002/verify_inputs.py
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
FW = BASE.parents[3]
WORKSPACE = FW.parent
REVIEW_EVIDENCE = (WORKSPACE / "integration" / "test-results" / "AP-01-review-006"
                   / "review-evidence.json")

# Real sha256 of every file this rework was about to edit, taken from disk BEFORE the first edit of
# this session (2026-09-09T13:13:25+02:00, `sha256sum *.py *.md plots/index.pl.md`). These are
# recorded values, not a reconstruction.
PRE_EDIT_SHA256 = {
    "analyze.py": "4a2989bec5296b2f9c2ace399d0f0356b8f449f38acd5a06a3272b568540dfa7",
    "baseline_manifest.py": "ab94793c5584d32310cccf3544042b6634630dcb2e21522ef6a810a0dfdc8045",
    "event_metrics.py": "e5502856c0b88ba140c9a3c23f4c185614547bdae2d0b858f95f980336b828cb",
    "make_plots.py": "3cfd3b2549b1c0e51d2801a5d18a5ea5bd71e4e6adba23922de6fc01535bfb65",
    "test_transients.py": "f8fd90a521b737e1c64922586578659c726b75b115c237a006b384a641057cb8",
    "transients_runner.py": "8300d8137c6e8a0f35813498e9a7cb4dd75a234f192df43f4e5380477d20a6c3",
    "README.md": "35c8c8367fe4b09eec1ea64cddb7919917de6950d48b96f1ac3c8b9762d4dd0a",
    "plots/index.pl.md": "b2b81780dd961a39ac57cb2801c1e57aae05de95fb65cf64cce9455aa70a3df4",
}

PREFIX = "/files/motor-controller-firmware/documentation/assist-pipeline-work/AP-01/transients-001/"


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def reviewer_hashes() -> dict[str, str]:
    """Flatten the reviewer's evidence into {relative path: sha256}."""
    d = json.loads(REVIEW_EVIDENCE.read_text(encoding="utf-8"))
    out = {}

    def walk(o, p=""):
        if isinstance(o, dict):
            for k, v in o.items():
                walk(v, f"{p}/{k}")
        elif isinstance(o, list):
            for i, v in enumerate(o):
                walk(v, f"{p}[{i}]")
        elif (isinstance(o, str) and len(o) == 64
              and all(c in "0123456789abcdef" for c in o) and p.startswith(PREFIX)):
            out[p[len(PREFIX):]] = o

    walk(d)
    return out


def main() -> int:
    rev = reviewer_hashes()
    report = dict(
        task="TASK-EVD-AP-01-TRANSIENTS-REWORK-002",
        baseline_evidence=str(REVIEW_EVIDENCE).replace("\\", "/"),
        baseline_evidence_sha256=sha(REVIEW_EVIDENCE),
        pre_edit_recorded_at="2026-09-09T13:13:25+02:00",
    )

    # 1. Did the rework start from the reviewed state?
    identity, mismatched = {}, []
    for rel, got in PRE_EDIT_SHA256.items():
        exp = rev.get(rel)
        ok = (exp == got)
        identity[rel] = dict(pre_edit_sha256=got, reviewer_sha256=exp, match=ok)
        if not ok:
            mismatched.append(rel)
    report["input_identity"] = dict(
        files=len(identity), matched=len(identity) - len(mismatched), mismatched=mismatched,
        all_match=not mismatched, entries=identity,
        note=("the pre-edit state of every edited file equals the state REVIEW-EVD-AP-01-006 "
              "examined, so this rework demonstrably built on the reviewed code" if not mismatched
              else "MISMATCH - the rework did not start from the reviewed state"))

    # 2. Are the protected artefacts still untouched AFTER the rework?
    protected_globs = ["results/*.csv", "results/transients-results.json",
                       "results/transients-metrics.json", "rework-001/*", "snapshot/*",
                       "manifest_before.json", "manifest_after.json", "baseline_manifest.py"]
    protected, changed, unchecked = {}, [], []
    for pattern in protected_globs:
        for p in sorted(BASE.glob(pattern)):
            if not p.is_file():
                continue
            rel = p.relative_to(BASE).as_posix()
            now = sha(p)
            exp = rev.get(rel)
            protected[rel] = dict(sha256_now=now, reviewer_sha256=exp,
                                  match=(None if exp is None else exp == now))
            if exp is None:
                unchecked.append(rel)
            elif exp != now:
                changed.append(rel)
    report["protected_artefacts"] = dict(
        checked=len(protected), changed=changed,
        not_in_reviewer_evidence=unchecked,
        all_unchanged=not changed, entries=protected,
        note=("every protected artefact covered by the reviewer's evidence still hashes the same "
              "after the rework; files listed under not_in_reviewer_evidence were not hashed by "
              "the reviewer and are reported, not asserted"))

    out = HERE / "input-verification.json"
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"input identity : {report['input_identity']['matched']}/"
          f"{report['input_identity']['files']} edited files match REVIEW-006's evidence")
    if mismatched:
        print("  MISMATCHED:", mismatched)
    print(f"protected      : {len(protected)} artefacts checked, "
          f"{len(changed)} changed, {len(unchecked)} not covered by the reviewer's evidence")
    if changed:
        print("  CHANGED:", changed)
    print(f"wrote {out.name}")
    return 1 if (mismatched or changed) else 0


if __name__ == "__main__":
    sys.exit(main())
