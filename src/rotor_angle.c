#include "rotor_angle.h"

/*
 * 10923 << 16 == 715849728 == one Hall sector. The division is done on the scaled value before the
 * shift, exactly as the legacy formula in main.c did it, so the arithmetic and its rounding are
 * unchanged - only the bound, the fallback and the ownership are new.
 */
#define ROTOR_ANGLE_SCALE_60 10923

static int32_t abs32(int32_t v)
{
	return (v < 0) ? -v : v;
}

void rotor_angle_reset(rotor_angle_state_t *state)
{
	if (state == 0) {
		return;
	}
	/*
	 * A fresh state is UNTRUSTED with no edges counted: no timing has been measured, so none is
	 * believed. The offset it will produce is the sector centre, not the boundary - see the header
	 * for why that difference is the one a start begins on.
	 */
	state->trusted = 0U;
	state->edges = 0U;
	state->last_direction = 0;
	state->prev_tim2 = 0U;
	state->have_prev_tim2 = 0U;
}

int32_t rotor_angle_sector_offset(const rotor_angle_input_t *input)
{
	if (input == 0) {
		return 0;
	}
	uint32_t period = input->tics_filtered_8 >> 3;
	if (period == 0U) {
		return 0;
	}
	uint32_t elapsed = input->tim2_recent;
	/*
	 * BOUNDED EXTRAPOLATION. Past the end of its own sector the interpolation is no longer
	 * interpolating - it is guessing, and it guesses further the slower the rotor gets, which is
	 * precisely when it is least entitled to. Clamped here, the angle waits at the sector end for
	 * the edge that must come, instead of running ahead of the rotor.
	 */
	if (elapsed > period) {
		elapsed = period;
	}
	int32_t magnitude =
		(int32_t)(((uint32_t)ROTOR_ANGLE_SCALE_60 * elapsed) / period) << 16;
	if (input->direction < 0) {
		return -magnitude;
	}
	if (input->direction > 0) {
		return magnitude;
	}
	return 0;
}

/*
 * The sign the sector extends in. The measured direction rules as soon as one has ever been seen;
 * before that - a cold boot at standstill - the caller's fallback stands in, and it is the same
 * sign the legacy six-step branch used, so nothing about a cold start changes.
 */
static int32_t centre_sign(const rotor_angle_state_t *state, const rotor_angle_input_t *input)
{
	if (state->last_direction < 0) {
		return -1;
	}
	if (state->last_direction > 0) {
		return 1;
	}
	return (input->fallback_sign < 0) ? -1 : 1;
}

int32_t rotor_angle_update(rotor_angle_state_t *state, const rotor_angle_input_t *input)
{
	if (state == 0 || input == 0) {
		return 0;
	}

	/* Latch the measured direction so a standstill keeps the sign it last actually saw. */
	if (input->direction > 0) {
		state->last_direction = 1;
	} else if (input->direction < 0) {
		state->last_direction = -1;
	}

	/*
	 * Hall edge detection. The hardware timer restarts at every edge, so the time-since-edge
	 * counting DOWN is the edge - the same technique walk_assist_motor.c uses on the same signal.
	 */
	uint16_t tim2 = (uint16_t)((input->tim2_recent > 0xFFFFU) ? 0xFFFFU : input->tim2_recent);
	bool edge = state->have_prev_tim2 != 0U && tim2 < state->prev_tim2;
	state->prev_tim2 = tim2;
	state->have_prev_tim2 = 1U;
	if (edge && state->edges < ROTOR_ANGLE_TRUST_EDGES) {
		state->edges++;
	}

	int32_t interp = rotor_angle_sector_offset(input);
	int32_t centre = centre_sign(state, input) * ROTOR_ANGLE_DEG_30;

	/*
	 * May the timing be believed at all? Two independent conditions, and BOTH have to hold:
	 *
	 *   - the speed hysteresis says the sector period is in a range where interpolating means
	 *     anything (this is the pre-existing six-step threshold, unchanged), and
	 *   - at least ROTOR_ANGLE_TRUST_EDGES edges have been seen since the anchor. After a
	 *     standstill the only period available is a stale one from before the stop, and one
	 *     uncertain sample is not a speed. This is the manufacturer's rule, not an invention.
	 */
	bool may_trust = !input->want_untrusted &&
		state->edges >= ROTOR_ANGLE_TRUST_EDGES;

	if (state->trusted != (may_trust ? 1U : 0U)) {
		if (!may_trust && (input->stalled || state->edges < ROTOR_ANGLE_TRUST_EDGES)) {
			/*
			 * Losing trust because the rotor stopped or the timing became meaningless cannot
			 * wait for a midpoint that no further edge will bring. Drop to the centre now, and
			 * make the next spin-up earn its trust again from zero edges.
			 */
			state->trusted = 0U;
			state->edges = 0U;
		} else if (abs32(interp) >= ROTOR_ANGLE_DEG_30) {
			/*
			 * THE BUMPLESS POINT. With the sector CENTRE as the untrusted value, the two
			 * formulas agree at exactly one place per sector - where the interpolation passes
			 * the midpoint. Waiting for it makes the change of formula a change of nothing.
			 * Symmetric in both directions, and bounded by one sector.
			 */
			state->trusted = may_trust ? 1U : 0U;
		}
	}

	int32_t base = input->hall_angle + input->angle_correction;
	return base + (state->trusted ? interp : centre);
}
