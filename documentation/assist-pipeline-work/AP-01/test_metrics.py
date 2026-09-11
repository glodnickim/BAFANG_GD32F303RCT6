"""AC2: verify each metric primitive against a constant signal, a known wave, and a known step
before trusting it on Controller Lab CSV output. Pure host Python, no production code involved.

REV2: added the exact counterexamples from REVIEW-EVD-AP-01-001 (AP01-R1) for stop_points, plus
input-validation tests for metrics.py's new _validate_series checks (R1) and the rise_time_10_90
"already past both thresholds" guard.

Run: python test_metrics.py
"""
import math
import sys

import metrics


def approx(a, b, tol=1e-9):
    return abs(a - b) <= tol


def expect_raises(fn, exc=ValueError):
    try:
        fn()
    except exc:
        return True
    return False


def test_constant_signal():
    v = [5.0] * 100
    assert approx(metrics.peak_to_peak(v), 0.0)
    assert approx(metrics.mean(v), 5.0)
    assert approx(metrics.rms(v), 5.0)
    assert approx(metrics.rms_about_mean(v), 0.0)
    assert approx(metrics.bias(v, 5.0), 0.0)
    assert approx(metrics.bias(v, 4.0), 1.0)
    assert metrics.relative_ripple(v) == 0.0
    assert metrics.relative_bias(v, 4.0) is not None


def test_relative_measures_near_zero_reference():
    v = [0.0, 0.0001, -0.0001]
    assert metrics.relative_ripple(v) is None or isinstance(metrics.relative_ripple(v), float)
    assert metrics.relative_ripple(v, reference=0.0) is None
    assert metrics.relative_bias(v, reference=0.0) is None


def test_known_sine_wave():
    n = 4000
    amp = 2.0
    offset = 10.0
    periods = 10
    values = [offset + amp * math.sin(2 * math.pi * periods * i / n) for i in range(n)]
    assert approx(metrics.mean(values), offset, tol=1e-3)
    assert abs(metrics.peak_to_peak(values) - 2 * amp) < 1e-2
    expected_rms = math.sqrt(offset ** 2 + amp ** 2 / 2)
    assert abs(metrics.rms(values) - expected_rms) < 1e-2
    expected_ripple_rms = amp / math.sqrt(2)
    assert abs(metrics.rms_about_mean(values) - expected_ripple_rms) < 1e-2
    expected_rel_ripple = expected_ripple_rms / offset
    assert abs(metrics.relative_ripple(values) - expected_rel_ripple) < 1e-3


def test_known_step_rise_time():
    dt = 0.001
    tau = 0.1
    n = 3000
    times = [i * dt for i in range(n)]
    values = [100.0 * (1 - math.exp(-t / tau)) for t in times]
    rt = metrics.rise_time_10_90(times, values, start_level=0.0, end_level=100.0)
    expected = tau * math.log(9)
    assert rt is not None
    assert abs(rt - expected) < 3 * dt


def test_step_never_settles_returns_none():
    times = [0.0, 0.1, 0.2]
    values = [0.0, 1.0, 2.0]
    rt = metrics.rise_time_10_90(times, values, start_level=0.0, end_level=100.0)
    assert rt is None


def test_step_already_past_thresholds_returns_none_not_zero():
    # AP01-R2/R3 adjacent guard: a signal already above both 10% and 90% thresholds at t0 must
    # NOT be reported as a 0.0 s reaction to a step this record never shows.
    times = [0.0, 0.1, 0.2]
    values = [95.0, 96.0, 97.0]
    rt = metrics.rise_time_10_90(times, values, start_level=0.0, end_level=100.0)
    assert rt is None


# --- AP01-R1 exact counterexamples from REVIEW-EVD-AP-01-001 ------------------------------

def test_stop_no_post_stop_samples():
    sp = metrics.stop_points([0, 1], [10, 10], stop_time=2, zero_tolerance=1)
    assert sp["status"] == "NO_DATA"
    assert approx(sp["value_at_stop"], 10)
    assert sp["confirmed_zero_time_s"] is None
    assert sp["time_to_confirmed_zero_s"] is None


def test_stop_never_zero_until_end():
    sp = metrics.stop_points([0, 1, 2, 3], [10, 10, 10, 10], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "NOT_REACHED"
    assert sp["confirmed_zero_time_s"] is None
    assert sp["time_to_confirmed_zero_s"] is None
    assert sp["last_observed_time_s"] == 3


def test_stop_first_zero_at_2():
    sp = metrics.stop_points([0, 1, 2, 3], [10, 10, 0, 0], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "CONFIRMED"
    assert sp["confirmed_zero_time_s"] == 2
    assert approx(sp["time_to_confirmed_zero_s"], 1)


def test_stop_empty_input():
    sp = metrics.stop_points([], [], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "NO_DATA"
    assert sp["value_at_stop"] is None
    assert approx(sp["time_to_confirmed_zero_s"], 0.0) if sp["time_to_confirmed_zero_s"] is not None else True
    assert sp["time_to_confirmed_zero_s"] is None


def test_stop_transient_zero_then_rises_again_not_confirmed_early():
    # A dip to zero at t=1 that rises again at t=2 must NOT be reported as the confirmed stop.
    sp = metrics.stop_points([0, 1, 2, 3, 4], [10, 0, 10, 0, 0], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "CONFIRMED"
    assert sp["confirmed_zero_time_s"] == 3  # not 1: t=1 does not hold through the record
    assert approx(sp["time_to_confirmed_zero_s"], 2)


def test_stop_already_zero_with_sufficient_observation():
    sp = metrics.stop_points([5, 6, 7, 8], [0, 0, 0, 0], stop_time=5, zero_tolerance=1)
    assert sp["status"] == "CONFIRMED"
    assert sp["confirmed_zero_time_s"] == 5
    assert approx(sp["time_to_confirmed_zero_s"], 0.0)
    assert sp["value_at_stop"] is None  # nothing strictly before stop_time in this record


def test_stop_missing_channel_like_input_raises():
    # Caller-side "missing channel" surfaces as an empty/None series before reaching stop_points;
    # stop_points itself must refuse silently-wrong empty-vs-nonempty mismatches.
    assert expect_raises(lambda: metrics.stop_points([1, 2], [1], stop_time=1, zero_tolerance=1))


def test_stop_invalid_tolerance_raises():
    assert expect_raises(lambda: metrics.stop_points([1, 2], [1, 0], stop_time=1, zero_tolerance=0))
    assert expect_raises(lambda: metrics.stop_points([1, 2], [1, 0], stop_time=1, zero_tolerance=-1))


def test_stop_non_monotonic_timestamps_raise():
    assert expect_raises(lambda: metrics.stop_points([1, 1, 2], [1, 1, 0], stop_time=0, zero_tolerance=1))
    assert expect_raises(lambda: metrics.stop_points([2, 1, 3], [1, 1, 0], stop_time=0, zero_tolerance=1))


def test_stop_non_finite_values_raise():
    assert expect_raises(lambda: metrics.stop_points([0, 1], [float("nan"), 0], stop_time=0, zero_tolerance=1))
    assert expect_raises(lambda: metrics.stop_points([0, 1], [float("inf"), 0], stop_time=0, zero_tolerance=1))


def test_window_mismatched_lengths_raises_not_silently_truncates():
    # AP01-R1: window() used to silently zip-truncate; it must now raise.
    assert expect_raises(lambda: metrics.window([0], [5, 6], 0, 1))


# --- AP01-V2-01: stop confirmation needs observation, and a true transition bracket ----------

def test_stop_single_trailing_zero_is_insufficient_observation():
    """Reviewer counterexample: the zero exists only in the last sample, so nothing was observed
    after it. Must NOT be CONFIRMED (it was, in REV2)."""
    sp = metrics.stop_points([0, 1, 2], [10, 10, 0], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "INSUFFICIENT_OBSERVATION", sp["status"]
    assert sp["confirmed_zero_time_s"] is None
    assert sp["time_to_confirmed_zero_s"] is None
    # information is not destroyed: the candidate instant and the hold actually observed are kept
    assert sp["candidate_zero_time_s"] == 2
    assert approx(sp["observed_hold_s"], 0.0)


def test_stop_sufficient_hold_is_confirmed_positive_case():
    """Same shape, but the record continues past the zero for longer than min_confirm_s."""
    sp = metrics.stop_points([0, 1, 2, 2.5, 3], [10, 10, 0, 0, 0], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "CONFIRMED"
    assert sp["confirmed_zero_time_s"] == 2
    assert approx(sp["time_to_confirmed_zero_s"], 1)
    assert approx(sp["observed_hold_s"], 1.0)


def test_stop_min_confirm_is_a_caller_controlled_measurement_parameter():
    t, v = [0, 1, 2, 2.5, 3], [10, 10, 0, 0, 0]
    assert metrics.stop_points(t, v, 1, 1, min_confirm_s=0.5)["status"] == "CONFIRMED"
    assert metrics.stop_points(t, v, 1, 1, min_confirm_s=5.0)["status"] == "INSUFFICIENT_OBSERVATION"


def test_stop_irregular_sampling_reports_true_transition_bracket():
    """Reviewer counterexample: the transition may lie anywhere in (1.2, 5], but REV2 offered a
    median dt of ~0.1 s as its resolution."""
    t = [0, 1, 1.1, 1.2, 5, 5.1, 5.2]
    v = [10, 10, 10, 10, 0, 0, 0]
    sp = metrics.stop_points(t, v, stop_time=1, zero_tolerance=1)
    assert sp["transition_bracket_s"] == [1.2, 5]
    assert approx(sp["transition_bracket_width_s"], 3.8)
    # the median dt is still reported, but explicitly not as the uncertainty
    assert sp["median_sample_dt_s"] is not None
    assert "NOT the uncertainty" in sp["median_sample_dt_note"]


def test_stop_observation_gap_limit_rejects_a_wide_bracket_and_accepts_a_narrow_one():
    wide_t, wide_v = [0, 1, 1.1, 1.2, 5, 5.1, 5.2], [10, 10, 10, 10, 0, 0, 0]
    sp_wide = metrics.stop_points(wide_t, wide_v, 1, 1, max_transition_gap_s=0.05)
    assert sp_wide["status"] == "OBSERVATION_GAP_TOO_LARGE"
    assert sp_wide["confirmed_zero_time_s"] is None
    tight_t = [0, 1, 1.1, 1.2, 1.21, 1.3, 1.4]
    tight_v = [10, 10, 10, 10, 0, 0, 0]
    sp_tight = metrics.stop_points(tight_t, tight_v, 1, 1, max_transition_gap_s=0.05)
    assert sp_tight["status"] == "CONFIRMED", sp_tight["status"]
    assert approx(sp_tight["transition_bracket_width_s"], 0.01)


def test_stop_bracket_absent_when_nothing_was_ever_out_of_tolerance():
    sp = metrics.stop_points([5, 6, 7, 8], [0, 0, 0, 0], stop_time=5, zero_tolerance=1)
    assert sp["status"] == "CONFIRMED"
    assert sp["transition_bracket_s"] == [None, 5]
    assert sp["transition_bracket_width_s"] is None


def test_stop_invalid_measurement_parameters_raise():
    assert expect_raises(lambda: metrics.stop_points([0, 1], [10, 0], 1, 1, min_confirm_s=-1))
    assert expect_raises(lambda: metrics.stop_points([0, 1], [10, 0], 1, 1, max_transition_gap_s=0))
    assert expect_raises(lambda: metrics.stop_points([0, 1], [10, 0], float("nan"), 1))


def test_rise_time_lo_crossing_before_record_is_not_a_measurement():
    """Reviewer counterexample: [50,90,100] for a 0->100 step returned 1 s although the 10%
    crossing is not in the record."""
    d = metrics.rise_time_10_90_detail([0, 1, 2], [50, 90, 100], start_level=0.0, end_level=100.0)
    assert d["status"] == "LO_CROSSING_BEFORE_RECORD", d["status"]
    assert d["rise_time_s"] is None
    assert metrics.rise_time_10_90([0, 1, 2], [50, 90, 100], 0.0, 100.0) is None


def test_rise_time_detail_measured_positive_case():
    dt, tau, n = 0.001, 0.1, 3000
    times = [i * dt for i in range(n)]
    values = [100.0 * (1 - math.exp(-t / tau)) for t in times]
    d = metrics.rise_time_10_90_detail(times, values, 0.0, 100.0)
    assert d["status"] == "MEASURED"
    assert abs(d["rise_time_s"] - tau * math.log(9)) < 3 * dt
    assert d["t_lo_s"] < d["t_hi_s"]


def test_rise_time_hi_crossing_not_reached_is_flagged():
    d = metrics.rise_time_10_90_detail([0, 1, 2], [0, 20, 30], start_level=0.0, end_level=100.0)
    assert d["status"] == "HI_CROSSING_NOT_REACHED"
    assert d["rise_time_s"] is None
    assert d["t_lo_s"] == 1


def test_window_invalid_bounds_raise():
    assert expect_raises(lambda: metrics.window([0, 1], [5, 6], 1.0, 1.0))
    assert expect_raises(lambda: metrics.window([0, 1], [5, 6], 2.0, 1.0))
    assert expect_raises(lambda: metrics.window([0, 1], [5, 6], float("nan"), 1.0))


# --- AP01-V2-02: limiting from real evidence, never from flatness ----------------------------

def test_perfectly_smooth_signal_stays_comparable():
    """Reviewer counterexample: [100]*20 was flagged SATURATED and excluded from comparison."""
    la = metrics.limit_assessment([100.0] * 20, channel="iq_ref", stages={})
    assert la["status"] == "FLAT_NO_LIMIT_EVIDENCE"
    assert la["comparable"] is True, "a perfectly smooth signal must not be disqualified for being smooth"
    assert la["limiting"] == "UNKNOWN"


def test_plateau_without_limit_evidence_is_not_an_exclusion():
    """AP01-V3-02 counterexample: [80,100,100,80,100,100] with IDENTICAL channels on both sides of
    every observed stage - i.e. no demonstrated change - was still excluded as CLIPPED_AT_MAX.
    A plateau can equally come from quantisation, a constant input or a change in effort."""
    clipped_shape = [80.0, 100.0, 100.0, 80.0, 100.0, 100.0]
    stages = metrics.stage_analysis({
        "iq_before_profile_limit": clipped_shape, "iq_mode_request": clipped_shape,
        "iq_requested": clipped_shape, "iq_allowed": clipped_shape})
    assert all(s["status"] == "UNCHANGED" for s in stages.values())
    la = metrics.limit_assessment(clipped_shape, channel="iq_ref", stages=stages)
    assert la["status"] == "PLATEAU_NO_LIMIT_EVIDENCE", la["status"]
    assert la["comparable"] is True, "shape alone must not exclude a result"
    assert la["limiting"] == "UNKNOWN"


def test_real_clipping_needs_a_demonstrated_reduction():
    """A genuine limit shows a request ABOVE the limit being cut down at an observed stage."""
    requested_over_limit = [120.0, 140.0, 160.0, 180.0, 200.0, 220.0]
    clamped = [100.0] * 6
    stages = metrics.stage_analysis({
        "iq_before_profile_limit": requested_over_limit, "iq_mode_request": clamped,
        "iq_requested": clamped, "iq_allowed": clamped})
    assert stages["profile_limit"]["status"] == "REDUCED"
    assert stages["profile_limit"]["attributed_cause"] is not None
    la = metrics.limit_assessment(clamped, channel="iq_mode_request", stages=stages)
    assert la["status"] == "LIMITED_UPSTREAM"
    assert la["comparable"] is False


def test_zero_window_is_not_saturation():
    la = metrics.limit_assessment([0.0] * 10, channel="iq_ref", stages={})
    assert la["status"] == "ZERO_NO_ASSIST"


def test_no_limit_observed_does_not_claim_the_whole_chain():
    la = metrics.limit_assessment([10.0, 20.0, 30.0, 40.0], channel="iq_ref", stages={})
    assert la["status"] == "NO_LIMIT_OBSERVED"
    assert la["comparable"] is True
    assert "not separated by these columns" in la["evidence"]


# --- AP01-V3-01: three separate stages of the real Iq chain ----------------------------------

def test_stage_profile_limit_is_between_before_and_mode_request():
    """Real data, floor cases, window 3-7.5s: 129-179 -> 7 at the profile limit."""
    pre = [129.0, 150.0, 179.0, 160.0]
    mode = [7.0] * 4
    st = metrics.stage_transition(pre, mode)
    assert st["status"] == "REDUCED"
    assert approx(st["fraction_reduced"], 1.0)
    assert st["upstream_range"] == [129.0, 179.0]
    assert st["downstream_range"] == [7.0, 7.0]


def test_both_floor_cases_show_cap_to_7_then_a_later_raise():
    """AP01-V3-01: BOTH floor cases are cut to 7 and then raised above that limit. REV3 labelled
    one of them plain CAP_BINDING, hiding the limit-ordering finding."""
    pre = [129.0, 150.0, 179.0, 160.0]
    mode = [7.0] * 4
    for later_value, expected in ((14.0, "RAISED"), (175.0, "RAISED")):
        stages = metrics.stage_analysis({
            "iq_before_profile_limit": pre, "iq_mode_request": mode,
            "iq_requested": [later_value] * 4, "iq_allowed": [later_value] * 4})
        assert stages["profile_limit"]["status"] == "REDUCED"
        assert stages["post_mode_request"]["status"] == expected
        # the later stage must NOT be attributed to floor/gate without evidence
        assert stages["post_mode_request"]["attributed_cause"] is None
        assert "not attributable" in stages["post_mode_request"]["attribution_note"]


def test_later_zeroing_is_not_called_a_cap():
    """without_rotation_repro: 285 -> 285 (no profile cut at all) -> 0 at the later stage.
    REV3 called this CAP_BINDING."""
    stages = metrics.stage_analysis({
        "iq_before_profile_limit": [285.0] * 5, "iq_mode_request": [285.0] * 5,
        "iq_requested": [0.0] * 5, "iq_allowed": [0.0] * 5})
    assert stages["profile_limit"]["status"] == "UNCHANGED"
    assert stages["post_mode_request"]["status"] == "REDUCED"
    assert stages["post_mode_request"]["attributed_cause"] is None
    # the channel upstream of the change stays comparable; only downstream ones are limited
    up = metrics.limit_assessment([285.0] * 5, channel="iq_mode_request", stages=stages)
    down = metrics.limit_assessment([0.0] * 5, channel="iq_requested", stages=stages)
    assert up["comparable"] is True
    assert down["comparable"] is False


def test_non_iq_channels_get_no_iq_limit_verdict():
    """AP01-V3-01 item 4: REV3 stamped the same limit label onto torque and cadence."""
    stages = metrics.stage_analysis({
        "iq_before_profile_limit": [179.0] * 4, "iq_mode_request": [7.0] * 4,
        "iq_requested": [175.0] * 4, "iq_allowed": [175.0] * 4})
    for ch in ("torque_run_native", "cadence_control_rpm"):
        la = metrics.limit_assessment([300.0, 320.0, 310.0, 305.0], channel=ch, stages=stages)
        assert la["status"] == metrics.NON_IQ_CHANNEL_STATUS, (ch, la["status"])
        assert la["comparable"] is True
        assert la["limiting"] == "NOT_APPLICABLE"


def test_stage_transition_length_mismatch_raises():
    assert expect_raises(lambda: metrics.stage_transition([1.0, 2.0], [1.0]))


# --- AP01-V2-02: comparison of known signals -------------------------------------------------

def _sine(amp, offset, n=2000, periods=10):
    return [offset + amp * math.sin(2 * math.pi * periods * i / n) for i in range(n)]


def test_compare_channel_large_improvement_is_not_penalised():
    """The core defect: a candidate five times smoother must not be rejected for being better."""
    baseline = _sine(10.0, 100.0)     # relative ripple ~ 0.0707
    candidate = _sine(2.0, 100.0)     # relative ripple ~ 0.0141 -> 80% reduction
    c = metrics.compare_channel(baseline, candidate)
    assert c["ripple_reduction_fraction"] > 0.75
    assert c["meets_ripple_target"] is True
    assert c["within_mean_budget"] is True
    assert c["verdict"] == "IMPROVED_WITHIN_BUDGET", c["verdict"]


def test_compare_channel_known_mean_shift_gives_exact_bias():
    baseline = _sine(10.0, 100.0)
    candidate = [v + 10.0 for v in baseline]   # exactly +10 counts = +10% of the 100 mean
    c = metrics.compare_channel(baseline, candidate)
    assert abs(c["mean_bias_abs"] - 10.0) < 1e-6
    assert abs(c["mean_bias_relative"] - 0.10) < 1e-6
    assert c["within_mean_budget"] is False      # 10% > the 5% budget
    assert c["verdict"] == "MEAN_OUT_OF_BUDGET"


def test_compare_channel_small_mean_shift_within_budget():
    baseline = _sine(10.0, 100.0)
    candidate = [v + 2.0 for v in baseline]      # +2% of the mean
    c = metrics.compare_channel(baseline, candidate)
    assert abs(c["mean_bias_relative"] - 0.02) < 1e-6
    assert c["within_mean_budget"] is True
    assert c["verdict"] == "PRESERVED_NOT_IMPROVED"


def test_compare_channel_ripple_regression_is_flagged():
    baseline = _sine(2.0, 100.0)
    candidate = _sine(10.0, 100.0)
    c = metrics.compare_channel(baseline, candidate)
    assert c["ripple_reduction_fraction"] < 0
    assert c["verdict"] == "RIPPLE_REGRESSED"


def test_compare_channel_zero_mean_is_not_comparable():
    c = metrics.compare_channel([0.0] * 10, [0.0] * 10)
    assert c["verdict"] == "NOT_COMPARABLE"


def test_compare_channel_new_ripple_from_a_flat_baseline_is_a_regression():
    """AP01-V3-02 counterexample: compare_channel([100]*6, [90,110,...]) returned
    PRESERVED_NOT_IMPROVED although the candidate introduces ripple where there was none."""
    c = metrics.compare_channel([100.0] * 6, [90.0, 110.0, 90.0, 110.0, 90.0, 110.0])
    assert c["verdict"] == "RIPPLE_REGRESSED_FROM_ZERO", c["verdict"]
    assert c["ripple_reduction_fraction"] is None, "a percentage reduction is undefined here"
    assert c["baseline_ripple_rms"] == 0.0
    assert c["candidate_ripple_rms"] > 0.0
    assert c["meets_ripple_target"] is False
    # the mean is unchanged, so this must NOT be reported as a mean problem
    assert c["within_mean_budget"] is True


def test_compare_channel_flat_to_flat_is_no_change():
    c = metrics.compare_channel([100.0] * 6, [100.0] * 6)
    assert c["verdict"] == "UNCHANGED_BOTH_FLAT"
    assert c["ripple_reduction_fraction"] is None


def test_compare_channel_flat_baseline_with_mean_shift_reports_the_mean():
    c = metrics.compare_channel([100.0] * 6, [130.0] * 6)
    assert c["verdict"] == "MEAN_OUT_OF_BUDGET"
    assert approx(c["mean_bias_relative"], 0.30)


def test_stop_confirmed_reports_earlier_excursions_separately():
    """CONFIRMED means the LAST stretch holds; it does not promise the channel never came back up
    earlier. That has to be visible, not implied away."""
    sp = metrics.stop_points([0, 1, 2, 3, 4, 5], [10, 0, 10, 0, 0, 0], stop_time=1, zero_tolerance=1)
    assert sp["status"] == "CONFIRMED"
    assert sp["confirmed_zero_time_s"] == 3
    assert sp["excursions_before_confirmation"] == 1, sp["excursions_before_confirmation"]
    clean = metrics.stop_points([0, 1, 2, 3, 4], [10, 10, 0, 0, 0], stop_time=1, zero_tolerance=1)
    assert clean["status"] == "CONFIRMED"
    assert clean["excursions_before_confirmation"] == 0


TESTS = [
    test_constant_signal,
    test_relative_measures_near_zero_reference,
    test_known_sine_wave,
    test_known_step_rise_time,
    test_step_never_settles_returns_none,
    test_step_already_past_thresholds_returns_none_not_zero,
    test_stop_no_post_stop_samples,
    test_stop_never_zero_until_end,
    test_stop_first_zero_at_2,
    test_stop_empty_input,
    test_stop_transient_zero_then_rises_again_not_confirmed_early,
    test_stop_already_zero_with_sufficient_observation,
    test_stop_missing_channel_like_input_raises,
    test_stop_invalid_tolerance_raises,
    test_stop_non_monotonic_timestamps_raise,
    test_stop_non_finite_values_raise,
    test_window_mismatched_lengths_raises_not_silently_truncates,
    # AP01-V2-01
    test_stop_single_trailing_zero_is_insufficient_observation,
    test_stop_sufficient_hold_is_confirmed_positive_case,
    test_stop_min_confirm_is_a_caller_controlled_measurement_parameter,
    test_stop_irregular_sampling_reports_true_transition_bracket,
    test_stop_observation_gap_limit_rejects_a_wide_bracket_and_accepts_a_narrow_one,
    test_stop_bracket_absent_when_nothing_was_ever_out_of_tolerance,
    test_stop_invalid_measurement_parameters_raise,
    test_rise_time_lo_crossing_before_record_is_not_a_measurement,
    test_rise_time_detail_measured_positive_case,
    test_rise_time_hi_crossing_not_reached_is_flagged,
    test_window_invalid_bounds_raise,
    # AP01-V2-02
    test_perfectly_smooth_signal_stays_comparable,
    test_compare_channel_large_improvement_is_not_penalised,
    test_compare_channel_known_mean_shift_gives_exact_bias,
    test_compare_channel_small_mean_shift_within_budget,
    test_compare_channel_ripple_regression_is_flagged,
    test_compare_channel_zero_mean_is_not_comparable,
    # AP01-V3-01 (three separate stages of the real Iq chain)
    test_stage_profile_limit_is_between_before_and_mode_request,
    test_both_floor_cases_show_cap_to_7_then_a_later_raise,
    test_later_zeroing_is_not_called_a_cap,
    test_non_iq_channels_get_no_iq_limit_verdict,
    test_stage_transition_length_mismatch_raises,
    # AP01-V3-02 (shape must not exclude; zero-baseline regression; stop excursions)
    test_plateau_without_limit_evidence_is_not_an_exclusion,
    test_real_clipping_needs_a_demonstrated_reduction,
    test_zero_window_is_not_saturation,
    test_no_limit_observed_does_not_claim_the_whole_chain,
    test_compare_channel_new_ripple_from_a_flat_baseline_is_a_regression,
    test_compare_channel_flat_to_flat_is_no_change,
    test_compare_channel_flat_baseline_with_mean_shift_reports_the_mean,
    test_stop_confirmed_reports_earlier_excursions_separately,
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
        except Exception as e:  # noqa: BLE001 - surface unexpected errors as failures too
            failures.append(t.__name__)
            print(f"ERROR {t.__name__}: {e!r}")
    if failures:
        print(f"\n{len(failures)}/{len(TESTS)} FAILED: {failures}")
        sys.exit(1)
    print(f"\nAll {len(TESTS)} metric self-tests PASS.")


if __name__ == "__main__":
    main()
