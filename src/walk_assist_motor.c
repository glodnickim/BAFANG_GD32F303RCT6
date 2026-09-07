#include "walk_assist_motor.h"

#include "config.h"
#include "walk_speed_controller.h"

/*
 * Absolute EBICS Iq units (CAL_I = 95 mA), independent of the configured phase
 * current. This ceiling preserves the safe WA scale used before the ride-current
 * ceiling was raised from 157 to 700.
 */
#define WA_MOTOR_IQ_ABS_MAX             157
#define WA_MOTOR_SAFE_LIMIT_IQ           15
#define WA_MOTOR_START_MAX_IQ            40

#define WA_MOTOR_TARGET_RPM_DEFAULT      WALK_ASSIST_RPM_DEFAULT
#define WA_MOTOR_TARGET_RPM_MIN          WALK_ASSIST_RPM_MIN
#define WA_MOTOR_TARGET_RPM_MAX          WALK_ASSIST_RPM_MAX
#define WA_MOTOR_TARGET_ERPS_DEFAULT     40  /* 30 obr/min x 4/3 */
#define WA_MOTOR_MAX_WHEEL_X100         700

/* M820: 80 electrical revolutions per crank revolution -> rpm * 4 / 3. */
#define WA_ERPS_PER_RPM_NUM               4
#define WA_ERPS_PER_RPM_DEN               3

#define WA_MOTOR_HALL_TIMER_HZ        500000UL
#define WA_MOTOR_HALL_AVG_SAMPLES        12
#define WA_MOTOR_ERPS_TIMEOUT_TICKS     800  /* 200 ms @ 4 kHz */

/*
 * FW-130: 6000 -> 12000. The grace has to outlast the start, and the start got longer in two
 * ways at once: the demand now ramps over 550 ms instead of 427 ms, and it climbs to a ceiling
 * that the rider can raise well above the old fixed 40 Iq. A heavy bike on a slope legitimately
 * spends longer below 15 % of target while the ramp is still building, and the old 1.5 s window
 * would have called that a jam and latched STALL. Everything after the grace is unchanged: a
 * genuinely stalled rotor under current still latches LIMIT and then STALL.
 */
#define WA_MOTOR_JAM_GRACE_TICKS      12000  /* 3 s for energetic start */
#define WA_MOTOR_JAM_NO_HALL_TICKS      400  /* 100 ms */
#define WA_MOTOR_JAM_MIN_ERPS             2
#define WA_MOTOR_JAM_CMD_IQ              24
#define WA_MOTOR_JAM_ACTUAL_IQ           24
#define WA_MOTOR_JAM_NO_MOTION_TICKS   1200  /* 300 ms */
#define WA_MOTOR_JAM_PARTIAL_TICKS      800  /* 200 ms */
#define WA_MOTOR_JAM_TARGET_PCT          15
#define WA_MOTOR_LIMIT_TICKS            1600  /* 400 ms, then latched STALL */
#define WA_MOTOR_RECOVERY_TARGET_PCT      25
#define WA_MOTOR_RECOVERY_MIN_ERPS         4
#define WA_MOTOR_REACQUIRE_IQ              24
#define WA_MOTOR_REACQUIRE_TICKS         6000  /* 1.5 s gentle Hall reacquisition */
#define WA_MOTOR_COAST_RECOVERY_IQ          24
#define WA_MOTOR_COAST_RECOVERY_TICKS    16000  /* 4 s: 0.75 s ramp + 3.25 s at 24 Iq */
#define WA_MOTOR_HALL_LOSS_DRIVE_IQ        30
#define WA_MOTOR_RUN_MIN_IQ                   0
#define WA_MOTOR_RUN_MAX_IQ                  40
/*
 * FW-130.1 keepalive, law B only. Owner requirement 2026-09-03: "as soon as it stops it cannot
 * read speed from the Hall, because there will not be one." The governor's own zero is reachable,
 * and a mid-drive rotor behind an open freewheel stops within a few hundred ms of losing current.
 * Once it stops there is no signal, the bounded recovery has to nudge it back, and the speed it
 * then reports is genuinely low - which the governor answers with full current. That round trip
 * is a self-sustaining on/off cycle that needs no help from the bike.
 *
 * The cure is to not enter it: while a walk session is actually running and a speed is actually
 * being measured, keep a current small enough to be no drive at all but enough that the rotor
 * keeps producing edges. FW-079 removed a 5 Iq floor because it made different targets converge;
 * this is well under half of that, and unlike FW-079's floor it is gated on a live speed reading,
 * so it can never be what holds the bike above target. Every safety zero stays a true zero: the
 * wheel cut-off, brake, release, fault, LIMIT and STALL all pass 0 here.
 */
#define WA_MOTOR_KEEPALIVE_IQ                 2
#define WA_MOTOR_COAST_EXIT_IQ                2
#define WA_MOTOR_FACTOR_ONE                 256  /* FW-130: Q8 unity for the governor factors */

static walk_motor_state_t wa_state;
static walk_speed_controller_t wa_controller;
static walk_speed_controller_output_t wa_control_output;
static int32_t wa_iq_cmd;
static int32_t wa_erps_filtered;
static uint16_t wa_jam_ticks;
static uint16_t wa_limit_ticks;
static uint16_t wa_grace_ticks;
static uint16_t wa_last_erps_age_ticks;
static uint16_t wa_hall_periods[WA_MOTOR_HALL_AVG_SAMPLES];
static uint32_t wa_hall_period_sum;
static uint8_t wa_hall_period_index;
static uint8_t wa_hall_period_count;
static uint8_t wa_ignore_first_hall;
static uint8_t wa_hall_timeout_seen;
static uint8_t wa_had_motion;
static uint8_t wa_blocked;
static uint8_t wa_jam_active;
static uint8_t wa_reacquire_active;
static uint8_t wa_coast_expected;
static uint8_t wa_coast_recovery_active;
static uint16_t wa_reacquire_ticks;
static int32_t wa_iq_cap_last;
static int32_t wa_wheel_factor_q8 = WA_MOTOR_FACTOR_ONE;   /* FW-130 */
static uint8_t wa_wheel_cut;                               /* FW-130 */

static int32_t abs32(int32_t value)
{
	return (value < 0) ? -value : value;
}

/*
 * FW-130: wheel speed is the fuse, never the controlled value.
 *
 * Below cut-off - taper it takes nothing away. Across the last WA_WHEEL_TAPER_X100 before the
 * cut-off it lowers the ceiling continuously, so a lighter gear (where the same chainring speed
 * drives the wheel faster) simply gets less current instead of the pre-FW-130 on/off switch that
 * zeroed the current and restarted the session. AT the cut-off the factor is 0 and *cut is set:
 * that is a safety limit, so the controller clamps in the same tick without the fall ramp. The
 * session itself is only torn down further up, at cut-off + WA_WHEEL_HARD_MARGIN_X100.
 */
#if (WALK_GOVERNOR_ENABLE != 0)
static int32_t wheel_factor_q8(uint16_t speed_x100, uint16_t cut_x100, uint8_t *cut)
{
	*cut = (speed_x100 >= cut_x100) ? 1U : 0U;
	if (*cut) {
		return 0;
	}
	int32_t full = (int32_t)cut_x100 - WA_WHEEL_TAPER_X100;
	if (full < 0) {
		full = 0;
	}
	if ((int32_t)speed_x100 <= full) {
		return WA_MOTOR_FACTOR_ONE;
	}
	int32_t span = (int32_t)cut_x100 - full;
	if (span <= 0) {
		return 0;
	}
	return (WA_MOTOR_FACTOR_ONE * ((int32_t)cut_x100 - (int32_t)speed_x100)) / span;
}
#endif /* WALK_GOVERNOR_ENABLE */

static uint16_t rpm_to_erps(uint16_t chainring_rpm)
{
	return (uint16_t)(((uint32_t)chainring_rpm * WA_ERPS_PER_RPM_NUM +
		(WA_ERPS_PER_RPM_DEN / 2U)) / WA_ERPS_PER_RPM_DEN);
}

static void hall_estimator_reset(void)
{
	wa_erps_filtered = 0;
	wa_last_erps_age_ticks = 0xFFFFU;
	wa_hall_period_sum = 0;
	wa_hall_period_index = 0;
	wa_hall_period_count = 0;
	wa_ignore_first_hall = 1;
	wa_hall_timeout_seen = 0;
}

static bool hall_estimator_update(const walk_motor_input_t *input)
{
	bool hall_valid =
		input->motor_erps_age_ticks <= WA_MOTOR_ERPS_TIMEOUT_TICKS;
	bool hall_event = hall_valid &&
		input->motor_erps_age_ticks < wa_last_erps_age_ticks;

	if (!hall_valid && !wa_hall_timeout_seen) {
		wa_hall_period_sum = 0;
		wa_hall_period_index = 0;
		wa_hall_period_count = 0;
		wa_ignore_first_hall = 1;
		wa_hall_timeout_seen = 1;
		wa_erps_filtered = 0;
	}
	if (hall_event) {
		wa_hall_timeout_seen = 0;
		if (wa_ignore_first_hall) {
			wa_ignore_first_hall = 0;
		} else if (input->motor_hall_ticks > 0U) {
			if (wa_hall_period_count < WA_MOTOR_HALL_AVG_SAMPLES) {
				wa_hall_periods[wa_hall_period_index] =
					input->motor_hall_ticks;
				wa_hall_period_sum += input->motor_hall_ticks;
				wa_hall_period_count++;
			} else {
				wa_hall_period_sum -=
					wa_hall_periods[wa_hall_period_index];
				wa_hall_periods[wa_hall_period_index] =
					input->motor_hall_ticks;
				wa_hall_period_sum += input->motor_hall_ticks;
			}
			if (++wa_hall_period_index >= WA_MOTOR_HALL_AVG_SAMPLES) {
				wa_hall_period_index = 0;
			}
			if (wa_hall_period_sum > 0U) {
				uint32_t numerator = WA_MOTOR_HALL_TIMER_HZ *
					(uint32_t)wa_hall_period_count;
				uint32_t denominator = wa_hall_period_sum * 6U;
				wa_erps_filtered = (int32_t)(numerator / denominator);
			}
		}
	}
	wa_last_erps_age_ticks = input->motor_erps_age_ticks;
	if (!hall_valid) {
		wa_erps_filtered = 0;
	}
	return hall_valid;
}

void walk_motor_reset(void)
{
	wa_state = WA_STATE_OFF;
	wa_iq_cmd = 0;
	wa_jam_ticks = 0;
	wa_limit_ticks = 0;
	wa_grace_ticks = WA_MOTOR_JAM_GRACE_TICKS;
	wa_had_motion = 0;
	wa_jam_active = 0;
	wa_reacquire_active = 0;
	wa_coast_expected = 0;
	wa_coast_recovery_active = 0;
	wa_reacquire_ticks = 0;
	wa_iq_cap_last = 0;
	wa_wheel_factor_q8 = WA_MOTOR_FACTOR_ONE;
	wa_wheel_cut = 0;
	wa_control_output = (walk_speed_controller_output_t){0};
	walk_speed_controller_reset(&wa_controller);
	hall_estimator_reset();
	/* wa_blocked intentionally survives until the complete WA request release. */
}

void walk_motor_release(void)
{
	walk_motor_reset();
	wa_blocked = 0;
}

static void publish_output(walk_motor_output_t *output, uint16_t target_erps,
	bool hall_valid)
{
	if (output == 0) {
		return;
	}
	output->iq_target = wa_iq_cmd;
	output->state = (uint8_t)wa_state;
	output->target_erps = target_erps;
	output->measured_erps = (uint16_t)
		((wa_erps_filtered > 65535) ? 65535 :
		((wa_erps_filtered < 0) ? 0 : wa_erps_filtered));
	output->error_erps = wa_control_output.error_erps;
	output->iq_cap = wa_iq_cap_last;
	output->integral_iq = wa_control_output.integral_iq;
	output->startup_iq = wa_control_output.startup_iq;
	output->gear_factor_q8 = wa_control_output.gear_factor_q8;   //FW-130
	output->wheel_factor_q8 = (int16_t)wa_wheel_factor_q8;       //FW-130
	output->flags = (uint8_t)
		((hall_valid ? WA_FLAG_HALL_VALID : 0) |
		(wa_jam_active ? WA_FLAG_JAM : 0) |
		(wa_blocked ? WA_FLAG_BLOCKED : 0) |
		(wa_control_output.saturated ? WA_FLAG_SATURATED : 0) |
		(wa_control_output.startup_active ? WA_FLAG_START_ACTIVE : 0) |
		(wa_control_output.above_target ? WA_FLAG_ABOVE_TARGET : 0) |
		((wa_state == WA_STATE_LIMIT) ? WA_FLAG_LIMIT : 0) |
		(wa_reacquire_active ? WA_FLAG_REACQUIRE : 0));
	/* FW-113.2: module-level reason bits. Activation bits (NO_CAN_REQUEST /
	 * BUTTON_RELEASED) are owned by main.c and OR-ed into the same field. There is
	 * deliberately no wall-clock hold-timeout bit: only a real gate can stop WA.
	 */
	output->reason = (uint16_t)
		((hall_valid ? 0 : WA_REASON_HALL) |
		/* FW-130: the fuse names itself the moment it actually takes everything away,
		 * whether that is the in-session clamp at the cut-off or the hard stop above it. */
		(wa_wheel_cut ? WA_REASON_SPEED_GATE : 0) |
		(wa_jam_active ? WA_REASON_JAM : 0) |
		(wa_blocked ? WA_REASON_STALL : 0) |
		((wa_state == WA_STATE_STALL) ? WA_REASON_STALL : 0) |
		((wa_state == WA_STATE_LIMIT) ? WA_REASON_LIMIT : 0));
}

static void enter_limit(void)
{
	wa_state = WA_STATE_LIMIT;
	wa_limit_ticks = 0;
	wa_jam_active = 1;
	wa_reacquire_active = 0;
	wa_coast_expected = 0;
	wa_coast_recovery_active = 0;
	wa_reacquire_ticks = 0;
}

int32_t walk_motor_update(const walk_motor_input_t *input,
	walk_motor_output_t *output)
{
	uint16_t target_erps = WA_MOTOR_TARGET_ERPS_DEFAULT;
	if (input != 0 &&
		input->target_chainring_rpm >= WA_MOTOR_TARGET_RPM_MIN &&
		input->target_chainring_rpm <= WA_MOTOR_TARGET_RPM_MAX) {
		target_erps = rpm_to_erps(input->target_chainring_rpm);
	}

	uint16_t max_wheel_x100 =
		(input != 0 && input->max_wheel_speed_x100 > 0U) ?
		input->max_wheel_speed_x100 : WA_MOTOR_MAX_WHEEL_X100;
	/*
	 * FW-130: the cut-off itself no longer kills the session. Reaching it clamps the current
	 * to zero (handled below, in the same tick), but the state machine, the Hall estimate and
	 * the ramp all stay alive so that dropping back below it is a single rise ramp instead of
	 * a fresh start. Only this margin above the cut-off is a real stop.
	 */
#if (WALK_GOVERNOR_ENABLE != 0)
	uint32_t hard_stop_x100 =
		(uint32_t)max_wheel_x100 + WA_WHEEL_HARD_MARGIN_X100;
	if (hard_stop_x100 > 0xFFFFUL) {
		hard_stop_x100 = 0xFFFFUL;
	}
#else
	/* Law A keeps the pre-FW-130 behaviour exactly: the cut-off itself stops the session. */
	uint32_t hard_stop_x100 = (uint32_t)max_wheel_x100;
#endif
	bool wheel_hard_stop = input != 0 &&
		(uint32_t)input->wheel_speed_x100 >= hard_stop_x100;
	if (input == 0 || !input->active || input->brake || input->fault ||
		wheel_hard_stop) {
		walk_motor_reset();
		publish_output(output, target_erps, false);
		/* FW-113.2: name which real gate stopped the drive. publish_output already set
		 * the HALL bit (no Hall was read); these are the input-level gates. */
		if (output != 0 && input != 0) {
			output->reason |= (uint16_t)
				((input->brake ? WA_REASON_BRAKE : 0) |
				(input->fault ? WA_REASON_ERROR : 0) |
				(wheel_hard_stop ? WA_REASON_SPEED_GATE : 0));
		}
		return 0;
	}

#if (WALK_GOVERNOR_ENABLE != 0)
	/* FW-130: recomputed every tick; publish_output and the controller both read it. */
	wa_wheel_factor_q8 =
		wheel_factor_q8(input->wheel_speed_x100, max_wheel_x100, &wa_wheel_cut);
#endif
	/*
	 * FW-130: the configured walk strength, clamped to the module's absolute WA ceiling.
	 * 0 keeps the pre-FW-130 fixed 40 Iq, so a caller that does not fill the field in still
	 * gets exactly the old behaviour.
	 */
	int32_t walk_iq_max = (input->walk_iq_max > 0) ?
		input->walk_iq_max : WA_MOTOR_START_MAX_IQ;
	if (walk_iq_max > WA_MOTOR_IQ_ABS_MAX) {
		walk_iq_max = WA_MOTOR_IQ_ABS_MAX;
	}
	if (wa_blocked) {
		wa_iq_cmd = 0;
		wa_state = WA_STATE_STALL;
		wa_jam_active = 1;
		publish_output(output, target_erps, false);
		return 0;
	}

	if (wa_state == WA_STATE_OFF) {
		wa_state = WA_STATE_REGULATE;
	}
	if (wa_grace_ticks > 0U) {
		wa_grace_ticks--;
	}

	bool hall_valid = hall_estimator_update(input);
	if (hall_valid && wa_erps_filtered >= WA_MOTOR_JAM_MIN_ERPS) {
		wa_had_motion = 1;
		if (wa_reacquire_active) {
			wa_reacquire_active = 0;
			wa_coast_recovery_active = 0;
			wa_coast_expected = 0;
			wa_reacquire_ticks = 0;
		}
	}

	if (wa_state == WA_STATE_REGULATE) {
		bool no_motion =
			input->motor_erps_age_ticks > WA_MOTOR_JAM_NO_HALL_TICKS ||
			wa_erps_filtered < WA_MOTOR_JAM_MIN_ERPS;
		bool partial_motion =
			target_erps > 0U &&
			wa_erps_filtered * 100 <
				(int32_t)target_erps * WA_MOTOR_JAM_TARGET_PCT;
		bool current_significant =
			wa_iq_cmd >= WA_MOTOR_JAM_CMD_IQ ||
			abs32(input->motor_iq_actual) >= WA_MOTOR_JAM_ACTUAL_IQ;
		bool drive_without_hall =
			wa_iq_cmd >= WA_MOTOR_HALL_LOSS_DRIVE_IQ ||
			abs32(input->motor_iq_actual) >= WA_MOTOR_HALL_LOSS_DRIVE_IQ;

		if (wa_had_motion && !hall_valid) {
			if (wa_coast_expected || wa_coast_recovery_active) {
				/*
				 * PI intentionally reached the 2 Iq keepalive after an overspeed. The motor can
				 * stop behind the freewheel and lose Hall even though the drivetrain
				 * is healthy. Resume only through the slow RUN slew, but allow it to
				 * reach the bounded recovery ceiling and remain there long enough to produce
				 * a Hall edge. This path never re-arms the 40 Iq START.
				 */
				wa_coast_recovery_active = 1;
				wa_reacquire_active = 1;
				if (wa_reacquire_ticks < 65000U) {
					wa_reacquire_ticks++;
				}
				if (wa_reacquire_ticks >=
					WA_MOTOR_COAST_RECOVERY_TICKS) {
					enter_limit();
				}
			} else if (wa_reacquire_active || !drive_without_hall) {
				/*
				 * A stop at low torque is not a Hall fault under drive.
				 * The gentle reacquire ceiling remains below the hard Hall-loss
				 * threshold. A bounded reacquire cannot classify its own command
				 * as an immediate fault, but it still times out into LIMIT/STALL
				 * if the rotor cannot move.
				 */
				wa_reacquire_active = 1;
				if (wa_reacquire_ticks < 65000U) {
					wa_reacquire_ticks++;
				}
				if (wa_reacquire_ticks >= WA_MOTOR_REACQUIRE_TICKS) {
					enter_limit();
				}
			} else if (drive_without_hall) {
				/* Missing Hall while torque is present is a real safety fault. */
				enter_limit();
			}
		} else if (wa_grace_ticks == 0U && current_significant &&
			(no_motion || partial_motion)) {
			if (wa_jam_ticks < 65000U) {
				wa_jam_ticks++;
			}
			wa_jam_active = 1;
			uint16_t threshold = no_motion ?
				WA_MOTOR_JAM_NO_MOTION_TICKS :
				WA_MOTOR_JAM_PARTIAL_TICKS;
			if (wa_jam_ticks >= threshold) {
				enter_limit();
			}
		} else {
			wa_jam_ticks = 0;
			wa_jam_active = 0;
		}
	}

	if (wa_state == WA_STATE_LIMIT) {
		int32_t recovery_erps =
			((int32_t)target_erps * WA_MOTOR_RECOVERY_TARGET_PCT) / 100;
		if (recovery_erps < WA_MOTOR_RECOVERY_MIN_ERPS) {
			recovery_erps = WA_MOTOR_RECOVERY_MIN_ERPS;
		}
		if (hall_valid && wa_erps_filtered >= recovery_erps) {
			wa_state = WA_STATE_REGULATE;
			wa_limit_ticks = 0;
			wa_jam_ticks = 0;
			wa_jam_active = 0;
			wa_grace_ticks = WA_MOTOR_JAM_PARTIAL_TICKS;
		} else if (++wa_limit_ticks >= WA_MOTOR_LIMIT_TICKS) {
			wa_blocked = 1;
			wa_state = WA_STATE_STALL;
			wa_iq_cmd = 0;
			wa_iq_cap_last = 0;
			walk_speed_controller_reset(&wa_controller);
			wa_control_output = (walk_speed_controller_output_t){0};
			publish_output(output, target_erps, hall_valid);
			return 0;
		}
	}

	wa_iq_cap_last = (wa_state == WA_STATE_LIMIT) ?
		WA_MOTOR_SAFE_LIMIT_IQ :
		(wa_coast_recovery_active ? WA_MOTOR_COAST_RECOVERY_IQ :
		(wa_reacquire_active ? WA_MOTOR_REACQUIRE_IQ :
#if (WALK_GOVERNOR_ENABLE == 0)
		(!wa_controller.startup_complete ? WA_MOTOR_START_MAX_IQ :
		WA_MOTOR_IQ_ABS_MAX)));
#else
		/*
		 * FW-130: no separate START ceiling. The ramp is the soft start, and the
		 * configured walk strength (walk_iq_max, applied inside the controller) is the
		 * one number that says how hard WA may push. This stays the absolute safety
		 * ceiling above it.
		 */
		WA_MOTOR_IQ_ABS_MAX));
#endif
	walk_speed_controller_input_t control_input = {
		.target_erps = target_erps,
		.measured_erps = (uint16_t)
			((wa_erps_filtered > 65535) ? 65535 : wa_erps_filtered),
		.hall_valid = hall_valid,
		.reacquire = wa_reacquire_active != 0U,
		.iq_ceiling = wa_iq_cap_last,
		.run_iq_min = (wa_state == WA_STATE_REGULATE &&
			wa_controller.startup_complete && !wa_reacquire_active) ?
#if (WALK_GOVERNOR_ENABLE == 0)
			WA_MOTOR_RUN_MIN_IQ : 0,
#else
			WA_MOTOR_KEEPALIVE_IQ : 0,   //FW-130.1, see the constant
#endif
		.run_iq_max = WA_MOTOR_RUN_MAX_IQ,
		.downstream_iq = input->motor_iq_reference,
		.iq_walk_max = walk_iq_max,                 //FW-130
		.wheel_factor_q8 = wa_wheel_factor_q8,      //FW-130
		.wheel_cut = wa_wheel_cut != 0U,            //FW-130
		/*
		 * FW-130.1: a REAL speed reading, not merely a fresh edge. hall_valid only says an edge
		 * arrived inside the timeout; the estimator needs a measured PERIOD before it can report
		 * a speed, so right after a gap hall_valid is true while wa_erps_filtered is still 0.
		 * Passing that on as "0 rpm" is exactly the lie the owner asked to be removed: a stopped
		 * rotor emits nothing, so silence must never be read as a speed.
		 */
		.speed_known = hall_valid && wa_erps_filtered > 0
	};
	wa_iq_cmd = walk_speed_controller_update(
		&wa_controller, &control_input, &wa_control_output);
	if (wa_state == WA_STATE_REGULATE && hall_valid &&
		wa_controller.startup_complete && !wa_reacquire_active) {
		/*
		 * Remember the PI's intent before the slower output ramp reaches zero.
		 * Otherwise Hall can expire during an intentional deceleration and the
		 * event is indistinguishable from an unexpected sensor loss.
		 */
		if (wa_control_output.above_target &&
			wa_controller.desired_iq <= 0) {
			wa_coast_expected = 1;
		} else if (wa_controller.desired_iq > 0 &&
			wa_iq_cmd >= WA_MOTOR_COAST_EXIT_IQ) {
			wa_coast_expected = 0;
		}
	}
	publish_output(output, target_erps, hall_valid);
	return wa_iq_cmd;
}
