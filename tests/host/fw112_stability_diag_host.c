/*
 * FW-112-STABILITY host suite: prove the new diagnostic evidence is captured
 * correctly by the REAL torque_input recovery automaton + REAL fw112_diag
 * recorder, and that the recorder is measurement-only (never perturbs the
 * production path).
 *
 * Scope (the FW-112 card's hypothesis - slow / pulsatile pedal load never lets
 * TRACK_FAST hold for the 560-tick dwell, so recovery collapses and command
 * current stays 0):
 *
 *   S1 SHARP STEP PASS      a load far above the deadband completes recovery;
 *                           the EXIT record must carry edge_stable_ticks == 560
 *                           and zero collapses.
 *   S2 SLOW RAMP            a monotonic ~1 s ramp completes; the EXIT evidence is
 *                           reported (this is OBSERVE, not forced to fail).
 *   S3 PULSATING INPUT      the streak resets per the production rule and each
 *                           TRACK_FAST->WAIT_FRESH_LOAD edge records a COLLAPSE
 *                           with 0 < pre-collapse streak < 560 and the filtered
 *                           assist (< deadband) that drove it; the episode
 *                           getters cross-check the wire, and a cancel closes the
 *                           episode with the collapse count on the EXIT record.
 *   S4 NEAR-THRESHOLD NOISE the automaton flaps around the deadband: at least one
 *                           honest collapse, never a completion.
 *   S5 + S6 EQUIVALENCE     each scenario runs TWICE - recorder fed vs never
 *                           called - and the full torque_input output trajectory
 *                           must be byte-identical (diagnostic-only proof).
 *
 * The recovery automaton lives in torque_input.c and the recorder in
 * fw112_diag.c; both are linked for real, driven exactly as main.c wires them:
 * torque_input_update() first, then begin/cancel, then fw112_diag_tick().
 * startup_zero(740) puts the harness zero at TORQUE_ZERO_TARGET_NATIVE, so raw
 * 740 == zero and a raw delta in native units maps 1:1 to delta_native.
 */

#include "check.h"

#include "torque_input.h"
#include "fw112_diag.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N_MAX 8192U

typedef struct {
	uint16_t filtered;
	uint16_t run;
	uint8_t  recovery_state;
	uint16_t load;
} out_t;

static out_t g_traj_a[N_MAX];
static out_t g_traj_b[N_MAX];

/* One fw112_diag observation for the current torque_input state - the same shape
 * main.c's fd_in block produces (see src/main.c diag block). The session is held
 * ACTIVE and latched with a non-zero setpoint for the whole run, so the ONLY
 * events the recorder can emit are the RECOVERY_* family - a clean read of the
 * stability evidence. */
static void diag_tick(uint32_t now)
{
	const torque_snapshot_t *snap = torque_input_get_snapshot();
	fw112_diag_input_t in;
	memset(&in, 0, sizeof(in));
	in.session_state = 1U;
	in.recovery_state = (uint8_t)torque_input_recovery_state();
	in.dir_state = 1U;
	in.fwd_run = (uint8_t)((now / 7U) & 0xFFU);
	in.cadence_rpm = 80U;
	in.latched = true;
	in.pwm_on = true;
	in.torque_sensor_valid = true;
	in.wheel_valid = true;
	in.load_centikg = snap->load_centikg;
	in.load_threshold_centikg = 0U;
	in.recovery_stable_ticks = torque_input_recovery_stable_ticks();
	in.recovery_stable_ticks_at_edge = torque_input_recovery_stable_ticks_at_edge();
	in.assist_delta_filtered_native = snap->assist_delta_filtered_native;
	in.iq_request = 400;
	in.iq_pre_ramp = 400;
	in.iq_setpoint = 400;
	in.iq_actual = 400;
	in.reason_bits = 0U;
	fw112_diag_tick(&in, now);
}

/* Profile functions: raw_native per 4 kHz control tick. startup_zero(740) makes
 * raw 740 == zero, so 740 + n is a delta of n native units. */
static uint16_t profile_sharp(uint32_t t) { return t < 10U ? 740U : 790U; }
static uint16_t profile_ramp(uint32_t t)
{
	uint16_t d = (uint16_t)(t / 67U); /* ~1 native per 16.75 ms: full assist in ~1 s */
	if (d > 60U) d = 60U;
	return (uint16_t)(740U + d);
}
static uint16_t profile_pulse(uint32_t t)
{
	uint32_t cyc = t % 450U; /* 150 ticks strong (delta 50) + 300 ticks zero */
	return (uint16_t)(740U + (cyc < 150U ? 50U : 0U));
}
static uint16_t profile_noise(uint32_t t)
{
	uint32_t cyc = t % 340U; /* alternates delta 34 vs 10 - hovers at the deadband */
	return (uint16_t)(740U + (cyc < 140U ? 34U : 10U));
}

/* Run a scenario. begin_rolling_rearm() is called on tick 1 (the recorder is
 * primed with the IDLE state on tick 0 first, exactly as a production tick would
 * have been). With close_with_cancel the episode getters are read, then the
 * automaton is cancelled and one final observation closes the episode. */
static uint32_t run_scenario(uint16_t (*profile)(uint32_t), uint32_t n,
	bool log_enabled, bool close_with_cancel, out_t *out,
	uint16_t *ep_current, uint16_t *ep_max, uint16_t *ep_collapse,
	uint32_t *completion_tick)
{
	torque_input_init();
	torque_input_set_run_window_deg(180U);
	torque_input_startup_zero(740);
	fw112_diag_init();
	fw112_diag_set_session_id(1U);

	*ep_current = 0U;
	*ep_max = 0U;
	*ep_collapse = 0U;
	*completion_tick = UINT32_MAX;

	uint8_t prev_state = TORQUE_RECOVERY_IDLE;
	for (uint32_t t = 0; t < n; t++) {
		uint16_t raw = profile(t);
		int16_t corrected = torque_input_correct(raw);
		torque_input_update(raw, corrected, true);
		if ((t % 7U) == 0U) {
			torque_input_run_filter_step(); /* crank-driven RUN window, as in main.c */
		}
		if (t == 0U) {
			if (log_enabled) diag_tick(t); /* primes prev_recovery_state == IDLE */
		} else if (t == 1U) {
			torque_input_begin_rolling_rearm(); /* RECOVERY_ENTER fires this tick */
			if (log_enabled) diag_tick(t);
		} else {
			if (log_enabled) diag_tick(t);
		}

		const torque_snapshot_t *snap = torque_input_get_snapshot();
		out[t].filtered = snap->assist_delta_filtered_native;
		out[t].run = snap->assist_delta_run_native;
		out[t].recovery_state = (uint8_t)torque_input_recovery_state();
		out[t].load = snap->load_centikg;

		uint8_t st = out[t].recovery_state;
		if (prev_state != TORQUE_RECOVERY_IDLE && st == TORQUE_RECOVERY_IDLE) {
			*completion_tick = t;
		}
		prev_state = st;
	}

	if (close_with_cancel) {
		*ep_current = fw112_diag_episode_stable_current();
		*ep_max = fw112_diag_episode_stable_max();
		*ep_collapse = fw112_diag_episode_collapse_count();
		torque_input_cancel_rolling_rearm();
		if (log_enabled) diag_tick(n); /* closes the episode with an EXIT record */
	}
	return n;
}

/* A queued record with its schema-3 edge metadata, collected into a local array
 * first (the queue peek/release API walks one record at a time). */
typedef struct {
	fw112_diag_record_t rec;
	uint16_t edge;
	uint16_t evtval;
} slot_view_t;

static uint32_t collect_records(uint8_t session, slot_view_t *views, uint32_t max)
{
	uint32_t n = 0U;
	while (fw112_diag_queue_count_session(session) > 0U && n < max) {
		if (!fw112_diag_queue_peek_session(session, &views[n].rec)) break;
		fw112_diag_queue_peek_meta(session, &views[n].edge, &views[n].evtval);
		fw112_diag_queue_release_session(session);
		n++;
	}
	return n;
}

static uint32_t count_event(const slot_view_t *views, uint32_t n, uint8_t evt)
{
	uint32_t cnt = 0U;
	for (uint32_t i = 0U; i < n; i++) {
		if (views[i].rec.event_type == evt) cnt++;
	}
	return cnt;
}

int main(void)
{
	printf("FW-112-STABILITY host suite: real torque_input.c + fw112_diag.c\n");

	/* ---- S1: SHARP STEP PASS ------------------------------------------------- */
	printf("[S1] sharp step: expect completed recovery, edge_stable==560, 0 collapses\n");
	{
		uint16_t ep_cur, ep_max, ep_col;
		uint32_t done;
		run_scenario(profile_sharp, 1200U, true, false, g_traj_a,
			&ep_cur, &ep_max, &ep_col, &done);
		CHECK(done != UINT32_MAX, "S1: recovery completed (state returned to IDLE)");
		CHECK(done <= 1200U, "S1: completed within the run");
		CHECK(g_traj_a[1199].recovery_state == TORQUE_RECOVERY_IDLE,
			"S1: final state IDLE");

		slot_view_t views[8];
		uint32_t nrec = collect_records(1U, views, 8U);
		CHECK(nrec == 2U, "S1: exactly 2 records (ENTER + EXIT)");
		CHECK(count_event(views, nrec, FW112_EVT_RECOVERY_COLLAPSE) == 0U,
			"S1: no COLLAPSE records");
		CHECK(count_event(views, nrec, FW112_EVT_RECOVERY_ENTER) == 1U,
			"S1: one ENTER record");
		CHECK(count_event(views, nrec, FW112_EVT_RECOVERY_EXIT) == 1U,
			"S1: one EXIT record");

		/* the EXIT record carries the completed-streak evidence on the wire */
		const slot_view_t *exitv = &views[nrec - 1U];
		CHECK(exitv->rec.event_type == FW112_EVT_RECOVERY_EXIT,
			"S1: last record is the EXIT");
		CHECK(exitv->edge == TORQUE_ROLLING_REARM_STABLE_TICKS,
			"S1: EXIT edge_stable_ticks == 560 (completed recovery)");
		CHECK(exitv->evtval == 0U, "S1: EXIT event_value (collapse count) == 0");
		CHECK(((exitv->rec.packed_state >> 4) & 0x03U) == TORQUE_RECOVERY_IDLE,
			"S1: EXIT recorded in IDLE");
	}
	fw112_diag_init();
	fw112_diag_set_session_id(1U);

	/* ---- S2: SLOW RAMP (OBSERVE) ---------------------------------------------- */
	printf("[S2] slow ramp: expect a completed recovery; results reported\n");
	{
		uint16_t ep_cur, ep_max, ep_col;
		uint32_t done;
		uint32_t n = run_scenario(profile_ramp, 5000U, true, false, g_traj_a,
			&ep_cur, &ep_max, &ep_col, &done);
		CHECK(done != UINT32_MAX, "S2: recovery completed (monotonic ramp)");
		slot_view_t views[8];
		uint32_t nrec = collect_records(1U, views, 8U);
		CHECK(nrec == 2U, "S2: exactly 2 records (no collapses on a monotonic ramp)");
		CHECK(count_event(views, nrec, FW112_EVT_RECOVERY_COLLAPSE) == 0U,
			"S2: no COLLAPSE records");
		printf("      S2 observe: completion at tick %u (~%u ms), %u ticks total\n",
			done, done / 4U, n);
	}
	fw112_diag_init();
	fw112_diag_set_session_id(1U);

	/* ---- S3: PULSATING INPUT -------------------------------------------------- */
	printf("[S3] pulsating input: expect one honest collapse per 450-tick cycle\n");
	{
		uint16_t ep_cur, ep_max, ep_col;
		uint32_t done;
		run_scenario(profile_pulse, 3600U, true, true, g_traj_a,
			&ep_cur, &ep_max, &ep_col, &done);
		CHECK(done == UINT32_MAX, "S3: never completes under pulsating load");
		CHECK(ep_col == 8U, "S3: episode getter reports 8 collapses (8 cycles)");
		CHECK(ep_max > 0U && ep_max < TORQUE_ROLLING_REARM_STABLE_TICKS,
			"S3: episode peak streak inside (0, 560) - never reached the dwell");

		slot_view_t views[24];
		uint32_t nrec = collect_records(1U, views, 24U);
		CHECK(nrec == 10U, "S3: 10 records = ENTER + 8 COLLAPSE + EXIT(cancel)");
		CHECK(count_event(views, nrec, FW112_EVT_RECOVERY_COLLAPSE) == 8U,
			"S3: 8 COLLAPSE records on the wire");

		uint32_t col = 0U;
		uint16_t worst_edge = 0U;
		for (uint32_t i = 0U; i < nrec; i++) {
			if (views[i].rec.event_type != FW112_EVT_RECOVERY_COLLAPSE) continue;
			col++;
			/* collapse fires the first tick the filtered assist drops BELOW the
			 * deadband, so the recorded value must be < 10 - the evidence that the
			 * streak's premise was exactly what failed. */
			CHECK(views[i].edge > 0U && views[i].edge < TORQUE_ROLLING_REARM_STABLE_TICKS,
				"S3: collapse streak inside (0, 560)");
			CHECK(views[i].evtval < TORQUE_ASSIST_DEADBAND_NATIVE,
				"S3: collapse carried filtered assist below the deadband");
			if (views[i].edge < worst_edge || worst_edge == 0U) {
				worst_edge = views[i].edge;
			}
		}
		CHECK(col == 8U, "S3: all 8 collapse records verified one by one");
		printf("      S3 observe: shortest pre-collapse streak %u ticks, episode peak %u\n",
			worst_edge, ep_max);

		/* the EXIT record (from the cancel) stamps the episode collapse count */
		const slot_view_t *exitv = &views[nrec - 1U];
		CHECK(exitv->rec.event_type == FW112_EVT_RECOVERY_EXIT,
			"S3: final record is the EXIT (cancel)");
		CHECK(exitv->evtval == 8U, "S3: EXIT event_value == 8 (episode collapse count)");
		CHECK(exitv->edge == 0U, "S3: EXIT edge_stable == 0 (cancelled from WAIT)");
	}
	fw112_diag_init();
	fw112_diag_set_session_id(1U);

	/* ---- S4: NEAR-THRESHOLD NOISE --------------------------------------------- */
	printf("[S4] near-threshold noise: expect honest collapses, no completion\n");
	{
		uint16_t ep_cur, ep_max, ep_col;
		uint32_t done;
		run_scenario(profile_noise, 2400U, true, true, g_traj_a,
			&ep_cur, &ep_max, &ep_col, &done);
		CHECK(done == UINT32_MAX, "S4: never completes under noise");
		CHECK(ep_max < TORQUE_ROLLING_REARM_STABLE_TICKS,
			"S4: peak streak below the 560-tick dwell");
		CHECK(ep_col >= 1U, "S4: at least one honest collapse");
		slot_view_t views[24];
		uint32_t nrec = collect_records(1U, views, 24U);
		CHECK(count_event(views, nrec, FW112_EVT_RECOVERY_COLLAPSE) == ep_col,
			"S4: wire collapse count matches the episode getter");
		for (uint32_t i = 0U; i < nrec; i++) {
			if (views[i].rec.event_type == FW112_EVT_RECOVERY_COLLAPSE) {
				CHECK(views[i].edge > 0U && views[i].edge < TORQUE_ROLLING_REARM_STABLE_TICKS,
					"S4: every collapse streak inside (0, 560)");
			}
		}
		printf("      S4 observe: %u collapses, episode peak streak %u\n", ep_col, ep_max);
	}
	fw112_diag_init();
	fw112_diag_set_session_id(1U);

	/* ---- S5: SHARP STEP EQUIVALENCE (recorder on vs never called) -------------- */
	printf("[S5] sharp-step equivalence: recorder must not perturb torque_input\n");
	{
		uint32_t n = 1200U;
		uint16_t a, b, c;
		uint32_t da, db;
		run_scenario(profile_sharp, n, true, false, g_traj_a, &a, &b, &c, &da);
		run_scenario(profile_sharp, n, false, false, g_traj_b, &a, &b, &c, &db);
		CHECK(da == db, "S5: same completion tick with and without the recorder");
		CHECK(memcmp(g_traj_a, g_traj_b, sizeof(out_t) * n) == 0,
			"S5: torque_input output trajectory byte-identical");
	}

	/* ---- S6: PULSATING EQUIVALENCE --------------------------------------------- */
	printf("[S6] pulsating equivalence: recorder must not perturb torque_input\n");
	{
		uint32_t n = 3600U;
		uint16_t a, b, c;
		uint32_t da, db;
		run_scenario(profile_pulse, n, true, false, g_traj_a, &a, &b, &c, &da);
		run_scenario(profile_pulse, n, false, false, g_traj_b, &a, &b, &c, &db);
		CHECK(da == db, "S6: same completion tick with and without the recorder");
		CHECK(memcmp(g_traj_a, g_traj_b, sizeof(out_t) * n) == 0,
			"S6: torque_input output trajectory byte-identical");
	}

	if (host_test_failures != 0) {
		printf("%d FAILURE(S)\n", host_test_failures);
		return 1;
	}
	printf("All FW-112-STABILITY host checks: PASS\n");
	return 0;
}