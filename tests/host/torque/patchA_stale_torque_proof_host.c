/*
 * FW-112 PATCH A - STALE-TORQUE SAFETY PROOF (card section 4/6), real production chain.
 *
 * S1  high torque -> reverse -> rider releases -> forward: zero stale torque reuse.
 * S2  high torque -> reverse -> rider keeps pressing -> forward: assist returns from CURRENT
 *     torque, not historical.
 * S3  reverse -> forward at torque=0: zero assist.
 * S4  noise around the deadband: zero false assist.
 * S5  brake/fault: zero regardless of torque.
 *
 * (S6/S7 from the card - real_stop during suspended keeps COLD, and the terminal-inhibit matrix
 * from BOTH TRACK_FAST and WAIT_FRESH_LOAD - are already proven directly by
 * fw112_run_rearm_recovery_host.c's check_terminal_cancel matrix and FW-112.1's REAL_STOP suite,
 * both updated and re-verified for PATCH A as part of this same card - not duplicated here.)
 */

#include <string.h>

#include "../common/check.h"

#include "assist_extended_boost.h"
#include "assist_modes.h"
#include "config.h"
#include "motor_core.h"
#include "pas_direction.h"
#include "rider_input.h"
#include "ride_control.h"
#include "ride_session.h"
#include "torque_input.h"
#include "tuning_config.h"

#define TEST_ASSIST_LEVEL 3U
#define TEST_SPEED_X100   1500U
#define TEST_BATTERY_MV   42000U
#define TEST_VOLTAGE_RAW  2000
#define TEST_TEMPERATURE_C 25
#define STEP_INTERVAL_TICKS 42U
#define RAW(delta) ((uint16_t)(TORQUE_ZERO_TARGET_NATIVE + (uint16_t)(delta)))
#define STRONG_DELTA 320U

static MotorState_t MS;
static uint32_t g_tick;
static uint8_t g_assist_level = TEST_ASSIST_LEVEL;
static uint8_t g_walk_active, g_calibration_active, g_safety_cut, g_real_stop;
static uint32_t g_battery_mv = TEST_BATTERY_MV;
static int32_t g_throttle_iq;

typedef struct { int32_t iq_request, iq_setpoint; uint8_t recovery_state, session_state; } probe_t;
static probe_t g_probe;

static void control_tick(uint16_t raw_mv, int event)
{
	g_tick++;
	if (event == 1) { pas_direction_on_step(1); torque_input_run_filter_step(); }
	else if (event == -1) { pas_direction_on_step(-1); }
	torque_input_update(raw_mv, torque_input_correct(raw_mv), true);

	const torque_snapshot_t *snap = torque_input_get_snapshot();
	bool forward_confirmed_this_tick = (event != 0) && pas_direction_forward_confirmed_last_call();
	uint8_t fwd_run = pas_direction_fwd_run();
	bool pedaling = (fwd_run >= tuning_config_start_steps());

	rider_input_t r;
	memset(&r, 0, sizeof(r));
	r.torque_raw_mv = raw_mv;
	r.torque_corrected_mv = torque_input_correct(raw_mv);
	r.torque_assist_filtered = snap->assist_delta_filtered_native;
	r.torque_run_filtered = snap->assist_delta_run_native;
	r.torque_load_centikg = torque_input_load_centikg();
	r.torque_assist_now_native = snap->assist_delta_native;
	r.cadence_rpm = 60U;
	r.wheel_speed_x100 = TEST_SPEED_X100;
	r.motor_erps = 200U;
	r.pas_forward = pedaling;
	r.pedaling_active = pedaling;
	r.crank_forward_steps = fwd_run;
	r.crank_direction_ok = true;
	r.real_stop = (g_real_stop != 0);
	r.direction_inhibit_active = pas_direction_direction_inhibit_active();
	r.forward_confirmed_this_tick = forward_confirmed_this_tick;
	r.sample_tick = g_tick;
	r.torque_sensor_valid = true;
	r.pas_sensor_valid = true;
	rider_input_update(&r);

	ride_control_input_t in;
	memset(&in, 0, sizeof(in));
	in.speed_x100 = TEST_SPEED_X100;
	in.cadence_rpm = r.cadence_rpm;
	in.assist_level_index = g_assist_level;
	in.battery_voltage_mv = g_battery_mv;
	in.iq_scale = (int32_t)PH_CURRENT_MAX;
	in.ride_core_iq_limit = (int32_t)PH_CURRENT_MAX;
	in.phase_current_max = (int32_t)PH_CURRENT_MAX;
	in.current_iq = MS.i_q_setpoint;
	in.current_id = MS.i_d_setpoint;
	in.voltage_raw = TEST_VOLTAGE_RAW;
	in.voltage_min_raw = VOLTAGE_MIN;
	in.controller_temperature_c = TEST_TEMPERATURE_C;
	in.cadence_filtered_x8 = (uint16_t)(r.cadence_rpm * 8U);
	in.walk_active = (g_walk_active != 0);
	in.position_calibration_active = (g_calibration_active != 0);
	in.safety_cut_non_direction = (g_safety_cut != 0);
	in.throttle_iq = g_throttle_iq;
	ride_control_update(&in);

	g_probe.iq_request = assist_modes_get_last_output()->iq_request;
	g_probe.iq_setpoint = MS.i_q_setpoint;
	g_probe.recovery_state = (uint8_t)torque_input_recovery_state();
	g_probe.session_state = (uint8_t)ride_control_get_session_state();
}

static uint32_t ride_forward(uint16_t raw_mv, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t t = 0; t + 1 < STEP_INTERVAL_TICKS; t++) control_tick(raw_mv, 0);
		control_tick(raw_mv, 1);
	}
	return n;
}

static void reset_all(void)
{
	torque_input_init();
	torque_input_set_run_window_deg(180U);
	assist_modes_init();
	assist_modes_set_active_bank(0);
	memset(&MS, 0, sizeof(MS));
	motor_core_init(&MS);
	ride_control_init();
	pas_direction_init();
	g_tick = 0;
	memset(&g_probe, 0, sizeof(g_probe));
	g_walk_active = g_calibration_active = g_safety_cut = g_real_stop = 0;
	g_battery_mv = TEST_BATTERY_MV;
}

static void warmup(void) { ride_forward(RAW(STRONG_DELTA), 60U); for (int i = 0; i < 1000; i++) control_tick(RAW(0), 0); }

int main(void)
{
	/* ---- S1: high torque -> reverse -> rider releases -> forward: zero stale reuse ---- */
	{
		reset_all();
		warmup();
		CHECK(g_probe.iq_request > 0, "S1: setup - warm demand positive before the reverse");
		control_tick(RAW(0), -1); /* reverse, rider releases at the same moment */
		CHECK(g_probe.session_state == RIDE_SESSION_SUSPENDED_BY_DIRECTION,
			"S1: reverse suspends the session the same tick");
		for (int i = 0; i < 1000; i++) control_tick(RAW(0), 0); /* settle the reverse edge + RIDE_HARD_CUT_RAMP_MS=200ms */
		CHECK(g_probe.iq_setpoint == 0,
			"S1: the real motor setpoint reached 0 well within the hard-cut ramp after the reverse");
		ride_forward(RAW(0), 20U); /* forward confirm, rider stays released (0 pressure) */
		bool ever_positive = false;
		for (int i = 0; i < 4000; i++) {
			control_tick(RAW(0), (i % (int)STEP_INTERVAL_TICKS) == (int)STEP_INTERVAL_TICKS - 1 ? 1 : 0);
			if (g_probe.iq_setpoint > 0) { ever_positive = true; break; }
		}
		CHECK(!ever_positive,
			"S1: NEVER a positive demand/setpoint after forward confirm while the rider stays released (no stale-high reuse)");
	}

	/* ---- S2: high torque -> reverse -> rider keeps pressing -> forward: fresh, not stale ---- */
	{
		reset_all();
		warmup();
		uint16_t warm_afilt = torque_input_get_snapshot()->assist_delta_filtered_native;
		control_tick(RAW(STRONG_DELTA), -1); /* reverse WHILE still pressing hard */
		CHECK(g_probe.session_state == RIDE_SESSION_SUSPENDED_BY_DIRECTION,
			"S2: reverse suspends the session the same tick even under continued pressure");
		for (int i = 0; i < 10; i++) control_tick(RAW(STRONG_DELTA), 0); /* still pressing */
		/* forward confirm while still pressing strong: fast_rearm_this_tick fires and demand must
		 * return from the CURRENT fresh signal, immediately - not a multi-hundred-tick refill. */
		uint32_t rearm_tick_returned = 0xFFFFFFFFU;
		for (uint32_t i = 0; i < 2U * STEP_INTERVAL_TICKS; i++) {
			control_tick(RAW(STRONG_DELTA), (i == STEP_INTERVAL_TICKS - 1U || i == 2U * STEP_INTERVAL_TICKS - 1U) ? 1 : 0);
			if (g_probe.iq_request > 0) { rearm_tick_returned = i; break; }
		}
		CHECK(rearm_tick_returned != 0xFFFFFFFFU,
			"S2: demand returned after the forward confirm under continued strong pressure");
		CHECK(rearm_tick_returned < STEP_INTERVAL_TICKS * 2U,
			"S2: demand returned FAST (within about 2 confirm steps), not after a slow historical refill");
		uint16_t afilt_at_return = torque_input_get_snapshot()->assist_delta_filtered_native;
		CHECK(afilt_at_return + 20U >= warm_afilt,
			"S2: the CURRENT fresh signal (not a stale window) is what came back - close to the still-held strong level");
	}

	/* ---- S3: reverse -> forward at torque=0: zero assist ---- */
	{
		reset_all();
		control_tick(RAW(0), -1);
		ride_forward(RAW(0), 10U);
		for (int i = 0; i < 4000; i++) {
			control_tick(RAW(0), (i % (int)STEP_INTERVAL_TICKS) == (int)STEP_INTERVAL_TICKS - 1 ? 1 : 0);
			CHECK(g_probe.iq_request == 0 && g_probe.iq_setpoint == 0,
				"S3: zero torque after a reverse+forward never produces assist");
		}
	}

	/* ---- S4: noise around the deadband: zero false assist ---- */
	{
		reset_all();
		/* No rearm at all - ordinary RUN, noise hovering just under/at the deadband. */
		for (int i = 0; i < 4000; i++) {
			uint16_t d = (i % 2 == 0) ? 5U : 9U; /* both strictly below TORQUE_ASSIST_DEADBAND_NATIVE=10 */
			control_tick(RAW(d), (i % (int)STEP_INTERVAL_TICKS) == (int)STEP_INTERVAL_TICKS - 1 ? 1 : 0);
			CHECK(g_probe.iq_request == 0,
				"S4: noise strictly below the deadband never produces assist");
		}
	}

	/* ---- S5: brake/fault (safety_cut): zero regardless of torque ---- */
	{
		reset_all();
		warmup();
		CHECK(g_probe.iq_request > 0, "S5: setup - warm demand positive before the safety cut");
		g_safety_cut = 1;
		/* FW-037 (pre-existing, untouched by PATCH A): a hard/safety cut zeroes the DOWNSTREAM
		 * pre-ramp target (a ride_control.c-internal value this probe does not expose) the same
		 * tick, but the REAL motor setpoint fades out over the existing firmware-owned hard-cut
		 * ramp (softens the drivetrain clash) rather than snapping - see assist_dynamics.c's own
		 * FW-037 comment. mode_output.iq_request itself (read by this probe) is the RAW mode
		 * calculation, computed BEFORE the hard-cut gate, so it is not asserted 0 here - what
		 * matters for S5's safety claim is the REAL setpoint, checked below to have reached (and
		 * stayed at) 0 well within the ramp's budget, continuing to hold strong pressure
		 * throughout to prove the cut - not the pressure disappearing - is what drove it to 0. */
		for (int i = 0; i < 1200; i++) {
			control_tick(RAW(STRONG_DELTA), 0); /* strong pressure held throughout */
		}
		CHECK(g_probe.iq_setpoint == 0,
			"S5: the real motor setpoint reached 0 well within the hard-cut ramp, despite continued strong pressure");
		for (int i = 0; i < 400; i++) {
			control_tick(RAW(STRONG_DELTA), 0);
			CHECK(g_probe.iq_setpoint == 0,
				"S5: setpoint stays 0 for as long as the safety cut is engaged, regardless of torque");
		}
		g_safety_cut = 0;

		/* THROTTLE variant: pedal-side `latched` is a completely separate lifecycle
		 * (ride_session.c) from the hard_cut gate this S5 is actually about - throttle assist
		 * is added AFTER the pedal path specifically so it keeps working without pedalling
		 * (ride_control.c's own FW-031 comment), which means a zero-torque, unlatched rider is
		 * NOT sufficient to prove the safety cut itself gates current: throttle would sail
		 * straight through unless hard_cut also (and independently) blocks it - see
		 * `if (!hard_cut && input->throttle_iq > 0)`. This is the one directly-observable
		 * consumer of hard_cut that ISN'T already covered by ordinary pedal/latched zeroing. */
		reset_all();
		g_throttle_iq = (int32_t)PH_CURRENT_MAX / 2;
		for (int i = 0; i < 400; i++) control_tick(RAW(0), 0); /* no pedalling, throttle only */
		CHECK(g_probe.iq_setpoint > 0, "S5 THROTTLE: setup - throttle alone produces a positive setpoint");
		g_safety_cut = 1;
		for (int i = 0; i < 1200; i++) control_tick(RAW(0), 0); /* safety cut engaged, throttle held */
		CHECK(g_probe.iq_setpoint == 0,
			"S5 THROTTLE: safety cut zeroes the setpoint even under continued THROTTLE (not just pedal) demand");
		g_safety_cut = 0;
		g_throttle_iq = 0;
	}

	if (host_test_failures != 0) {
		printf("%d check(s) FAILED\n", host_test_failures);
		return 1;
	}
	printf("All checks PASS\n");
	return 0;
}
