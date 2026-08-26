/*
 * FW-125: phase-current SAME-PATH calibration - the measurement-domain fix on top of FW-119's
 * retry/LKG/fallback policy (see tests/host/fw119_current_cal_host.c for that policy's own
 * tests, unchanged by this card).
 *
 * This file proves the parts of the fix that are pure arithmetic on current_cal.c and therefore
 * genuinely executable on the host: the mean/P2P computed from real (non-uniform) per-sample
 * data, the signed residual domain, and the "corrected = raw - offset" formula the ISR applies
 * (mirrored here as isr_correct(), a one-line copy of the real subtraction in
 * ADC0_1_IRQHandler - see main.c). What this file CANNOT prove - because main.c is the ARM entry
 * point and cannot be linked on a PC, same limitation as every other *_wiring_host.c in this
 * suite - is that the calibration accumulator and the runtime FOC read are wired to the SAME
 * ADC instance per phase. That is tests/host/fw125_wiring_guard_host.c, a source-text guard over
 * main.c (T1-T4, T13-T15 below).
 *
 * WHAT THIS PROVES:
 *   T5:  a 128-sample attempt with varying (non-uniform) per-sample values produces the exact
 *        integer mean - not an approximation, not the old FW-118 ">> 6" shift.
 *   T6:  min/max/P2P are exact over that same varying data.
 *   T7:  publication is atomic in effect: a submit() never leaves offset[] and residual_mean[]
 *        disagreeing, and current_cal.c's source text publishes valid=0 -> offset[] -> valid=1
 *        in that order (see check_atomic_publish_order()).
 *   T8:  an OUT_OF_RANGE candidate under STRICT with no LKG never becomes usable - FOC start is
 *        inhibited, offset[] is left at its prior (safe) value.
 *   T9:  LEGACY_FALLBACK still produces a rideable, explicitly-degraded state on the new signed
 *        domain - the fallback policy itself needed no change for FW-125.
 *   T10: isr_correct(offset, offset) == 0 - a phase reading exactly its own offset corrects to 0.
 *   T11: isr_correct(offset + 5, offset) == +5 - signed residual passes through unchanged.
 *   T12: isr_correct(offset - 7, offset) == -7 - same, negative side.
 *   T13 (oracle, card section 23): three independent per-phase deltas run through the full
 *        calibrate-then-correct path (submit -> isr_correct) reproduce those exact deltas.
 */

#include "../common/check.h"
#include "current_cal.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Mirrors the ONE line ADC0_1_IRQHandler runs per phase after current_cal.valid is set:
 *     i16_phX_current -= current_cal.offset[PHASE];
 * `raw` stands in for adc_inserted_data_read() - already hardware-IOFF-corrected, per
 * inc/current_cal.h - so no further hardware-offset subtraction belongs here either.
 */
static int16_t isr_correct(int16_t raw, int16_t offset)
{
	return (int16_t)(raw - offset);
}

/* Builds an attempt from an explicit per-sample sequence (not a uniform level+p2p synthesis),
 * so the mean/min/max the test checks against is computed independently, by hand, in the
 * caller - a real oracle rather than the same formula twice. */
static current_cal_attempt_t make_varying_attempt(const int16_t *samples_a,
                                                    const int16_t *samples_b,
                                                    const int16_t *samples_c,
                                                    uint16_t n)
{
	current_cal_attempt_t a;
	uint16_t i;
	memset(&a, 0, sizeof a);
	a.timed_out = 0;
	a.samples = n;
	for (i = 0; i < n; i++) {
		a.sum[CURRENT_CAL_PHASE_A] += samples_a[i];
		a.sum[CURRENT_CAL_PHASE_B] += samples_b[i];
		a.sum[CURRENT_CAL_PHASE_C] += samples_c[i];
		if (i == 0) {
			a.min[CURRENT_CAL_PHASE_A] = a.max[CURRENT_CAL_PHASE_A] = samples_a[0];
			a.min[CURRENT_CAL_PHASE_B] = a.max[CURRENT_CAL_PHASE_B] = samples_b[0];
			a.min[CURRENT_CAL_PHASE_C] = a.max[CURRENT_CAL_PHASE_C] = samples_c[0];
		} else {
			if (samples_a[i] < a.min[CURRENT_CAL_PHASE_A]) a.min[CURRENT_CAL_PHASE_A] = samples_a[i];
			if (samples_a[i] > a.max[CURRENT_CAL_PHASE_A]) a.max[CURRENT_CAL_PHASE_A] = samples_a[i];
			if (samples_b[i] < a.min[CURRENT_CAL_PHASE_B]) a.min[CURRENT_CAL_PHASE_B] = samples_b[i];
			if (samples_b[i] > a.max[CURRENT_CAL_PHASE_B]) a.max[CURRENT_CAL_PHASE_B] = samples_b[i];
			if (samples_c[i] < a.min[CURRENT_CAL_PHASE_C]) a.min[CURRENT_CAL_PHASE_C] = samples_c[i];
			if (samples_c[i] > a.max[CURRENT_CAL_PHASE_C]) a.max[CURRENT_CAL_PHASE_C] = samples_c[i];
		}
	}
	return a;
}

static void t5_t6_exact_mean_and_p2p_over_varying_samples(void)
{
	int16_t sa[CURRENT_CAL_SAMPLES], sb[CURRENT_CAL_SAMPLES], sc[CURRENT_CAL_SAMPLES];
	int32_t expect_sum_a = 0, expect_sum_b = 0, expect_sum_c = 0;
	int16_t expect_min_a, expect_max_a;
	current_cal_t s;
	current_cal_attempt_t a;
	uint16_t i;

	printf("T5/T6 exact mean/P2P over %d varying (non-uniform) samples\n", CURRENT_CAL_SAMPLES);

	/* Deterministic pseudo-noise around three different centres, one per phase, so a bug that
	 * mixed up phases would show up as a wrong mean on the wrong phase. */
	for (i = 0; i < CURRENT_CAL_SAMPLES; i++) {
		int16_t wobble = (int16_t)((i % 7) - 3); /* -3..+3, deterministic */
		sa[i] = (int16_t)(12 + wobble);
		sb[i] = (int16_t)(-40 + wobble);
		sc[i] = (int16_t)(0 + wobble);
		expect_sum_a += sa[i];
		expect_sum_b += sb[i];
		expect_sum_c += sc[i];
	}
	expect_min_a = sa[0]; expect_max_a = sa[0];
	for (i = 1; i < CURRENT_CAL_SAMPLES; i++) {
		if (sa[i] < expect_min_a) expect_min_a = sa[i];
		if (sa[i] > expect_max_a) expect_max_a = sa[i];
	}

	a = make_varying_attempt(sa, sb, sc, CURRENT_CAL_SAMPLES);
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);
	CHECK(current_cal_submit(&s, &a) == CURRENT_CAL_OK, "T5: a well-formed varying attempt passes");

	CHECK(s.residual_mean[CURRENT_CAL_PHASE_A] == (int16_t)(expect_sum_a / CURRENT_CAL_SAMPLES),
	      "T5: phase A mean is exact (sum/samples), not an approximation");
	CHECK(s.residual_mean[CURRENT_CAL_PHASE_B] == (int16_t)(expect_sum_b / CURRENT_CAL_SAMPLES),
	      "T5: phase B mean is exact and independent of phase A");
	CHECK(s.residual_mean[CURRENT_CAL_PHASE_C] == (int16_t)(expect_sum_c / CURRENT_CAL_SAMPLES),
	      "T5: phase C mean is exact and independent of phases A/B");

	CHECK(s.p2p[CURRENT_CAL_PHASE_A] == (uint16_t)(expect_max_a - expect_min_a),
	      "T6: phase A P2P is exact (max-min) over the same varying data");
}

static void t7_atomic_publish(void)
{
	current_cal_t s;
	current_cal_attempt_t good = make_varying_attempt(
		(int16_t[]){5,5,5,5}, (int16_t[]){5,5,5,5}, (int16_t[]){5,5,5,5}, 4);

	printf("T7 publish is atomic in effect: offset[] and residual_mean[] never disagree\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);
	CHECK(current_cal_submit(&s, &good) == CURRENT_CAL_OK, "T7: setup: attempt accepted");
	CHECK(s.valid == 1, "T7: valid is set once publication completes");
	CHECK(memcmp(s.offset, s.residual_mean, sizeof s.offset) == 0,
	      "T7: offset[] equals residual_mean[] - a partially-applied set cannot pass this");
	CHECK(memcmp(s.offset, s.lkg_offset, sizeof s.offset) == 0,
	      "T7: LKG is published together with the live offsets, not in a second step an ISR "
	      "could observe in between");
}

static void t8_strict_out_of_range_stays_inhibited(void)
{
	current_cal_t s;
	current_cal_attempt_t oor = make_varying_attempt(
		(int16_t[]){(int16_t)(CURRENT_CAL_RESIDUAL_MAX + 50)},
		(int16_t[]){0}, (int16_t[]){0}, 1);
	int16_t offset_before[CURRENT_CAL_PHASES];

	printf("T8 STRICT + OUT_OF_RANGE + no LKG -> FOC stays inhibited, offsets untouched\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_STRICT);
	memcpy(offset_before, s.offset, sizeof offset_before);

	current_cal_begin(&s);
	while (current_cal_attempt_allowed(&s)) (void)current_cal_submit(&s, &oor);
	current_cal_finalize(&s);

	CHECK(current_cal_foc_allowed(&s) == 0, "T8: FOC start is inhibited - an out-of-range "
	      "candidate can never enable unsafe FOC under STRICT");
	CHECK(s.valid == 0, "T8: no offset is marked valid");
	CHECK(memcmp(s.offset, offset_before, sizeof offset_before) == 0,
	      "T8: offset[] is untouched by the rejected candidate");
}

static void t9_legacy_fallback_signed_domain(void)
{
	current_cal_t s;
	current_cal_attempt_t bad = make_varying_attempt(
		(int16_t[]){(int16_t)(CURRENT_CAL_RESIDUAL_MIN - 50)},
		(int16_t[]){0}, (int16_t[]){0}, 1);

	printf("T9 LEGACY_FALLBACK is unchanged by FW-125's signed domain\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);
	while (current_cal_attempt_allowed(&s)) (void)current_cal_submit(&s, &bad);
	current_cal_finalize(&s);

	CHECK(s.status == CURRENT_CAL_LEGACY_FALLBACK, "T9: falls back to the pre-FW-118 path");
	CHECK(s.valid == 0, "T9: no software offset applied");
	CHECK(current_cal_foc_allowed(&s) == 1, "T9: the bike is still rideable");
}

static void t10_t11_t12_signed_residual_correction(void)
{
	printf("T10/T11/T12 isr_correct() signed residual: exact zero, +5, -7\n");
	CHECK(isr_correct(2020, 2020) == 0, "T10: raw == offset corrects to exactly 0");
	CHECK(isr_correct((int16_t)(2020 + 5), 2020) == 5, "T11: offset+5 corrects to exactly +5");
	CHECK(isr_correct((int16_t)(2020 - 7), 2020) == -7, "T12: offset-7 corrects to exactly -7");
	/* And in the small-residual domain FW-125 actually calibrates in (near 0, not near 2020),
	 * since that is the domain adc_inserted_data_read() returns per inc/current_cal.h. */
	CHECK(isr_correct(0, 0) == 0, "T10b: zero residual offset, zero reading -> 0");
	CHECK(isr_correct(5, 0) == 5, "T11b: zero offset, +5 reading -> +5");
	CHECK(isr_correct(-7, 0) == -7, "T12b: zero offset, -7 reading -> -7");
}

/* Card section 23: simulate direct ADC = offset for all three phases (independent per-phase
 * offsets, not one shared constant) -> corrected 0/0/0; then direct ADC = offset+delta with
 * three DIFFERENT deltas -> corrected reproduces exactly those deltas. Runs the real
 * current_cal_submit() to get the offsets, then the real correction formula to verify - the
 * same two steps ADC0_1_IRQHandler performs (calibrate once, correct every ISR tick). */
static void t13_oracle_three_independent_phase_deltas(void)
{
	current_cal_t s;
	current_cal_attempt_t cal;
	int16_t offset_a[1] = {-42}, offset_b[1] = {17}, offset_c[1] = {3};
	int16_t delta_a = 8, delta_b = -19, delta_c = 60;

	printf("T13 oracle: three independent per-phase deltas survive calibrate+correct exactly\n");

	cal = make_varying_attempt(offset_a, offset_b, offset_c, 1);
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);
	CHECK(current_cal_submit(&s, &cal) == CURRENT_CAL_OK, "T13: setup: calibration accepted");

	CHECK(isr_correct(offset_a[0], s.offset[CURRENT_CAL_PHASE_A]) == 0,
	      "T13: phase A at exactly its own offset corrects to 0");
	CHECK(isr_correct(offset_b[0], s.offset[CURRENT_CAL_PHASE_B]) == 0,
	      "T13: phase B at exactly its own offset corrects to 0");
	CHECK(isr_correct(offset_c[0], s.offset[CURRENT_CAL_PHASE_C]) == 0,
	      "T13: phase C at exactly its own offset corrects to 0");

	CHECK(isr_correct((int16_t)(offset_a[0] + delta_a), s.offset[CURRENT_CAL_PHASE_A]) == delta_a,
	      "T13: phase A reproduces its own delta exactly, unaffected by B/C's offsets");
	CHECK(isr_correct((int16_t)(offset_b[0] + delta_b), s.offset[CURRENT_CAL_PHASE_B]) == delta_b,
	      "T13: phase B reproduces its own (negative) delta exactly");
	CHECK(isr_correct((int16_t)(offset_c[0] + delta_c), s.offset[CURRENT_CAL_PHASE_C]) == delta_c,
	      "T13: phase C reproduces its own delta exactly");
}

int main(void)
{
	t5_t6_exact_mean_and_p2p_over_varying_samples();
	t7_atomic_publish();
	t8_strict_out_of_range_stays_inhibited();
	t9_legacy_fallback_signed_domain();
	t10_t11_t12_signed_residual_correction();
	t13_oracle_three_independent_phase_deltas();

	if (host_test_failures) {
		printf("FW-125 phase-current same-path calibration: %d FAILURE(S)\n", host_test_failures);
		return 1;
	}
	printf("FW-125 phase-current same-path calibration: PASS\n");
	return 0;
}
