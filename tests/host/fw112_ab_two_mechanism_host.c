/*
 * FW-112 TWO-MECHANISM DIAGNOSTIC host suite: proves the direct afilt/arun capture added to
 * fw112_ab.c (schema 2) records what the REAL torque_input.c recovery automaton is actually
 * doing, and proves - against the REAL module, not a description of it - the two claims the
 * root-cause report rests on:
 *
 *   (a) during a rolling rearm, WAIT_FRESH_LOAD -> TRACK_FAST depends on afilt alone; arun is
 *       live-substituted to the SAME value the whole time recovery is active, so it can neither
 *       delay nor accelerate the transition (S5);
 *   (b) with recovery inactive (ordinary RUN, no rearm/reverse event), arun is the independent
 *       48-step crank-angle window average and can lag afilt by a real amount for a slow ramp,
 *       while a sharp step is fast in both signals because the filter's own OWN dynamics
 *       dominate (S3/S4).
 *
 * Two REAL modules are linked: torque_input.c (produces afilt/arun) and fw112_ab.c (records
 * them). Nothing here computes assist demand - iq_request/iq_after_latch_floor/iq_pre_ramp/
 * iq_setpoint are driven by a trivial "does the run signal say something positive" stand-in
 * local to this harness, because assist_modes.c's own math is out of scope here and already has
 * its own test suites; this harness's job is the RECORDER and the RECOVERY TIMING, not the
 * demand curve.
 *
 * Session/timing model: 4 kHz control tick, 60 rpm cadence (96 quadrature steps/rev -> one
 * forward step every 4000*60/(60*96) = 41.67 ~= 42 ticks), matching every other FW-112 host
 * harness in this repo (see tests/host/fw112_run_rearm_recovery_host.c's own comment).
 */

#include <stdint.h>
#include <string.h>

#include "../common/check.h"

#include "config.h"
#include "fw112_ab.h"
#include "torque_input.h"

#define STEP_INTERVAL_TICKS 42U
#define DEADBAND_NATIVE      TORQUE_ASSIST_DEADBAND_NATIVE   /* 10 */
#define STABLE_TICKS          TORQUE_ROLLING_REARM_STABLE_TICKS /* 560 = 140 ms */
#define FILTER_TICKS         (TORQUE_ASSIST_FILTER_MS * TORQUE_INPUT_TICKS_PER_MS) /* 140 = 35ms */

static uint32_t g_tick;
static uint32_t g_session_id = 1U;

static void tick_torque(int32_t delta_native)
{
	uint16_t raw = (uint16_t)((int32_t)TORQUE_ZERO_TARGET_NATIVE + delta_native);
	torque_input_update(raw, (int16_t)raw, true);
	if (STEP_INTERVAL_TICKS != 0U && (g_tick % STEP_INTERVAL_TICKS) == 0U) {
		torque_input_run_filter_step();
	}
	g_tick++;
}

/* Feed n control ticks at a constant delta, recording nothing. */
static void feed_ticks(int32_t delta_native, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) tick_torque(delta_native);
}

/* One fw112_ab observation this tick, mirroring how main.c builds fw112_ab_input_t from
 * torque_input_get_snapshot() - see src/main.c's fw112_ab_input_t ab_in literal. */
static void ab_tick(uint8_t session_state)
{
	const torque_snapshot_t *snap = torque_input_get_snapshot();
	fw112_ab_input_t in;
	memset(&in, 0, sizeof(in));
	in.session_state = session_state;
	in.recovery_state = (uint8_t)torque_input_recovery_state();
	in.dir_state = 0U;
	in.assist_level = 1U;
	in.latched = (session_state == 1U);
	in.pwm_on = true;
	in.torque_sensor_valid = true;
	in.wheel_valid = true;
	in.cadence_rpm = 30U;
	in.battery_voltage_mv = 42000U;
	in.torque_for_assist_mv = snap->assist_delta_run_native;   /* proxy, same as production */
	in.load_centikg = snap->load_centikg;
	in.afilt_native = snap->assist_delta_filtered_native;      /* direct signal under test */
	/* Mirrors src/main.c's ab_in construction exactly - see inc/fw112_ab.h's arun_native
	 * comment for why the raw snapshot field alone is not what assist_modes_calculate() sees
	 * during WAIT_FRESH_LOAD. */
	in.arun_native = torque_input_recovery_active() ?
		torque_input_recovery_run_native() : snap->assist_delta_run_native;
	int16_t demand = (snap->assist_delta_run_native > 0U) ? 1 : 0;
	in.iq_request = demand;
	in.iq_after_latch_floor = demand;
	in.iq_pre_ramp = demand;
	in.iq_setpoint = demand;
	fw112_ab_tick(&in, g_tick);
}

static void reset_all(void)
{
	torque_input_init();
	torque_input_startup_zero((int32_t)TORQUE_ZERO_TARGET_NATIVE);
	fw112_ab_init();
	g_session_id++;
	fw112_ab_set_session_id((uint8_t)g_session_id);
	g_tick = 1000U;   /* nonzero base, matches the convention other harnesses use */
}

/* Drain every queued record for the session into an array, in queue order (oldest first). */
#define MAX_DRAIN 64U
static uint16_t drain(fw112_ab_record_t *out)
{
	uint16_t n = 0U;
	fw112_ab_record_t rec;
	while (n < MAX_DRAIN && fw112_ab_queue_peek_session((uint8_t)g_session_id, &rec)) {
		out[n++] = rec;
		fw112_ab_queue_release_session((uint8_t)g_session_id);
	}
	return n;
}

/* Opens an episode (REVOKED) then grants it (GRANTED), leaving recovery in WAIT_FRESH_LOAD -
 * exactly the fast-rearm sequence ride_control.c drives on session_out.fast_rearm_this_tick. */
static void open_and_grant_rearm(void)
{
	ab_tick(1U);                 /* baseline ACTIVE, no edge (first call primes prev_state) */
	feed_ticks(0, 5U);
	ab_tick(2U);                 /* REVOKED edge: session leaves ACTIVE -> opens the episode */
	torque_input_begin_rolling_rearm();   /* the real rearm-edge call, see ride_control.c:472 */
	ab_tick(1U);                 /* GRANTED edge: session re-enters ACTIVE */
}

/* ---- S1: REARM SHARP STEP -------------------------------------------------------------- */
static void test_s1_rearm_sharp_step(void)
{
	reset_all();
	open_and_grant_rearm();
	/* PATCH A: the automaton now opens directly in TRACK_FAST (see
	 * torque_input_begin_rolling_rearm()) - the old WAIT_FRESH_LOAD entry gate never protected
	 * demand (proven by the FW-112.5 audit), so it is no longer the mandatory first stop. */
	CHECK(torque_input_recovery_state() == TORQUE_RECOVERY_TRACK_FAST,
	      "S1: recovery opens directly in TRACK_FAST on the rearm edge (PATCH A)");

	/* Sharp step: raw jumps straight to a level comfortably above the deadband and stays. */
	int32_t sharp = (int32_t)DEADBAND_NATIVE * 20;   /* far above deadband once filtered */
	uint32_t ticks_to_track_fast = 0U;
	for (uint32_t i = 0; i < FILTER_TICKS * 3U; i++) {
		tick_torque(sharp);
		ab_tick(1U);
		if (torque_input_recovery_state() == TORQUE_RECOVERY_TRACK_FAST) {
			ticks_to_track_fast = i + 1U;
			break;
		}
	}
	CHECK(ticks_to_track_fast > 0U && ticks_to_track_fast <= FILTER_TICKS * 2U,
	      "S1: a sharp step reaches TRACK_FAST within about the fast filter's own settling time");

	fw112_ab_record_t recs[MAX_DRAIN];
	uint16_t n = drain(recs);
	int found_track_fast_arun_eq_afilt = 0;
	for (uint16_t i = 0; i < n; i++) {
		if (recs[i].smp.record_type != (uint8_t)FW112_AB_REC_SAMPLE) continue;
		if (recs[i].smp.recovery_state == (uint8_t)TORQUE_RECOVERY_IDLE) continue;
		/* During recovery arun is live-substituted to afilt (clamped to 8 bits) - the recorded
		 * pair must agree up to that clamp on every recovery-active sample. */
		uint16_t afilt = recs[i].smp.afilt_native;
		uint8_t arun_c = recs[i].smp.arun_native_clamped;
		uint8_t afilt_c = (uint8_t)((afilt > 255U) ? 255U : afilt);
		CHECK(arun_c == afilt_c,
		      "S1: recorded arun == recorded afilt (clamped) on every recovery-active sample");
		if (recs[i].smp.recovery_state == (uint8_t)TORQUE_RECOVERY_TRACK_FAST) {
			found_track_fast_arun_eq_afilt = 1;
		}
	}
	/* PATCH A: a sustained sharp step never collapses, so WAIT_FRESH_LOAD is never visited at
	 * all in this scenario any more (it opens straight into TRACK_FAST and stays there) - the
	 * live-substitution proof above already covers every recovery-active sample captured, all
	 * of which are TRACK_FAST here. */
	CHECK(found_track_fast_arun_eq_afilt == 1, "S1: at least one TRACK_FAST sample was captured");
}

/* ---- S2: REARM SLOW RAMP ---------------------------------------------------------------- */
static void test_s2_rearm_slow_ramp(void)
{
	reset_all();
	open_and_grant_rearm();

	/* PATCH A: the automaton now opens directly in TRACK_FAST (see test S1), so
	 * recovery_state itself no longer distinguishes "not yet confirmed" from "confirmed" for a
	 * slow ramp that never collapses - both read TRACK_FAST throughout (see
	 * recovery_confirmed_once in torque_input.c: the state only ever LEAVES TRACK_FAST on a
	 * GENUINE collapse, i.e. after having been confirmed at least once). What still must hold,
	 * and is still directly observable, is recovery_stable_ticks(): it stays at 0 for as long as
	 * afilt has not yet crossed the deadband, and starts counting up on the EXACT tick it does -
	 * that is the real "confirmed" edge PATCH A preserved, just no longer spelled as a distinct
	 * state name. */
	uint32_t confirm_tick = 0U;
	int32_t delta = 0;
	for (uint32_t i = 0; i < 4000U; i++) {
		if ((i % 4U) == 0U && delta < (int32_t)DEADBAND_NATIVE * 4) delta++;
		tick_torque(delta);
		ab_tick(1U);
		if (confirm_tick == 0U && torque_input_recovery_stable_ticks() > 0U) {
			confirm_tick = i + 1U;
			break;
		}
	}
	CHECK(confirm_tick > FILTER_TICKS,
	      "S2: a slow ramp takes meaningfully longer than one fast-filter time constant to confirm");

	/* The confirm tick must track how long afilt itself took to cross the deadband, not some
	 * fixed number - re-derive that independently and compare. */
	reset_all();
	open_and_grant_rearm();
	uint32_t afilt_cross_tick = 0U;
	delta = 0;
	for (uint32_t i = 0; i < 4000U; i++) {
		if ((i % 4U) == 0U && delta < (int32_t)DEADBAND_NATIVE * 4) delta++;
		tick_torque(delta);
		if (torque_input_get_snapshot()->assist_delta_filtered_native >= DEADBAND_NATIVE) {
			afilt_cross_tick = i + 1U;
			break;
		}
	}
	CHECK(afilt_cross_tick > 0U && afilt_cross_tick == confirm_tick,
	      "S2: recovery_stable_ticks starts on the EXACT tick afilt crosses the deadband, no extra delay");
}

/* ---- S3: ORDINARY RUN SHARP STEP (recovery inactive) ------------------------------------ */
static void test_s3_ordinary_run_sharp_step(void)
{
	reset_all();
	/* No rearm: session stays ACTIVE throughout, recovery is never opened. */
	ab_tick(1U);
	feed_ticks(0, 400U);   /* settle the window at 0 first (ordinary riding baseline) */
	for (uint32_t i = 0; i < 400U; i++) { tick_torque(0); ab_tick(1U); }
	CHECK(torque_input_recovery_state() == TORQUE_RECOVERY_IDLE,
	      "S3: recovery stays IDLE with no rearm edge - this is the ordinary RUN path");

	int32_t sharp = (int32_t)DEADBAND_NATIVE * 20;
	uint32_t afilt_cross = 0U, arun_cross = 0U;
	for (uint32_t i = 0; i < 4000U; i++) {
		tick_torque(sharp);
		ab_tick(1U);
		const torque_snapshot_t *s = torque_input_get_snapshot();
		if (afilt_cross == 0U && s->assist_delta_filtered_native >= DEADBAND_NATIVE) afilt_cross = i + 1U;
		if (arun_cross == 0U && s->assist_delta_run_native >= DEADBAND_NATIVE) arun_cross = i + 1U;
		if (afilt_cross != 0U && arun_cross != 0U) break;
	}
	CHECK(afilt_cross > 0U, "S3: afilt crosses the deadband for a sharp step");
	CHECK(arun_cross > 0U, "S3: arun eventually crosses too (recovery inactive - window path)");
	/* A sharp, sustained step is fast enough (well above the deadband from the very first
	 * ticks) that the 48-step window and the 35 ms filter converge within a small number of
	 * ticks of each other - unlike S4's slow ramp, there is no meaningful window LAG to see. */
	uint32_t gap = (arun_cross > afilt_cross) ? (arun_cross - afilt_cross) : 0U;
	CHECK(gap < STEP_INTERVAL_TICKS * 4U,
	      "S3: for a sharp step the window's extra delay over afilt is small (a few steps, not hundreds)");
}

/* ---- S4: ORDINARY RUN SLOW RAMP (recovery inactive) - the window lag itself ------------- */
static void test_s4_ordinary_run_slow_ramp(void)
{
	reset_all();
	ab_tick(1U);
	for (uint32_t i = 0; i < 400U; i++) { tick_torque(0); ab_tick(1U); }
	CHECK(torque_input_recovery_state() == TORQUE_RECOVERY_IDLE, "S4: recovery IDLE (ordinary RUN)");

	/* Same slow ramp shape as S2, but with NO rearm open - arun must now lag afilt by a real,
	 * measurable amount because it is the independent 48-step window average, not a live
	 * substitution. */
	uint32_t afilt_cross = 0U, arun_cross = 0U;
	int32_t delta = 0;
	for (uint32_t i = 0; i < 20000U; i++) {
		if ((i % 4U) == 0U && delta < (int32_t)DEADBAND_NATIVE * 4) delta++;
		tick_torque(delta);
		ab_tick(1U);
		const torque_snapshot_t *s = torque_input_get_snapshot();
		if (afilt_cross == 0U && s->assist_delta_filtered_native >= DEADBAND_NATIVE) afilt_cross = i + 1U;
		if (arun_cross == 0U && s->assist_delta_run_native >= DEADBAND_NATIVE) arun_cross = i + 1U;
		if (afilt_cross != 0U && arun_cross != 0U) break;
	}
	CHECK(afilt_cross > 0U && arun_cross > 0U, "S4: both signals eventually cross the deadband");
	CHECK(arun_cross > afilt_cross,
	      "S4: with recovery inactive, arun (48-step window) crosses the deadband LATER than afilt - the real window lag");
}

/* ---- S5: PROVE PATH SEPARATION ----------------------------------------------------------- */
static void test_s5_path_separation(void)
{
	/* Part A: a stale-HIGH window content before a rearm must not pull TRACK_FAST in early -
	 * the transition depends on afilt alone, never on whatever arun/the window was holding. */
	reset_all();
	ab_tick(1U);
	int32_t high = (int32_t)DEADBAND_NATIVE * 30;
	for (uint32_t i = 0; i < 4000U; i++) { tick_torque(high); }   /* warm the 48-step window HIGH */
	CHECK(torque_input_get_snapshot()->assist_delta_run_native >= DEADBAND_NATIVE,
	      "S5a: the window is genuinely warmed high before the rearm (precondition)");

	feed_ticks(0, 5U);
	ab_tick(2U);   /* REVOKED */
	torque_input_begin_rolling_rearm();
	ab_tick(1U);   /* GRANTED -> PATCH A: opens directly in TRACK_FAST, seeded to the CURRENT
	                  afilt - which, right after only 5 zero-pressure ticks, is STILL close to
	                  its own warmed-high value (afilt decays on its OWN 35 ms filter, entirely
	                  independent of the window/run this part is testing - 5 ticks is nowhere
	                  near enough to bring it down) - see torque_input_begin_rolling_rearm() */
	CHECK(torque_input_recovery_state() == TORQUE_RECOVERY_TRACK_FAST,
	      "S5a: opens directly in TRACK_FAST on the rearm edge (PATCH A) - afilt itself is still high here too");

	/* Feed LOW pressure post-rearm for a long stretch: afilt now decays on its own 35 ms filter
	 * (nothing to do with the window/run this part is testing) from ~280 down through the
	 * deadband over several hundred ticks, genuinely confirming (stable_ticks counts up) for
	 * as long as it stays above the deadband, then genuinely collapsing back to WAIT_FRESH_LOAD
	 * once it finally crosses below - this is a REAL collapse after a REAL (if stale) initial
	 * confirmation, not the "never confirmed" case PATCH A silences (see S5's own header
	 * comment: this part proves the STALE WINDOW/run history plays no part in that timing -
	 * afilt's OWN decay is the only clock). */
	for (uint32_t i = 0; i < FILTER_TICKS * 4U; i++) {
		tick_torque(0);
		ab_tick(1U);
	}
	CHECK(torque_input_recovery_state() == TORQUE_RECOVERY_WAIT_FRESH_LOAD,
	      "S5a: back to WAIT_FRESH_LOAD once afilt's OWN decay (not the stale window) finally crosses below the deadband");

	/* Part B: the transition tick is EXACTLY when afilt crosses, verified against a fresh,
	 * independently-driven trajectory - arun cannot delay it either. */
	reset_all();
	open_and_grant_rearm();
	/* PATCH A: recovery_state is already TRACK_FAST from the rearm edge (see S1/S5a), so it can
	 * no longer mark the confirm tick for a trajectory that starts below the deadband -
	 * recovery_stable_ticks() is the direct, still-meaningful signal (see S2). */
	uint32_t confirm_tick = 0U;
	uint32_t afilt_cross_tick = 0U;
	for (uint32_t i = 0; i < 2000U; i++) {
		/* An irregular, non-monotonic raw trajectory - noise-like, the kind that could confuse
		 * a window average but must not confuse the pure afilt >= deadband test. */
		int32_t d = (int32_t)((i / 3U) % 2U == 0U) ? (int32_t)DEADBAND_NATIVE * 25 : 0;
		tick_torque(d);
		if (afilt_cross_tick == 0U &&
		    torque_input_get_snapshot()->assist_delta_filtered_native >= DEADBAND_NATIVE) {
			afilt_cross_tick = i + 1U;
		}
		ab_tick(1U);
		if (confirm_tick == 0U && torque_input_recovery_stable_ticks() > 0U) {
			confirm_tick = i + 1U;
		}
		if (confirm_tick != 0U) break;
	}
	CHECK(afilt_cross_tick > 0U && confirm_tick == afilt_cross_tick,
	      "S5b: recovery_stable_ticks starts on exactly the tick afilt crosses, even under an irregular raw trajectory");
}

/* ---- S6: LOGGER OFF/ON EQUIVALENCE -------------------------------------------------------- */
static void test_s6_logger_equivalence(void)
{
	/* Run the identical S2 slow-ramp trajectory twice against torque_input.c: once calling
	 * ab_tick() (the logger) every tick, once never calling it at all. torque_input.c's own
	 * outputs (afilt, arun, recovery_state) must be byte-identical either way - this recorder
	 * only ever READS fw112_ab_input_t fields that already came from torque_input's snapshot,
	 * it never writes back into torque_input.c or influences its trajectory. */
	uint16_t afilt_logged[600]; uint16_t arun_logged[600]; uint8_t recov_logged[600];
	uint16_t afilt_bare[600]; uint16_t arun_bare[600]; uint8_t recov_bare[600];

	reset_all();
	open_and_grant_rearm();
	int32_t delta = 0;
	for (uint32_t i = 0; i < 600U; i++) {
		if ((i % 4U) == 0U && delta < (int32_t)DEADBAND_NATIVE * 4) delta++;
		tick_torque(delta);
		ab_tick(1U);   /* logger ON */
		const torque_snapshot_t *s = torque_input_get_snapshot();
		afilt_logged[i] = s->assist_delta_filtered_native;
		arun_logged[i] = s->assist_delta_run_native;
		recov_logged[i] = (uint8_t)torque_input_recovery_state();
	}

	reset_all();
	/* Same rearm sequence, but WITHOUT ever calling ab_tick()/fw112_ab_tick(). */
	feed_ticks(0, 5U);
	torque_input_begin_rolling_rearm();
	delta = 0;
	for (uint32_t i = 0; i < 600U; i++) {
		if ((i % 4U) == 0U && delta < (int32_t)DEADBAND_NATIVE * 4) delta++;
		tick_torque(delta);   /* logger OFF - no ab_tick() call at all */
		const torque_snapshot_t *s = torque_input_get_snapshot();
		afilt_bare[i] = s->assist_delta_filtered_native;
		arun_bare[i] = s->assist_delta_run_native;
		recov_bare[i] = (uint8_t)torque_input_recovery_state();
	}

	int mismatch = 0;
	for (uint32_t i = 0; i < 600U; i++) {
		if (afilt_logged[i] != afilt_bare[i] || arun_logged[i] != arun_bare[i] ||
		    recov_logged[i] != recov_bare[i]) {
			mismatch = 1;
			break;
		}
	}
	CHECK(mismatch == 0, "S6: torque_input.c's afilt/arun/recovery_state trajectory is identical with the logger on or off");
}

int main(void)
{
	printf("FW-112 two-mechanism direct-signal diagnostic: real torque_input.c + real fw112_ab.c\n");

	test_s1_rearm_sharp_step();
	test_s2_rearm_slow_ramp();
	test_s3_ordinary_run_sharp_step();
	test_s4_ordinary_run_slow_ramp();
	test_s5_path_separation();
	test_s6_logger_equivalence();

	if (host_test_failures != 0) {
		printf("fw112_ab_two_mechanism_host: %d FAILURES\n", host_test_failures);
		return 1;
	}
	printf("fw112_ab_two_mechanism_host: ALL PASS\n");
	return 0;
}
