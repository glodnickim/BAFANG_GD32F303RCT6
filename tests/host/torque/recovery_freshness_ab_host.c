/*
 * FW-112.5 A/B HOST HARNESS: ordinary RUN vs rolling-rearm WAIT_FRESH_LOAD, IDENTICAL torque
 * input, same real production chain as tests/host/fw112_run_rearm_recovery_host.c.
 *
 * Purpose (card part C/F): drive the exact same slow torque ramp (delta = 0,8,10,11,12,14,16,
 * 18,20,25,30 native) through two starting conditions -
 *   A) ORDINARY RUN     - recovery_state == IDLE the whole time (no rearm ever happened).
 *   B) ROLLING REARM     - a real reverse + immediate resume puts recovery_state into
 *                          WAIT_FRESH_LOAD right before the SAME ramp starts.
 * and record, for each delta step, AFILT, ARUN, mode iq_request and the real motor
 * MS.i_q_setpoint - to answer one question with real numbers instead of static reasoning:
 * does WAIT_FRESH_LOAD actually require MORE physical pressure than ordinary RUN before the
 * rider gets ANY positive assist?
 *
 * Why this is not obvious from reading the code alone: torque_input.c's WAIT_FRESH_LOAD ->
 * TRACK_FAST transition compares AFILT (already deadband-subtracted) against
 * TORQUE_ASSIST_DEADBAND_NATIVE AGAIN (torque_input.c, the "if (recovery_state ==
 * TORQUE_RECOVERY_WAIT_FRESH_LOAD) { if (assist_delta_filtered_native >=
 * TORQUE_ASSIST_DEADBAND_NATIVE)" branch) - a literal double application of the same constant.
 * But ride_control.c substitutes torque_input_recovery_run_native() (== AFILT, single deadband)
 * directly into the mode calculation for the ENTIRE duration recovery_active() is true (both
 * WAIT_FRESH_LOAD and TRACK_FAST, see ride_control.c's "if (torque_input_recovery_active())"
 * block) - so whether the double threshold has any USER-FELT effect can only be settled by
 * actually running the numbers, which is what this harness does.
 */

#include <stdint.h>
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
#define STEP_INTERVAL_TICKS 42U /* 60 rpm, 96 steps/rev, 4 kHz */
#define RAW(delta) ((uint16_t)(TORQUE_ZERO_TARGET_NATIVE + (uint16_t)(delta)))

static MotorState_t MS;
static uint32_t g_tick;
static uint8_t g_assist_level = TEST_ASSIST_LEVEL;
static uint8_t g_walk_active, g_calibration_active, g_safety_cut, g_real_stop;
static uint32_t g_battery_mv = TEST_BATTERY_MV;

typedef struct {
	uint16_t afilt, arun;
	int32_t iq_request, iq_setpoint;
	uint8_t recovery_state;
} probe_t;

static probe_t g_probe;

static void control_tick(uint16_t raw_mv, int event)
{
	g_tick++;
	if (event == 1) {
		pas_direction_on_step(1);
		torque_input_run_filter_step();
	} else if (event == -1) {
		pas_direction_on_step(-1);
	}
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
	r.pas_backward = false;
	r.pedaling_active = pedaling;
	r.crank_forward_steps = fwd_run;
	r.crank_direction_ok = true;
	r.real_stop = (g_real_stop != 0);
	r.direction_inhibit_active = pas_direction_direction_inhibit_active();
	r.forward_confirmed_this_tick = forward_confirmed_this_tick;
	r.sample_tick = g_tick;
	r.start_phase = false;
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
	in.speed_limit_x100 = 0;
	in.legal_enabled = false;
	in.offroad = false;
	in.walk_active = (g_walk_active != 0);
	in.position_calibration_active = (g_calibration_active != 0);
	in.safety_cut_non_direction = (g_safety_cut != 0);
	in.throttle_iq = 0;
	ride_control_update(&in);

	g_probe.afilt = snap->assist_delta_filtered_native;
	g_probe.arun = snap->assist_delta_run_native;
	g_probe.iq_request = assist_modes_get_last_output()->iq_request;
	g_probe.iq_setpoint = MS.i_q_setpoint;
	g_probe.recovery_state = (uint8_t)torque_input_recovery_state();
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
	g_walk_active = 0;
	g_calibration_active = 0;
	g_safety_cut = 0;
	g_real_stop = 0;
	g_battery_mv = TEST_BATTERY_MV;
}

/* Drives raw_mv steadily for `settle_ticks`, taking one probe at the end. Used to let AFILT
 * settle to (near) its steady-state value for a given delta before reading demand - the point
 * of this A/B is to compare STEADY behaviour at each pressure level, not transient timing. */
static probe_t settle_at(uint16_t delta, uint32_t settle_ticks)
{
	for (uint32_t i = 0; i < settle_ticks; i++) {
		/* one forward step every STEP_INTERVAL_TICKS so the window/step-driven paths (WAIT_
		 * FRESH_LOAD's per-step reseed) are exercised too, not just the tick-driven ones. */
		int event = ((i % STEP_INTERVAL_TICKS) == (STEP_INTERVAL_TICKS - 1U)) ? 1 : 0;
		control_tick(RAW(delta), event);
	}
	return g_probe;
}

static const uint16_t DELTAS[] = { 0, 8, 10, 11, 12, 14, 16, 18, 20, 25, 30 };
#define N_DELTAS (sizeof(DELTAS) / sizeof(DELTAS[0]))

int main(void)
{
	printf("=== SCENARIO A: ordinary RUN (recovery_state == IDLE throughout) ===\n");
	probe_t a[N_DELTAS];
	{
		reset_all();
		/* No rearm anywhere - torque_input_init() already leaves recovery_state == IDLE. */
		for (size_t i = 0; i < N_DELTAS; i++) {
			a[i] = settle_at(DELTAS[i], 4000U); /* 1 s settle at each level, real 4 kHz ticks */
			printf("  delta=%2u  afilt=%3u arun=%3u  iq_request=%4d iq_setpoint=%4d  recovery_state=%u\n",
				DELTAS[i], a[i].afilt, a[i].arun, a[i].iq_request, a[i].iq_setpoint,
				a[i].recovery_state);
		}
	}

	printf("=== SCENARIO B: rolling rearm (recovery_state == WAIT_FRESH_LOAD entering the ramp) ===\n");
	probe_t b[N_DELTAS];
	{
		reset_all();
		/* Warm up ordinary, then reverse (suspends), then resume forward - fast_rearm_this_tick
		 * fires, opening WAIT_FRESH_LOAD - exactly like a real direction reversal on the bike. */
		for (uint32_t i = 0; i < 4000U; i++) {
			int event = ((i % STEP_INTERVAL_TICKS) == (STEP_INTERVAL_TICKS - 1U)) ? 1 : 0;
			control_tick(RAW(30), event);
		}
		control_tick(RAW(0), -1); /* reverse: suspends the session */
		for (int i = 0; i < 10; i++) control_tick(RAW(0), 0);
		/* PAS_REVERSE_RECOVERY_CONFIRM_STEPS forward edges to actually confirm direction and
		 * fire fast_rearm_this_tick - one edge alone is still "confirming", not confirmed. */
		for (uint32_t i = 0; i < PAS_REVERSE_RECOVERY_CONFIRM_STEPS; i++) {
			control_tick(RAW(0), 1);
		}
		/* PATCH A (post-FW-112.5): the automaton now opens directly in TRACK_FAST - see
		 * src/torque_input.c's torque_input_begin_rolling_rearm(). Still `recovery_active()`
		 * either way, which is what this A/B actually depends on (see the file header). */
		CHECK(g_probe.recovery_state == (uint8_t)TORQUE_RECOVERY_TRACK_FAST,
			"setup: recovery is in TRACK_FAST right after the rearm confirm (PATCH A)");
		for (size_t i = 0; i < N_DELTAS; i++) {
			b[i] = settle_at(DELTAS[i], 4000U);
			printf("  delta=%2u  afilt=%3u arun=%3u  iq_request=%4d iq_setpoint=%4d  recovery_state=%u\n",
				DELTAS[i], b[i].afilt, b[i].arun, b[i].iq_request, b[i].iq_setpoint,
				b[i].recovery_state);
		}
	}

	printf("=== PHYSICAL LOAD REQUIRED FOR FIRST POSITIVE ASSIST ===\n");
	int first_a = -1, first_b = -1;
	for (size_t i = 0; i < N_DELTAS; i++) {
		if (first_a < 0 && a[i].iq_request > 0) first_a = (int)DELTAS[i];
		if (first_b < 0 && b[i].iq_request > 0) first_b = (int)DELTAS[i];
	}
	printf("  ordinary RUN:   first positive iq_request at delta=%d\n", first_a);
	printf("  rolling rearm:  first positive iq_request at delta=%d\n", first_b);

	CHECK(first_a >= 0, "ordinary RUN: some delta in the sweep produces positive demand");
	CHECK(first_b >= 0, "rolling rearm: some delta in the sweep produces positive demand");
	CHECK(first_a == first_b,
		"ordinary RUN and rolling rearm require the SAME physical delta for first positive assist");

	if (host_test_failures != 0) {
		printf("%d check(s) FAILED\n", host_test_failures);
		return 1;
	}
	printf("All checks PASS\n");
	return 0;
}
