#include "battery_iq_cap.h"

/*
 * QS-3C upstream battery Iq cap. See inc/battery_iq_cap.h for the full design.
 *
 * The exit hysteresis is 90 % of the configured maximum. At the moment of entry the cap is
 * exactly at the point that drew battery_current_max, and it evolves continuously with
 * u_abs, so entry and re-entry are bumpless (no step in the allowed-Iq quantity the caller
 * min's against). The cap that is not active is PH_CURRENT_MAX, which the caller's min()
 * arbitration treats as "no constraint".
 */

#define BATTERY_CAP_HYSTERESIS_PCT_NUM 9
#define BATTERY_CAP_HYSTERESIS_PCT_DEN 10
#define CAP_DIV_SCALE_MASK 2048

void battery_iq_cap_reset(battery_iq_cap_output_t *out)
{
	if (out == 0) {
		return;
	}
	out->bc_active = false;
	out->iq_battery_cap = 0;
}

void battery_iq_cap_update(
	int32_t battery_current_mA,
	int32_t battery_current_max,
	int32_t phase_current_max,
	int32_t iq_ref,
	int32_t u_abs,
	int32_t cal_i,
	battery_iq_cap_output_t *out)
{
	if (out == 0) {
		return;
	}

	if (phase_current_max < 1) phase_current_max = 1;

	/* Entry / exit latch with hysteresis. Reuse the previous latch state so a battery
	 * current hovering exactly at the threshold does not chatter the flag. */
	if (battery_current_mA > battery_current_max) {
		out->bc_active = true;
	} else if (battery_current_mA <
		(int32_t)((int64_t)battery_current_max * BATTERY_CAP_HYSTERESIS_PCT_NUM /
		          BATTERY_CAP_HYSTERESIS_PCT_DEN)) {
		out->bc_active = false;
	}
	/* Between the two bands, the latch holds its prior value (hysteresis). */

	if (!out->bc_active) {
		out->iq_battery_cap = phase_current_max;   /* no constraint for a min() */
		return;
	}

	if (cal_i <= 0 || u_abs <= 0) {
		/* No meaningful division; apply the tightest (most conservative) effective cap by
		 * returning phase_current_max only if the physics allow it. With u_abs == 0 the
		 * motor is not being driven, so a cap is irrelevant — return no constraint. */
		out->iq_battery_cap = phase_current_max;
		return;
	}

	/* Iq_max = battery_current_max * 2048 / (CAL_I * u_abs)
	 * float for a single total-scale division; 32-bit int would overflow. */
	double numerator = (double)battery_current_max * (double)CAP_DIV_SCALE_MASK;
	double denominator = (double)cal_i * (double)u_abs;
	int32_t cap = (int32_t)(numerator / denominator);

	/* Clamp into the Iq domain. Negative or zero cap -> 0 (full limit). */
	if (cap > phase_current_max) cap = phase_current_max;
	if (cap < 0) cap = 0;

	/*
	 * Bumpless re-entry: never let the cap exceed what the rider is actually asking for.
	 * If the rider is already below the computed cap, the cap must not raise it back up
	 * (min() arbitration guarantees this anyway, but clamping here keeps the module's
	 * own emitted cap continuous with the incoming reference).
	 */
	if (iq_ref >= 0 && cap > iq_ref) cap = iq_ref;

	out->iq_battery_cap = cap;
}
