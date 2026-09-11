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
	state->have_output = 0U;
	state->last_output = 0;
	state->transfer_offset = 0;
	state->prev_hall_sequence = 0U;
	state->pending_ticks = 0U;
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

	bool direction_changed = state->last_direction != 0 && input->direction != 0 &&
		((input->direction > 0) != (state->last_direction > 0));
	if (direction_changed) state->edges = 0U;
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
	bool timer_restarted = state->have_prev_tim2 != 0U && tim2 < state->prev_tim2;
	bool sequence_changed = input->hall_sequence_valid &&
		input->hall_sequence != state->prev_hall_sequence;
	bool edge = state->have_prev_tim2 != 0U &&
		(input->hall_sequence_valid ? sequence_changed : timer_restarted);

	/*
	 * FW-131.2: the timer restart and the sequence number come from different contexts, so a FOC
	 * tick can legitimately see the first without the second. Count how long the restart has been
	 * waiting for its sector instead of calling it lost immediately. The count only ever runs on
	 * the production path - a legacy caller without sequence numbers keeps the timer comparison
	 * it has always had.
	 */
	if (input->hall_sequence_valid && !sequence_changed &&
	    (timer_restarted || state->pending_ticks > 0U)) {
		if (state->pending_ticks < 0xFFU) state->pending_ticks++;
	} else {
		state->pending_ticks = 0U;
	}
	bool edge_pending = state->pending_ticks > 0U &&
		state->pending_ticks <= ROTOR_ANGLE_EDGE_PENDING_TICKS;
	/*
	 * Past the bound nothing published a sector, so this was never an edge: a 16-bit rollover, or
	 * a Hall signal that really is gone. That is the case the original condition was written for
	 * and it still loses trust here, only now it waits to be sure.
	 */
	bool lost_timing = input->stalled ||
		(input->hall_sequence_valid && state->pending_ticks > ROTOR_ANGLE_EDGE_PENDING_TICKS);

	state->prev_hall_sequence = input->hall_sequence;
	state->prev_tim2 = tim2;
	state->have_prev_tim2 = 1U;

	/*
	 * Hold the angle across the race. Returning here is deliberate: it leaves trust, edge history
	 * and transfer_offset exactly as the last complete Hall snapshot left them, which is the whole
	 * point - there is no new information yet, so there is nothing to react to.
	 *
	 * A hold may only ever cover the ABSENCE of information. Anything that is itself information
	 * has to be let through, and there are two such facts here: a direction change, and
	 * lost_timing. The second one matters most - input->stalled says the 4 kHz layer has already
	 * MEASURED that the rotor stopped, which is not an ISR that is running late. Holding across it
	 * would keep trusted=1 and the edge history alive for a rotor that is standing still, which is
	 * exactly the state the learning gate exists to refuse.
	 */
	if (edge_pending && !lost_timing && state->have_output && !direction_changed) {
		return state->last_output;
	}
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
	uint8_t was_trusted = state->trusted;
	/* A timeout clears history even if trust was already dropped by hysteresis. */
	if (lost_timing) state->edges = 0U;
	bool may_trust = !input->want_untrusted && !lost_timing &&
		state->edges >= ROTOR_ANGLE_TRUST_EDGES;

	if (state->trusted != (may_trust ? 1U : 0U)) {
		if (!may_trust && (lost_timing || state->edges < ROTOR_ANGLE_TRUST_EDGES)) {
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

	/* Modular arithmetic is required at the signed q31 wrap. Preserve the previous
	 * applied angle on a formula change, then remove the offset at a bounded rate.
	 * Merely testing interp >= 30 degrees does NOT imply the formulas agree. */
	uint32_t raw = (uint32_t)input->hall_angle + (uint32_t)input->angle_correction +
		(uint32_t)(state->trusted ? interp : centre);
	if (state->have_output && (was_trusted != state->trusted || direction_changed)) {
		state->transfer_offset = (int32_t)((uint32_t)state->last_output - raw);
	} else if (state->transfer_offset > ROTOR_ANGLE_TRANSFER_STEP) {
		state->transfer_offset -= ROTOR_ANGLE_TRANSFER_STEP;
	} else if (state->transfer_offset < -ROTOR_ANGLE_TRANSFER_STEP) {
		state->transfer_offset += ROTOR_ANGLE_TRANSFER_STEP;
	} else {
		state->transfer_offset = 0;
	}
	state->last_output = (int32_t)(raw + (uint32_t)state->transfer_offset);
	state->have_output = 1U;
	return state->last_output;
}
