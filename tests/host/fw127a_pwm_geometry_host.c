/*
 * FW-127A: the applied-PWM-geometry clamp, against the REAL module (src/pwm_geometry.c).
 *
 * WHAT THIS PROTECTS. Later real-SVPWM electrical-SIL evidence corrected the original FW-127
 * reachability analysis: the current svpwm() remains inside [0, ARR] throughout a 360-degree
 * sweep at _U_MAX=1920. This module is therefore a defensive last-line invariant, not a normal
 * saturation mechanism. The tests below deliberately inject illegal requests to prove they can
 * never reach the timer; normal-SVPWM reachability is tested by sim/foc_electrical_sil.c.
 *
 * So the tests below are not about tidiness. A4/A5 are the invariant this card exists for.
 */

#include "common/check.h"
#include "../../inc/pwm_geometry.h"
#include "../../inc/config.h"

#include <stdio.h>
#include <string.h>

#define ARR ((uint16_t)_T)          /* TIMER0 period: timer_initpara.period = _T */

int main(void)
{
	uint16_t applied[PWM_GEOMETRY_PHASES];
	uint8_t mask;

	/* --- A1: a request already inside the range is passed through untouched --------------- */
	pwm_geometry_init();
	{
		const int32_t req[3] = { 0, (int32_t)ARR / 2, (int32_t)ARR };
		mask = pwm_geometry_apply(req, applied, ARR);
		CHECK(mask == 0, "A1a. nothing is clamped when every phase is legal");
		CHECK(applied[0] == 0 && applied[1] == ARR / 2 && applied[2] == ARR,
		      "A1b. the values pass through unchanged, including both endpoints");
		CHECK(pwm_geometry_get_stats()->clamp_total == 0, "A1c. no clamp is counted");
	}

	/* --- A2: negative request -> legal ---------------------------------------------------- */
	pwm_geometry_init();
	{
		/* This is the case unsigned storage hid completely: -1 became 65535, a compare far
		 * above ARR, and nothing anywhere could tell. */
		const int32_t req[3] = { -1, -20000, 100 };
		mask = pwm_geometry_apply(req, applied, ARR);
		CHECK(applied[0] == 0 && applied[1] == 0, "A2a. a negative request becomes 0, not 65535");
		CHECK(applied[2] == 100, "A2b. the legal phase is untouched");
		CHECK(mask == 0x03, "A2c. exactly the two clamped phases are reported");
		CHECK(pwm_geometry_get_stats()->min_requested == -20000,
		      "A2d. the REQUEST is recorded, which is the evidence about the controller");
	}

	/* --- A3: request above ARR -> legal ---------------------------------------------------- */
	pwm_geometry_init();
	{
		const int32_t req[3] = { (int32_t)ARR + 1, 4952, 1875 };
		mask = pwm_geometry_apply(req, applied, ARR);
		CHECK(applied[0] == ARR && applied[1] == ARR, "A3a. an over-range request becomes ARR");
		CHECK(applied[2] == 1875, "A3b. the legal phase is untouched");
		CHECK(mask == 0x03, "A3c. exactly the two clamped phases are reported");
		CHECK(pwm_geometry_get_stats()->peak_requested == 4952,
		      "A3d. the peak REQUEST is recorded, not the clamped value");
	}

	/* --- A4: THE invariant - nothing outside [0, ARR] can ever be produced ----------------- */
	pwm_geometry_init();
	{
		int bad = 0;
		for (int32_t v = -8000; v <= 8000; v += 7) {
			const int32_t req[3] = { v, -v, v / 2 };
			(void)pwm_geometry_apply(req, applied, ARR);
			for (int p = 0; p < PWM_GEOMETRY_PHASES; p++) {
				if (applied[p] > ARR) bad = 1;   /* uint16: < 0 is not representable */
			}
		}
		CHECK(bad == 0, "A4. across a full sweep, every applied compare is inside [0, ARR]");
	}

	/* --- A4b: defensive containment of deliberately impossible upstream geometry ------------- */
	pwm_geometry_init();
	{
		const int32_t req[3] = { -4000, (int32_t)ARR + 4000, 1875 };
		mask = pwm_geometry_apply(req, applied, ARR);
		CHECK(mask == 0x03, "A4b. deliberately impossible A/B geometry is detected");
		CHECK(applied[0] == 0 && applied[1] == ARR && applied[2] == 1875,
		      "A4c. impossible geometry is contained without changing the legal phase");
	}

	/* --- A5: no illegal CH3 may ever be programmed ------------------------------------------ */
	{
		CHECK(pwm_geometry_compare_legal(0, ARR) != 0, "A5a. 0 is a legal compare");
		CHECK(pwm_geometry_compare_legal((int32_t)ARR, ARR) != 0, "A5b. ARR is a legal compare");
		CHECK(pwm_geometry_compare_legal((int32_t)ARR + 1, ARR) == 0,
		      "A5c. ARR+1 is refused - it can never match inside a period");
		CHECK(pwm_geometry_compare_legal(-1, ARR) == 0, "A5d. a negative compare is refused");
		CHECK(pwm_geometry_compare_legal(4942, ARR) == 0,
		      "A5e. the value the old trigger branch would have programmed is refused");
	}

	/* --- A6: the counters are exact ---------------------------------------------------------- */
	pwm_geometry_init();
	{
		const int32_t legal[3] = { 100, 200, 300 };
		const int32_t one_bad[3] = { 100, 9000, 300 };
		const int32_t two_bad[3] = { -5, 9000, 300 };

		(void)pwm_geometry_apply(legal, applied, ARR);
		(void)pwm_geometry_apply(one_bad, applied, ARR);
		(void)pwm_geometry_apply(one_bad, applied, ARR);
		(void)pwm_geometry_apply(two_bad, applied, ARR);

		const pwm_geometry_stats_t *s = pwm_geometry_get_stats();
		/* clamp_total counts APPLIES in which anything was clamped - i.e. affected PWM periods,
		 * which is the question the hardware session asks. Not per-phase hits. */
		CHECK(s->clamp_total == 3, "A6a. clamp_total counts affected applies, not phases");
		CHECK(s->clamp_phase[0] == 1, "A6b. phase A was clamped once");
		CHECK(s->clamp_phase[1] == 3, "A6c. phase B was clamped three times");
		CHECK(s->clamp_phase[2] == 0, "A6d. phase C was never clamped");
		CHECK(s->peak_requested == 9000, "A6e. peak request");
		CHECK(s->min_requested == -5, "A6f. min request");
		CHECK(s->peak_applied == ARR, "A6g. peak applied is bounded by ARR");
		CHECK(s->min_applied == 0, "A6h. min applied");
	}

	/* --- A6i: the first apply seeds min/max from measurement, not from an assumed zero ------- */
	pwm_geometry_init();
	{
		const int32_t req[3] = { 1000, 1100, 1200 };
		(void)pwm_geometry_apply(req, applied, ARR);
		const pwm_geometry_stats_t *s = pwm_geometry_get_stats();
		CHECK(s->min_requested == 1000 && s->peak_requested == 1200,
		      "A6i. min/max come from the first real apply, not from a 0 seed");
		CHECK(s->min_applied == 1000 && s->peak_applied == 1200, "A6j. ...applied likewise");
	}

	/* --- A7: init is the only reset point ---------------------------------------------------- */
	{
		const pwm_geometry_stats_t *s = pwm_geometry_get_stats();
		CHECK(s->clamp_total == 0, "A7a. a fresh init clears the evidence");
		const int32_t req[3] = { 9000, 0, 0 };
		(void)pwm_geometry_apply(req, applied, ARR);
		CHECK(s->clamp_total == 1, "A7b. and it accumulates again from there");
	}

	/* --- defensive: a null argument must not write anything --------------------------------- */
	CHECK(pwm_geometry_apply(NULL, applied, ARR) == 0, "A8a. a null request is refused");
	{
		const int32_t req[3] = { 0, 0, 0 };
		CHECK(pwm_geometry_apply(req, NULL, ARR) == 0, "A8b. a null output is refused");
	}

	if (host_test_failures == 0) {
		printf("FW-127A applied PWM geometry: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-127A applied PWM geometry: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
