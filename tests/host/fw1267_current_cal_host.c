/*
 * FW-126.7: the production phase-current calibration, exercised against the REAL module
 * (src/current_cal.c) - not a re-implementation of it.
 *
 * WHAT THIS EXISTS TO PROTECT. Four cards in a row shipped a calibration that measured a
 * SATURATED current-sense amplifier and passed every check it had, because the only quality
 * test was peak-to-peak and saturation is extremely quiet (dark spread 10 LSB, dump P2P 17-19).
 * The gate below is therefore a CONJUNCTION - window first, stability second - and the tests
 * that matter most here are the ones proving that neither half alone is enough:
 *
 *     S4  a value in the window but still moving      -> not eligible
 *     S5  a rock-steady value outside the window      -> not eligible   <-- the old defect
 *
 * The hardware numbers used as fixtures come from FW-126.5 (log 2026-08-26 15:21, DIAG 0.0440):
 * rail JDR ~ +2058/+2049/+2067, settled JDR ~ -16/-5/+8.
 */

#include "common/check.h"
#include "../../inc/current_cal.h"
#include "../../inc/config.h"

#include <stdio.h>
#include <string.h>

/* The settled reading measured on the bike. */
static const int16_t SETTLED[CURRENT_CAL_PHASES] = { -16, -5, 8 };
/* The first conversion after MOE ON - the amplifier still at its rail. */
static const int16_t RAIL[CURRENT_CAL_PHASES]    = { 2058, 2049, 2067 };

static void feed(current_cal_t *s, const int16_t v[CURRENT_CAL_PHASES], int n)
{
	for (int i = 0; i < n; i++) (void)current_cal_sample(s, v, 1U);
}

/* Drive a full, clean calibration: enough steady in-window cycles to satisfy both gates. */
static void run_good(current_cal_t *s)
{
	current_cal_init(s);
	current_cal_begin_attempt(s);
	feed(s, SETTLED, CURRENT_CAL_STABLE_CYCLES + CURRENT_CAL_COLLECT_SAMPLES + 1);
}

int main(void)
{
	current_cal_t s;

	/* --- baseline: what a fresh controller must look like ------------------------------- */
	current_cal_init(&s);
	CHECK(s.state == CURRENT_CAL_ST_UNCALIBRATED, "B1. power-on state is UNCALIBRATED");
	CHECK(s.valid == 0, "B2. no offsets are applied before anything is measured");
	CHECK(current_cal_foc_allowed(&s) == 0, "B3. active FOC is forbidden at power-on");
	CHECK(current_cal_wants_dwell(&s) == 0, "B4. nothing is asked of the dwell before an attempt");
	CHECK(current_cal_needs_calibration(&s) != 0, "B5. a calibration is wanted");

	/* --- S3: the rail sample can never be eligible --------------------------------------- */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, RAIL, CURRENT_CAL_STABLE_CYCLES + CURRENT_CAL_COLLECT_SAMPLES);
	CHECK(s.eligible == 0, "S3a. a railed reading contributes no eligible sample");
	CHECK(s.state != CURRENT_CAL_ST_VALID, "S3b. ...and cannot produce a valid calibration");

	/* The B0 transient specifically: rail once, then settle. The rail sample must not be in
	 * the set, and the result must still be the settled value. */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, RAIL, 1);
	feed(&s, SETTLED, CURRENT_CAL_STABLE_CYCLES + CURRENT_CAL_COLLECT_SAMPLES + 1);
	CHECK(s.state == CURRENT_CAL_ST_VALID, "S3c. a B0 transient does not prevent a later good set");
	CHECK(s.offset[0] == SETTLED[0] && s.offset[1] == SETTLED[1] && s.offset[2] == SETTLED[2],
	      "S3d. ...and the offsets are the SETTLED values, unpolluted by it");

	/* --- S4: in the window, but not steady -> not eligible -------------------------------- */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	for (int i = 0; i < CURRENT_CAL_STABLE_CYCLES * 4; i++) {
		/* alternates by more than the stability band, but every value is inside the window */
		int16_t v[CURRENT_CAL_PHASES];
		int16_t swing = (int16_t)((i & 1) ? (CURRENT_CAL_STABLE_BAND + 10) : 0);
		for (int p = 0; p < CURRENT_CAL_PHASES; p++) v[p] = (int16_t)(SETTLED[p] + swing);
		(void)current_cal_sample(&s, v, 1U);
	}
	CHECK(s.eligible == 0, "S4. inside the window but still moving is NOT eligible");

	/* --- S5: rock-steady, but outside the window -> not eligible ------------------------- */
	/*  THE OLD DEFECT. The dark reading was stable to 10 LSB and completely wrong. */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, RAIL, CURRENT_CAL_STABLE_CYCLES * 4);
	CHECK(s.eligible == 0, "S5a. perfectly steady but outside the window is NOT eligible");
	CHECK(s.state != CURRENT_CAL_ST_VALID, "S5b. ...and stability alone never validates");

	/* --- S6: window + stability -> collection starts ------------------------------------- */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, SETTLED, CURRENT_CAL_STABLE_CYCLES + 1);   /* +1: the first cycle only sets the reference */
	CHECK(s.state == CURRENT_CAL_ST_COLLECTING, "S6a. both gates satisfied -> COLLECTING");
	CHECK(s.eligible == 1, "S6b. the first eligible sample is the one that completed the gate");
	CHECK(s.stable_count >= CURRENT_CAL_STABLE_CYCLES, "S6c. stability was proven, not assumed");

	/* --- S7: the estimator sees only eligible samples ------------------------------------- */
	/* Rail for a while, settle, then rail again mid-collection: the restart policy must throw
	 * the partial set away rather than blend it. */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, RAIL, 3);
	feed(&s, SETTLED, CURRENT_CAL_STABLE_CYCLES + 5);
	CHECK(s.eligible == 5, "S7a. exactly the post-gate samples were accepted");
	feed(&s, RAIL, 1);
	CHECK(s.eligible == 0, "S7b. losing the gate discards the partial set instead of blending");
	CHECK(s.restarts == 1, "S7c. ...and says so");
	feed(&s, SETTLED, CURRENT_CAL_STABLE_CYCLES + CURRENT_CAL_COLLECT_SAMPLES + 1);
	CHECK(s.state == CURRENT_CAL_ST_VALID, "S7d. a clean run after the restart still succeeds");
	CHECK(s.offset[0] == SETTLED[0], "S7e. and the result is the settled value");

	/* --- the estimator itself: mean, min, max, P2P ---------------------------------------- */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, SETTLED, CURRENT_CAL_STABLE_CYCLES + 1);      /* gate satisfied, 1 eligible */
	{
		/* one sample 4 LSB high on phase A, the rest identical - inside the stability band */
		int16_t hi[CURRENT_CAL_PHASES] = { (int16_t)(SETTLED[0] + 4), SETTLED[1], SETTLED[2] };
		feed(&s, hi, 1);
		feed(&s, SETTLED, CURRENT_CAL_COLLECT_SAMPLES - 2);
	}
	CHECK(s.state == CURRENT_CAL_ST_VALID, "E1. the set completed");
	CHECK(s.eligible == CURRENT_CAL_COLLECT_SAMPLES, "E2. exactly the configured count was used");
	CHECK(s.acc[0].min == SETTLED[0] && s.acc[0].max == (int16_t)(SETTLED[0] + 4),
	      "E3. min/max span the actual eligible samples");
	CHECK(s.p2p[0] == 4, "E4. P2P is max-min over the accepted set");
	/* 31 samples at -16 plus one at -12 is -15.875. Truncation toward zero would give -15 and
	 * round a NEGATIVE phase differently from a positive one; round-to-nearest gives -16. */
	CHECK(s.mean[0] == -16, "E5. the mean rounds to nearest, symmetrically about zero");
	CHECK(s.offset[0] == s.mean[0], "E6. the installed offset IS the measured JDR mean");

	/* --- S10: the attempt is bounded ------------------------------------------------------ */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, RAIL, CURRENT_CAL_MAX_CYCLES + 2);
	CHECK(s.state == CURRENT_CAL_ST_FAILED, "S10a. an attempt that never settles FAILS");
	CHECK(s.failure_reason == CURRENT_CAL_FAIL_TIMEOUT, "S10b. ...as TIMEOUT");
	CHECK(s.timeout_hit != 0, "S10c. ...and records that the budget ran out");
	CHECK(s.cycles <= CURRENT_CAL_MAX_CYCLES + 1, "S10d. it stopped AT the budget, not past it");
	CHECK(current_cal_wants_dwell(&s) == 0, "S10e. a failed attempt stops asking for the dwell");

	/* --- S11: there is no fallback ------------------------------------------------------- */
	current_cal_init(&s);
	for (int a = 0; a < CURRENT_CAL_MAX_ATTEMPTS; a++) {
		current_cal_begin_attempt(&s);
		feed(&s, RAIL, CURRENT_CAL_MAX_CYCLES + 2);
	}
	CHECK(s.valid == 0, "S11a. after every attempt failed, nothing is installed");
	CHECK(current_cal_foc_allowed(&s) == 0, "S11b. ...and active FOC stays forbidden - no legacy path");
	CHECK(s.offset[0] == 0 && s.offset[1] == 0 && s.offset[2] == 0,
	      "S11c. ...and no stale offsets are left pretending to be valid");
	CHECK(s.source == CURRENT_CAL_SRC_NONE, "S11d. the source stays NONE, never a fallback");
	CHECK(current_cal_needs_calibration(&s) == 0, "S11e. the attempt budget is spent, not infinite");

	/* --- S12: a success is not repeated ---------------------------------------------------- */
	run_good(&s);
	CHECK(s.state == CURRENT_CAL_ST_VALID, "S12a. the good run validated");
	{
		uint8_t attempts_after_success = s.attempts;
		int16_t kept[CURRENT_CAL_PHASES];
		memcpy(kept, s.offset, sizeof(kept));
		current_cal_begin_attempt(&s);      /* a later start in the same power cycle */
		CHECK(s.state == CURRENT_CAL_ST_VALID, "S12b. a later start does not re-open calibration");
		CHECK(s.attempts == attempts_after_success, "S12c. ...and does not spend an attempt");
		CHECK(current_cal_wants_dwell(&s) == 0, "S12d. ...and does not lengthen that start's dwell");
		CHECK(memcmp(kept, s.offset, sizeof(kept)) == 0, "S12e. ...and the offsets are untouched");
		CHECK(current_cal_needs_calibration(&s) == 0, "S12f. nothing more is wanted");
	}

	/* --- S14: current_cal_foc_allowed() means exactly one thing ---------------------------- */
	current_cal_init(&s);
	CHECK(current_cal_foc_allowed(&s) == 0, "S14a. UNCALIBRATED -> false");
	current_cal_begin_attempt(&s);
	CHECK(current_cal_foc_allowed(&s) == 0, "S14b. SETTLING -> false");
	feed(&s, SETTLED, CURRENT_CAL_STABLE_CYCLES + 1);
	CHECK(s.state == CURRENT_CAL_ST_COLLECTING && current_cal_foc_allowed(&s) == 0,
	      "S14c. COLLECTING -> false");
	feed(&s, SETTLED, CURRENT_CAL_COLLECT_SAMPLES);
	CHECK(s.state == CURRENT_CAL_ST_VALID && current_cal_foc_allowed(&s) != 0,
	      "S14d. VALID -> true");
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	feed(&s, RAIL, CURRENT_CAL_MAX_CYCLES + 2);
	CHECK(current_cal_foc_allowed(&s) == 0, "S14e. FAILED -> false");

	/* --- the electrical state is required, not assumed ------------------------------------- */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	(void)current_cal_sample(&s, SETTLED, 0U);
	CHECK(s.state == CURRENT_CAL_ST_FAILED, "N1. a non-neutral bridge fails the attempt outright");
	CHECK(s.failure_reason == CURRENT_CAL_FAIL_NOT_NEUTRAL, "N2. ...with the honest reason");
	CHECK(s.neutral_ok == 0, "N3. ...and the report will not claim the state held");
	run_good(&s);
	CHECK(s.neutral_ok != 0, "N4. a clean run records that it did hold");

	/* --- a wrong-but-steady zero is rejected by the final range check ---------------------- */
	current_cal_init(&s);
	current_cal_begin_attempt(&s);
	{
		/* Just inside the per-sample window so the gate opens, then walk out of it in steps
		 * small enough to stay "steady" - the final mean must still be judged on its own. */
		int16_t v[CURRENT_CAL_PHASES] = { (int16_t)(CURRENT_CAL_RESIDUAL_MAX - 1),
		                                  (int16_t)(CURRENT_CAL_RESIDUAL_MAX - 1),
		                                  (int16_t)(CURRENT_CAL_RESIDUAL_MAX - 1) };
		feed(&s, v, CURRENT_CAL_STABLE_CYCLES + CURRENT_CAL_COLLECT_SAMPLES + 1);
	}
	CHECK(s.state == CURRENT_CAL_ST_VALID,
	      "R1. a steady value at the edge of the window is accepted (the window is the contract)");
	CHECK(s.offset[0] == (int16_t)(CURRENT_CAL_RESIDUAL_MAX - 1), "R2. ...at its measured value");

	if (host_test_failures == 0) {
		printf("FW-126.7 current calibration: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-126.7 current calibration: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
