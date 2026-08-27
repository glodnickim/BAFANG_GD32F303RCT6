#include "sample_window.h"
#include "pwm_geometry.h"

/*
 * FW-127C. See inc/sample_window.h for the derivation of every constant and for why the trigger
 * is a derived position rather than something that moves with duty. Pure arithmetic: no register
 * is touched here, which is what makes it host-testable.
 */

static const uint8_t DIRECT_BIT[CURRENT_SAMPLE_PHASES] = {
	CURRENT_SAMPLE_DIRECT_A, CURRENT_SAMPLE_DIRECT_B, CURRENT_SAMPLE_DIRECT_C
};

void sample_window_decide(const uint16_t applied[CURRENT_SAMPLE_PHASES], uint16_t arr,
                          uint16_t trigger_ccr, sample_window_t *out)
{
	uint8_t i, direct_mask = 0U, direct_count = 0U;
	uint8_t blocked = SAMPLE_WINDOW_RECONSTRUCT_NONE, blocked_count = 0U;
	int32_t latest_start;

	if (!out) return;
	out->state = (uint8_t)CURRENT_SAMPLE_INVALID;
	out->sector = SAMPLE_WINDOW_RECONSTRUCT_NONE;
	out->direct_mask = 0U;
	out->trigger_ccr = trigger_ccr;
	if (!applied) return;

	/*
	 * The trigger itself must be a compare the timer can match inside a period. An illegal one
	 * is never programmed - it is reported as INVALID, which is the whole point of returning a
	 * verdict instead of computing a value and clamping it into a different sampling instant
	 * than the one that was reasoned about.
	 */
	if (!pwm_geometry_compare_legal((int32_t)trigger_ccr, arr)) return;

	/*
	 * The latest CCR a phase may have and still conduct throughout the aperture. On the DOWN
	 * slope the aperture runs from CH3 down to CH3 - T_ACQ, and the low side turns off as CNT
	 * falls below CCR, so the aperture must end above CCR by the dead-time plus a settling
	 * allowance. See the derivation in the header.
	 */
	latest_start = (int32_t)trigger_ccr - (int32_t)SAMPLE_WINDOW_GUARD_TICKS;
	if (latest_start < 0) return;      /* no usable aperture exists at all at this trigger */

	for (i = 0; i < CURRENT_SAMPLE_PHASES; i++) {
		if ((int32_t)applied[i] <= latest_start) {
			direct_mask |= DIRECT_BIT[i];
			direct_count++;
		} else {
			blocked = i;
			blocked_count++;
		}
	}

	if (direct_count == CURRENT_SAMPLE_PHASES) {
		/*
		 * PRIMARY: every shunt conducts through the whole aperture, so all three phases are
		 * measured directly and nothing is reconstructed. This is the normal case at ordinary
		 * duty - 1875 + 397 = 2272, comfortably inside a 3740 trigger.
		 *
		 * Note this is a deliberate improvement on the old behaviour, which ALWAYS reconstructed
		 * whichever phase had the highest duty, even when that phase was perfectly measurable.
		 * A direct reading beats a reconstructed one whenever it is trustworthy.
		 */
		out->state = (uint8_t)CURRENT_SAMPLE_PRIMARY;
		out->sector = SAMPLE_WINDOW_RECONSTRUCT_NONE;
		out->direct_mask = direct_mask;
		return;
	}

	if (blocked_count == 1U) {
		/* ALTERNATE: one phase cannot be trusted; the other two can, so 2-of-3 recovers it. */
		out->state = (uint8_t)CURRENT_SAMPLE_ALTERNATE;
		out->sector = blocked;
		out->direct_mask = direct_mask;
		return;
	}

	/*
	 * Two or more phases unusable. 2-of-3 needs two trustworthy readings, so there is nothing
	 * honest to build a current vector from. INVALID - and deliberately NOT a "closest possible"
	 * trigger: no legal CH3 improves this, because the aperture is already as high as the timer
	 * allows and moving it down only makes fewer phases qualify.
	 */
	out->state = (uint8_t)CURRENT_SAMPLE_INVALID;
	out->sector = SAMPLE_WINDOW_RECONSTRUCT_NONE;
	out->direct_mask = direct_mask;   /* recorded as evidence, not as permission */
}

void sample_window_reconstruct(uint8_t sector, int16_t *i_a, int16_t *i_b, int16_t *i_c)
{
	if (!i_a || !i_b || !i_c) return;

	/* Kirchhoff: ia + ib + ic = 0, so the blocked phase is minus the sum of the other two. */
	switch (sector) {
	case 0: *i_a = (int16_t)(-(*i_b) - (*i_c)); break;
	case 1: *i_b = (int16_t)(-(*i_a) - (*i_c)); break;
	case 2: *i_c = (int16_t)(-(*i_a) - (*i_b)); break;
	default:
		/* RECONSTRUCT_NONE: every phase was measured directly. Nothing to rebuild - and
		 * overwriting one anyway would replace a good reading with an arithmetic identity. */
		break;
	}
}
