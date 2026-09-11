/*
 * FW-129B state-hygiene harness — real src/ride_control.c and its whole shipped dependency
 * chain, linked, not mirrored.
 *
 * FW-129 turned the requested motor POWER from a ceiling into the request itself. That single
 * change made three latent properties of the demand path suddenly matter, and all three were
 * found and fixed while implementing it. This harness exists to PIN them: each block
 * reproduces the original failing sequence, so a future change that reintroduces one fails
 * here rather than on the bike.
 *
 *   T1  ghost assist. A charged power filter must never be an independent source of torque:
 *       no valid rider demand -> no positive motor demand, over a long window, not one tick.
 *   T2  zero-demand decay. Charging the filter hard and then removing the demand must not let
 *       the filter's own decay out as a motor request.
 *   T3  re-entry. When a valid demand returns, the CALCULATED TARGET must come from the
 *       current inputs - the power filter may not add a second, hidden soft-start.
 *   T4  ...while the EXPLICIT downstream shaping (the Iq ramp) stays in place. Correct target
 *       is not the same thing as instant motor torque, and this card must not remove the ramp.
 *   T5  dirty-state reinit. Every dynamic runtime field must come back to its cold value.
 *   T6  reinit output equivalence - the real proof of T5: identical input sequences produce
 *       identical outputs whether the controller was freshly initialised or had just been
 *       hammered with an unrelated high-demand sequence.
 *   T7  mode switch: one mode's filter state may not produce torque in another.
 *   T8  reverse / invalid direction: a backpedal may never recover the old positive state.
 *   T9  calibration invariance: none of the above touched the sensor characteristic.
 *   T10 FW-129 golden behaviour: the Power correction (~2.17x) must not be walked back.
 *   T12 no positive output without grant, as a property over combinations of states.
 *
 * (T11, the five-mode steady-state matrix, is not duplicated here: tests/host/
 * fw129_unit_domain_host.c already owns it and is the source of truth for those numbers.
 * FW-129B's contract against it is a table diff, recorded in the card report.)
 */

#include <stdint.h>
#include <string.h>

#include "../common/check.h"

#include "assist_dynamics.h"
#include "assist_extended_boost.h"
#include "assist_modes.h"
#include "assist_start.h"
#include "config.h"
#include "fast_iq_slew.h"
#include "motor_core.h"
#include "pas_direction.h"
#include "pas_quadrature.h"
#include "ride_control.h"
#include "ride_session.h"
#include "rider_input.h"
#include "torque_input.h"
#include "tuning_config.h"

#define TEST_ASSIST_LEVEL 3U
#define TEST_SPEED_X100   1500U
#define TEST_BATTERY_MV   42000U
#define TEST_VOLTAGE_RAW  2000
#define TEST_TEMPERATURE_C 30

/* 16 kHz FOC / 4 kHz control: the ISR runs the final slew integration four times per
 * control tick (see pi_iq_apply_inputs() in src/main.c). */
#define FOC_TICKS_PER_CTRL 4U

/* A firm, unambiguous push, and the near-zero residual the ghost-assist case was found at:
 * ~0.04 kg of equivalent load, which is sensor noise, not a rider. */
#define STRONG_ASSIST_DELTA_NATIVE 310U
#define GHOST_RESIDUAL_NATIVE 1U

static MotorState_t MS;

static uint32_t g_tick;

/* --- scaffolding: the same shape main.c builds every tick ---------------------------------- */

typedef struct {
	uint16_t torque_load_centikg;
	uint16_t torque_assist_filtered;
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
	if (raw > TORQUE_SPAN_MAX_NATIVE) {
		raw = TORQUE_SPAN_MAX_NATIVE;
	}
	torque_input_update((uint16_t)raw, torque_input_correct((uint16_t)raw), true);
}

static void do_tick(step_t *s, int event)
{
	g_tick++;
	if (event != EV_NONE) {
		int8_t decoded_dir = (event == EV_INVALID) ? 0 : (int8_t)event;
		(void)pas_direction_on_step(decoded_dir);
	}
	if (event == EV_FORWARD) {
		torque_input_run_filter_step();
	}
	feed_pressure(s->torque_assist_filtered);

	const torque_snapshot_t *snap = torque_input_get_snapshot();
	bool direction_inhibit_active = pas_direction_direction_inhibit_active();
	bool forward_confirmed_this_tick =
		(event != EV_NONE) && pas_direction_forward_confirmed_last_call();
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
	r.pedaling_active = pedaling;
	r.crank_forward_steps = fwd_run;
	r.crank_direction_ok = crank_direction_ok;
	r.real_stop = s->real_stop;
	r.direction_inhibit_active = direction_inhibit_active;
	r.forward_confirmed_this_tick = forward_confirmed_this_tick;
	r.sample_tick = g_tick;
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
	/* QS-3D: ride_control_update() now only PUBLISHES the final Iq demand to the 16 kHz
	 * mailbox (fast_iq_slew_publish). MS.i_q_setpoint is produced by the real 16 kHz owner
	 * (fast_iq_slew_tick) in pi_iq_apply_inputs(), i.e. once per FOC tick within this control
	 * tick - mirror the production wiring exactly so the ramp is observable here. */
	fast_iq_slew_mailbox_t *mb = ride_control_final_iq_slew_mailbox();
	for (unsigned q = 0; q < FOC_TICKS_PER_CTRL; q++) {
		fast_iq_slew_tick(mb, &MS.i_q_setpoint);
	}
}

static void fwd1(step_t *s) { do_tick(s, EV_FORWARD); }
static void rev1(step_t *s) { do_tick(s, EV_REVERSE); }
static void hold(step_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) do_tick(s, EV_NONE); }

/* Ride forward for `steps` PAS steps, one step every `interval` ticks. */
#define STEP_INTERVAL_TICKS 42U
static void ride_forward(step_t *s, uint32_t steps)
{
	for (uint32_t k = 0; k < steps; k++) {
		for (uint32_t t = 0; t < STEP_INTERVAL_TICKS; t++) {
			do_tick(s, (t == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
		}
	}
}

static int32_t mode_iq(void)
{
	return assist_modes_get_last_output()->iq_request;
}

static int32_t live_target(void)
{
	ride_arm_snapshot_t snap;
	ride_control_get_arm_snapshot(&snap);
	return snap.iq_pre_ramp;
}

static bool is_latched(void)
{
	return (ride_control_get_debug_flags() & RIDE_DBG_NOT_LATCHED) == 0;
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
}

static step_t base_step(void)
{
	step_t s;
	memset(&s, 0, sizeof(s));
	s.assist_level_index = TEST_ASSIST_LEVEL;
	s.speed_x100 = TEST_SPEED_X100;
	s.pedaling_signal_present = true;
	s.torque_load_centikg = 200U;
	s.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
	return s;
}

/* Get to a warm, latched, assisting state - the precondition every issue was found from. */
static step_t establish_assist(void)
{
	step_t s = base_step();
	/*
	 * AP-03: 60 steps, was 30. Ordinary RUN is the FW-085 crank-angle window again (the
	 * FW-112.4 tick-clocked filter that used to overwrite it is deleted), and the default
	 * window is 48 steps - so 30 forward steps could no longer establish a fully charged
	 * estimate, and T2's "the filter really is charged" PRECONDITION stopped being met.
	 * This lengthens the SETUP so the precondition holds for a step-clocked estimator; it
	 * does not touch a single asserted property. The properties below must still pass on
	 * their own - if they do not, that is a real ghost-assist regression, not a test issue.
	 */
	ride_forward(&s, 60U);
	CHECK(is_latched(), "setup: latched after a normal forward start");
	CHECK(mode_iq() > 0, "setup: a real demand is flowing before the sequence under test");
	return s;
}

/* ------------------------------------------------------------------ T1 */

/*
 * ISSUE 1 REPRODUCTION. The original: a warm ride, the rider releases, a backpedal, and a
 * residual sensor reading worth about 0.04 kg. The mode still asked for 127 Iq counts, out of
 * a power filter that was still holding 253 W from before the reverse. The rider was asking
 * for nothing; the filter was the entire source of the request.
 */
static void t1_ghost_assist(void)
{
	printf("T1 (ISSUE 1) ghost assist: a charged filter is not a torque source\n");
	reset_all();
	step_t s = establish_assist();

	/* The rider lets go, then backpedals - with the filter still full. */
	s.torque_assist_filtered = GHOST_RESIDUAL_NATIVE;
	rev1(&s);
	CHECK(!is_latched(), "T1: the backpedal suspends the session");

	/* The forward confirm restores PERMISSION as a pure direction fact (FW-112 v2). The
	 * rider is still not pressing, so it must restore no DEMAND whatsoever. */
	for (uint32_t i = 0; i < 2U; i++) fwd1(&s);
	CHECK(is_latched(), "T1: permission comes back on the confirm, as it should");

	/*
	 * Let the SENSOR chain flush first. The RUN estimator averages pedal effort over half a
	 * crank turn and falls with its own documented time constant (FW-085/112.4): right after a
	 * release it still reports the effort of the stroke that just happened, and that is an
	 * honest answer, not a ghost. What this test is about is what happens AFTER the rider's
	 * input has genuinely gone - whether anything downstream keeps a demand alive on its own.
	 */
	for (uint32_t i = 0; i < 6000U; i++) {
		do_tick(&s, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
	}

	int32_t worst_mode = 0;
	int32_t worst_target = 0;
	int32_t worst_setpoint = 0;
	for (uint32_t i = 0; i < 4000U; i++) {
		do_tick(&s, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
		if (mode_iq() > worst_mode) worst_mode = mode_iq();
		if (live_target() > worst_target) worst_target = live_target();
		if (MS.i_q_setpoint > worst_setpoint) worst_setpoint = MS.i_q_setpoint;
	}
	char label[160];
	snprintf(label, sizeof(label),
		"T1: no rider demand -> no mode demand, over 4000 ticks (worst %ld)",
		(long)worst_mode);
	CHECK(worst_mode == 0, label);
	snprintf(label, sizeof(label),
		"T1: ...and no ride-control target either (worst %ld)", (long)worst_target);
	CHECK(worst_target == 0, label);
	snprintf(label, sizeof(label),
		"T1: ...and nothing reaches the motor (worst setpoint %ld)", (long)worst_setpoint);
	CHECK(worst_setpoint == 0, label);
}

/* ------------------------------------------------------------------ T2 */

/*
 * The same contract without the direction event: charge the filter hard, then simply take the
 * rider away while they keep pedalling forward. The filter's decay is allowed to exist
 * internally; it is not allowed out as a motor request.
 */
static void t2_zero_demand_decay(void)
{
	printf("T2 (ISSUE 1) a decaying filter may not leak out as demand\n");
	reset_all();
	step_t s = establish_assist();
	int32_t charged = mode_iq();
	CHECK(charged > 100, "T2: the filter really is charged before the demand is removed");

	/*
	 * Pressure to a clean zero, cranks still turning forward: permission stays, demand goes.
	 *
	 * THE PROPERTY, checked on every one of the 12000 ticks: the published request never
	 * exceeds what the CURRENT inputs justify. That is the exact form of "the filter may
	 * smooth a legal demand but may not be an independent source" - a lag on the way DOWN
	 * shows up here immediately and unambiguously, with no need to guess a settling time.
	 *
	 * This is the check that caught the original: with the power filter in the target path it
	 * asked for 313 W against a raw 113 W, and 56 W against a raw 10 W.
	 */
	s.torque_assist_filtered = 0U;
	int32_t worst_excess_w = 0;
	int32_t worst = 0;
	bool reached_zero = false;
	for (uint32_t i = 0; i < 12000U; i++) {
		do_tick(&s, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
		const assist_mode_output_t *o = assist_modes_get_last_output();
		int32_t excess = (int32_t)o->motor_power_w - (int32_t)o->raw_motor_power_w;
		if (excess > worst_excess_w) worst_excess_w = excess;
		if (reached_zero && mode_iq() > worst) worst = mode_iq();
		if (!reached_zero && mode_iq() == 0) reached_zero = true;
	}
	char label[200];
	snprintf(label, sizeof(label),
		"T2: the request never exceeds what the current inputs justify "
		"(worst excess %ld W over the raw request)", (long)worst_excess_w);
	CHECK(worst_excess_w == 0, label);
	CHECK(reached_zero, "T2: the demand does reach zero once the rider stops pressing");
	snprintf(label, sizeof(label),
		"T2: and never comes back on its own afterwards (worst %ld)", (long)worst);
	CHECK(worst == 0, label);
}

/* ------------------------------------------------------------------ T3 */

/*
 * ISSUE 2 REPRODUCTION. The original: after a lull the same push produced 46 Iq counts where a
 * clean start produced 211, because the power filter was ramping into the request from near
 * zero - a second soft-start on top of the Iq ramp that already owns that job.
 *
 * The comparison is CALCULATED TARGET (assist_modes' own output) against CALCULATED TARGET, at
 * the same inputs. No number is hard-coded: run A measures what a clean start produces with
 * this build's configuration, and run B has to match it.
 */
static void t3_re_entry(void)
{
	printf("T3 (ISSUE 2) re-entry uses the current inputs, not a filter's history\n");

	/* --- A: clean start, settle, read the target. --- */
	reset_all();
	step_t a = establish_assist();
	ride_forward(&a, 30U);
	int32_t clean_target = mode_iq();
	CHECK(clean_target > 0, "T3: clean-start target is positive");

	/* --- B: the same, then a lull long enough to empty everything, then the same push. --- */
	reset_all();
	step_t b = establish_assist();
	ride_forward(&b, 30U);
	CHECK(mode_iq() == clean_target, "T3: the two runs really are at the same operating point");

	b.torque_assist_filtered = 0U;
	for (uint32_t i = 0; i < 8000U; i++) {
		do_tick(&b, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
	}
	CHECK(mode_iq() == 0, "T3: the demand really did go away before the re-entry");

	/*
	 * Pressure returns. The RUN estimator has its own honest, documented rise (FW-112.4), so
	 * the target is NOT full on the first tick, and it should not be - that lag is owned,
	 * named and in the right domain.
	 *
	 * So the comparison is not against the settled value, which would just be measuring the
	 * RUN estimator. It is against a COLD CONTROLLER given the same push for the same 75 ms:
	 * if history adds nothing on re-entry, the two are identical. This is the card's own
	 * "mode target A == mode target B for identical current inputs", and it is exactly what
	 * the original defect failed - 43 counts on re-entry where a cold start gave 228.
	 */
	b.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
	for (uint32_t i = 0; i < 300U; i++) {
		do_tick(&b, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
	}
	int32_t re_entry_at_75ms = mode_iq();

	/* The same 75 ms of the same push, on a controller that has never assisted. */
	reset_all();
	step_t c = base_step();
	for (uint32_t i = 0; i < 300U; i++) {
		do_tick(&c, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
	}
	int32_t cold_at_75ms = mode_iq();

	/*
	 * The two are NOT expected to be identical, and the reason is worth stating so nobody
	 * "fixes" it here: a COLD ARM calls torque_input_seed_run(), which pre-loads the RUN
	 * estimator to the current fast signal so a launch is crisp. A mid-ride re-engagement on
	 * a session that never left ACTIVE gets no such seed, so the estimator rises on its own
	 * 120 ms constant. That difference belongs to FW-090/FW-091 (which left the sustained-rise
	 * re-seed switched OFF deliberately, because re-seeding on rises brought back per-leg
	 * pulsing), not to this card. Printed, not asserted.
	 */
	printf("      re-entry at 75 ms = %ld, cold start at 75 ms = %ld, settled = %ld"
		" (difference is the RUN estimator's cold-arm seed, FW-090/091)\n",
		(long)re_entry_at_75ms, (long)cold_at_75ms, (long)clean_target);

	/*
	 * What this card DOES assert, at the level it is about: the mode module adds no history
	 * of its own. Identical inputs, identical answer, whatever it was asked before - which is
	 * the property the power filter broke, and the reason it is no longer in the target path.
	 */
	char label[200];
	{
		assist_level_config_t cfg = *assist_modes_get_default_level(TEST_ASSIST_LEVEL);
		rider_input_t r;
		memset(&r, 0, sizeof(r));
		r.cadence_rpm = 60U;
		r.torque_run_filtered = STRONG_ASSIST_DELTA_NATIVE;
		r.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
		r.torque_load_centikg =
			torque_input_native_delta_to_centikg(STRONG_ASSIST_DELTA_NATIVE);
		r.motor_voltage_utilization = 1024U;
		r.torque_sensor_valid = true;
		r.pas_sensor_valid = true;

		assist_mode_output_t fresh;
		assist_modes_reset();
		memset(&fresh, 0, sizeof(fresh));
		assist_modes_calculate(&r, &cfg, TEST_BATTERY_MV, (int32_t)PH_CURRENT_MAX, &fresh);

		/* Hammer it with a completely different demand, then ask the same question again. */
		rider_input_t other = r;
		other.torque_run_filtered = 3000U;
		other.torque_assist_filtered = 3000U;
		other.torque_load_centikg = torque_input_native_delta_to_centikg(3000U);
		assist_mode_output_t scratch;
		for (uint32_t i = 0; i < 500U; i++) {
			memset(&scratch, 0, sizeof(scratch));
			assist_modes_calculate(&other, &cfg, TEST_BATTERY_MV,
				(int32_t)PH_CURRENT_MAX, &scratch);
		}
		rider_input_t nothing = r;
		nothing.torque_run_filtered = 0U;
		nothing.torque_assist_filtered = 0U;
		nothing.torque_load_centikg = 0U;
		for (uint32_t i = 0; i < 500U; i++) {
			memset(&scratch, 0, sizeof(scratch));
			assist_modes_calculate(&nothing, &cfg, TEST_BATTERY_MV,
				(int32_t)PH_CURRENT_MAX, &scratch);
		}

		assist_mode_output_t again;
		memset(&again, 0, sizeof(again));
		assist_modes_calculate(&r, &cfg, TEST_BATTERY_MV, (int32_t)PH_CURRENT_MAX, &again);
		snprintf(label, sizeof(label),
			"T3: the mode carries NO history - the same inputs give the same target after "
			"500 ticks of a different demand and 500 of none (%ld vs %ld)",
			(long)again.iq_request, (long)fresh.iq_request);
		CHECK(again.iq_request == fresh.iq_request, label);
		snprintf(label, sizeof(label),
			"T3: ...and the same requested power too (%u W vs %u W)",
			again.motor_power_w, fresh.motor_power_w);
		CHECK(again.motor_power_w == fresh.motor_power_w, label);
	}

	/* And once the RUN estimator has caught up, it settles on the clean-start target - no
	 * residual offset, no permanently different operating point. */
	reset_all();
	step_t d = establish_assist();
	ride_forward(&d, 30U);
	d.torque_assist_filtered = 0U;
	for (uint32_t i = 0; i < 8000U; i++) {
		do_tick(&d, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
	}
	d.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
	ride_forward(&d, 60U);
	snprintf(label, sizeof(label),
		"T3: settled re-entry target == clean-start target (%ld vs %ld)",
		(long)mode_iq(), (long)clean_target);
	CHECK(mode_iq() == clean_target, label);
}

/* ------------------------------------------------------------------ T4 */

/*
 * The other half of ISSUE 2, and the guard against overcorrecting it: the EXPLICIT downstream
 * shaping must still be there. A correct target is not the same thing as instant motor torque.
 */
static void t4_downstream_slew_intact(void)
{
	printf("T4 (ISSUE 2) the explicit Iq ramp is still in place downstream\n");
	reset_all();
	step_t s = base_step();

	/*
	 * Cold start, full pressure from the first tick. Sampled on the tick the demand first
	 * becomes positive: the mode target may arrive at once, the MOTOR setpoint may not - the
	 * per-level Iq ramp is the explicit owner of how fast current builds, and removing the
	 * power filter must not have removed it too.
	 */
	int32_t target_at_first_demand = 0;
	int32_t setpoint_at_first_demand = 0;
	for (uint32_t k = 0; k < 40U && target_at_first_demand == 0; k++) {
		for (uint32_t t = 0; t < STEP_INTERVAL_TICKS; t++) {
			do_tick(&s, (t == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
			if (live_target() > 0) {
				target_at_first_demand = live_target();
				setpoint_at_first_demand = MS.i_q_setpoint;
				break;
			}
		}
	}
	char label[160];
	CHECK(target_at_first_demand > 0, "T4: a target does appear");
	snprintf(label, sizeof(label),
		"T4: the motor setpoint is still ramping toward the target, not snapped to it "
		"(%ld of %ld on the first demanding tick)",
		(long)setpoint_at_first_demand, (long)target_at_first_demand);
	CHECK(setpoint_at_first_demand < target_at_first_demand, label);

	/* ...and it does get there, so the ramp is a delay and not a permanent ceiling. */
	ride_forward(&s, 120U);
	snprintf(label, sizeof(label),
		"T4: the ramp reaches the target given time (%ld vs %ld)",
		(long)MS.i_q_setpoint, (long)mode_iq());
	CHECK(MS.i_q_setpoint >= mode_iq() - 2, label);
}

/* ------------------------------------------------------------------ T5/T6 */

/*
 * ISSUE 3. T5 dirties every dynamic state there is by riding hard, then reinitialises and
 * checks the observable state is back to cold. T6 is the real proof: identical inputs must
 * produce identical outputs regardless of what happened before the init.
 */
static void t5_dirty_state_reinit(void)
{
	printf("T5 (ISSUE 3) ride_control_init() returns every runtime state to cold\n");

	/* What a cold controller looks like. */
	reset_all();
	uint8_t cold_flags = ride_control_get_debug_flags();
	uint8_t cold_session = ride_control_get_session_state();
	ride_arm_snapshot_t cold_arm;
	ride_control_get_arm_snapshot(&cold_arm);
	assist_mode_output_t cold_mode = *assist_modes_get_last_output();
	assist_smooth_start_output_t cold_smooth = *assist_start_get_last_smooth_output();
	assist_extended_boost_diag_t cold_boost;
	assist_extended_boost_get_diag(&cold_boost);

	/* Now make a mess of all of it: a hard ride, a reverse, a release. */
	step_t s = establish_assist();
	ride_forward(&s, 40U);
	rev1(&s);
	hold(&s, 50U);
	s.torque_assist_filtered = 0U;
	ride_forward(&s, 5U);
	CHECK(MS.i_q_setpoint != 0 || mode_iq() != 0 || ride_control_get_debug_flags() != cold_flags,
		"T5: the controller really is dirty before the reinit");

	/* The lifecycle reset the card is about. MS is the motor's own state, not ride_control's,
	 * so it is cleared here the way main.c's start-up path does. */
	ride_control_init();
	memset(&MS, 0, sizeof(MS));
	motor_core_init(&MS);

	CHECK(ride_control_get_debug_flags() == cold_flags,
		"T5: debug/limit flags are back to cold");
	CHECK(ride_control_get_session_state() == cold_session,
		"T5: the ride session is back to cold");
	/*
	 * Compared FIELD BY FIELD, not with memcmp: these are structs with padding, and padding
	 * bytes are indeterminate after a struct copy. A memcmp here reports differences that do
	 * not exist and hides the ones that do.
	 */
	ride_arm_snapshot_t after_arm;
	ride_control_get_arm_snapshot(&after_arm);
	CHECK(after_arm.load_centikg == cold_arm.load_centikg &&
		after_arm.fast_native == cold_arm.fast_native &&
		after_arm.run_seed_native == cold_arm.run_seed_native &&
		after_arm.iq_pre_ramp == cold_arm.iq_pre_ramp &&
		after_arm.iq_after_latch_floor == cold_arm.iq_after_latch_floor &&
		after_arm.fast_rearm == cold_arm.fast_rearm,
		"T5: the arm snapshot carries nothing from the previous ride");
	const assist_mode_output_t *after_mode = assist_modes_get_last_output();
	CHECK(after_mode->iq_request == cold_mode.iq_request &&
		after_mode->iq_before_pu == cold_mode.iq_before_pu &&
		after_mode->iq_launch_request == cold_mode.iq_launch_request &&
		after_mode->iq_normal_request == cold_mode.iq_normal_request &&
		after_mode->motor_power_w == cold_mode.motor_power_w &&
		after_mode->raw_motor_power_w == cold_mode.raw_motor_power_w &&
		after_mode->assist_load_centikg == cold_mode.assist_load_centikg &&
		after_mode->cadence_comp_permille == cold_mode.cadence_comp_permille,
		"T5: the published mode output is back to cold");
	const assist_smooth_start_output_t *after_smooth = assist_start_get_last_smooth_output();
	CHECK(after_smooth->iq_target == cold_smooth.iq_target &&
		after_smooth->envelope_permille == cold_smooth.envelope_permille &&
		after_smooth->active == cold_smooth.active,
		"T5: the smooth-start envelope is back to cold");
	assist_extended_boost_diag_t after_boost;
	assist_extended_boost_get_diag(&after_boost);
	CHECK(after_boost.state == cold_boost.state &&
		after_boost.peak_load_centikg == cold_boost.peak_load_centikg &&
		after_boost.boost_iq == cold_boost.boost_iq,
		"T5: Extended Boost is back to cold");
}

/*
 * T6: the behavioural proof. Whatever T5's field-by-field comparison can and cannot see, this
 * one cannot be fooled - if ANY state survives the init and matters, the two runs diverge.
 */
#define T6_SAMPLES 400U

static void t6_capture(int32_t *mode_out, int32_t *target_out, int32_t *setpoint_out)
{
	step_t s = base_step();
	for (uint32_t i = 0; i < T6_SAMPLES; i++) {
		do_tick(&s, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
		mode_out[i] = mode_iq();
		target_out[i] = live_target();
		setpoint_out[i] = MS.i_q_setpoint;
	}
}

static void t6_reinit_output_equivalence(void)
{
	printf("T6 (ISSUE 3) a reinitialised controller behaves exactly like a fresh one\n");
	static int32_t a_mode[T6_SAMPLES], a_target[T6_SAMPLES], a_setpoint[T6_SAMPLES];
	static int32_t b_mode[T6_SAMPLES], b_target[T6_SAMPLES], b_setpoint[T6_SAMPLES];

	/* Run A: fresh init, then the input sequence. */
	reset_all();
	t6_capture(a_mode, a_target, a_setpoint);

	/* Run B: an unrelated high-demand sequence FIRST, then the same init, then the same
	 * sequence. The sensor chain is reinitialised alongside ride_control, because on the bike
	 * these two come up together and torque_input owns the sensor's own lifecycle. */
	reset_all();
	{
		step_t junk = establish_assist();
		ride_forward(&junk, 60U);
		rev1(&junk);
		hold(&junk, 200U);
		junk.torque_assist_filtered = 0U;
		ride_forward(&junk, 20U);
	}
	torque_input_init();
	torque_input_set_run_window_deg(180U);
	ride_control_init();
	pas_direction_init();
	memset(&MS, 0, sizeof(MS));
	motor_core_init(&MS);
	g_tick = 0;
	t6_capture(b_mode, b_target, b_setpoint);

	uint32_t first_diff = T6_SAMPLES;
	for (uint32_t i = 0; i < T6_SAMPLES; i++) {
		if (a_mode[i] != b_mode[i] || a_target[i] != b_target[i] ||
			a_setpoint[i] != b_setpoint[i]) {
			first_diff = i;
			break;
		}
	}
	char label[200];
	if (first_diff < T6_SAMPLES) {
		snprintf(label, sizeof(label),
			"T6: fresh and reinitialised runs diverge at tick %u "
			"(mode %ld/%ld, target %ld/%ld, setpoint %ld/%ld)",
			first_diff, (long)a_mode[first_diff], (long)b_mode[first_diff],
			(long)a_target[first_diff], (long)b_target[first_diff],
			(long)a_setpoint[first_diff], (long)b_setpoint[first_diff]);
	} else {
		snprintf(label, sizeof(label), "T6: identical over all %u ticks", T6_SAMPLES);
	}
	CHECK(first_diff == T6_SAMPLES, label);
}

/* ------------------------------------------------------------------ T7 */

static void t7_mode_switch(void)
{
	printf("T7 one mode's filter state may not produce torque in another\n");
	const assist_mode_type_t modes[] = {
		ASSIST_MODE_POWER_LINEAR, ASSIST_MODE_POWER_PROGRESSIVE, ASSIST_MODE_POWER_CURVE,
		ASSIST_MODE_EMTB, ASSIST_MODE_TORQUE,
	};
	const char *names[] = { "Power Linear", "Power Progressive", "Power Curve", "eMTB", "Torque" };

	for (unsigned from = 0; from < 5U; from++) {
		for (unsigned to = 0; to < 5U; to++) {
			if (from == to) continue;
			char label[200];

			/* Reference: what `to` produces from a clean start at this operating point. */
			reset_all();
			assist_level_config_t cfg_to = *assist_modes_get_default_level(TEST_ASSIST_LEVEL);
			cfg_to.mode_type = modes[to];
			assist_level_config_t cfg_from = cfg_to;
			cfg_from.mode_type = modes[from];

			rider_input_t r;
			memset(&r, 0, sizeof(r));
			r.cadence_rpm = 60U;
			r.torque_run_filtered = STRONG_ASSIST_DELTA_NATIVE;
			r.torque_assist_filtered = STRONG_ASSIST_DELTA_NATIVE;
			r.torque_load_centikg =
				torque_input_native_delta_to_centikg(STRONG_ASSIST_DELTA_NATIVE);
			r.motor_voltage_utilization = 1024U;
			r.torque_sensor_valid = true;
			r.pas_sensor_valid = true;

			assist_mode_output_t out;
			assist_modes_reset();
			memset(&out, 0, sizeof(out));
			assist_modes_calculate(&r, &cfg_to, TEST_BATTERY_MV,
				(int32_t)PH_CURRENT_MAX, &out);
			int32_t clean = out.iq_request;

			/* Now: run `from` hard, drop the demand to nothing, then switch to `to` while
			 * the rider is STILL not pressing. Nothing may come out. */
			assist_modes_reset();
			for (uint32_t i = 0; i < 200U; i++) {
				memset(&out, 0, sizeof(out));
				assist_modes_calculate(&r, &cfg_from, TEST_BATTERY_MV,
					(int32_t)PH_CURRENT_MAX, &out);
			}
			CHECK(out.iq_request > 0, "T7: the source mode was really producing demand");

			rider_input_t empty = r;
			empty.torque_run_filtered = 0U;
			empty.torque_assist_filtered = 0U;
			empty.torque_load_centikg = 0U;
			memset(&out, 0, sizeof(out));
			assist_modes_calculate(&empty, &cfg_to, TEST_BATTERY_MV,
				(int32_t)PH_CURRENT_MAX, &out);
			snprintf(label, sizeof(label),
				"T7: %s -> %s with no rider demand produces nothing (%ld)",
				names[from], names[to], (long)out.iq_request);
			CHECK(out.iq_request == 0, label);

			/* And with the demand back, `to` produces its own answer, not the previous
			 * mode's - the filter must have re-seeded on the mode change. */
			memset(&out, 0, sizeof(out));
			assist_modes_calculate(&r, &cfg_to, TEST_BATTERY_MV,
				(int32_t)PH_CURRENT_MAX, &out);
			snprintf(label, sizeof(label),
				"T7: %s -> %s gives %s's own value (%ld vs clean %ld)",
				names[from], names[to], names[to], (long)out.iq_request, (long)clean);
			CHECK(out.iq_request == clean, label);
		}
	}
}

/* ------------------------------------------------------------------ T8 */

static void t8_reverse_direction(void)
{
	printf("T8 a backpedal never recovers the old positive state\n");
	reset_all();
	step_t s = establish_assist();
	ride_forward(&s, 20U);
	int32_t before = mode_iq();
	CHECK(before > 0, "T8: assisting before the reverse");

	/* Reverse WHILE still pressing hard: permission must go, and current with it. */
	rev1(&s);
	CHECK(!is_latched(), "T8: the session suspends on the backpedal");
	CHECK(live_target() == 0, "T8: and the target is cut the same tick");

	int32_t worst = 0;
	for (uint32_t i = 0; i < 200U; i++) {
		hold(&s, 1U);
		if (live_target() > worst) worst = live_target();
	}
	CHECK(worst == 0, "T8: nothing leaks through the whole reverse hold");

	/* The rider lets go DURING the hold, then pedals forward again. Permission returns as a
	 * direction fact; the demand must follow the (now absent) pressure, not the old state.
	 * The sensor chain is given the same flush window T1 uses - what is on trial here is
	 * whether anything is RECOVERED from before the reverse, not the RUN estimator's own
	 * documented fall. */
	s.torque_assist_filtered = GHOST_RESIDUAL_NATIVE;
	hold(&s, 200U);
	for (uint32_t i = 0; i < 2U; i++) fwd1(&s);
	CHECK(is_latched(), "T8: permission restored on the forward confirm");
	for (uint32_t i = 0; i < 6000U; i++) {
		do_tick(&s, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
	}
	int32_t after = 0;
	for (uint32_t i = 0; i < 2000U; i++) {
		do_tick(&s, (i % STEP_INTERVAL_TICKS == STEP_INTERVAL_TICKS - 1U) ? EV_FORWARD : EV_NONE);
		if (live_target() > after) after = live_target();
	}
	char label[160];
	snprintf(label, sizeof(label),
		"T8: no demand is recovered from before the reverse (worst %ld, was %ld)",
		(long)after, (long)before);
	CHECK(after == 0, label);
}

/* ------------------------------------------------------------------ T9 */

static void t9_calibration_untouched(void)
{
	printf("T9 the sensor characteristic is exactly where FW-129 left it\n");
	reset_all();
	/* The measured factory points, and the interpolated 60 kg span. If any of these move, the
	 * kilogram scale moved, and every mode moved with it. */
	CHECK(TORQUE_ZERO_TARGET_NATIVE == 740U, "T9: zero point unchanged");
	CHECK(TORQUE_DEFAULT_LOW_NATIVE == 146U && TORQUE_DEFAULT_LOW_CENTIKG == 600U,
		"T9: lower measured point unchanged");
	CHECK(TORQUE_DEFAULT_HIGH_NATIVE == 1580U && TORQUE_DEFAULT_HIGH_CENTIKG == 8400U,
		"T9: upper measured point unchanged");
	CHECK(TORQUE_DEFAULT_SPAN_NATIVE == 1139U, "T9: the 60 kg span is unchanged");
	CHECK(torque_input_native_delta_to_centikg(146U) == 600U,
		"T9: 146 native still reads 6.00 kg");
	CHECK(torque_input_native_delta_to_centikg(1139U) >= 5990U &&
		torque_input_native_delta_to_centikg(1139U) <= 6010U,
		"T9: the span still reads 60.00 kg");
	CHECK(torque_input_calibration_source() == TORQUE_CAL_SOURCE_DEFAULT,
		"T9: nothing in this card switched the controller to a user calibration");
}

/* ------------------------------------------------------------------ T10 */

static void t10_fw129_golden(void)
{
	printf("T10 the FW-129 Power correction is not walked back\n");
	reset_all();

	/*
	 * The pre-FW-129 anchor, one line, so the guard is against the ACTUAL old behaviour:
	 *   Iq = iq_limit * (load_ckg * ratio / 3000) / 1000     ("60 kg at 500 % = full Iq")
	 * FW-129 measured ~2.17x more than this at every Power operating point. This must not
	 * come back down - and it is a FLOOR, not an equality, so a future deliberate change
	 * upward is not blocked by it.
	 */
	const uint16_t loads[] = { 1000U, 2000U, 4000U };
	const uint8_t cadences[] = { 40U, 60U, 80U };
	assist_level_config_t cfg = *assist_modes_get_default_level(TEST_ASSIST_LEVEL);
	cfg.mode_type = ASSIST_MODE_POWER_LINEAR;
	cfg.startup_boost.enabled = false;

	for (unsigned l = 0; l < 3U; l++) {
		for (unsigned c = 0; c < 3U; c++) {
			uint16_t native = torque_input_centikg_to_native_delta(loads[l]);
			uint16_t u_abs = (uint16_t)(1024U * cadences[c] / 60U);
			rider_input_t r;
			memset(&r, 0, sizeof(r));
			r.cadence_rpm = cadences[c];
			r.torque_run_filtered = native;
			r.torque_assist_filtered = native;
			r.torque_load_centikg = loads[l];
			r.motor_voltage_utilization = u_abs;
			r.torque_sensor_valid = true;
			r.pas_sensor_valid = true;

			assist_mode_output_t out;
			memset(&out, 0, sizeof(out));
			assist_modes_reset();
			assist_modes_calculate(&r, &cfg, TEST_BATTERY_MV,
				(int32_t)PH_CURRENT_MAX, &out);

			uint32_t old_permille =
				((uint32_t)loads[l] * cfg.support_ratio_pct + 1500U) / 3000U;
			if (old_permille > 1000U) old_permille = 1000U;
			int32_t old_iq = (int32_t)(((uint32_t)PH_CURRENT_MAX * old_permille + 999U) / 1000U);

			char label[200];
			snprintf(label, sizeof(label),
				"T10: Power Linear at %u.%02u kg / %u rpm is still ~2x the old anchor "
				"(%ld vs old %ld)", loads[l] / 100U, loads[l] % 100U, cadences[c],
				(long)out.iq_request, (long)old_iq);
			CHECK(out.iq_request >= old_iq * 2, label);
		}
	}
}

/* ------------------------------------------------------------------ T12 */

/*
 * The general property: wherever the ride path says the rider has no grant, no positive
 * motoring current may be requested. Swept over the state combinations a rider can actually
 * produce, rather than argued from the code.
 */
static void t12_no_output_without_grant(void)
{
	printf("T12 property: no grant, no positive motoring demand\n");
	int32_t violations = 0;
	int32_t checked = 0;

	for (unsigned level = 0; level < 2U; level++) {
		for (unsigned cut = 0; cut < 2U; cut++) {
			for (unsigned stop = 0; stop < 2U; stop++) {
				for (unsigned reverse = 0; reverse < 2U; reverse++) {
					reset_all();
					step_t s = establish_assist();
					ride_forward(&s, 20U);

					/* Apply the combination while the filter is fully charged. */
					s.assist_level_index = level ? 0U : TEST_ASSIST_LEVEL;
					s.non_direction_safety_cut = (cut != 0U);
					s.real_stop = (stop != 0U);
					if (reverse) rev1(&s);

					bool no_grant = (level != 0U) || (cut != 0U) || (stop != 0U) ||
						(reverse != 0U);
					if (!no_grant) continue;

					/*
					 * The card's invariant is about Iq_REQUESTED, and that is what is
					 * checked on every tick: the moment the grant is gone, the request is
					 * zero.
					 *
					 * The motor SETPOINT is deliberately NOT held to the same tick: it is
					 * the explicit downstream reference, and stepping it from full current
					 * to zero is an audible clunk through the gearbox - which is the whole
					 * reason the hard-cut ramp exists (see the HARD CUT RAMP comment in
					 * ride_control.c). What it IS held to is arriving: it must reach zero
					 * and stay there, checked separately below.
					 */
					for (uint32_t i = 0; i < 500U; i++) {
						hold(&s, 1U);
						checked++;
						if (live_target() > 0) violations++;
					}
					/* The fade must actually complete, and then stay completed. */
					for (uint32_t i = 0; i < 4000U; i++) hold(&s, 1U);
					if (MS.i_q_setpoint > 0) {
						violations++;
						printf("  T12: setpoint never faded to zero "
							"(level0=%u cut=%u stop=%u reverse=%u, setpoint %ld)\n",
							level, cut, stop, reverse, (long)MS.i_q_setpoint);
					}
				}
			}
		}
	}
	char label[160];
	snprintf(label, sizeof(label),
		"T12: %ld positive-demand violations out of %ld no-grant ticks",
		(long)violations, (long)checked);
	CHECK(violations == 0, label);
	CHECK(checked > 0, "T12: the sweep actually exercised no-grant states");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
	printf("FW-129B state hygiene: real ride_control.c + assist_modes.c + assist_dynamics.c "
		"+ assist_start.c chain\n");

	t1_ghost_assist();
	t2_zero_demand_decay();
	t3_re_entry();
	t4_downstream_slew_intact();
	t5_dirty_state_reinit();
	t6_reinit_output_equivalence();
	t7_mode_switch();
	t8_reverse_direction();
	t9_calibration_untouched();
	t10_fw129_golden();
	t12_no_output_without_grant();

	if (host_test_failures == 0) {
		printf("FW-129B state hygiene: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-129B state hygiene: %d check(s) FAILED\n", host_test_failures);
	return 1;
}
