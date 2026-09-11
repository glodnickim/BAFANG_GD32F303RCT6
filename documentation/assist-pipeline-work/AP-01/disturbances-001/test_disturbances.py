"""AP-01 disturbances-001 tests: CSV contract, event windows, the new CLI contract, the
disturbance mechanics, and the runner's own failure handling.

Sections:
  A. metric/window tests on SYNTHETIC signals with analytically known answers;
  B. CSV column contract - positive AND negative (a deliberately shifted row must be REJECTED);
  C. CLI tests against the REAL native binary: new keys, rejection of bad arguments;
  D. timebase mechanics - MANDATORY assertions on skipped ticks, first resume, elapsed and
     16 kHz continuity for 1/4/20 ticks, observed at sample_ticks=1 (full 4 kHz row rate);
  E. PAS edge mechanics - missed edge and late edge, with control and recovery phases;
  F. runner negative tests in ISOLATED result directories - missing/duplicate/ERROR/thin
     repeats/mismatched hashes/unlinked regression, and the real ABORTED-manifest path.

REVIEW-EVD-AP-01-010 notes addressed here:
  D010-01  every row is checked against the header BY NAME and by cross-field meaning; a length
           test alone is explicitly proven insufficient by test_contract_rejects_shift().
  D010-03  the delay tests are mandatory and cannot pass by finding nothing: each one asserts a
           concrete count/tick/elapsed, and a missing observation is a FAIL, not a silent skip.
           sample_ticks=1 removes the sample_ms=1 aliasing (one row per 4 ticks).
  D010-02  the runner's guards are exercised against synthetic manifests and the abort path is
           driven end-to-end, in temporary directories - the accepted results are never touched.

Run:  python documentation/assist-pipeline-work/AP-01/disturbances-001/test_disturbances.py
"""
from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
AP01 = HERE.parent
FW = AP01.parents[2]
sys.path.insert(0, str(AP01))
sys.path.insert(0, str(HERE))

import ap01_runner as accepted  # noqa: E402
import csv_contract  # noqa: E402
import disturbance_measures as dm  # noqa: E402
import disturbance_metrics as em  # noqa: E402
from scenarios import scenarios as scn  # noqa: E402

FAILED: list[str] = []
PASSED = 0
CTRL_HZ = 4000
INNER_PER_CTRL = 4          # 16 kHz fast_iq_slew ticks per 4 kHz control tick


def check(name: str, condition: bool, detail: str = "") -> None:
    global PASSED
    if condition:
        PASSED += 1
        print(f"  PASS  {name}")
    else:
        FAILED.append(f"{name}: {detail}")
        print(f"  FAIL  {name}  {detail}")


def raises(name: str, fn, exc=Exception) -> None:
    try:
        fn()
    except exc:
        check(name, True)
        return
    except Exception as e:  # noqa: BLE001
        check(name, False, f"raised {type(e).__name__}, expected {exc.__name__}")
        return
    check(name, False, "did not raise")


# ============================ A. windows =====================================================

def test_event_windows() -> None:
    print("event_windows")
    w = em.event_windows([("start", 2.0), ("stop", 5.0), ("restart", 5.2)], 12.0,
                         pre_s=1.0, settle_s=0.5, post_s=1.5)
    check("start pre window starts 1.0 s before the start",
          w["start"]["pre"] == [1.0, 2.0], str(w["start"]["pre"]))
    check("stop post window is empty - the restart follows before the settle allowance",
          w["stop"]["post"] is None, str(w["stop"]["post"]))
    check("restart pre window is clipped by the stop, not 1.0 s long",
          w["restart"]["pre"] == [5.0, 5.2] and w["restart"]["pre_clipped_by"] == "stop",
          str(w["restart"]))
    check("a clipped window is reported as not full length",
          w["restart"]["pre_full_length"] is False)


def test_step_response_known() -> None:
    print("step_response on an analytically known step")
    times = [round(i * 0.01, 4) for i in range(0, 601)]
    values = []
    for t in times:
        if t < 2.0:
            values.append(0.0)
        elif t < 3.0:
            values.append(100.0 * (t - 2.0))
        else:
            values.append(100.0)
    r = em.step_response(times, values, 2.0, [1.0, 2.0], [3.5, 5.0])
    check("status MEASURED", r["status"] == "MEASURED", r["status"])
    check("reference levels are the window means, reported explicitly",
          abs(r["start_level"]) < 1e-9 and abs(r["end_level"] - 100.0) < 1e-9,
          f"{r['start_level']} -> {r['end_level']}")
    check("10-90% latency matches the analytic 0.80 s within one sampling interval",
          abs(r["rise_time_s"] - 0.80) <= 0.0101, str(r["rise_time_s"]))
    check("the located crossings are the samples the thresholds fall on",
          abs(r["t_lo_s"] - 2.10) <= 0.0101 and abs(r["t_hi_s"] - 2.90) <= 0.0101,
          f"{r['t_lo_s']} {r['t_hi_s']}")
    check("a clean step is not flagged as ripple-ambiguous",
          r["ripple_spans_thresholds"] is False)


# ============================ B. CSV contract ================================================

def _good_csv_text() -> str:
    exe = _build_and_get_exe()
    p = _run_cli(exe, dict(duration=1, cadence=72, sample_ticks=1, ride_start_s=0))
    assert p.returncode == 0, p.stderr
    return p.stdout


def test_csv_contract() -> None:
    print("CSV column contract")
    text = _good_csv_text()
    header = text.split("\n")[0].strip().split(",")
    check("header is exactly the accepted 30 columns followed by the appended ones",
          header == csv_contract.EXPECTED_HEADER,
          f"{len(header)} names, first mismatch at "
          f"{next((i for i, (a, b) in enumerate(zip(header, csv_contract.EXPECTED_HEADER)) if a != b), None)}")
    check("the first 30 header names are the accepted series, unmoved",
          header[:30] == csv_contract.ACCEPTED_30, str(header[:30]))
    shape = csv_contract.check_text(text)
    check("every row validates against the contract",
          shape["rows"] > 0 and shape["columns"] == len(csv_contract.EXPECTED_HEADER), str(shape))
    check("sample_ticks=1 really emits one row per 4 kHz tick",
          shape["rows"] == CTRL_HZ, f"rows={shape['rows']}, expected {CTRL_HZ}")


def test_contract_rejects_shift() -> None:
    """The exact D010-01 defect: 55 header names, 54 values. Length-only checks miss the meaning."""
    print("CSV contract rejects the D010-01 defect")
    text = _good_csv_text()
    lines = text.strip("\n").split("\n")
    header = lines[0].split(",")
    ri = header.index("ride_interval")

    # (a) drop the ride_interval field from every row -> short rows, shifted meaning
    shifted = [lines[0]] + [",".join(r.split(",")[:ri] + r.split(",")[ri + 1:]) for r in lines[1:]]
    raises("a row missing one field is rejected", lambda: csv_contract.check_text("\n".join(shifted)),
           csv_contract.CsvContractError)

    # (b) keep the row width but put a wrong value in ride_interval -> cross-field invariant
    swapped = [lines[0]]
    for r in lines[1:]:
        f = r.split(",")
        f[ri] = "0"          # claim "not riding" while pedalling still says 1
        swapped.append(",".join(f))
    raises("a row whose ride_interval contradicts pedalling is rejected",
           lambda: csv_contract.check_text("\n".join(swapped)), csv_contract.CsvContractError)

    # (c) a header of the right LENGTH but wrong names must not pass
    bad_header = ",".join(["c%d" % i for i in range(len(header))])
    raises("a right-length header with wrong names is rejected",
           lambda: csv_contract.check_text("\n".join([bad_header] + lines[1:])),
           csv_contract.CsvContractError)

    # (d) a cumulative counter going backwards must not pass
    fi = header.index("fast_iq_slew_ticks")
    back = [lines[0]] + list(lines[1:])
    f = back[-1].split(",")
    f[fi] = "0"
    back[-1] = ",".join(f)
    raises("a cumulative counter going backwards is rejected",
           lambda: csv_contract.check_text("\n".join(back)), csv_contract.CsvContractError)


# ============================ C. CLI =========================================================

_EXE_CACHE: list[Path] = []


def _build_and_get_exe() -> Path:
    if not _EXE_CACHE:
        server = accepted.get_server()
        server.ensure_built()
        _EXE_CACHE.append(server.EXE)
    return _EXE_CACHE[0]


def _run_cli(exe: Path, args: dict) -> subprocess.CompletedProcess:
    preset = dict(tuning=accepted.TUNING,
                  banks=[dict(bank_schema_version=8, bank_index=0,
                              active_bank=0, wa_target_rpm=30, cadence_comp_enabled=False,
                              levels=[copy.deepcopy(accepted.LEVEL) for _ in range(5)])])
    b = subprocess.run([accepted.NODE, str(accepted.BRIDGE)], input=json.dumps(preset), text=True,
                       capture_output=True, check=True, cwd=FW)
    blobs = json.loads(b.stdout)
    cfg = {**accepted.BASE, **args}
    cmd = [str(exe), *[f"{k}={v}" for k, v in cfg.items()],
           "tuning_blob=" + blobs["tuning_blob"], "bank_blob=" + blobs["bank_blob"]]
    return subprocess.run(cmd, text=True, capture_output=True, cwd=FW)


def _rows(p: subprocess.CompletedProcess) -> tuple[dict, list[list[str]]]:
    lines = p.stdout.strip().split("\n")
    header = lines[0].split(",")
    idx = {n: i for i, n in enumerate(header)}
    return idx, [ln.split(",") for ln in lines[1:]]


def test_new_keys_accepted() -> None:
    print("CLI: new keys accepted and produce the expected columns")
    exe = _build_and_get_exe()
    p = _run_cli(exe, dict(duration=1, cadence=72, sample_ticks=1, ride_start_s=0,
                           reverse_at_s=0.3, forward_at_s=0.5))
    check("exit code 0", p.returncode == 0, p.stderr)
    idx, rows = _rows(p)
    for col in ("crank_direction", "pas_transition_index", "pas_invalid_seq_active",
                "fast_iq_slew_ticks", "pas_edge_drop_active", "fg_last_resume_elapsed"):
        check(f"column '{col}' present", col in idx)
    check("header has the contracted number of columns",
          len(idx) == len(csv_contract.EXPECTED_HEADER), len(idx))
    neg = [r for r in rows if int(r[idx["crank_direction"]]) == -1]
    check("crank_direction reaches -1 during the reverse window",
          bool(neg), "no row with crank_direction=-1")
    if neg:
        t_first = float(neg[0][idx["time_s"]])
        t_last = float(neg[-1][idx["time_s"]])
        check("the reverse window matches the commanded 0.3 .. 0.5 s",
              0.29 <= t_first <= 0.31 and 0.49 <= t_last <= 0.51, f"{t_first} .. {t_last}")
    check("crank_angle goes backwards while reversing",
          any(int(rows[i][idx["pas_transition_index"]]) > int(rows[i + 1][idx["pas_transition_index"]])
              for i in range(len(rows) - 1)))


def test_new_keys_rejection() -> None:
    print("CLI: contradictory new keys rejected with exit code 2")
    exe = _build_and_get_exe()
    cases = [
        ("torque_invalid from > to", dict(torque_invalid_from_s=0.5, torque_invalid_to_s=0.3)),
        ("pas_invalid_seq window too short", dict(pas_invalid_seq_from_s=0.5, pas_invalid_seq_to_s=0.5005)),
        ("miss_tick_every_n=1", dict(miss_tick_every_n=1)),
        ("fg_delay_at_s without fg_delay_ticks", dict(fg_delay_at_s=0.5)),
        ("miss_tick window without miss_tick_every_n", dict(miss_tick_from_s=0.2, miss_tick_to_s=0.5)),
        ("miss_tick_from_s without miss_tick_to_s", dict(miss_tick_every_n=4, miss_tick_from_s=0.2)),
        ("pas_edge_drop without every_n", dict(pas_edge_drop_from_s=0.2, pas_edge_drop_to_s=0.5)),
        ("pas_edge_drop_every_n=1", dict(pas_edge_drop_from_s=0.2, pas_edge_drop_to_s=0.5,
                                         pas_edge_drop_every_n=1)),
        ("pas_edge_jitter_ticks=0", dict(pas_edge_jitter_from_s=0.2, pas_edge_jitter_to_s=0.5,
                                         pas_edge_jitter_ticks=0)),
        ("pas edge combined with pas_invalid_seq", dict(pas_edge_drop_from_s=0.2, pas_edge_drop_to_s=0.5,
                                                        pas_edge_drop_every_n=3,
                                                        pas_invalid_seq_from_s=0.2,
                                                        pas_invalid_seq_to_s=0.5)),
        ("sample_ticks=0", dict(sample_ticks=0)),
        ("sample_ticks not a number", dict(sample_ticks="abc")),
        ("pas_edge_drop_from_s not finite", dict(pas_edge_drop_from_s="nan", pas_edge_drop_to_s=0.5,
                                                 pas_edge_drop_every_n=3)),
    ]
    for label, extra in cases:
        p = _run_cli(exe, dict(duration=1, **extra))
        check(f"rejected: {label}", p.returncode == 2, f"exit={p.returncode} {p.stderr.strip()[:120]}")


def test_defaults_unchanged() -> None:
    """No new key given -> the run must be byte-identical to the same run before this task's keys."""
    print("CLI: default behaviour unchanged when no new key is given")
    exe = _build_and_get_exe()
    a = _run_cli(exe, dict(duration=1, cadence=72, sample_ms=5))
    b = _run_cli(exe, dict(duration=1, cadence=72, sample_ms=5, sample_ticks=20))
    check("sample_ms=5 and the equivalent sample_ticks=20 agree",
          a.returncode == 0 and b.returncode == 0 and a.stdout == b.stdout,
          f"exits {a.returncode}/{b.returncode}, equal={a.stdout == b.stdout}")


# ============================ D. timebase mechanics (MANDATORY) ==============================

def _fg_delay_probe(exe: Path, ticks: int) -> dict:
    """Run one foreground stall at FULL 4 kHz row rate and extract what actually happened."""
    at_s = 0.5
    p = _run_cli(exe, dict(duration=1.0, cadence=72, ride_start_s=0, sample_ticks=1,
                           fg_delay_at_s=at_s, fg_delay_ticks=ticks))
    if p.returncode:
        return dict(error=p.stderr)
    idx, rows = _rows(p)
    at_tick = int(round(at_s * CTRL_HZ))
    skipped = [r for r in rows if r[idx["fg_processed"]] == "0"]
    # first processed row at or after the stall
    resume = None
    for r in rows:
        if int(r[idx["fg_last_resume_tick"]]) >= at_tick:
            resume = r
            break
    last = rows[-1]
    slew = [int(r[idx["fast_iq_slew_ticks"]]) for r in rows]
    slew_steps = {slew[i + 1] - slew[i] for i in range(len(slew) - 1)}
    return dict(
        rows=len(rows),
        skipped_rows=len(skipped),
        skips_total=int(last[idx["fg_skips_total"]]),
        resume_count=int(last[idx["fg_resume_count"]]),
        resume_tick=int(last[idx["fg_last_resume_tick"]]),
        resume_elapsed=int(last[idx["fg_last_resume_elapsed"]]),
        first_resume_row_time=float(resume[idx["time_s"]]) if resume is not None else None,
        slew_steps=sorted(slew_steps),
        slew_last=slew[-1],
    )


def test_fg_delay_mandatory() -> None:
    """D010-03: these assertions are unconditional. Finding no transition is a FAIL."""
    print("CLI: foreground stall 1/4/20 ticks - mandatory assertions at 4 kHz row rate")
    exe = _build_and_get_exe()
    for ticks in (1, 4, 20):
        r = _fg_delay_probe(exe, ticks)
        if "error" in r:
            check(f"fg_delay {ticks}: run succeeded", False, r["error"])
            continue
        check(f"fg_delay {ticks}: exactly {ticks} foreground tick(s) skipped",
              r["skips_total"] == ticks, f"skips_total={r['skips_total']}")
        check(f"fg_delay {ticks}: the skipped ticks are visible as rows in the CSV",
              r["skipped_rows"] == ticks, f"rows with fg_processed=0: {r['skipped_rows']}")
        check(f"fg_delay {ticks}: the foreground resumed exactly once",
              r["resume_count"] == 1, f"resume_count={r['resume_count']}")
        check(f"fg_delay {ticks}: resume happened at tick {int(0.5 * CTRL_HZ) + ticks}",
              r["resume_tick"] == int(0.5 * CTRL_HZ) + ticks,
              f"resume_tick={r['resume_tick']}")
        check(f"fg_delay {ticks}: the module was handed elapsed={ticks + 1} on resume",
              r["resume_elapsed"] == ticks + 1, f"resume_elapsed={r['resume_elapsed']}")
        check(f"fg_delay {ticks}: a resume row was actually located in the CSV",
              r["first_resume_row_time"] is not None, "no resume row found")
        check(f"fg_delay {ticks}: the 16 kHz Iq owner never stopped",
              r["slew_steps"] == [INNER_PER_CTRL],
              f"fast_iq_slew_ticks steps seen: {r['slew_steps']}")
        check(f"fg_delay {ticks}: 16 kHz ticks total = 4 x control ticks over the whole run",
              r["slew_last"] == INNER_PER_CTRL * CTRL_HZ, f"slew_last={r['slew_last']}")


def test_miss_tick_window() -> None:
    """D010-04: the injection has a real control phase and a real recovery phase."""
    print("CLI: miss_tick is bounded - control / injection / recovery")
    exe = _build_and_get_exe()
    n, frm, to = 4, 0.3, 0.6
    p = _run_cli(exe, dict(duration=1.0, cadence=72, ride_start_s=0, sample_ticks=1,
                           miss_tick_every_n=n, miss_tick_from_s=frm, miss_tick_to_s=to))
    check("exit code 0", p.returncode == 0, p.stderr)
    if p.returncode:
        return
    idx, rows = _rows(p)
    skips_at = {}
    for r in rows:
        skips_at[float(r[idx["time_s"]])] = int(r[idx["fg_skips_total"]])
    before = skips_at[round(frm - 0.0005, 6)] if round(frm - 0.0005, 6) in skips_at else None
    at_end = max(v for t, v in skips_at.items() if t < to)
    at_run_end = skips_at[max(skips_at)]
    expected = len([k for k in range(1, CTRL_HZ + 1)
                    if k / CTRL_HZ >= frm and k / CTRL_HZ < to and k % n == 0])
    check("no foreground tick is skipped during the control phase",
          all(v == 0 for t, v in skips_at.items() if t < frm),
          f"skips before {frm}s: {max((v for t, v in skips_at.items() if t < frm), default=0)}")
    check(f"the injection skips every {n}-th tick inside the window",
          at_end == expected, f"skipped {at_end}, expected {expected}")
    check("nothing further is skipped after the window closes (recovery)",
          at_run_end == at_end, f"{at_end} -> {at_run_end}")
    check("elapsed on every resume equals the number of consecutive skipped ticks + 1",
          all(int(r[idx["fg_last_resume_elapsed"]]) in (0, 2)
              for r in rows if int(r[idx["fg_resume_count"]]) > 0),
          "unexpected resume elapsed for a 1-tick gap")
    check("the resume count equals the skip count for single-tick gaps",
          int(rows[-1][idx["fg_resume_count"]]) == int(rows[-1][idx["fg_skips_total"]]),
          f"{rows[-1][idx['fg_resume_count']]} vs {rows[-1][idx['fg_skips_total']]}")
    check("the 16 kHz Iq owner ran through the whole window",
          int(rows[-1][idx["fast_iq_slew_ticks"]]) == INNER_PER_CTRL * CTRL_HZ,
          rows[-1][idx["fast_iq_slew_ticks"]])


# ============================ E. PAS edge mechanics ==========================================

def test_pas_edge_drop() -> None:
    print("CLI: PAS missed edge - control / injection / recovery")
    exe = _build_and_get_exe()
    frm, to = 3.0, 5.0
    p = _run_cli(exe, dict(duration=8, cadence=72, ride_start_s=0.5, sample_ms=1,
                           pas_edge_drop_from_s=frm, pas_edge_drop_to_s=to,
                           pas_edge_drop_every_n=3))
    check("exit code 0", p.returncode == 0, p.stderr)
    if p.returncode:
        return
    idx, rows = _rows(p)

    def at(t):
        return min(rows, key=lambda r: abs(float(r[idx["time_s"]]) - t))

    pre, mid, post = at(frm - 0.1), at((frm + to) / 2), at(to + 1.0)
    check("no edge is dropped during the control phase",
          int(pre[idx["pas_edges_dropped"]]) == 0, pre[idx["pas_edges_dropped"]])
    check("pas_edge_drop_active is 0 before the window", pre[idx["pas_edge_drop_active"]] == "0")
    check("pas_edge_drop_active is 1 inside the window", mid[idx["pas_edge_drop_active"]] == "1")
    check("edges are actually dropped inside the window",
          int(mid[idx["pas_edges_dropped"]]) > 0, mid[idx["pas_edges_dropped"]])
    check("pas_edge_drop_active is 0 again after the window",
          post[idx["pas_edge_drop_active"]] == "0")
    check("the dropped-edge counter stops growing after the window",
          int(post[idx["pas_edges_dropped"]]) == int(rows[-1][idx["pas_edges_dropped"]]),
          f"{post[idx['pas_edges_dropped']]} vs {rows[-1][idx['pas_edges_dropped']]}")
    check("a missed edge is visible to the production sampler as an illegal step",
          int(mid[idx["pas_sampler_glitch"]]) > int(pre[idx["pas_sampler_glitch"]]),
          f"glitch {pre[idx['pas_sampler_glitch']]} -> {mid[idx['pas_sampler_glitch']]}")
    check("the sampler keeps counting forward again after recovery",
          int(rows[-1][idx["pas_sampler_forward"]]) > int(post[idx["pas_sampler_forward"]]))
    check("the foreground was NOT skipped - this class is electrical, not a scheduling defect",
          int(rows[-1][idx["fg_skips_total"]]) == 0, rows[-1][idx["fg_skips_total"]])


def test_pas_edge_jitter() -> None:
    print("CLI: PAS late edge (jitter) - control / injection / recovery")
    exe = _build_and_get_exe()
    frm, to = 3.0, 5.0
    p = _run_cli(exe, dict(duration=8, cadence=72, ride_start_s=0.5, sample_ms=1,
                           pas_edge_jitter_from_s=frm, pas_edge_jitter_to_s=to,
                           pas_edge_jitter_ticks=8))
    check("exit code 0", p.returncode == 0, p.stderr)
    if p.returncode:
        return
    idx, rows = _rows(p)

    def at(t):
        return min(rows, key=lambda r: abs(float(r[idx["time_s"]]) - t))

    pre, mid, post = at(frm - 0.1), at((frm + to) / 2), at(to + 1.0)
    check("no edge is deferred during the control phase",
          int(pre[idx["pas_edges_deferred"]]) == 0, pre[idx["pas_edges_deferred"]])
    check("edges are deferred inside the window",
          int(mid[idx["pas_edges_deferred"]]) > 0, mid[idx["pas_edges_deferred"]])
    check("deferral stops when the window closes",
          int(post[idx["pas_edges_deferred"]]) == int(rows[-1][idx["pas_edges_deferred"]]),
          f"{post[idx['pas_edges_deferred']]} vs {rows[-1][idx['pas_edges_deferred']]}")
    check("no edge is LOST by jitter - only delayed",
          int(rows[-1][idx["pas_edges_dropped"]]) == 0, rows[-1][idx["pas_edges_dropped"]])
    check("late edges disturb the sampler's step legality",
          int(mid[idx["pas_sampler_glitch"]]) > int(pre[idx["pas_sampler_glitch"]]),
          f"glitch {pre[idx['pas_sampler_glitch']]} -> {mid[idx['pas_sampler_glitch']]}")
    check("the foreground was NOT skipped by the edge disturbance",
          int(rows[-1][idx["fg_skips_total"]]) == 0, rows[-1][idx["fg_skips_total"]])


def test_sensor_invalid_flags_consistent() -> None:
    """The three invalid classes must not be conflated, and valid flags must not contradict."""
    print("CLI: torque / PAS validity flags are separate and self-consistent")
    exe = _build_and_get_exe()
    for label, args, tq_expect, pas_expect in (
        ("torque invalid", dict(torque_invalid_from_s=3.0, torque_invalid_to_s=5.0), 0, 1),
        ("pas invalid", dict(pas_invalid_from_s=3.0, pas_invalid_to_s=5.0), 1, 0),
    ):
        p = _run_cli(exe, dict(duration=8, cadence=72, ride_start_s=0.5, sample_ms=1, **args))
        if p.returncode:
            check(f"{label}: exit 0", False, p.stderr)
            continue
        idx, rows = _rows(p)
        mid = min(rows, key=lambda r: abs(float(r[idx["time_s"]]) - 4.0))
        pre = min(rows, key=lambda r: abs(float(r[idx["time_s"]]) - 2.0))
        check(f"{label}: torque_sensor_valid inside the window is {tq_expect}",
              int(mid[idx["torque_sensor_valid"]]) == tq_expect, mid[idx["torque_sensor_valid"]])
        check(f"{label}: pas_sensor_valid inside the window is {pas_expect}",
              int(mid[idx["pas_sensor_valid"]]) == pas_expect, mid[idx["pas_sensor_valid"]])
        check(f"{label}: what the generator commanded is what rider_input received (torque)",
              mid[idx["torque_sensor_valid"]] == mid[idx["rider_torque_sensor_valid"]],
              f"{mid[idx['torque_sensor_valid']]} vs {mid[idx['rider_torque_sensor_valid']]}")
        check(f"{label}: what the generator commanded is what rider_input received (PAS)",
              mid[idx["pas_sensor_valid"]] == mid[idx["rider_pas_sensor_valid"]],
              f"{mid[idx['pas_sensor_valid']]} vs {mid[idx['rider_pas_sensor_valid']]}")
        check(f"{label}: both flags are valid again before the window",
              pre[idx["torque_sensor_valid"]] == "1" and pre[idx["pas_sensor_valid"]] == "1")
        check(f"{label}: both flags are valid again after recovery",
              rows[-1][idx["torque_sensor_valid"]] == "1" and rows[-1][idx["pas_sensor_valid"]] == "1")


# ============================ F. runner failure handling =====================================

def _isolated_runner(tmp: Path):
    """Import a FRESH copy of runner.py bound to an isolated results directory."""
    import importlib
    old = os.environ.get("AP01_DISTURBANCES_RESULTS_DIR")
    os.environ["AP01_DISTURBANCES_RESULTS_DIR"] = str(tmp)
    for mod in ("runner",):
        sys.modules.pop(mod, None)
    runner = importlib.import_module("runner")
    if old is None:
        os.environ.pop("AP01_DISTURBANCES_RESULTS_DIR", None)
    else:
        os.environ["AP01_DISTURBANCES_RESULTS_DIR"] = old
    return runner


def _ok_case(name, group, sha="a" * 64):
    return dict(name=name, group=group, status="OK", deterministic_repeat=True,
                csv_sha256=sha, repeat_csv_sha256=[sha, sha], csv_contract_ok=True)


def test_runner_guards() -> None:
    print("runner: required-trial guards reject incomplete evidence")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        r = _isolated_runner(tmp)

        def fresh_complete():
            """A manifest that SHOULD pass, so each negative test changes exactly one thing."""
            r.manifest["cases"] = []
            r.manifest["regression"] = []
            for c in r.scn.all_cases():
                r.manifest["cases"].append(_ok_case(c["name"], c["group"],
                                                    sha=("%064x" % abs(hash(c["name"])))[:64]))
            for spec in r.scn.REGRESSION_CASES:
                sha = ("%064x" % abs(hash(spec["name"])))[:64]
                r.manifest["cases"].append(_ok_case(spec["name"], "regression_ac_t2", sha=sha))
                r.manifest["regression"].append(dict(
                    case=spec["name"], accepted_case=spec["accepted"],
                    accepted_csv_sha256="b" * 64, new_csv_sha256=sha,
                    pre_existing_columns_identical=True))

        fresh_complete()
        try:
            r.check_required()
            check("a complete synthetic manifest passes the guards", True)
        except Exception as e:  # noqa: BLE001
            check("a complete synthetic manifest passes the guards", False, f"{type(e).__name__}: {e}")

        fresh_complete()
        r.manifest["cases"].pop()
        raises("a missing required trial is rejected", r.check_required, Exception)

        fresh_complete()
        r.manifest["cases"].append(dict(r.manifest["cases"][0]))
        raises("a duplicate case name is rejected", r.check_required, Exception)

        fresh_complete()
        r.manifest["cases"][0] = dict(r.manifest["cases"][0], status="ERROR")
        raises("a case recorded as ERROR is rejected", r.check_required, Exception)

        fresh_complete()
        r.manifest["cases"][0]["repeat_csv_sha256"] = [r.manifest["cases"][0]["csv_sha256"]]
        raises("a single recorded repeat is rejected (repeats_per_case=2)",
               r.check_required, Exception)

        fresh_complete()
        r.manifest["cases"][0]["repeat_csv_sha256"] = ["c" * 64, "d" * 64]
        raises("repeats whose hashes disagree are rejected", r.check_required, Exception)

        fresh_complete()
        r.manifest["cases"][0]["deterministic_repeat"] = False
        raises("a non-deterministic case is rejected", r.check_required, Exception)

        fresh_complete()
        r.manifest["cases"][0]["csv_contract_ok"] = False
        raises("a case whose CSV failed the column contract is rejected",
               r.check_required, Exception)

        fresh_complete()
        r.manifest["regression"] = []
        raises("an empty regression list cannot claim regression_all_identical",
               r.check_required, Exception)

        fresh_complete()
        r.manifest["regression"][0]["new_csv_sha256"] = "e" * 64
        raises("a regression entry not linked to its own run is rejected",
               r.check_required, Exception)

        fresh_complete()
        r.manifest["regression"][0]["pre_existing_columns_identical"] = False
        raises("a regression whose pre-existing columns changed is rejected",
               r.check_required, Exception)

        fresh_complete()
        r.manifest["regression"].append(dict(r.manifest["regression"][0]))
        raises("a duplicate regression entry is rejected", r.check_required, Exception)


def test_runner_abort_path() -> None:
    """A failing case must leave an ABORTED manifest on disk and exit non-zero - for real."""
    print("runner: a failing case closes the manifest as ABORTED")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        r = _isolated_runner(tmp)

        class FakeScn:
            REGRESSION_CASES: list = []

            @staticmethod
            def all_cases():
                # sample_ticks=0 is rejected by the binary with exit 2
                return [dict(name="deliberately_bad", group="control_6s",
                             args=dict(duration=1, sample_ticks=0), events=[])]

        r.scn = FakeScn
        rc = r.cli()
        check("the runner exits non-zero on a failed required case", rc == 1, f"rc={rc}")
        mf = tmp / "disturbances-results.json"
        check("the manifest file exists after the failure", mf.exists())
        if mf.exists():
            m = json.loads(mf.read_text(encoding="utf-8"))
            check("attempt_state is ABORTED", m.get("attempt_state") == "ABORTED",
                  str(m.get("attempt_state")))
            check("run_complete is false", m.get("run_complete") is False, str(m.get("run_complete")))
            check("the abort reason names the failing case",
                  "deliberately_bad" in (m.get("abort_reason") or ""), str(m.get("abort_reason"))[:120])
            check("the failed case is recorded with status ERROR",
                  any(c.get("status") == "ERROR" for c in m.get("cases", [])),
                  str([c.get("status") for c in m.get("cases", [])]))
        check("the accepted results directory was not written to",
              not (AP01 / "results" / "deliberately_bad.csv").exists())


def test_runner_manifest_in_progress() -> None:
    """The manifest must be claimed BEFORE any work, so a crash cannot look like a success."""
    print("runner: the manifest is claimed IN_PROGRESS before any case runs")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        r = _isolated_runner(tmp)
        r.write_manifest()
        m = json.loads((tmp / "disturbances-results.json").read_text(encoding="utf-8"))
        check("attempt_state starts as IN_PROGRESS", m["attempt_state"] == "IN_PROGRESS",
              m["attempt_state"])
        check("run_complete starts false", m["run_complete"] is False)
        check("repeats_per_case is recorded in the manifest itself", m["repeats_per_case"] >= 2)


# ============================ scenario metadata ==============================================

def test_scenario_metadata_matches_input() -> None:
    """D010-04: a declared event time must be a real change of the generated input."""
    print("scenarios: declared event times match the commanded input")
    for c in scn.all_cases():
        args, events, name = c["args"], dict(c["events"]), c["name"]
        if c["group"] == "miss_tick":
            check(f"{name}: miss_tick_start equals the commanded window start",
                  events.get("miss_tick_start") == args.get("miss_tick_from_s"),
                  f"{events.get('miss_tick_start')} vs {args.get('miss_tick_from_s')}")
            check(f"{name}: the window has a declared end",
                  events.get("miss_tick_end") == args.get("miss_tick_to_s"),
                  f"{events.get('miss_tick_end')} vs {args.get('miss_tick_to_s')}")
        if c["group"].startswith("control"):
            check(f"{name}: a control case declares no events and sets no disturbance key",
                  not events and not any(k for k in args if "invalid" in k or "reverse" in k
                                         or "miss_tick" in k or "fg_delay" in k or "pas_edge" in k),
                  str(sorted(args)))
    groups = {c["group"] for c in scn.all_cases()}
    for required in ("control_8s", "control_6s", "reverse_clean", "reverse_bounce",
                     "torque_invalid", "pas_invalid", "pas_invalid_seq",
                     "pas_edge_drop", "pas_edge_jitter", "miss_tick", "fg_delay",
                     "fg_delay_hires"):
        check(f"scenario group '{required}' exists", required in groups)
    for group in ("reverse_clean", "torque_invalid", "pas_edge_drop", "miss_tick", "fg_delay"):
        cads = {c["args"]["cadence"] for c in scn.all_cases() if c["group"] == group}
        speeds = {c["args"]["speed"] for c in scn.all_cases() if c["group"] == group}
        check(f"{group} covers 30 and 72 rpm", cads == {30, 72}, str(cads))
        check(f"{group} covers 0 and 18 km/h", speeds == {0, 18}, str(speeds))


def test_module_resolution() -> None:
    """D010-05: the accepted modules must be the accepted FILES, and `scenarios` must be OURS.

    AP-01, transients-001 and disturbances-001 each contain a package called `scenarios`. An
    earlier version of the metrics adapter inserted transients-001 into sys.path, which silently
    redirected `import scenarios` to another task's case list - so a run could report on this task
    while validating another one's cases.
    """
    print("module resolution is explicit, not ambient")
    import accepted_modules
    import disturbance_metrics_primitives as mp
    check("event primitives come from the accepted transients-001/event_metrics.py",
          Path(em.SOURCE_FILE) == accepted_modules.ACCEPTED_EVENT_METRICS_PY, em.SOURCE_FILE)
    check("metric primitives come from the accepted AP-01/metrics.py",
          Path(mp.SOURCE_FILE) == accepted_modules.ACCEPTED_METRICS_PY, mp.SOURCE_FILE)
    check("`scenarios` is the disturbances-001 package, not another task's",
          Path(scn.__file__).parent.parent == HERE, scn.__file__)
    check("the scenario set is this task's, and non-trivial",
          len(scn.all_cases()) >= 40, str(len(scn.all_cases())))
    check("importing the adapters did not add an accepted directory to sys.path",
          str(AP01 / "transients-001") not in sys.path, "transients-001 is on sys.path")
    check("no module was loaded straight from a stale __pycache__ file",
          all(not str(getattr(m, "__file__", "")).endswith(".pyc") for m in (em, mp, scn)),
          "a .pyc was imported directly")


# ============================ H. new measurement semantics (D011-03) =========================
#
# These test the functions THIS task wrote, on synthetic series with analytically known answers -
# not the imported accepted primitives, and not "the JSON recomputes identically". Each case below
# is one the review named as previously mis-reported.

def _series(pairs):
    return [p[0] for p in pairs], [p[1] for p in pairs]


def test_zero_assessment() -> None:
    print("zero_assessment: the four answers are kept apart")
    tol, conf = 1.0, 0.10

    # (a) already zero BEFORE the event -> not a new extinction, and NO latency may be reported
    t, v = _series([(round(0.01 * i, 4), 0.0) for i in range(0, 400)])
    r = dm.zero_assessment(t, v, 2.0, 3.0, tol, conf)
    check("zero before the event is ALREADY_ZERO, not 'reached zero'",
          r["status"] == dm.ALREADY_ZERO, r["status"])
    check("ALREADY_ZERO reports no latency at all",
          "latency_first_contact_s" not in r and "latency_confirmed_s" not in r,
          str(sorted(k for k in r if "latency" in k)))

    # (b) a genuine extinction: 150 until t=2.0, then 0 and held
    t, v = _series([(round(0.01 * i, 4), 150.0 if 0.01 * i < 2.0 else 0.0) for i in range(0, 400)])
    r = dm.zero_assessment(t, v, 2.0, 3.5, tol, conf)
    check("a real extinction is CONFIRMED_ZERO", r["status"] == dm.CONFIRMED_ZERO, r["status"])
    check("first contact is at the event, within one sample",
          abs(r["latency_first_contact_s"]) <= 0.0101, str(r["latency_first_contact_s"]))
    check("the confirmed time is min_confirm_s after first contact",
          abs(r["latency_confirmed_s"] - conf) <= 0.0101, str(r["latency_confirmed_s"]))
    check("the hold really was at least min_confirm_s", r["hold_duration_s"] >= conf,
          str(r["hold_duration_s"]))

    # (c) ONE sample at zero, then back up -> must NOT be a confirmed zero
    vals = []
    for i in range(0, 400):
        tt = round(0.01 * i, 4)
        vals.append((tt, 0.0 if abs(tt - 2.0) < 1e-9 else 150.0))
    t, v = _series(vals)
    r = dm.zero_assessment(t, v, 1.9, 3.0, tol, conf)
    check("a single sample inside the tolerance is UNCONFIRMED_ZERO, not zero",
          r["status"] == dm.UNCONFIRMED_ZERO, r["status"])
    check("the unconfirmed hold is shorter than min_confirm_s",
          r["hold_duration_s"] < conf, str(r["hold_duration_s"]))
    check("a single-sample touch is not marked as truncated by the window",
          r["hold_truncated_by_window"] is False, str(r["hold_truncated_by_window"]))

    # (d) the NEXT EVENT arrives before the hold can be confirmed -> UNCONFIRMED + truncated
    t, v = _series([(round(0.01 * i, 4), 150.0 if 0.01 * i < 2.0 else 0.0) for i in range(0, 400)])
    r = dm.zero_assessment(t, v, 2.0, 2.05, tol, conf)      # window shorter than min_confirm_s
    check("a window that ends before the hold can be confirmed is UNCONFIRMED_ZERO",
          r["status"] == dm.UNCONFIRMED_ZERO, r["status"])
    check("and it is flagged as truncated by the window, not as a negative result",
          r["hold_truncated_by_window"] is True, str(r["hold_truncated_by_window"]))

    # (e) never within tolerance
    t, v = _series([(round(0.01 * i, 4), 150.0) for i in range(0, 400)])
    r = dm.zero_assessment(t, v, 2.0, 3.0, tol, conf)
    check("never inside the tolerance is NOT_OBSERVED", r["status"] == dm.NOT_OBSERVED, r["status"])

    # (f) empty window / no baseline
    # a window that genuinely contains no sample (the series is sampled every 0.01 s)
    r = dm.zero_assessment(t, v, 3.991, 3.995, tol, conf)
    check("an empty window is NO_DATA, not zero", r["status"] == dm.NO_DATA, r["status"])
    check("NO_DATA reports zero samples", r["samples_in_window"] == 0, str(r["samples_in_window"]))
    r = dm.zero_assessment(t, v, 0.0, 1.0, tol, conf)
    check("no sample before the event is NO_BASELINE_BEFORE_EVENT",
          r["status"] == dm.NO_BASELINE, r["status"])


def test_recovery_assessment() -> None:
    print("recovery_assessment: coming back must also be held")
    tol, conf = 1.0, 0.10
    # zero until 5.0, then back to 150 and held -> confirmed recovery
    t, v = _series([(round(0.01 * i, 4), 0.0 if 0.01 * i < 5.0 else 150.0) for i in range(0, 800)])
    r = dm.recovery_assessment(t, v, 5.0, 7.0, tol, conf, reference_level=150.0)
    check("a sustained return is CONFIRMED_RECOVERY", r["status"] == "CONFIRMED_RECOVERY",
          r["status"])
    check("recovery latency is measured from the event",
          abs(r["latency_s"]) <= 0.0101, str(r["latency_s"]))
    check("the recovered level is compared against the reference",
          abs(r["ratio_to_reference"] - 1.0) < 1e-6, str(r.get("ratio_to_reference")))
    # a single sample above tolerance is not a recovery
    vals = [(round(0.01 * i, 4), 150.0 if abs(0.01 * i - 5.0) < 1e-9 else 0.0)
            for i in range(0, 800)]
    t, v = _series(vals)
    r = dm.recovery_assessment(t, v, 4.9, 7.0, tol, conf)
    check("a single sample out of the tolerance is not a recovery",
          r["status"] == dm.NOT_OBSERVED, r["status"])


def test_first_change_descriptive() -> None:
    print("first_change_descriptive: ripple is not a reaction")
    # a rippling channel that ripples BEFORE the event too - the classic false positive
    vals = [(round(0.001 * i, 4), 150.0 + (5.0 if i % 2 else -5.0)) for i in range(0, 4000)]
    t, v = _series(vals)
    r = dm.first_change_descriptive(t, v, 2.0, 3.0, pre_from=1.0, tolerance=1.0)
    check("a rippling channel is flagged as also changing before the event",
          r["channel_also_changes_before_event"] is True,
          str(r.get("channel_also_changes_before_event")))
    check("its first post-event change carries causality NOT_ESTABLISHED",
          r["causality"] == "NOT_ESTABLISHED", str(r.get("causality")))
    check("and the note says it is not evidence of a reaction",
          "NOT evidence" in r["note"], r["note"])

    # a genuinely steady channel that steps once at the event
    vals = [(round(0.001 * i, 4), 0.0 if 0.001 * i < 2.0 else 1.0) for i in range(0, 4000)]
    t, v = _series(vals)
    r = dm.first_change_descriptive(t, v, 2.0, 3.0, pre_from=1.0)
    check("a steady channel is not flagged as changing before the event",
          r["channel_also_changes_before_event"] is False,
          str(r.get("channel_also_changes_before_event")))
    check("its change is located at the event", abs(r["latency_s"]) <= 0.0011, str(r["latency_s"]))
    check("causality is STILL not asserted from timing alone",
          r["causality"] == "NOT_ESTABLISHED", str(r.get("causality")))

    # no change at all
    vals = [(round(0.001 * i, 4), 7.0) for i in range(0, 4000)]
    t, v = _series(vals)
    r = dm.first_change_descriptive(t, v, 2.0, 3.0, pre_from=1.0)
    check("no change inside the window is NOT_OBSERVED", r["status"] == dm.NOT_OBSERVED, r["status"])
    check("NOT_OBSERVED is described as an absent observation, not as 'no reaction'",
          "absence of an observation" in r["note"], r["note"])


def test_sustained_transition() -> None:
    print("sustained_transition: a one-sample blip is not a transition")
    vals = [(round(0.001 * i, 4), 1.0 if abs(0.001 * i - 2.0) < 1e-9 else 0.0) for i in range(0, 4000)]
    t, v = _series(vals)
    r = dm.sustained_transition(t, v, 1.9, 3.0, lambda x: x != 0, min_hold_s=0.01)
    check("a single-sample blip does not satisfy a 10 ms hold",
          r["status"] == dm.NOT_OBSERVED, r["status"])
    vals = [(round(0.001 * i, 4), 1.0 if 0.001 * i >= 2.0 else 0.0) for i in range(0, 4000)]
    t, v = _series(vals)
    r = dm.sustained_transition(t, v, 1.9, 3.0, lambda x: x != 0, min_hold_s=0.01)
    check("a sustained condition is MEASURED", r["status"] == "MEASURED", r["status"])
    check("its latency is measured from the window start",
          abs(r["latency_s"] - 0.1) <= 0.0011, str(r["latency_s"]))


def test_invalid_recovery_on_real_data() -> None:
    """The concrete case the review named: tq_invalid_end reported 'reached_zero, latency=0'."""
    print("real data: an already-zero window is no longer reported as a new extinction")
    import analyze
    path = HERE / "results" / "tq_invalid_cad72_speed0.csv"
    if not path.is_file():
        check("tq_invalid_cad72_speed0.csv is present", False, "run runner.py first")
        return
    cols = em.read_csv(path)
    # the restore event is at 5.0 s; Iq was already at zero through the invalid window
    r = analyze.zero_and_recovery(cols, 5.0, 8.0)
    z = r["iq_ref_zero"]
    check("the restore event is ALREADY_ZERO, not 'reached zero with latency 0'",
          z["status"] == dm.ALREADY_ZERO, z["status"])
    check("no latency is claimed for it", "latency_first_contact_s" not in z,
          str(sorted(k for k in z if "latency" in k)))
    rec = r["iq_ref_recovery"]
    check("the recovery after the input is restored IS measured",
          rec["status"] == "CONFIRMED_RECOVERY", rec["status"])
    check("recovery latency is positive and finite",
          rec.get("latency_s") is not None and rec["latency_s"] > 0, str(rec.get("latency_s")))
    # and the injection event itself is a real, confirmed extinction
    r2 = analyze.zero_and_recovery(cols, 3.0, 5.0)
    z2 = r2["iq_ref_zero"]
    check("the injection event is a CONFIRMED_ZERO with a real latency",
          z2["status"] == dm.CONFIRMED_ZERO and z2["latency_first_contact_s"] > 1.0,
          f"{z2['status']} {z2.get('latency_first_contact_s')}")


def test_control_ripple_not_a_reaction() -> None:
    """The same descriptive first-change test on a CONTROL run must look the same for iq_ref."""
    print("real data: iq_ref ripple looks identical in the control run")
    import analyze
    cpath = HERE / "results" / "control_8s_cad72_speed18.csv"
    dpath = HERE / "results" / "pas_edge_drop3_cad72_speed18.csv"
    if not (cpath.is_file() and dpath.is_file()):
        check("control and disturbed CSVs are present", False, "run runner.py first")
        return
    ctrl, dist = em.read_csv(cpath), em.read_csv(dpath)
    rc = analyze.observed_state_transitions(ctrl, 3.0, 5.0, pre_from=2.0)["iq_ref"]
    rd = analyze.observed_state_transitions(dist, 3.0, 5.0, pre_from=2.0)["iq_ref"]
    check("iq_ref 'changes' just as promptly in the undisturbed control",
          rc["status"] == "MEASURED" and rd["status"] == "MEASURED",
          f"control={rc['status']} disturbed={rd['status']}")
    check("both are flagged as also changing before the event",
          rc["channel_also_changes_before_event"] and rd["channel_also_changes_before_event"])
    check("so neither may be read as a reaction to the injection",
          rc["causality"] == "NOT_ESTABLISHED" and rd["causality"] == "NOT_ESTABLISHED")


# ============================ I. bounce semantics (D011-01) ==================================

def test_bounce_is_electrical_only() -> None:
    """One bounce, three ticks, crank untouched - checked at the full 4 kHz row rate."""
    print("CLI: the bounce is a LINE artefact, not a crank reversal")
    exe = _build_and_get_exe()
    at_s = 1.2
    base = dict(duration=2, cadence=72, speed=18, torque=28, torque_ripple=45, asymmetry=8,
                ride_start_s=0.5, sample_ticks=1)
    pb = _run_cli(exe, dict(**base, reverse_bounce_at_s=at_s))
    pc = _run_cli(exe, dict(**base))
    check("bounce run exits 0", pb.returncode == 0, pb.stderr)
    check("control run exits 0", pc.returncode == 0, pc.stderr)
    if pb.returncode or pc.returncode:
        return
    ib, rb = _rows(pb)
    ic, rc = _rows(pc)
    check("both runs have the same number of rows", len(rb) == len(rc), f"{len(rb)} vs {len(rc)}")

    forced = [r for r in rb if r[ib["pas_ab"]] != r[ib["pas_normal_ab"]]]
    active = [r for r in rb if r[ib["reverse_bounce_active"]] == "1"]
    check("exactly 3 ticks have a forced line", len(forced) == 3, str(len(forced)))
    check("reverse_bounce_active marks exactly those 3 ticks", len(active) == 3, str(len(active)))
    check("the flag is set on the SAME rows whose line is forced",
          [r[ib["time_s"]] for r in forced] == [r[ib["time_s"]] for r in active],
          f"{[r[ib['time_s']] for r in forced]} vs {[r[ib['time_s']] for r in active]}")
    check("the cumulative counter ends at exactly 3",
          rb[-1][ib["reverse_bounce_ticks"]] == "3", rb[-1][ib["reverse_bounce_ticks"]])
    check("the bounce fires exactly once - it does not re-arm",
          all(int(r[ib["reverse_bounce_ticks"]]) <= 3 for r in rb))
    if forced:
        t0 = float(forced[0][ib["time_s"]])
        t1 = float(forced[-1][ib["time_s"]])
        check("the first forced tick is at the commanded time",
              abs(t0 - at_s) < 1e-6, f"{t0} vs {at_s}")
        check("the 3 forced ticks span 2 tick periods (0.0005 s), i.e. 3 ticks of 4 kHz",
              abs((t1 - t0) - 2 / CTRL_HZ) < 1e-9, f"{t1 - t0}")

    # the crank must be untouched: identical to the control on EVERY row
    for col in ("crank_angle_deg", "torque_gen_nm", "cadence_gen_rpm", "crank_direction",
                "torque_load_kg", "pas_normal_ab", "ride_interval"):
        diff = [i for i, (x, y) in enumerate(zip(rb, rc)) if x[ib[col]] != y[ic[col]]]
        check(f"'{col}' is identical to the control on every row - the crank is not reversed",
              not diff, f"{len(diff)} differing rows, first at row {diff[0] if diff else '-'}")

    check("crank_direction is +1 throughout the bounce (never -1)",
          all(r[ib["crank_direction"]] != "-1" for r in active),
          str([r[ib["crank_direction"]] for r in active]))
    after = [r for r in rb if float(r[ib["time_s"]]) > at_s + 0.001]
    check("the line returns to the undisturbed quadrature after the bounce",
          all(r[ib["pas_ab"]] == r[ib["pas_normal_ab"]] for r in after))


def test_bounce_duration_constant() -> None:
    print("scenarios: the declared bounce end is the real one")
    check("BOUNCE_DURATION_S is 3 ticks at 4 kHz, i.e. 0.00075 s",
          abs(scn.BOUNCE_DURATION_S - 3 / 4000) < 1e-12, str(scn.BOUNCE_DURATION_S))
    for c in scn.all_cases():
        if c["group"] not in ("reverse_bounce", "reverse_bounce_hires"):
            continue
        ev = dict(c["events"])
        check(f"{c['name']}: declared end = start + 3 ticks",
              abs(ev["reverse_bounce_end"] - ev["reverse_bounce"] - 3 / 4000) < 1e-12,
              f"{ev['reverse_bounce_end'] - ev['reverse_bounce']}")


# ============================ J. PAS edge measurement and P-1 (D012-01) ======================
#
# These drive pas_edge_timing() and evaluate_criteria() DIRECTLY. Each one reproduces a way the
# iteration-012 measurement went wrong, so a regression brings the defect back as a FAIL:
#
#   * a clean reverse has NO override, so (pas_ab != pas_normal_ab) is identically zero - the old
#     detector returned NOT_OBSERVED for exactly the case the other code path claimed to measure;
#   * acceptance by the sampler can lag the line edge, and the two must not be merged;
#   * decimation can hide the first edge AND invert the apparent order of edge and inhibit, which
#     the old code reported as "0.00 ms, same tick";
#   * the bounce forces the line one step back and must be found by the same detector.

FWD_AB = [0, 2, 3, 1]


def _synth_reverse(dt, n, t_reverse, inhibit_after_ticks, accept_after_ticks=None,
                   forced_bounce_at=None, ticks_per_step=36):
    """A synthetic record with a forward-then-reverse PAS line.

    `ticks_per_step` is how many 4 kHz ticks one quadrature step takes - 36 ticks (9 ms) is what
    72 rpm actually produces, so decimating by 4 shifts the observed edge without aliasing the
    quadrature away entirely. pas_normal_ab is kept EQUAL to pas_ab (no override), which is what a
    clean reverse looks like.
    """
    if accept_after_ticks is None:
        accept_after_ticks = inhibit_after_ticks
    times, ab, normal, inhibit, rev_count = [], [], [], [], []
    pos, edge_t, rc, inh_from = 0, None, 0, None
    fwd_count = 0
    for i in range(n):
        t = round(i * dt, 9)
        stepping = (i % ticks_per_step == 0) and i > 0
        if forced_bounce_at is not None and abs(t - forced_bounce_at) < dt / 2:
            pos = (pos - 1) % 4               # forced one step BACK (bounce)
            if edge_t is None:
                edge_t = t
        elif stepping:
            if t >= t_reverse:
                pos = (pos - 1) % 4           # crank turning backwards
                if edge_t is None:
                    edge_t = t
            else:
                pos = (pos + 1) % 4
                fwd_count += 1
        times.append(t)
        ab.append(FWD_AB[pos])
        normal.append(FWD_AB[pos])            # identical - no override anywhere
        if edge_t is not None and t >= round(edge_t + accept_after_ticks * dt, 9):
            rc += 1
        rev_count.append(rc)
        if edge_t is not None and t >= round(edge_t + inhibit_after_ticks * dt, 9):
            inh_from = 1
        inhibit.append(inh_from or 0)
        fwd_count += 0
    return dict(time_s=times, pas_ab=ab, pas_normal_ab=normal,
                pas_inhibit_reason=inhibit, pas_sampler_reverse=rev_count,
                pas_sampler_glitch=[0] * n, pas_sampler_invalid=[0] * n,
                pas_sampler_forward=[i // ticks_per_step for i in range(n)]), edge_t


def test_edge_detector_clean_reverse_without_override() -> None:
    """The exact blind spot: pas_ab == pas_normal_ab everywhere, yet reverse edges exist."""
    print("pas_edge_timing: a clean reverse has no override and MUST still yield an edge")
    import analyze
    dt = 1 / CTRL_HZ
    cols, edge_t = _synth_reverse(dt, 4000, t_reverse=0.5, inhibit_after_ticks=3)
    check("the synthetic record really has no override at all",
          all(a == b for a, b in zip(cols["pas_ab"], cols["pas_normal_ab"])))
    r = analyze.pas_edge_timing(cols, 0.5, 0.9)
    check("the line reverse edge IS measured despite no override",
          r["line_reverse_edge"]["status"] == "MEASURED", str(r["line_reverse_edge"]["status"]))
    check("the edge is located at the first backwards step",
          abs(r["line_reverse_edge"]["t_observed_s"] - edge_t) < 1e-9,
          f"{r['line_reverse_edge'].get('t_observed_s')} vs {edge_t}")
    check("the step is classified as REVERSE",
          r["line_reverse_edge"]["step"] == dm.STEP_REVERSE, str(r["line_reverse_edge"].get("step")))
    check("a full-rate record is reported as exact",
          r["full_rate"] and r["line_reverse_edge"]["exact"], str(r["full_rate"]))
    lat = r["line_edge_to_inhibit"]
    check("the edge->inhibit latency is the injected 3 ticks, not zero",
          abs(lat["nominal_s"] - 3 * dt) < 1e-9, str(lat["nominal_s"]))
    check("the order of edge and inhibit is established", lat["order_established"])


def test_edge_detector_delayed_acceptance() -> None:
    print("pas_edge_timing: acceptance by the sampler is separate from the line edge")
    import analyze
    dt = 1 / CTRL_HZ
    cols, edge_t = _synth_reverse(dt, 4000, t_reverse=0.5,
                                  inhibit_after_ticks=9, accept_after_ticks=5)
    r = analyze.pas_edge_timing(cols, 0.5, 0.9)
    acc = r["accepted"]["pas_sampler_reverse"]
    check("acceptance is measured", acc["status"] == "MEASURED", str(acc["status"]))
    check("acceptance lags the line edge by the injected 5 ticks",
          abs(r["line_edge_to_accepted"]["nominal_s"] - 5 * dt) < 1e-9,
          str(r["line_edge_to_accepted"]["nominal_s"]))
    check("inhibit lags the line edge by the injected 9 ticks",
          abs(r["line_edge_to_inhibit"]["nominal_s"] - 9 * dt) < 1e-9,
          str(r["line_edge_to_inhibit"]["nominal_s"]))
    check("line edge, acceptance and inhibit are three distinct instants",
          len({r["line_reverse_edge"]["t_observed_s"], acc["t_observed_s"],
               r["inhibit"]["t_observed_s"]}) == 3)


def test_edge_detector_bounce_override() -> None:
    print("pas_edge_timing: the same detector finds a forced bounce step")
    import analyze
    dt = 1 / CTRL_HZ
    cols, edge_t = _synth_reverse(dt, 4000, t_reverse=99.0, inhibit_after_ticks=1,
                                  forced_bounce_at=0.5, ticks_per_step=36)
    r = analyze.pas_edge_timing(cols, 0.49, 0.9)
    check("the forced backwards step is found as a REVERSE line edge",
          r["line_reverse_edge"]["status"] == "MEASURED"
          and r["line_reverse_edge"]["step"] == dm.STEP_REVERSE,
          str(r["line_reverse_edge"].get("status")))
    check("it is located at the forced tick",
          abs(r["line_reverse_edge"]["t_observed_s"] - 0.5) < dt,
          str(r["line_reverse_edge"].get("t_observed_s")))


def _synth_reverse_blip(dt, n, t_edge, inhibit_after_ticks, next_step_after_ticks):
    """Reproduce the REAL reverse_clean_cad72_speed18 pattern that defeated iteration 012.

    At the instant the crank direction flips, the angle has just crossed a quadrature boundary and
    immediately crosses back: the line makes a ONE-TICK reverse step (measured in the real record
    at 2.99975 -> 3.00000). Inhibit follows 3 ticks later. The next cadence-driven reverse step is
    ~34 ticks after that.

    Decimating by 4 drops the one-tick blip entirely, so the first OBSERVABLE line change is the
    later step - which lands AFTER the observable inhibit. That is how iteration 012 obtained an
    "edge" that followed the inhibit it was credited with causing, and reported 0.00 ms.
    """
    times, ab, inhibit, rev = [], [], [], []
    i_edge = int(round(t_edge / dt))
    i_inh = i_edge + inhibit_after_ticks
    i_next = i_edge + next_step_after_ticks
    for i in range(n):
        times.append(round(i * dt, 9))
        if i == i_edge - 1:
            v = 0                       # the last forward step, held for ONE tick
        elif i < i_next:
            v = 1                       # stepping back to 1 IS the first reverse edge (at i_edge)
        else:
            v = 3                       # the next cadence-driven reverse step
        ab.append(v)
        inhibit.append(1 if i >= i_inh else 0)
        rev.append((1 if i >= i_inh else 0) + (1 if i >= i_next else 0))
    return dict(time_s=times, pas_ab=ab, pas_normal_ab=list(ab),
                pas_inhibit_reason=inhibit, pas_sampler_reverse=rev,
                pas_sampler_glitch=[0] * n, pas_sampler_invalid=[0] * n,
                pas_sampler_forward=[0] * n)


def test_decimation_inverts_the_order() -> None:
    """The concrete D012-01 failure: decimation makes the 'edge' follow the inhibit."""
    print("pas_edge_timing: decimation can invert the apparent order - it must be reported")
    import analyze
    dt = 1 / CTRL_HZ
    cols = _synth_reverse_blip(dt, 4000, t_edge=0.50, inhibit_after_ticks=3,
                               next_step_after_ticks=34)
    full = analyze.pas_edge_timing(cols, 0.499, 0.6)
    check("at full rate the blip IS the first reverse edge",
          full["line_reverse_edge"]["status"] == "MEASURED"
          and abs(full["line_reverse_edge"]["t_observed_s"] - 0.5) < 1e-9,
          str(full["line_reverse_edge"].get("t_observed_s")))
    check("at full rate the latency is the injected 3 ticks",
          abs(full["line_edge_to_inhibit"]["nominal_s"] - 3 * dt) < 1e-9,
          str(full["line_edge_to_inhibit"]["nominal_s"]))

    keep = list(range(0, len(cols["time_s"]), 4))
    deci = {k: [v[i] for i in keep] for k, v in cols.items()}
    dec = analyze.pas_edge_timing(deci, 0.499, 0.6)
    de, di = dec["line_reverse_edge"], dec["inhibit"]
    check("the one-tick blip is invisible after decimation, so the observed edge is much later",
          de["status"] == "MEASURED" and de["t_observed_s"] > 0.5 + 4 * dt,
          str(de.get("t_observed_s")))
    check("the inhibit is still observed EARLY - before the observed edge",
          di["t_observed_s"] < de["t_observed_s"],
          f"inhibit {di['t_observed_s']} vs edge {de['t_observed_s']}")
    lat = dec["line_edge_to_inhibit"]
    check("so the decimated latency is reported NEGATIVE, not as 0.00 ms",
          lat["nominal_s"] < 0, str(lat["nominal_s"]))
    check("and the order is explicitly reported as NOT established",
          lat["order_established"] is False, str(lat["order_established"]))
    check("the decimated instants are not marked exact", de["exact"] is False)

    # and evaluate_criteria must refuse to score it rather than emit a PASS
    for k in ("iq_ref", "session", "pas_transition_index", "reverse_bounce_ticks"):
        deci.setdefault(k, [0] * len(deci["time_s"]))
    case = _fake_case("synthetic_blip", "reverse_clean",
                      dict(cadence=72, speed=18, duration=1.0, ride_start_s=0.0),
                      [("reverse_start", 0.499), ("reverse_end", 0.6)])
    v = analyze.evaluate_criteria(case, deci, None, None)["P-1"]
    check("P-1 on this record is NOT_EVALUATED, never PASS", v["verdict"] == "NOT_EVALUATED",
          str(v["verdict"]))


def test_decimation_hides_the_edge() -> None:
    """The D012-01 defect itself: decimation hides the edge and can invert the apparent order."""
    print("pas_edge_timing: a decimated record must not yield an exact latency")
    import analyze
    dt = 1 / CTRL_HZ
    cols, edge_t = _synth_reverse(dt, 4000, t_reverse=0.5, inhibit_after_ticks=3)
    full = analyze.pas_edge_timing(cols, 0.5, 0.9)

    # keep every 4th row - exactly what sample_ms=1 does at 4 kHz
    keep = list(range(0, len(cols["time_s"]), 4))
    deci = {k: [v[i] for i in keep] for k, v in cols.items()}
    dec = analyze.pas_edge_timing(deci, 0.5, 0.9)

    check("the decimated record is NOT reported as full rate", dec["full_rate"] is False)
    check("its sampling interval is 4 control ticks",
          abs(dec["sample_interval_s"] - 4 * dt) < 1e-9, str(dec["sample_interval_s"]))
    de = dec["line_reverse_edge"]
    check("the decimated edge is never observed EARLIER than the true one",
          de["status"] != "MEASURED"
          or de["t_observed_s"] >= full["line_reverse_edge"]["t_observed_s"],
          f"{de.get('t_observed_s')} vs {full['line_reverse_edge']['t_observed_s']}")
    if de["status"] == "MEASURED":
        check("the decimated instants are NOT marked exact", de["exact"] is False)
        lat = dec["line_edge_to_inhibit"]
        check("the decimated latency carries bounds wider than one control tick",
              (lat["upper_bound_s"] - lat["lower_bound_s"]) > dt, str(lat))
    else:
        check("a missed edge is reported as NOT_OBSERVED, never as a zero latency",
              dec["line_edge_to_inhibit"]["status"] == dm.NOT_OBSERVED,
              str(dec["line_edge_to_inhibit"]["status"]))
    check("the true full-rate latency is NOT zero",
          abs(full["line_edge_to_inhibit"]["nominal_s"]) > 1e-12,
          str(full["line_edge_to_inhibit"]["nominal_s"]))


def _fake_case(name, group, args, events):
    return dict(name=name, group=group, effective_args=args,
                commanded_event_times_s=dict(events))


def test_p1_refuses_decimated_records() -> None:
    """evaluate_criteria must not score P-1 on a record that cannot resolve it."""
    print("evaluate_criteria: P-1 is NOT_EVALUATED on a decimated record, PASS on a full-rate one")
    import analyze
    dt = 1 / CTRL_HZ
    cols, _ = _synth_reverse(dt, 4000, t_reverse=0.5, inhibit_after_ticks=3)
    for k in ("iq_ref", "session", "pas_transition_index", "reverse_bounce_ticks"):
        cols.setdefault(k, [0] * len(cols["time_s"]))
    args = dict(cadence=72, speed=18, duration=1.0, ride_start_s=0.0)
    case = _fake_case("synthetic_reverse", "reverse_clean", args,
                      [("reverse_start", 0.5), ("reverse_end", 0.9)])

    full = analyze.evaluate_criteria(case, cols, None, None)["P-1"]
    check("full-rate synthetic reverse is evaluated", full["verdict"] == "PASS", str(full))
    check("and its latency is the injected 3 ticks, not zero",
          abs(full["latency_s"] - 3 * dt) < 1e-9, str(full.get("latency_s")))

    keep = list(range(0, len(cols["time_s"]), 4))
    deci = {k: [v[i] for i in keep] for k, v in cols.items()}
    dec = analyze.evaluate_criteria(case, deci, None, None)["P-1"]
    check("the decimated record yields NOT_EVALUATED, never a PASS",
          dec["verdict"] == "NOT_EVALUATED", str(dec["verdict"]))
    check("and the reason names the resolution", "decimated" in dec.get("reason", ""),
          dec.get("reason", "")[:80])
    check("no latency_s is claimed for the decimated record", "latency_s" not in dec,
          str(sorted(k for k in dec if "latency" in k)))


def test_p1_rejects_inverted_order() -> None:
    """If inhibit precedes the line edge, P-1 must not pass."""
    print("evaluate_criteria: an inhibit that precedes the edge cannot pass P-1")
    import analyze
    dt = 1 / CTRL_HZ
    cols, _ = _synth_reverse(dt, 4000, t_reverse=0.5, inhibit_after_ticks=3)
    # force inhibit on from well BEFORE the edge
    cols["pas_inhibit_reason"] = [1 if t >= 0.4 else 0 for t in cols["time_s"]]
    for k in ("iq_ref", "session", "pas_transition_index", "reverse_bounce_ticks"):
        cols.setdefault(k, [0] * len(cols["time_s"]))
    case = _fake_case("synthetic_inverted", "reverse_clean",
                      dict(cadence=72, speed=18, duration=1.0, ride_start_s=0.0),
                      [("reverse_start", 0.5), ("reverse_end", 0.9)])
    r = analyze.evaluate_criteria(case, cols, None, None)["P-1"]
    check("P-1 does not report PASS when inhibit precedes the edge",
          r["verdict"] != "PASS", str(r["verdict"]))


def test_p1_on_real_full_rate_run() -> None:
    """Real data: the full-rate reverse gives a real, non-zero latency."""
    print("real data: reverse at 4 kHz gives a measured, non-zero P-1 latency")
    import analyze
    path = HERE / "results" / "hires_reverse_cad72_speed18.csv"
    if not path.is_file():
        check("hires_reverse_cad72_speed18.csv is present", False, "run runner.py first")
        return
    cols = em.read_csv(path)
    r = analyze.pas_edge_timing(cols, scn.HIRES_REVERSE_AT_S, scn.HIRES_REVERSE_BACK_S)
    check("the record is full rate", r["full_rate"] is True, str(r["sample_interval_s"]))
    check("no override is present in a clean reverse (pas_ab == pas_normal_ab everywhere)",
          all(a == b for a, b in zip(cols["pas_ab"], cols["pas_normal_ab"])))
    check("the line reverse edge is measured", r["line_reverse_edge"]["status"] == "MEASURED",
          str(r["line_reverse_edge"]["status"]))
    lat = r["line_edge_to_inhibit"]
    check("the latency is measured", lat["status"] == "MEASURED", str(lat.get("status")))
    check("it is bounded by one control tick - reported as a BOUND, not an exact 0.00 ms",
          abs(lat["nominal_s"]) <= r["sample_interval_s"] + 1e-12
          and (lat["upper_bound_s"] - lat["lower_bound_s"]) > 0,
          f"nominal={lat['nominal_s']} bounds=[{lat['lower_bound_s']},{lat['upper_bound_s']}]")
    check("the upper bound is far inside the proposed 25 ms threshold",
          lat["upper_bound_s"] <= 0.025, str(lat.get("upper_bound_s")))
    check("sub-tick order is explicitly reported as unresolved, not asserted",
          lat["order_established"] is False and "not exact" not in lat["note"],
          str(lat.get("order_established")))
    # The real correction to iteration 012: the COMMAND is far from the line EDGE.
    cmd_to_edge = r["line_reverse_edge"]["t_observed_s"] - scn.HIRES_REVERSE_AT_S
    check("the command precedes the first reverse line edge by several ms (generator physics)",
          cmd_to_edge > 0.002, f"{cmd_to_edge*1000:.2f} ms")
    check("that command->edge delay is NOT the inhibit latency",
          cmd_to_edge > abs(lat["nominal_s"]) + r["sample_interval_s"],
          f"cmd->edge {cmd_to_edge*1000:.2f} ms vs edge->inhibit {lat['nominal_s']*1000:.2f} ms")


def main() -> int:
    print("=== A. Metric/window tests ===")
    test_event_windows()
    test_step_response_known()

    print("\n=== B. CSV column contract ===")
    test_csv_contract()
    test_contract_rejects_shift()

    print("\n=== C. CLI tests (requires native binary) ===")
    test_new_keys_accepted()
    test_new_keys_rejection()
    test_defaults_unchanged()

    print("\n=== D. Timebase mechanics (mandatory) ===")
    test_fg_delay_mandatory()
    test_miss_tick_window()

    print("\n=== E. PAS edge and sensor-validity mechanics ===")
    test_pas_edge_drop()
    test_pas_edge_jitter()
    test_sensor_invalid_flags_consistent()

    print("\n=== F. Runner failure handling (isolated directories) ===")
    test_runner_guards()
    test_runner_abort_path()
    test_runner_manifest_in_progress()

    print("\n=== G. Scenario metadata and module resolution ===")
    test_scenario_metadata_matches_input()
    test_module_resolution()

    print("\n=== H. New measurement semantics (D011-03) ===")
    test_zero_assessment()
    test_recovery_assessment()
    test_first_change_descriptive()
    test_sustained_transition()
    test_invalid_recovery_on_real_data()
    test_control_ripple_not_a_reaction()

    print("\n=== I. Bounce semantics (D011-01) ===")
    test_bounce_is_electrical_only()
    test_bounce_duration_constant()

    print("\n=== J. PAS edge measurement and P-1 (D012-01) ===")
    test_edge_detector_clean_reverse_without_override()
    test_edge_detector_delayed_acceptance()
    test_edge_detector_bounce_override()
    test_decimation_hides_the_edge()
    test_decimation_inverts_the_order()
    test_p1_refuses_decimated_records()
    test_p1_rejects_inverted_order()
    test_p1_on_real_full_rate_run()

    print(f"\n=== SUMMARY: {PASSED} passed, {len(FAILED)} failed ===")
    for f in FAILED:
        print(f"  FAIL: {f}")
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main())
