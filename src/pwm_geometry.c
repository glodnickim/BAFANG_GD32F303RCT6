#include "pwm_geometry.h"

#include <string.h>

/*
 * FW-127A. See inc/pwm_geometry.h for why this layer exists and what it deliberately does not
 * try to be. Pure arithmetic: no register is touched here, which is what makes it host-testable.
 */

static pwm_geometry_stats_t stats;

void pwm_geometry_init(void)
{
	memset(&stats, 0, sizeof(stats));
}

const pwm_geometry_stats_t *pwm_geometry_get_stats(void)
{
	return &stats;
}

uint8_t pwm_geometry_compare_legal(int32_t value, uint16_t arr)
{
	return (uint8_t)(value >= 0 && value <= (int32_t)arr);
}

uint8_t pwm_geometry_apply(const int32_t requested[PWM_GEOMETRY_PHASES],
                           uint16_t applied[PWM_GEOMETRY_PHASES],
                           uint16_t arr)
{
	uint8_t i, mask = 0U;

	if (!requested || !applied) return 0U;

	for (i = 0; i < PWM_GEOMETRY_PHASES; i++) {
		int32_t v = requested[i];

		/* Track the REQUEST before it is changed - that is the evidence about the voltage
		 * controller. Tracking the applied value alone would only ever show [0, arr]. */
		if (!stats.seen) {
			stats.peak_requested = v;
			stats.min_requested = v;
		} else {
			if (v > stats.peak_requested) stats.peak_requested = v;
			if (v < stats.min_requested) stats.min_requested = v;
		}

		if (v < 0) {
			v = 0;
			mask |= (uint8_t)(1U << i);
			if (stats.clamp_phase[i] < 0xFFFFFFFFu) stats.clamp_phase[i]++;
		} else if (v > (int32_t)arr) {
			v = (int32_t)arr;
			mask |= (uint8_t)(1U << i);
			if (stats.clamp_phase[i] < 0xFFFFFFFFu) stats.clamp_phase[i]++;
		}

		applied[i] = (uint16_t)v;

		if (!stats.seen) {
			stats.peak_applied = applied[i];
			stats.min_applied = applied[i];
			stats.seen = 1U;
		} else {
			if (applied[i] > stats.peak_applied) stats.peak_applied = applied[i];
			if (applied[i] < stats.min_applied) stats.min_applied = applied[i];
		}
	}

	/* One count per APPLY in which anything was clamped, not per phase - so the total answers
	 * "how many PWM periods were affected", which is the question the hardware session asks. */
	if (mask && stats.clamp_total < 0xFFFFFFFFu) stats.clamp_total++;

	return mask;
}
