#include "pas_cadence.h"
#include "config.h"   /* PAS_STEPS_PER_PULSE, PAS_CADENCE_RPM_NUMERATOR */

static pas_cadence_state_t st;

void pas_cadence_reset(void)
{
	st.rpm = 0U;
	st.valid = 0U;
	st.epoch_valid = 0U;
	st.last_period_ticks = 0U;
	st.anchor_tick = 0U;
	st.fwd_steps = 0U;
	/* The break counters are NOT cleared here. They are cumulative ride evidence, and a stop
	 * between two pedal strokes must not erase what happened before it. */
}

void pas_cadence_break_epoch(uint8_t cause)
{
	if (st.epoch_valid) {
		switch (cause) {
		case 0:  st.broken_reverse++;  break;
		case 1:  st.broken_invalid++;  break;
		default: st.broken_overflow++; break;
		}
	}
	st.epoch_valid = 0U;
	/* Restart the step count too: the next pulse must span a full, known number of steps. */
	st.fwd_steps = 0U;
}

pas_cadence_step_t pas_cadence_forward_step(uint32_t tick, uint8_t interval_restart)
{
	pas_cadence_step_t out = {0U, 0U, 0U, 0U};

	if (interval_restart) {
		/* FW-086: this step is the ORIGIN of the interval, not a count inside it. */
		st.anchor_tick = tick;
		st.epoch_valid = 1U;
		st.fwd_steps = 0U;
		return out;
	}

	st.fwd_steps++;
	if (st.fwd_steps < PAS_STEPS_PER_PULSE) return out;

	st.fwd_steps = 0U;
	out.pulse = 1U;

	if (st.epoch_valid) {
		/* Unsigned subtraction: a uint32 wrap of the tick counter needs no special case, it
		 * yields the correct elapsed count on its own. */
		uint32_t period = tick - st.anchor_tick;
		if (period > 64000U) period = 64000U;
		out.period_ticks = (uint16_t)period;
		/*
		 * The >70 floor is the old guard against dividing by a period so short it could only
		 * come from noise: at PAS_CADENCE_RPM_NUMERATOR = 10000 it caps the publishable
		 * reading at 142 rpm. It is kept unchanged - this card fixes the timebase, it does not
		 * retune a threshold - but note what it now means: with a real timebase a short period
		 * is genuinely a short period, not an artefact of a fast main loop.
		 */
		if (period > 70U) {
			out.measured = 1U;
			out.rpm = (uint16_t)(PAS_CADENCE_RPM_NUMERATOR / period);
			st.rpm = out.rpm;
			st.valid = 1U;
			st.last_period_ticks = out.period_ticks;
			st.measurements++;
		}
	}

	/* The pulse boundary always becomes the next interval's origin, measured or not - otherwise
	 * a single rejected interval would make the following one span two pulses and read half. */
	st.anchor_tick = tick;
	st.epoch_valid = 1U;
	return out;
}

const pas_cadence_state_t *pas_cadence_get(void)
{
	return &st;
}
