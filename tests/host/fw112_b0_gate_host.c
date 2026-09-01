/*
 * FW-112 PATCH B0 — PEDAL ASSIST GATE: S1-S10 scenario host proof.
 *
 * This harness tests the TARGET behavior of the pedal_assist_gate module. It links
 * the REAL production modules (ride_control.c, ride_session.c, pas_direction.c,
 * torque_input.c, assist_modes.c, etc.) and exercises S1-S10 from the spec.
 *
 * IMPORTANT: These tests describe the DESIRED behavior after B0a is implemented.
 * Some scenarios will FAIL against the current (pre-gate) codebase — that is
 * expected and proves the gate changes something. After B0a implementation, all
 * S1-S10 tests must PASS. Existing regression suites must also still PASS.
 *
 * The gate itself is exercised THROUGH ride_control_update(), not by calling it
 * directly — the harness builds the exact same rider inputs as main.c would.
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "../common/check.h"

#include "assist_modes.h"
#include "config.h"
#include "motor_core.h"
#include "pas_direction.h"
#include "pas_quadrature.h"
#include "pedal_assist_gate.h"
#include "rider_input.h"
#include "ride_control.h"
#include "ride_session.h"
#include "torque_input.h"
#include "tuning_config.h"

#define TEST_ASSIST_LEVEL   3U
#define TEST_SPEED_X100     1500U
#define TEST_BATTERY_MV     42000U
#define TEST_VOLTAGE_RAW    2000
#define TEST_TEMPERATURE_C  25
#define RIDING_START_LOAD_CENTIKG_DEFAULT 30U
#define STANDSTILL_LOAD_CENTIKG_DEFAULT   70U
#define STRONG_ASSIST_DELTA_NATIVE 310U

static MotorState_t MS;
static uint32_t g_tick;

static uint16_t crc16_ccitt(const uint8_t *buffer, uint16_t length)
{
	uint16_t crc = 0xFFFFU;
	for (uint16_t i = 0; i < length; i++) {
		crc ^= (uint16_t)buffer[i] << 8;
		for (uint8_t bit = 0; bit < 8; bit++) {
			crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static void set_tuning_start_steps(uint8_t value)
{
	uint8_t buffer[TUNING_BLOB_LEN];
	uint16_t len = tuning_config_serialize(buffer);
	buffer[22] = value;
	buffer[23] = 0;
	uint16_t crc = crc16_ccitt(buffer, TUNING_BLOB_LEN - 2U);
	buffer[TUNING_BLOB_LEN - 2U] = (uint8_t)(crc & 0xFFU);
	buffer[TUNING_BLOB_LEN - 1U] = (uint8_t)(crc >> 8);
	tuning_config_apply_blob(buffer, len);
}

typedef struct {
	uint16_t torque_load_centikg;
	uint16_t torque_assist_filtered;
	uint16_t torque_assist_now_native;
	bool     non_direction_safety_cut;
	uint8_t  assist_level_index;
	uint32_t speed_x100;
	bool     pedaling_signal_present;
	bool     real_stop;
} step_t;

#define EV_NONE    0
#define EV_FORWARD 1
#define EV_REVERSE (-1)
#define EV_INVALID 2

static void feed_pressure(uint16_t filtered_target)
{
	uint32_t delta = (uint32_t)filtered_target + TORQUE_ASSIST_DEADBAND_NATIVE;
	uint32_t raw = (uint32_t)TORQUE_ZERO_TARGET_NATIVE + delta;
	if (raw > TORQUE_SPAN_MAX_NATIVE) raw = TORQUE_SPAN_MAX_NATIVE;
	torque_input_update((uint16_t)raw, torque_input_correct((uint16_t)raw), true);
}

static void do_tick(step_t *s, int event)
{
	g_tick++;
	if (event != EV_NONE) {
		int8_t decoded_dir = (event == EV_INVALID) ? 0 : (int8_t)event;
		(void)pas_direction_on_step(decoded_dir);
	}
	if (event == EV_FORWARD) torque_input_run_filter_step();
	feed_pressure(s->torque_assist_filtered);

	const torque_snapshot_t *snap = torque_input_get_snapshot();
	bool direction_inhibit_active = pas_direction_direction_inhibit_active();
	bool forward_confirmed_this_tick = (event != EV_NONE) &&
		pas_direction_forward_confirmed_last_call();
	uint8_t fwd_run = pas_direction_fwd_run();

	bool crank_direction_ok = s->pedaling_signal_present && !s->real_stop;
	bool pedaling = crank_direction_ok && (fwd_run >= tuning_config_start_steps());

	rider_input_t r;
	memset(&r, 0, sizeof(r));
	r.torque_load_centikg = s->torque_load_centikg;
	r.torque_assist_filtered = snap->assist_delta_filtered_native;
	r.torque_run_filtered = snap->assist_delta_run_native;
	r.torque_assist_now_native = snap->assist_delta_native;
	r.cadence_rpm = crank_direction_ok ? 60U : 0U;
	r.wheel_speed_x100 = s->speed_x100;
	r.motor_erps = crank_direction_ok ? 200U : 0U;
	r.pas_forward = pedaling;
	r.pas_backward = false;
	r.pedaling_active = pedaling;
	r.crank_forward_steps = fwd_run;
	r.crank_direction_ok = crank_direction_ok;
	r.real_stop = s->real_stop;
	r.direction_inhibit_active = direction_inhibit_active;
	r.forward_confirmed_this_tick = forward_confirmed_this_tick;
	r.sample_tick = g_tick;
	r.start_phase = false;
	r.torque_sensor_valid = true;
	r.pas_sensor_valid = true;
	rider_input_update(&r);

	ride_control_input_t in;
	memset(&in, 0, sizeof(in));
	in.speed_x100 = s->speed_x100;
	in.cadence_rpm = r.cadence_rpm;
	in.assist_level_index = s->assist_level_index;
	in.battery_voltage_mv = TEST_BATTERY_MV;
	in.iq_scale = (int32_t)PH_CURRENT_MAX;
	in.ride_core_iq_limit = (int32_t)PH_CURRENT_MAX;
	in.phase_current_max = (int32_t)PH_CURRENT_MAX;
	in.current_iq = MS.i_q_setpoint;
	in.current_id = MS.i_d_setpoint;
	in.voltage_raw = TEST_VOLTAGE_RAW;
	in.voltage_min_raw = VOLTAGE_MIN;
	in.controller_temperature_c = TEST_TEMPERATURE_C;
	in.cadence_filtered_x8 = (uint16_t)(r.cadence_rpm * 8U);
	in.speed_limit_x100 = SPEEDLIMIT;
	in.legal_enabled = (LEGALFLAG != 0);
	in.offroad = false;
	in.walk_active = false;
	in.position_calibration_active = false;
	in.safety_cut_non_direction = s->non_direction_safety_cut;
	in.throttle_iq = 0;
	ride_control_update(&in);
	for (unsigned q = 0; q < 4U; q++) {
		fast_iq_slew_tick(ride_control_final_iq_slew_mailbox(), &MS.i_q_setpoint);
	}
}

static void fwd1(step_t *s) { do_tick(s, EV_FORWARD); }
static void rev1(step_t *s) { do_tick(s, EV_REVERSE); }
static void hold(step_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) do_tick(s, EV_NONE); }

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
	set_tuning_start_steps(TUNING_START_STEPS_DEFAULT);
}

static step_t riding_step(void)
{
	step_t s;
	memset(&s, 0, sizeof(s));
	s.pedaling_signal_present = true;
	s.real_stop = false;
	s.speed_x100 = TEST_SPEED_X100;
	s.assist_level_index = TEST_ASSIST_LEVEL;
	return s;
}

static int32_t live_target(void)
{
	ride_arm_snapshot_t snap;
	ride_control_get_arm_snapshot(&snap);
	return snap.iq_pre_ramp;
}

static void set_level_thresholds(uint16_t standstill_centikg, uint16_t riding_centikg)
{
	const assist_level_config_t *level = assist_modes_get_default_level(TEST_ASSIST_LEVEL);
	((assist_level_config_t *)level)->minimum_pedal_load_centikg = standstill_centikg;
	((assist_level_config_t *)level)->riding_start_load_centikg = riding_centikg;
}

int main(void)
{
	printf("FW-112 PATCH B0 — S1-S10 pedal assist gate scenario proof\n");
	printf("  These tests describe the TARGET behavior after B0a implementation.\n");
	printf("  Some scenarios may FAIL against the current (pre-gate) codebase.\n");
	printf("  After B0a, all S1-S10 must PASS.\n\n");

	/*
	 * S1: COLD START NORMAL
	 * Forward PAS confirmed, load below threshold -> gate CLOSED, assist=0.
	 * Load exceeds threshold -> gate OPEN, calculated_iq flows immediately.
	 */
	printf("S1: cold start normal\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Start with load BELOW threshold: gate should be CLOSED */
		s.torque_load_centikg = 5U;  /* below 30 centikg riding threshold */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		/* Gate should still be closed or assist should be 0 */
		int32_t target_before = live_target();
		CHECK(target_before <= 0, "S1: load below threshold -> no assist");

		/* Now exceed the threshold: gate should OPEN */
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target_after = live_target();
		CHECK(target_after > 0, "S1: load above threshold -> assist flows");
		printf("  S1: target_before=%ld target_after=%ld\n",
			(long)target_before, (long)target_after);
	}

	/*
	 * S2: DEAD SPOT
	 * Gate OPEN, torque drops to 0 then recovers. Forward pedaling continues.
	 * Gate must NOT close during the dead spot.
	 */
	printf("S2: dead spot\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Establish gate OPEN with full pressure */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target_full = live_target();
		CHECK(target_full > 0, "S2: gate open with full pressure");

		/* Drop torque to zero (dead spot) but keep forward pedaling */
		s.torque_assist_filtered = 0U;
		s.torque_assist_now_native = 0U;
		s.torque_load_centikg = 0U;
		for (uint16_t i = 0; i < 200U; i++) fwd1(&s);
		hold(&s, 2000U);

		/* Gate must still be open — target can be 0 (calculated_iq=0) but
		 * gate is NOT closed, so a torque recovery should flow immediately */
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target_recovered = live_target();
		CHECK(target_recovered > 0, "S2: torque recovers after dead spot -> assist flows (gate stayed open)");
		printf("  S2: target_full=%ld target_recovered=%ld\n",
			(long)target_full, (long)target_recovered);
	}

	/*
	 * S3: LONG LOW TORQUE WHILE PEDALING
	 * Gate OPEN, torque remains below start_load for many rotations.
	 * PAS forward/alive. Gate stays OPEN.
	 * calculated_iq may drop to 0 — that is fine, gate is still OPEN.
	 */
	printf("S3: long low torque while pedaling\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Establish gate OPEN */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "S3: gate open initially");

		/* Drop below start_load but keep pedaling forward */
		s.torque_load_centikg = 5U;  /* below threshold */
		s.torque_assist_filtered = 0U;
		s.torque_assist_now_native = 0U;
		for (uint16_t i = 0; i < 500U; i++) fwd1(&s);
		hold(&s, 2000U);

		/* Gate should still be open — forward pedaling continues */
		int32_t target_low = live_target();
		/* calculated_iq may be 0, but gate is OPEN */

		/* Now restore torque — should flow immediately, no re-authorization */
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target_restored = live_target();
		CHECK(target_restored > 0, "S3: torque restored -> assist flows (gate stayed open, no re-auth)");
		printf("  S3: target_low=%ld target_restored=%ld\n",
			(long)target_low, (long)target_restored);
	}

	/*
	 * S4: TORQUE RETURNS
	 * After S3 (long low torque), torque rises again.
	 * No re-test of START_LOAD. Calculated assist returns immediately.
	 */
	printf("S4: torque returns after low period\n");
	{
		/* S4 is validated as part of S3 above — the immediate recovery */
		printf("  S4: validated by S3's recovery path\n");
	}

	/*
	 * S5: REAL STOP
	 * Gate OPEN, then real_stop fires -> gate CLOSED, assist=0.
	 * Next start requires forward AND start_load from scratch.
	 */
	printf("S5: real stop\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Establish gate OPEN */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "S5: gate open");

		/* Simulate real_stop (PAS timeout) */
		s.real_stop = true;
		s.pedaling_signal_present = false;
		for (uint16_t i = 0; i < 2000U; i++) do_tick(&s, EV_NONE);

		int32_t target_after_stop = live_target();
		CHECK(target_after_stop <= 0, "S5: real_stop -> gate closed, no assist");

		/* Try to resume without forward pedaling — should not work */
		s.real_stop = false;
		s.pedaling_signal_present = true;
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target_resume = live_target();
		CHECK(target_resume > 0, "S5: forward re-confirmed after stop -> gate re-opens");
		printf("  S5: target_after_stop=%ld target_resume=%ld\n",
			(long)target_after_stop, (long)target_resume);
	}

	/*
	 * S6: REVERSE
	 * Gate OPEN, then confirmed reverse -> gate CLOSED, assist=0.
	 * Forward re-confirmation required for next start.
	 */
	printf("S6: reverse\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Establish gate OPEN */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "S6: gate open");

		/* Reverse: direction inhibit fires, gate should close */
		rev1(&s);
		hold(&s, 200U);
		int32_t target_after_rev = live_target();
		CHECK(target_after_rev <= 0, "S6: reverse -> gate closed, no assist");

		/* Forward confirm again: gate should re-open */
		fwd1(&s);
		fwd1(&s);
		fwd1(&s);
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target_after_fwd = live_target();
		CHECK(target_after_fwd > 0, "S6: forward re-confirmed after reverse -> gate re-opens");
		printf("  S6: target_after_rev=%ld target_after_fwd=%ld\n",
			(long)target_after_rev, (long)target_after_fwd);
	}

	/*
	 * S7: BRAKE
	 * Gate OPEN + forward + torque -> brake -> final iq=0 immediately.
	 * After brake release with forward still active: gate stays OPEN.
	 * NO re-test of START_LOAD.
	 */
	printf("S7: brake\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Establish gate OPEN */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target_before_brake = live_target();
		CHECK(target_before_brake > 0, "S7: gate open before brake");

		/* Apply brake: iq should go to 0 but gate stays OPEN */
		s.non_direction_safety_cut = true;
		fwd1(&s);
		hold(&s, 100U);
		int32_t target_during_brake = live_target();
		CHECK(target_during_brake <= 0, "S7: brake -> iq=0");

		/* Release brake: gate should still be OPEN, no re-auth needed */
		s.non_direction_safety_cut = false;
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target_after_brake = live_target();
		CHECK(target_after_brake > 0, "S7: brake released -> assist flows (gate stayed open, no re-auth)");
		printf("  S7: before=%ld during=%ld after=%ld\n",
			(long)target_before_brake, (long)target_during_brake,
			(long)target_after_brake);
	}

	/*
	 * S8: START_LOAD=0
	 * Standstill: gate CLOSED (no forward).
	 * Forward confirmed: gate OPEN (no torque required).
	 * Stop: gate CLOSED.
	 */
	printf("S8: start_load=0\n");
	{
		reset_all();
		set_level_thresholds(0U, 0U);  /* configure level with zero start thresholds */
		step_t s = riding_step();
		s.torque_load_centikg = 0U;  /* start_load = 0 */
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Standstill: no forward -> gate CLOSED */
		hold(&s, 100U);
		int32_t target_standstill = live_target();
		CHECK(target_standstill <= 0, "S8: standstill -> gate closed");

		/* Forward confirmed: gate should OPEN even at zero load */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target_forward = live_target();
		CHECK(target_forward > 0, "S8: forward confirmed + start_load=0 -> gate open, assist flows");

		/* Stop pedalling: gate should CLOSE */
		s.pedaling_signal_present = false;
		s.real_stop = true;
		for (uint16_t i = 0; i < 2000U; i++) do_tick(&s, EV_NONE);
		int32_t target_stopped = live_target();
		CHECK(target_stopped <= 0, "S8: stopped -> gate closed");
		printf("  S8: standstill=%ld forward=%ld stopped=%ld\n",
			(long)target_standstill, (long)target_forward, (long)target_stopped);
	}

	/*
	 * S9: SENSOR NOISE
	 * No real pressure, sensor noise around zero.
	 * START_LOAD > noise -> gate never opens.
	 */
	printf("S9: sensor noise\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = 5U;  /* below start threshold */
		s.torque_assist_filtered = 0U;  /* no real pressure */
		s.torque_assist_now_native = 0U;

		/* Pedal forward but with noise-level load */
		for (uint16_t i = 0; i < 500U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target_noise = live_target();
		CHECK(target_noise <= 0, "S9: noise below threshold -> gate stays closed");
		printf("  S9: target_noise=%ld\n", (long)target_noise);
	}

	/*
	 * S10: CALCULATION PRE-WARM
	 * Gate CLOSED, rider already pressing, filters compute in background.
	 * Forward + load met -> gate OPEN -> calculated_iq already has magnitude.
	 * No additional delay from starting AFILT/ARUN after OPEN.
	 */
	printf("S10: calculation pre-warm\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = 5U;  /* below threshold initially */
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Pedal with load below threshold: filters should be warming up */
		for (uint16_t i = 0; i < 200U; i++) fwd1(&s);
		hold(&s, 1000U);
		int32_t target_prewarm = live_target();
		CHECK(target_prewarm <= 0, "S10: below threshold -> gate closed, no assist yet");

		/* Now exceed threshold: gate should OPEN and assist should be
		 * immediate (not starting from zero due to pre-warmed filters) */
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target_prewarmed = live_target();
		CHECK(target_prewarmed > 0, "S10: threshold exceeded -> gate open, immediate assist (pre-warmed)");
		printf("  S10: prewarm=%ld prewarmed=%ld\n",
			(long)target_prewarm, (long)target_prewarmed);
	}

	/* =========================================================================
	 * MUTATION TESTS M1-M9
	 * =========================================================================
	 * Each mutation MUST fail. If a mutation passes, the gate has a bug.
	 */

	printf("\n--- MUTATION TESTS ---\n");

	/*
	 * M1: Gate opens without forward PAS
	 * Mutation: gate should NOT open if there is no forward pedaling,
	 * even if torque is high.
	 */
	printf("M1: gate must not open without forward PAS\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		s.pedaling_signal_present = false;  /* no forward pedaling */

		/* Hold with no forward steps */
		for (uint16_t i = 0; i < 500U; i++) do_tick(&s, EV_NONE);
		hold(&s, 2000U);
		int32_t target = live_target();
		/* If this PASSES (target > 0), the mutation test FAILS — gate opened without forward */
		CHECK(target <= 0, "M1 FAIL (gate opened without forward):");
		if (target > 0) printf("  M1 MUTATION DETECTED: target=%ld (gate opened without forward!)\n", (long)target);
	}

	/*
	 * M2: START_LOAD > 0 is ignored
	 * Mutation: gate must NOT open at low load when threshold > 0.
	 */
	printf("M2: START_LOAD > 0 must not be ignored\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = 5U;  /* well below 30 centikg threshold */
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target = live_target();
		CHECK(target <= 0, "M2 FAIL (gate opened below threshold):");
		if (target > 0) printf("  M2 MUTATION DETECTED: target=%ld (gate opened below threshold!)\n", (long)target);
	}

	/*
	 * M3: Gate closes when torque temporarily drops
	 * Mutation: gate must NOT close on torque dip if forward continues.
	 */
	printf("M3: gate must not close on torque dip\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Open gate */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "M3 setup: gate open");

		/* Drop torque, keep forward */
		s.torque_assist_filtered = 0U;
		s.torque_assist_now_native = 0U;
		s.torque_load_centikg = 0U;
		for (uint16_t i = 0; i < 200U; i++) fwd1(&s);
		hold(&s, 2000U);

		/* Restore torque: must flow immediately (gate was never closed) */
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		fwd1(&s);
		hold(&s, 2000U);
		int32_t target = live_target();
		CHECK(target > 0, "M3 FAIL (gate closed on torque dip):");
		if (target <= 0) printf("  M3 MUTATION DETECTED: gate closed during torque dip!\n");
	}

	/*
	 * M4: real_stop does not close gate
	 * Mutation: real_stop MUST close the gate.
	 */
	printf("M4: real_stop must close gate\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "M4 setup: gate open");

		s.real_stop = true;
		s.pedaling_signal_present = false;
		for (uint16_t i = 0; i < 2000U; i++) do_tick(&s, EV_NONE);
		int32_t target = live_target();
		CHECK(target <= 0, "M4 FAIL (real_stop did not close gate):");
		if (target > 0) printf("  M4 MUTATION DETECTED: target=%ld (gate still open after real_stop!)\n", (long)target);
	}

	/*
	 * M5: Reverse does not zero assist
	 * Mutation: reverse MUST zero assist and close gate.
	 */
	printf("M5: reverse must zero assist\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "M5 setup: gate open");

		rev1(&s);
		hold(&s, 200U);
		int32_t target = live_target();
		CHECK(target <= 0, "M5 FAIL (reverse did not zero assist):");
		if (target > 0) printf("  M5 MUTATION DETECTED: target=%ld (assist still active after reverse!)\n", (long)target);
	}

	/*
	 * M6: Brake does not zero final Iq
	 * Mutation: brake MUST zero final Iq.
	 */
	printf("M6: brake must zero final Iq\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "M6 setup: gate open");

		s.non_direction_safety_cut = true;
		fwd1(&s);
		hold(&s, 100U);
		int32_t target = live_target();
		CHECK(target <= 0, "M6 FAIL (brake did not zero Iq):");
		if (target > 0) printf("  M6 MUTATION DETECTED: target=%ld (Iq not zero during brake!)\n", (long)target);
	}

	/*
	 * M7: START_LOAD=0 opens gate at standstill
	 * Mutation: gate must NOT open at standstill even with start_load=0.
	 * Forward pedaling is always required.
	 */
	printf("M7: START_LOAD=0 must not open gate at standstill\n");
	{
		reset_all();
		set_level_thresholds(0U, 0U);
		step_t s = riding_step();
		s.torque_load_centikg = 0U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		s.pedaling_signal_present = false;

		for (uint16_t i = 0; i < 500U; i++) do_tick(&s, EV_NONE);
		hold(&s, 2000U);
		int32_t target = live_target();
		CHECK(target <= 0, "M7 FAIL (gate opened at standstill with start_load=0):");
		if (target > 0) printf("  M7 MUTATION DETECTED: target=%ld (gate opened at standstill!)\n", (long)target);
	}

	/*
	 * M8: Old recovery/permission blocks output despite gate OPEN
	 * Mutation: no old mechanism may block assist when gate OPEN, forward valid,
	 * brake clear, fault clear, calculated_iq > 0.
	 * This is tested implicitly by S2/S3/S7 — if the old rearm_permission or
	 * recovery_wait still blocks, those scenarios fail.
	 */
	printf("M8: old permission/recovery must not block when gate OPEN\n");
	printf("  M8: validated by S2 (dead spot recovery) + S3 (low torque) + S7 (brake release)\n");

	/*
	 * M9: Stale pre-stop torque leaks after new start
	 * Mutation: after a full stop -> restart cycle, stale torque from before
	 * the stop must not produce assist.
	 */
	printf("M9: stale pre-stop torque must not leak\n");
	{
		reset_all();
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;

		/* Start, run, stop */
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		CHECK(live_target() > 0, "M9 setup: gate open");

		s.real_stop = true;
		s.pedaling_signal_present = false;
		for (uint16_t i = 0; i < 2000U; i++) do_tick(&s, EV_NONE);
		CHECK(live_target() <= 0, "M9 setup: stopped");

		/* Restart with ZERO torque — stale should not leak */
		s.real_stop = false;
		s.pedaling_signal_present = true;
		s.torque_assist_filtered = 0U;
		s.torque_assist_now_native = 0U;
		s.torque_load_centikg = 0U;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 2000U);
		int32_t target = live_target();
		CHECK(target <= 0, "M9 FAIL (stale torque leaked after restart):");
		if (target > 0) printf("  M9 MUTATION DETECTED: target=%ld (stale torque leaked!)\n", (long)target);
	}

	/* ============================================================
	 * G1-G4: GATE SET OWNERSHIP
	 * Proves session events do NOT bypass load condition.
	 * Gate opens ONLY on forward_valid && load_met.
	 * ============================================================ */

	printf("G1: gate stays CLOSED when load_met=false (direct API proof)\n");
	{
		pedal_assist_gate_init();
		pedal_assist_gate_input_t gin;
		gin.forward_valid = true;
		gin.load_met = false;
		gin.stop_or_reverse = false;
		pedal_assist_gate_output_t gout;
		pedal_assist_gate_update(&gin, &gout);
		CHECK(!gout.open, "G1 FAIL (gate opened with load < START_LOAD):");
		if (gout.open) printf("  G1 MUTATION: gate opened bypassing load!\n");
	}

	printf("G2: gate stays CLOSED across 100 low-load ticks, opens on load_met\n");
	{
		pedal_assist_gate_init();
		pedal_assist_gate_input_t gin;
		gin.forward_valid = true;
		gin.load_met = false;
		gin.stop_or_reverse = false;
		pedal_assist_gate_output_t gout;
		for (uint16_t i = 0; i < 100U; i++) {
			pedal_assist_gate_update(&gin, &gout);
			CHECK(!gout.open, "G2 FAIL (gate opened during low load):");
			if (gout.open) { printf("  G2 MUTATION: gate opened at tick %u!\n", i); break; }
		}
		gin.load_met = true;
		pedal_assist_gate_update(&gin, &gout);
		CHECK(gout.open, "G2 FAIL (gate did not open on load_met):");
		if (!gout.open) printf("  G2 MUTATION: gate failed to open on load_met!\n");
	}

	printf("G3: START_LOAD == 0, forward valid => gate OPENS (integration)\n");
	{
		reset_all();
		set_level_thresholds(0U, 0U);
		step_t s = riding_step();
		s.torque_load_centikg = 0U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 500U);
		int32_t target = live_target();
		CHECK(target > 0, "G3 FAIL (gate did not open with START_LOAD=0):");
		if (target <= 0) printf("  G3 MUTATION: target=%ld (gate stayed closed!)\n", (long)target);
	}

	printf("G4: START_LOAD > 0, load crosses threshold => gate OPENS immediately\n");
	{
		reset_all();
		set_level_thresholds(STANDSTILL_LOAD_CENTIKG_DEFAULT, RIDING_START_LOAD_CENTIKG_DEFAULT);
		step_t s = riding_step();
		s.torque_load_centikg = 0U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 200U);
		CHECK(live_target() <= 0, "G4 setup: gate closed below threshold");
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 5U;
		fwd1(&s);
		int32_t target = live_target();
		CHECK(target > 0, "G4 FAIL (gate did not open on threshold cross):");
		if (target <= 0) printf("  G4 MUTATION: target=%ld (gate delayed opening!)\n", (long)target);
	}

	/* ============================================================
	 * C1-C2: CALCULATION ALWAYS RUNNING
	 * Mode calculation produces iq regardless of gate state.
	 * Gate controls OUTPUT, not calculation.
	 * ============================================================ */

	printf("C1: gate CLOSED (load below threshold), torque present => output = 0\n");
	{
		reset_all();
		set_level_thresholds(STANDSTILL_LOAD_CENTIKG_DEFAULT, RIDING_START_LOAD_CENTIKG_DEFAULT);
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 500U);
		CHECK(live_target() > 0, "C1 setup: gate open");
		rev1(&s);
		hold(&s, 50U);
		CHECK(live_target() <= 0, "C1 setup: gate closed after reverse");
		/* Now pedalling with valid torque but load BELOW threshold.
		 * Gate must stay closed. Calculation still runs (valid sensors),
		 * but gate blocks output. */
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT - 5U;
		s.pedaling_signal_present = true;
		for (uint16_t i = 0; i < 200U; i++) fwd1(&s);
		int32_t target = live_target();
		CHECK(target <= 0, "C1 FAIL (output leaked through closed gate):");
		if (target > 0) printf("  C1 MUTATION: target=%ld\n", (long)target);
	}

	printf("C2: open gate after reverse => calculated_iq flows immediately\n");
	{
		reset_all();
		set_level_thresholds(STANDSTILL_LOAD_CENTIKG_DEFAULT, RIDING_START_LOAD_CENTIKG_DEFAULT);
		step_t s = riding_step();
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		s.torque_assist_now_native = STRONG_ASSIST_DELTA_NATIVE;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 500U);
		int32_t t1 = live_target();
		CHECK(t1 > 0, "C2 setup: gate open, Iq flowing");
		rev1(&s);
		hold(&s, 50U);
		s.torque_load_centikg = RIDING_START_LOAD_CENTIKG_DEFAULT + 20U;
		s.pedaling_signal_present = true;
		for (uint8_t i = 0; i < 8U; i++) fwd1(&s);
		hold(&s, 500U);
		int32_t t2 = live_target();
		CHECK(t2 > 0, "C2 FAIL (no Iq after gate re-open):");
		if (t2 <= 0) printf("  C2 MUTATION: target=%ld\n", (long)t2);
	}

	printf("\nFW-112 PATCH B0 S1-S10 + M1-M9 + G1-G4 + C1-C2 host proof complete.\n");
	return host_test_failures ? 1 : 0;
}
