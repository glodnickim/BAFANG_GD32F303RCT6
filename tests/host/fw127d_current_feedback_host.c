/*
 * FW-127D: the validity / last-valid model, against the REAL module (src/current_feedback.c).
 *
 * WHAT THIS PROTECTS. Until FW-127 the only predicate anywhere was "the conversion completed".
 * That proves the ADC finished and nothing about whether the shunts were conducting, so a sample
 * taken outside the conduction window went straight into Clarke, Park and the PI regulators with
 * no marker of any kind.
 *
 * D3/D4 are the tests that matter: an INVALID sample must not reach the control loop, and must
 * not be allowed to poison the substitute either.
 */

#include "common/check.h"
#include "../../inc/current_feedback.h"

#include <stdio.h>

#define PRIMARY   ((uint8_t)CURRENT_SAMPLE_PRIMARY)
#define ALTERNATE ((uint8_t)CURRENT_SAMPLE_ALTERNATE)
#define INVALID   ((uint8_t)CURRENT_SAMPLE_INVALID)

int main(void)
{
	int16_t a, b, c;

	/* --- D5: before any trustworthy sample there is nothing to fall back on -------------- */
	current_feedback_reset();
	a = 111; b = 222; c = 333;
	CHECK(current_feedback_update(INVALID, &a, &b, &c) == 0,
	      "D5a. an INVALID sample with no history is refused - the caller must not run control");
	CHECK(current_feedback_get()->starved_count == 1, "D5b. ...and the starvation is counted");
	CHECK(current_feedback_get()->have_last == 0, "D5c. nothing was invented as a substitute");

	/* --- D1/D2/D6: a trustworthy sample is used as it is and becomes the substitute -------- */
	current_feedback_reset();
	a = 10; b = -20; c = 30;
	CHECK(current_feedback_update(PRIMARY, &a, &b, &c) == 1, "D1a. PRIMARY is usable");
	CHECK(a == 10 && b == -20 && c == 30, "D1b. ...and is passed through untouched");
	CHECK(current_feedback_get()->sample_age == 0, "D1c. the age resets on a good sample");
	CHECK(current_feedback_get()->have_last == 1, "D6. the first good sample establishes last-valid");

	a = 40; b = -50; c = 10;
	CHECK(current_feedback_update(ALTERNATE, &a, &b, &c) == 1, "D2a. ALTERNATE is equally usable");
	CHECK(a == 40 && b == -50 && c == 10, "D2b. ...and is also passed through untouched");
	CHECK(current_feedback_get()->sample_age == 0, "D2c. it too resets the age");

	/* --- D3/D4: THE point of this card ------------------------------------------------------ */
	{
		/* the suspect fresh values must never survive into the control loop */
		a = 30000; b = -30000; c = 12345;
		CHECK(current_feedback_update(INVALID, &a, &b, &c) == 1,
		      "D3a. an INVALID sample is served from last-valid, not refused, once history exists");
		CHECK(a == 40 && b == -50 && c == 10,
		      "D3b. the suspect fresh values are REPLACED - they never reach FOC");
		CHECK(current_feedback_get()->sample_age == 1, "D3c. the age advances");
		CHECK(current_feedback_get()->reuse_count == 1, "D3d. the reuse is counted");

		/* and the bad values must not become the new substitute */
		a = 0; b = 0; c = 0;
		(void)current_feedback_update(INVALID, &a, &b, &c);
		CHECK(a == 40 && b == -50 && c == 10,
		      "D4. an INVALID sample never updates last-valid - the substitute stays the good one");
		CHECK(current_feedback_get()->sample_age == 2, "D4b. ...and the age keeps advancing");
	}

	/* --- D7: a fresh good sample clears the age --------------------------------------------- */
	{
		a = 7; b = 8; c = -15;
		CHECK(current_feedback_update(PRIMARY, &a, &b, &c) == 1, "D7a. a good sample returns");
		CHECK(current_feedback_get()->sample_age == 0, "D7b. the age resets");
		a = 0; b = 0; c = 0;
		(void)current_feedback_update(INVALID, &a, &b, &c);
		CHECK(a == 7 && b == 8 && c == -15, "D7c. the substitute is now the NEWEST good sample");
	}

	/* --- D8: a rearm drops ownership, but keeps the session evidence -------------------------- */
	{
		const current_feedback_state_t *s = current_feedback_get();
		const uint32_t primaries = s->primary_count;
		const uint32_t reuses = s->reuse_count;

		current_feedback_reset();
		CHECK(s->have_last == 0, "D8a. a rearm drops last-valid ownership");
		CHECK(s->sample_age == 0, "D8b. ...and the age");
		CHECK(s->primary_count == primaries && s->reuse_count == reuses,
		      "D8c. ...but the session counters survive - a total is what the ride test asks for");

		a = 99; b = 99; c = 99;
		CHECK(current_feedback_update(INVALID, &a, &b, &c) == 0,
		      "D8d. and a current from before the shutdown can never be reused after it");
	}

	/* --- the age histogram places a long stall in the right bucket ---------------------------- */
	current_feedback_reset();
	{
		a = 1; b = 2; c = -3;
		(void)current_feedback_update(PRIMARY, &a, &b, &c);
		const uint32_t before8 = current_feedback_get()->age_bucket[3];
		for (int i = 0; i < 12; i++) {
			int16_t x = 0, y = 0, z = 0;
			(void)current_feedback_update(INVALID, &x, &y, &z);
		}
		const current_feedback_state_t *s = current_feedback_get();
		CHECK(s->sample_age == 12, "H1. twelve consecutive INVALID samples age to 12");
		CHECK(s->max_sample_age >= 12, "H2. the peak age is recorded");
		CHECK(s->age_bucket[3] > before8, "H3. a long stall lands in the 8+ bucket");
		CHECK(s->age_bucket[0] >= 1, "H4. the first of the run landed in the age-1 bucket");
	}

	/* --- counters separate the three states ---------------------------------------------------- */
	current_feedback_reset();
	{
		const current_feedback_state_t *s = current_feedback_get();
		const uint32_t p0 = s->primary_count, a0 = s->alternate_count, i0 = s->invalid_count;
		int16_t x, y, z;
		x = 1; y = 1; z = -2; (void)current_feedback_update(PRIMARY, &x, &y, &z);
		x = 1; y = 1; z = -2; (void)current_feedback_update(ALTERNATE, &x, &y, &z);
		x = 1; y = 1; z = -2; (void)current_feedback_update(INVALID, &x, &y, &z);
		CHECK(s->primary_count == p0 + 1 && s->alternate_count == a0 + 1 && s->invalid_count == i0 + 1,
		      "E1. PRIMARY, ALTERNATE and INVALID are counted separately");
	}

	/* --- defensive ------------------------------------------------------------------------------ */
	CHECK(current_feedback_update(PRIMARY, NULL, &b, &c) == 0, "N1. a null output is refused");

	if (host_test_failures == 0) {
		printf("FW-127D current feedback validity: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-127D current feedback validity: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
