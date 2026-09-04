#ifndef WALK_SPEED_CONTROLLER_H_
#define WALK_SPEED_CONTROLLER_H_

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/*
 * Pure Walk Assist speed controller.
 *
 * The caller owns Hall estimation, hard safety gates and the LIMIT/STALL state
 * machine. This module owns the complete Iq trajectory while WA is active.
 *
 * Two laws live here, selected by WALK_GOVERNOR_ENABLE in inc/config.h so an A/B
 * ride is two .bin files rather than two branches:
 *
 *   0 (A) - FW-060..082: one-shot start preload, breakaway floor, speed PI with
 *           anti-windup, output slew.
 *   1 (B) - FW-130: no integrator. A demand ramped at a constant rate (G532
 *           character) whose ceiling is continuously lowered by the gear-RPM
 *           governor and by the wheel-speed fuse, then min()-ed with the caller's
 *           hard safety ceiling. Speed is held by taking current away, never by
 *           integrating an error.
 */
typedef struct {
	int32_t iq_command_q;
	int32_t desired_iq;
	uint32_t session_ticks;
	int32_t last_gear_q8;    /* FW-130.1: last verdict made from a real speed reading */
	bool startup_complete;
	bool reacquire_active;
#if (WALK_GOVERNOR_ENABLE == 0)
	int32_t integral_q;
	uint8_t control_divider;
#endif
} walk_speed_controller_t;

typedef struct {
	uint16_t target_erps;
	uint16_t measured_erps;
	bool hall_valid;
	bool reacquire;                 /* bounded recovery after Hall loss */
	int32_t iq_ceiling;             /* hard safety ceiling: LIMIT/REACQUIRE/absolute */
	int32_t run_iq_min;             /* normal RUN keepalive; 0 disables */
	int32_t run_iq_max;             /* law A only: soft RUN ceiling, descended through the fall slew */
	int32_t downstream_iq;
	/*
	 * FW-130 (law B). iq_walk_max is the configured walk strength - the bank's
	 * "Walk current" percentage resolved into Iq by the caller; 0 selects the
	 * module's built-in fallback. wheel_factor_q8 is the wheel-speed fuse in
	 * Q8 (256 = no reduction) and always ramps. wheel_cut is the separate, explicit
	 * "wheel is AT or above the cut-off" signal: it forces zero in the same tick,
	 * without the fall ramp, because a safety limit may clamp at once - unlike the
	 * gear governor, which only ever takes current away through the ramp.
	 */
	int32_t iq_walk_max;
	int32_t wheel_factor_q8;
	bool wheel_cut;
	/*
	 * FW-130.1, owner requirement 2026-09-03: THE ABSENCE OF A HALL SIGNAL IS NOT "SPEED ZERO".
	 * A stopped rotor produces no edges at all, so a missing reading says only that the speed is
	 * unknown - and a governor that reads it as "not moving" answers with full current, which is
	 * the worst possible reply. This flag is true ONLY when the estimator actually measured a
	 * period this session: hall_valid alone is not enough, because the first edge after a gap
	 * makes it valid while the computed speed is still 0.
	 */
	bool speed_known;
} walk_speed_controller_input_t;

typedef struct {
	int32_t iq_target;
	int16_t error_erps;
	int16_t controlled_error_erps;
	int16_t p_iq;
	int16_t integral_iq;
	int16_t startup_iq;
	int16_t gear_factor_q8;         /* FW-130: gear-RPM governor, 0..256 */
	int16_t wheel_factor_q8;        /* FW-130: wheel-speed fuse as applied, 0..256 */
	bool startup_active;
	bool above_target;
	bool saturated;
} walk_speed_controller_output_t;

void walk_speed_controller_reset(walk_speed_controller_t *controller);

int32_t walk_speed_controller_update(
	walk_speed_controller_t *controller,
	const walk_speed_controller_input_t *input,
	walk_speed_controller_output_t *output);

#endif /* WALK_SPEED_CONTROLLER_H_ */
