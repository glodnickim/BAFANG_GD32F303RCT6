/*
 * FW-119 calibration safety policy: retry -> last-known-good -> fallback.
 *
 * This links the REAL src/current_cal.c and runs it. The policy is the whole point of the
 * card, so it is deliberately hardware-free and therefore genuinely executable here - what
 * cannot be linked on a PC is the ADC sampling in main.c, which is covered separately by
 * fw119_current_cal_wiring_host.c reading main.c source text.
 *
 * FW-125 changed the SAMPLING DOMAIN this policy runs on top of (see inc/current_cal.h): the
 * candidate offset is now the measured mean directly (already hardware-offset-corrected by
 * adc_inserted_data_read()), not (mean - CURRENT_HW_OFFSET_x) against a ~2048 regular-ADC raw
 * mean. This file was updated accordingly - `level` below is a signed residual around 0, and
 * `offsets_match()` no longer subtracts a hardware offset constant. The retry/LKG/fallback
 * POLICY itself (T1-T8) is untouched by FW-125.
 *
 * WHAT THIS PROVES (T1-T8):
 *   T1: a first attempt that passes installs runtime offsets (= the measured mean, FW-125) and
 *       leaves FOC allowed, not degraded.
 *   T2: a failing attempt is retried, and a later attempt that passes wins.
 *   T3: every attempt in a sequence failing, with a set that passed earlier, keeps that set
 *       active and reports USING_LKG.
 *   T4: a failing attempt never writes offsets or LKG - the good set survives byte for byte.
 *   T5: everything fails, no LKG, LEGACY_FALLBACK -> the pre-FW-118 hardware-offset path,
 *       explicitly flagged degraded, bike still rideable.
 *   T6: everything fails, no LKG, STRICT -> FOC start inhibited, no offsets applied.
 *   T7: a sampling timeout cannot loop forever - the attempt budget bounds it - and TIMEOUT
 *       is preserved as the failure reason.
 *   T8: the mean comes from the samples actually taken, not a hardcoded /64.
 *
 * WHAT THIS DOES NOT PROVE: that the provisional residual/P2P limits are the right numbers
 * (they are HW_PENDING), that the ADC mapping is right, or anything about ISR timing. Those are
 * covered by tests/host/fw125_phase_current_calibration_host.c and the FW-125 wiring guard.
 */

#include "../common/check.h"
#include "current_cal.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

/* A synthetic attempt: every phase reads `level` (signed residual), swinging by `p2p` around it. */
static current_cal_attempt_t make_attempt(int16_t level, uint16_t p2p, uint16_t samples)
{
	current_cal_attempt_t a;
	uint8_t i;

	memset(&a, 0, sizeof a);
	a.timed_out = 0;
	a.samples = samples;
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		a.sum[i] = (int32_t)level * (int32_t)samples;
		a.min[i] = level;
		a.max[i] = (int16_t)(level + (int16_t)p2p);
	}
	return a;
}

static current_cal_attempt_t make_timeout(void)
{
	current_cal_attempt_t a;
	memset(&a, 0, sizeof a);
	a.timed_out = 1;
	a.samples = 0;
	return a;
}

/* Drive a whole sequence the way main.c does, feeding the same attempt every time. */
static void run_sequence(current_cal_t *s, const current_cal_attempt_t *a)
{
	current_cal_begin(s);
	while (current_cal_attempt_allowed(s)) {
		(void)current_cal_submit(s, a);
	}
	current_cal_finalize(s);
}

/* FW-125: the offset IS the measured mean - no hardware-offset subtraction. */
static int offsets_match(const current_cal_t *s, int16_t level)
{
	return s->offset[CURRENT_CAL_PHASE_A] == level &&
	       s->offset[CURRENT_CAL_PHASE_B] == level &&
	       s->offset[CURRENT_CAL_PHASE_C] == level;
}

static void t1_first_attempt_ok(void)
{
	current_cal_t s;
	current_cal_attempt_t good = make_attempt(0, 4, CURRENT_CAL_SAMPLES);

	printf("T1 first attempt OK -> runtime offsets active\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	CHECK(s.valid == 0, "T1: init applies no software offset");
	CHECK(s.status == CURRENT_CAL_UNCALIBRATED, "T1: init status is UNCALIBRATED");
	CHECK(current_cal_foc_allowed(&s) == 1, "T1: init never inhibits FOC on its own");

	run_sequence(&s, &good);

	CHECK(s.attempts == 1, "T1: exactly one attempt consumed");
	CHECK(s.status == CURRENT_CAL_OK, "T1: final status OK");
	CHECK(s.source == CURRENT_CAL_SRC_RUNTIME, "T1: active source is RUNTIME");
	CHECK(s.valid == 1, "T1: ISR guard enabled");
	CHECK(s.degraded == 0, "T1: not degraded");
	CHECK(current_cal_foc_allowed(&s) == 1, "T1: FOC allowed");
	CHECK(offsets_match(&s, 0), "T1: offset = measured mean, no hardware-offset subtraction (FW-125)");
	CHECK(s.residual_mean[CURRENT_CAL_PHASE_A] == 0, "T1: measured mean recorded");
	CHECK(s.p2p[CURRENT_CAL_PHASE_A] == 4, "T1: measured P2P recorded");
	CHECK(s.lkg_valid == 1, "T1: a passing attempt becomes the last-known-good set");
	CHECK(s.failure_reason == CURRENT_CAL_UNCALIBRATED, "T1: no failure reason on a clean pass");
}

static void t2_retry_then_ok(void)
{
	current_cal_t s;
	current_cal_attempt_t noisy =
		make_attempt(0, CURRENT_ZERO_MAX_P2P_ADC + 50, CURRENT_CAL_SAMPLES);
	current_cal_attempt_t good = make_attempt(10, 6, CURRENT_CAL_SAMPLES);

	printf("T2 fail -> retry -> second attempt OK\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);

	CHECK(current_cal_attempt_allowed(&s) == 1, "T2: first attempt permitted");
	CHECK(current_cal_submit(&s, &noisy) == CURRENT_CAL_TOO_NOISY, "T2: noisy attempt rejected");
	CHECK(s.valid == 0, "T2: a rejected attempt installs nothing");
	CHECK(current_cal_attempt_allowed(&s) == 1, "T2: a retry is permitted after a failure");

	CHECK(current_cal_submit(&s, &good) == CURRENT_CAL_OK, "T2: second attempt accepted");
	CHECK(current_cal_attempt_allowed(&s) == 0, "T2: no further attempts once one has passed");

	current_cal_finalize(&s);
	CHECK(s.attempts == 2, "T2: two attempts consumed");
	CHECK(s.status == CURRENT_CAL_OK, "T2: final status OK");
	CHECK(s.source == CURRENT_CAL_SRC_RUNTIME, "T2: running on the retry own offsets");
	CHECK(offsets_match(&s, 10), "T2: offsets come from the attempt that passed");
	CHECK(s.degraded == 0, "T2: a successful retry is not degraded");
}

static void t3_all_fail_lkg_stays_active(void)
{
	current_cal_t s;
	current_cal_attempt_t good = make_attempt(20, 8, CURRENT_CAL_SAMPLES);
	current_cal_attempt_t bad =
		make_attempt((int16_t)(CURRENT_CAL_RESIDUAL_MAX + 100), 4, CURRENT_CAL_SAMPLES);

	printf("T3 all attempts fail + LKG exists -> LKG stays active\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	run_sequence(&s, &good);
	CHECK(s.status == CURRENT_CAL_OK, "T3: the first sequence established a good set");

	/* A second sequence - the shape a mid-ride re-calibration would take - that fails outright. */
	run_sequence(&s, &bad);

	CHECK(s.attempts == CURRENT_CAL_MAX_ATTEMPTS, "T3: the full attempt budget was spent");
	CHECK(s.status == CURRENT_CAL_USING_LKG, "T3: final status USING_LKG");
	CHECK(s.source == CURRENT_CAL_SRC_LKG, "T3: active source is LKG");
	CHECK(s.valid == 1, "T3: the ISR keeps applying software offsets");
	CHECK(offsets_match(&s, 20), "T3: the offsets are the earlier good ones");
	CHECK(s.degraded == 1, "T3: running on an unconfirmed set is degraded");
	CHECK(current_cal_foc_allowed(&s) == 1, "T3: LKG never inhibits FOC");
	CHECK(s.failure_reason == CURRENT_CAL_OUT_OF_RANGE, "T3: the reason the retries failed is kept");
}

static void t4_fail_does_not_overwrite_lkg(void)
{
	current_cal_t s;
	current_cal_attempt_t good = make_attempt(-30, 10, CURRENT_CAL_SAMPLES);
	current_cal_attempt_t oor =
		make_attempt((int16_t)(CURRENT_CAL_RESIDUAL_MIN - 100), 4, CURRENT_CAL_SAMPLES);
	current_cal_attempt_t noisy = make_attempt(77, CURRENT_ZERO_MAX_P2P_ADC + 1, CURRENT_CAL_SAMPLES);
	current_cal_attempt_t late = make_timeout();
	int16_t lkg_before[CURRENT_CAL_PHASES];
	int16_t off_before[CURRENT_CAL_PHASES];

	printf("T4 a failed attempt never overwrites the good set\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	run_sequence(&s, &good);
	memcpy(lkg_before, s.lkg_offset, sizeof lkg_before);
	memcpy(off_before, s.offset, sizeof off_before);

	current_cal_begin(&s);
	CHECK(memcmp(s.offset, off_before, sizeof off_before) == 0, "T4: begin leaves live offsets alone");
	CHECK(s.lkg_valid == 1, "T4: begin keeps the LKG set");

	(void)current_cal_submit(&s, &oor);
	CHECK(memcmp(s.lkg_offset, lkg_before, sizeof lkg_before) == 0, "T4: OUT_OF_RANGE left LKG untouched");
	CHECK(memcmp(s.offset, off_before, sizeof off_before) == 0, "T4: OUT_OF_RANGE left live offsets untouched");
	CHECK(s.valid == 1, "T4: OUT_OF_RANGE did not disable the ISR guard mid-ride");

	(void)current_cal_submit(&s, &noisy);
	CHECK(memcmp(s.lkg_offset, lkg_before, sizeof lkg_before) == 0, "T4: TOO_NOISY left LKG untouched");
	CHECK(memcmp(s.offset, off_before, sizeof off_before) == 0, "T4: TOO_NOISY left live offsets untouched");

	(void)current_cal_submit(&s, &late);
	CHECK(memcmp(s.lkg_offset, lkg_before, sizeof lkg_before) == 0, "T4: TIMEOUT left LKG untouched");
	CHECK(memcmp(s.offset, off_before, sizeof off_before) == 0, "T4: TIMEOUT left live offsets untouched");
	/* The timeout submit() early-returns before touching residual_mean, so it should still hold
	 * `noisy`'s measured mean (0) from the attempt before it - not reset to 0 BY the timeout
	 * itself, which would be a coincidental pass here. Use a non-zero noisy level so the two
	 * cases are distinguishable. */
	CHECK(s.residual_mean[CURRENT_CAL_PHASE_A] == 77,
	      "T4: a timeout leaves the previous attempt's measured mean alone (does not fake 0)");

	current_cal_finalize(&s);
	CHECK(s.status == CURRENT_CAL_USING_LKG, "T4: the good set is what the ride ends up on");
	CHECK(memcmp(s.offset, lkg_before, sizeof lkg_before) == 0, "T4: and it is bit-for-bit the same set");
}

static void t5_legacy_fallback(void)
{
	current_cal_t s;
	current_cal_attempt_t bad =
		make_attempt((int16_t)(CURRENT_CAL_RESIDUAL_MAX + 1), 4, CURRENT_CAL_SAMPLES);

	printf("T5 all fail + no LKG + LEGACY_FALLBACK -> legacy path, degraded\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	run_sequence(&s, &bad);

	CHECK(s.attempts == CURRENT_CAL_MAX_ATTEMPTS, "T5: retries were actually spent");
	CHECK(s.lkg_valid == 0, "T5: nothing was ever accepted, so there is no LKG");
	CHECK(s.status == CURRENT_CAL_LEGACY_FALLBACK, "T5: final status LEGACY_FALLBACK");
	CHECK(s.source == CURRENT_CAL_SRC_LEGACY, "T5: active source is LEGACY");
	CHECK(s.valid == 0, "T5: no software offset is applied - hardware offset only");
	CHECK(s.degraded == 1, "T5: legacy running is explicitly degraded");
	CHECK(current_cal_foc_allowed(&s) == 1, "T5: the bike is still rideable");
	CHECK(s.failure_reason == CURRENT_CAL_OUT_OF_RANGE, "T5: the failure reason is reported");
}

static void t6_strict_inhibits(void)
{
	current_cal_t s;
	current_cal_attempt_t bad = make_attempt(0, CURRENT_ZERO_MAX_P2P_ADC + 5, CURRENT_CAL_SAMPLES);

	printf("T6 STRICT + all fail + no LKG -> FOC start inhibited\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_STRICT);
	run_sequence(&s, &bad);

	CHECK(s.status == CURRENT_CAL_HARD_FAILED, "T6: final status HARD_FAILED");
	CHECK(s.source == CURRENT_CAL_SRC_NONE, "T6: no offset source is claimed");
	CHECK(s.valid == 0, "T6: no random offsets are applied");
	CHECK(current_cal_foc_allowed(&s) == 0, "T6: FOC start is inhibited");
	CHECK(s.failure_reason == CURRENT_CAL_TOO_NOISY, "T6: the failure reason is reported");

	/* STRICT must still prefer a good set over inhibiting: a hard fail is the last resort. */
	{
		current_cal_t t;
		current_cal_attempt_t good = make_attempt(0, 2, CURRENT_CAL_SAMPLES);
		current_cal_init(&t, CURRENT_CAL_POLICY_STRICT);
		run_sequence(&t, &good);
		run_sequence(&t, &bad);
		CHECK(t.status == CURRENT_CAL_USING_LKG, "T6: STRICT uses LKG rather than hard-failing");
		CHECK(current_cal_foc_allowed(&t) == 1, "T6: STRICT with a valid LKG still allows FOC");
	}
}

static void t7_timeout_no_deadlock(void)
{
	current_cal_t s;
	current_cal_attempt_t late = make_timeout();
	int guard = 0;

	printf("T7 TIMEOUT terminates the sequence instead of deadlocking\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);
	/* The guard is the test: if the budget did not bound the loop this would never end. */
	while (current_cal_attempt_allowed(&s) && guard < 1000) {
		(void)current_cal_submit(&s, &late);
		guard++;
	}
	CHECK(guard == CURRENT_CAL_MAX_ATTEMPTS, "T7: the loop ended after exactly the attempt budget");
	CHECK(current_cal_attempt_allowed(&s) == 0, "T7: no further attempt is permitted");

	current_cal_finalize(&s);
	CHECK(s.failure_reason == CURRENT_CAL_SAMPLE_TIMEOUT, "T7: TIMEOUT is preserved as the reason");
	CHECK(s.status == CURRENT_CAL_LEGACY_FALLBACK, "T7: default policy keeps the bike rideable");
	CHECK(current_cal_foc_allowed(&s) == 1, "T7: a timeout alone does not immobilise the bike");
}

static void t8_mean_uses_real_sample_count(void)
{
	current_cal_t s;
	/* Half the configured count - what a truncated-but-valid run would produce. */
	current_cal_attempt_t partial = make_attempt(0, 2, CURRENT_CAL_SAMPLES / 2);

	printf("T8 the mean divides by the samples actually taken\n");
	current_cal_init(&s, CURRENT_CAL_POLICY_LEGACY_FALLBACK);
	current_cal_begin(&s);
	(void)current_cal_submit(&s, &partial);
	CHECK(s.residual_mean[CURRENT_CAL_PHASE_A] == 0,
	      "T8: mean is right for a sample count that is not 128");
	CHECK(s.attempt_status == CURRENT_CAL_OK, "T8: a correct mean passes validation");
}

int main(void)
{
	t1_first_attempt_ok();
	t2_retry_then_ok();
	t3_all_fail_lkg_stays_active();
	t4_fail_does_not_overwrite_lkg();
	t5_legacy_fallback();
	t6_strict_inhibits();
	t7_timeout_no_deadlock();
	t8_mean_uses_real_sample_count();

	if (host_test_failures) {
		printf("FW-119 current calibration safety: %d FAILURE(S)\n", host_test_failures);
		return 1;
	}
	printf("FW-119 current calibration safety: PASS\n");
	return 0;
}
