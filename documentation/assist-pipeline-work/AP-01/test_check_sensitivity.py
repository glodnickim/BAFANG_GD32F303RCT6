"""Prove the REFINED defect checks are still failure-sensitive.

Four checks in repro_v2_findings.py were refined during the REWORK-002 fix: their original
substring forms also matched the sentences that WITHDRAW the old claim, and one of them inspected a
heuristic (pp < 0.5) that no longer makes the decision. A refined check that merely stops firing is
worthless - the review's central complaint about the V2 runner test was exactly a check that
reported PASS without exercising the thing it claimed to exercise.

So each refined predicate is fed V2-LIKE data here and MUST still report the defect. If any of
these tests fails, the corresponding check has been weakened and its "DEFECT_FIXED" verdict in
results/repro_v2_after_fix.json cannot be trusted.

Run: python test_check_sensitivity.py
"""
from __future__ import annotations

import sys
from pathlib import Path

AP01 = Path(__file__).resolve().parent
sys.path.insert(0, str(AP01))

import repro_v2_findings as R  # noqa: E402


def test_flatness_check_still_flags_the_v2_flatness_rule():
    """V2 decided comparability with pp < 0.5. That rule must still be caught."""
    def v2_is_comparable(values, stages):
        return not (max(values) - min(values) < 0.5)

    defect, details = R.flatness_defect(v2_is_comparable)
    assert defect is True, "the refined check no longer catches the V2 flatness rule"
    assert details["smooth_excluded_from_comparison"] is True


def test_flatness_check_flags_the_intermediate_shape_based_rule():
    """The V3 (REV3) rule kept excluding a plateau on shape alone. AP01-V3-02 overruled that, so
    the predicate must now catch this too - otherwise the correction is untested."""
    def rev3_is_comparable(values, stages):
        pp = max(values) - min(values)
        if pp <= 1e-9:
            return True                              # flat stayed comparable in REV3
        vmax = max(values)
        at_max = sum(1 for v in values if abs(v - vmax) <= 1e-9)
        return not (at_max / len(values) > 0.5)      # plateau excluded on shape alone

    defect, details = R.flatness_defect(rev3_is_comparable)
    assert defect is True, "the predicate does not catch shape-only exclusion"
    assert details["plateau_excluded_without_evidence"] is True


def test_flatness_check_flags_an_assessor_that_ignores_real_evidence():
    """Never exclude anything is equally wrong: demonstrated limiting must still be caught."""
    def always_comparable(values, stages):
        return True

    defect, details = R.flatness_defect(always_comparable)
    assert defect is True
    assert details["evidenced_clipping_missed"] is True


def test_flatness_check_accepts_only_an_evidence_based_assessor():
    """A correct, evidence-based assessor must pass, so the check is not simply always-true."""
    def correct_is_comparable(values, stages):
        changed = any(s["status"] != "UNCHANGED" for s in (stages or {}).values())
        return not changed                            # shape plays no part in the decision

    defect, details = R.flatness_defect(correct_is_comparable)
    assert defect is False, f"an evidence-based assessor is being reported as defective: {details}"


def test_bias_check_still_flags_a_v2_like_extractor_and_results():
    v2_extractor = "def channel_summary(vals):\n    return dict(pp=..., possibly_saturated=pp<0.5)\n"
    v2_results = {"cases": {"c": {"steady": {"iq_ref": {"pp": 0.0, "mean": 100.0,
                                                         "possibly_saturated": True}}}}}
    defect, details = R.bias_and_limit_defect(v2_extractor, v2_results)
    assert defect is True, "the refined check no longer catches the missing bias / unused limit helper"
    assert details["extractor_reads_limit_channel_pair"] is False
    assert details["has_computed_bias"] is False


def test_bias_check_passes_only_with_real_limit_evidence_and_bias():
    fixed_extractor = ("PRE_LIMIT_CHANNEL = 'iq_before_profile_limit'\n"
                       "metrics.limit_assessment(vals)\n")
    fixed_results = {"cases": {"c": {"steady": {"iq_ref": {"bias_vs_reference": 1.5,
                                                           "limit_status": "NOT_LIMITED"}}}}}
    defect, _ = R.bias_and_limit_defect(fixed_extractor, fixed_results)
    assert defect is False


def test_overclaim_check_still_flags_a_v2_like_inventory():
    """Phrases in an ordinary field, and coverage entries with no evidence_level."""
    v2_inventory = {
        "categories": [{
            "id": "reverse_pedalling",
            "existing_coverage": [{
                "file": "tests/host/pas_direction_host.c",
                "covers": "reverse safety",
                "what_the_source_shows": "exhaustive automaton coverage",
                "does_not_cover": "Reverse-direction SAFETY correctness is already proven exhaustively and does not need new evidence.",
            }],
        }],
    }
    defect, details = R.overclaim_defect(v2_inventory)
    assert defect is True, "the refined check no longer catches an overclaiming inventory"
    assert details["live_overclaims_outside_withdrawals"], details
    assert details["entries_missing_evidence_level"], details


def test_overclaim_check_still_flags_a_falsely_claimed_execution():
    inventory = {"categories": [{"existing_coverage": [{
        "file": "tests/host/pas_direction_host.c",
        "evidence_level": "EXECUTED_IN_THIS_SESSION",
        "what_the_source_shows": "ran green",
    }]}]}
    defect, details = R.overclaim_defect(inventory)
    assert defect is True, "claiming execution that did not happen must be flagged"
    assert details["entries_claiming_execution"]


def test_overclaim_check_allows_a_quoted_withdrawal():
    inventory = {
        "scope_statement": "REV2 wrongly wrote 'already proven exhaustively'. That claim is withdrawn.",
        "categories": [{"existing_coverage": [{
            "file": "tests/host/pas_direction_host.c",
            "evidence_level": "READ_IN_THIS_SESSION",
            "what_the_source_shows": "the file states it covers every transition cell",
        }]}],
    }
    defect, details = R.overclaim_defect(inventory)
    assert defect is False, f"a properly scoped inventory is being flagged: {details}"


def test_csv_walk_check_still_flags_a_v2_like_pair():
    v2_inventory = {"categories": [{
        "id": "load_step_during_pas",
        "existing_coverage": [{"file": "tests/host/fw141_torque_elapsed_time_host.c",
                               "evidence_level": "READ_IN_THIS_SESSION",
                               "what_the_source_shows": "filter timebase"}],
        "residual_gap": "Controller Lab is the only harness producing a metrics.py-consumable CSV.",
    }]}
    v2_criteria = {"criteria": [{
        "id": "I2_low_cadence_reported_separately",
        "justification": "the AGENTS.md FW143 Walk-range note treats low-cadence tracking "
                         "precision as an accepted trade-off",
    }]}
    defect, details = R.csv_and_walk_defect(v2_inventory, v2_criteria)
    assert defect is True, "the refined check no longer catches the exclusivity claim / Walk justification"
    assert details["names_l4_csv_emitter"] is False
    assert details["live_exclusivity_claim"] is True
    assert details["low_cadence_explicitly_disclaims_walk"] is False
    assert details["other_walk_mentions_in_criteria"], details


def test_csv_walk_check_flags_a_stray_walk_mention_even_with_a_disclaimer():
    inventory = {"csv_emitting_harnesses": {"found": [
        {"file": "sim/l4/virtual_bike_l4.c:473"}, {"file": "sim/replay/replay_fw.c:76"}]}}
    criteria = {"criteria": [
        {"id": "I2_low_cadence_reported_separately",
         "justification": "fewer revolutions per window; no Walk-based justification is used"},
        {"id": "I1_ripple_improvement",
         "justification": "target consistent with the Walk 10-60 rpm contract"},   # stray
    ]}
    defect, details = R.csv_and_walk_defect(inventory, criteria)
    assert defect is True, "a stray Walk justification elsewhere in the criteria must be flagged"
    assert details["other_walk_mentions_in_criteria"]


TESTS = [
    test_flatness_check_still_flags_the_v2_flatness_rule,
    test_flatness_check_flags_the_intermediate_shape_based_rule,
    test_flatness_check_flags_an_assessor_that_ignores_real_evidence,
    test_flatness_check_accepts_only_an_evidence_based_assessor,
    test_bias_check_still_flags_a_v2_like_extractor_and_results,
    test_bias_check_passes_only_with_real_limit_evidence_and_bias,
    test_overclaim_check_still_flags_a_v2_like_inventory,
    test_overclaim_check_still_flags_a_falsely_claimed_execution,
    test_overclaim_check_allows_a_quoted_withdrawal,
    test_csv_walk_check_still_flags_a_v2_like_pair,
    test_csv_walk_check_flags_a_stray_walk_mention_even_with_a_disclaimer,
]


def main():
    failures = []
    for t in TESTS:
        try:
            t()
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failures.append(t.__name__)
            print(f"FAIL {t.__name__}: {e}")
        except Exception as e:  # noqa: BLE001
            failures.append(t.__name__)
            print(f"ERROR {t.__name__}: {e!r}")
    if failures:
        print(f"\n{len(failures)}/{len(TESTS)} FAILED: {failures}")
        sys.exit(1)
    print(f"\nAll {len(TESTS)} check-sensitivity tests PASS "
          f"(the refined checks still catch the V2 defects).")


if __name__ == "__main__":
    main()
