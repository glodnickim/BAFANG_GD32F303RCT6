#include "current_feedback.h"

#include <string.h>

/*
 * FW-127D. See inc/current_feedback.h for why a substituted last-valid beats both zero and a
 * silent hold-in-place, and for why no maximum age is defined yet. Pure state: no register is
 * touched, which is what makes it host-testable.
 */

static current_feedback_state_t fb;

void current_feedback_reset(void)
{
	/* Ownership only. The counters below are session evidence and survive on purpose. */
	fb.last_a = 0;
	fb.last_b = 0;
	fb.last_c = 0;
	fb.have_last = 0U;
	fb.sample_age = 0U;
}

const current_feedback_state_t *current_feedback_get(void)
{
	return &fb;
}

static void note_age(uint32_t age)
{
	uint8_t b;

	if (age > fb.max_sample_age) fb.max_sample_age = age;

	/* age 1 -> 0, 2-3 -> 1, 4-7 -> 2, 8+ -> 3. A long tail stays visible without an array. */
	if (age <= 1U)      b = 0U;
	else if (age <= 3U) b = 1U;
	else if (age <= 7U) b = 2U;
	else                b = 3U;
	if (fb.age_bucket[b] < 0xFFFFFFFFu) fb.age_bucket[b]++;
}

uint8_t current_feedback_update(uint8_t sample_state, int16_t *i_a, int16_t *i_b, int16_t *i_c)
{
	if (!i_a || !i_b || !i_c) return 0U;

	if (sample_state == (uint8_t)CURRENT_SAMPLE_PRIMARY ||
	    sample_state == (uint8_t)CURRENT_SAMPLE_ALTERNATE) {
		if (sample_state == (uint8_t)CURRENT_SAMPLE_PRIMARY) {
			if (fb.primary_count < 0xFFFFFFFFu) fb.primary_count++;
		} else {
			if (fb.alternate_count < 0xFFFFFFFFu) fb.alternate_count++;
		}

		/* A trustworthy reading: use it as it is, and it becomes the substitute for whatever
		 * comes next. The age resets here and only here. */
		fb.last_a = *i_a;
		fb.last_b = *i_b;
		fb.last_c = *i_c;
		fb.have_last = 1U;
		fb.sample_age = 0U;
		return 1U;
	}

	/* INVALID from here down. */
	if (fb.invalid_count < 0xFFFFFFFFu) fb.invalid_count++;

	if (!fb.have_last) {
		/*
		 * Nothing trustworthy has been measured in this run yet, so there is nothing honest to
		 * substitute. Refusing is the only correct answer: handing back zero would tell the PI
		 * the motor draws no current, and handing back the struct's initial values would be
		 * inventing a measurement. The caller must not run current control on this.
		 */
		if (fb.starved_count < 0xFFFFFFFFu) fb.starved_count++;
		return 0U;
	}

	/* Substitute the last trustworthy reading, and make the staleness visible. The suspect
	 * fresh values are overwritten here - that is the whole point: they must not reach FOC. */
	*i_a = fb.last_a;
	*i_b = fb.last_b;
	*i_c = fb.last_c;
	if (fb.sample_age < 0xFFFFFFFFu) fb.sample_age++;
	if (fb.reuse_count < 0xFFFFFFFFu) fb.reuse_count++;
	note_age(fb.sample_age);
	return 1U;
}
