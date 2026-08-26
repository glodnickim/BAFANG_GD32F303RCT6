/*
 * FW-112-STABILITY retention: host tests for the full-ring eviction policy in
 * src/fw112_diag.c - a full queue no longer refuses every later event, it evicts
 * the OLDEST COMPLETED recovery saga and keeps the in-flight one.
 *
 * A "recovery saga" is the span a reader already joins to a fw111 rearm record:
 * it STARTS at a PERMISSION_REVOKED and ENDS at the first RECOVERY_EXIT
 * (inclusive) or at the next PERMISSION_REVOKED (exclusive) - whichever comes
 * first in ring order. A saga that has neither terminated nor been closed by a
 * later REVOKED is the IN-FLIGHT saga (always the newest). The policy: on a full
 * ring, evict the OLDEST COMPLETED saga; never evict the in-flight saga; a new
 * PERMISSION_REVOKED arriving while a saga has not terminated closes it for
 * retention at that boundary. Only when nothing is evictable is the event
 * refused (rejected_total++ as before).
 *
 * What these tests pin down (S1..S10) and what each M-mutation is (M1..M5):
 *
 *   S1  capacity normal: 24 events (6 complete sagas) all accepted below the
 *       ceiling; nothing rejected and nothing evicted.
 *   S2  full + one completed old saga: the 25th event evicts the OLDEST
 *       completed saga and is itself accepted; rejected stays 0.
 *   S3  multiple sagas: each full-ring append evicts only the OLDEST saga - the
 *       second-oldest and every newer completed saga survive in order.
 *   S4  active-saga protection: a full ring holding ONLY the in-flight saga
 *       refuses a COLLAPSE and an EXIT (nothing completed to evict) and leaves
 *       the whole saga intact.
 *   S5  a new REVOKED closes the unfinished saga: a full ring of one open saga
 *       is wholly evicted by the incoming REVOKED and the new REVOKED lands.
 *   S6  wraparound: with q_head advanced (a partial dump) and the ring wrapped
 *       across the physical 0 boundary, eviction still removes the oldest saga
 *       and the survivors stay in chronological order.
 *   S7  >100 sagas: 100 complete sagas drive 94 evictions with ZERO refusals -
 *       the recorder keeps late-ride evidence instead of losing it.
 *   S8  single saga > ring capacity: an open saga larger than the ring is
 *       refused only once there is nothing to evict; no corruption, the first
 *       24 records survive in order.
 *   S9  dump order: after many evictions the drained stream is still strictly
 *       chronological (event_id increasing by one, saga pattern intact).
 *   S10 logger OFF/ON equivalence (real torque_input.c recovery automaton): the
 *       same long ride runs twice - recorder fed vs never called - and the full
 *       torque_input output trajectory is byte-identical, while the recorder
 *       proves the eviction engaged and refused nothing.
 *
 *   M1  evict the NEWEST completed saga instead of the oldest -> S3 fails (the
 *       second-oldest is gone, the oldest survives).
 *   M2  evict the ACTIVE (in-flight) saga -> S4 fails (the protected saga is
 *       disturbed by the refused events).
 *   M3  off-by-one that leaves one record of the old saga -> S2 and S6 fail
 *       (a survivor of the evicted saga is still queued).
 *   M4  the block shift is wrong under wraparound -> S6 fails (the surviving
 *       ids are not 12..35 in order).
 *   M5  the old reject-on-full fallback (no eviction at all) -> S2, S5 and S7
 *       fail (rejected_total rises and evicted_* stay 0).
 *
 * Each mutation is caught by the corresponding BEHAVIOURAL test above (the real
 * module linked), not by a source-text scan.
 */

#include "../common/check.h"

#include "fw112_diag.h"
#include "torque_input.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t tick;
static fw112_diag_input_t in;

static const uint8_t ST_ACTIVE    = 1U;
static const uint8_t ST_SUSPENDED = 2U;
static const uint8_t REC_IDLE     = 0U;
static const uint8_t REC_WAIT     = 1U;
static const uint8_t REC_TRACK    = 2U;

static void reset(void)
{
	tick = 0;
	memset(&in, 0, sizeof(in));
	fw112_diag_init();
	fw112_diag_set_session_id(1);
}

static void step(void)
{
	fw112_diag_tick(&in, tick);
	tick++;
}

/* Steady ACTIVE baseline: latched, fwd_run>0, iq>0, recovery IDLE, no hold - no
 * edge fires, so it only advances the tick. */
static void baseline(void)
{
	in.session_state = ST_ACTIVE;
	in.recovery_state = REC_IDLE;
	in.dir_state = 1;
	in.fwd_run = 6;
	in.latched = true;
	in.assist_hold_ticks = 0;
	in.load_centikg = 0;
	in.load_threshold_centikg = 0;
	in.iq_request = 300;
	in.iq_pre_ramp = 300;
	in.iq_setpoint = 300;
	in.iq_actual = 290;
	in.reason_bits = 0;
}

/* One COMPLETE recovery saga: REVOKED -> GRANTED (fast rearm) -> ENTER -> EXIT.
 * 4 records. */
static void saga_complete(void)
{
	in.session_state = ST_SUSPENDED;
	in.latched = false;
	in.fwd_run = 0;
	in.iq_setpoint = 0;
	step();                                     /* PERMISSION_REVOKED */
	in.session_state = ST_ACTIVE;
	in.latched = true;
	in.fwd_run = 6;
	in.iq_setpoint = 300;
	step();                                     /* PERMISSION_GRANTED (fast rearm) */
	in.recovery_state = REC_WAIT;
	step();                                     /* RECOVERY_ENTER */
	in.recovery_state = REC_IDLE;
	step();                                     /* RECOVERY_EXIT */
	baseline();
}

/* An OPEN (in-flight) saga: REVOKED -> GRANTED -> ENTER, recovery left in WAIT.
 * 3 records. */
static void saga_open(void)
{
	in.session_state = ST_SUSPENDED;
	in.latched = false;
	in.fwd_run = 0;
	in.iq_setpoint = 0;
	step();                                     /* PERMISSION_REVOKED */
	in.session_state = ST_ACTIVE;
	in.latched = true;
	in.fwd_run = 6;
	in.iq_setpoint = 300;
	step();                                     /* PERMISSION_GRANTED (fast rearm) */
	in.recovery_state = REC_WAIT;
	step();                                     /* RECOVERY_ENTER */
}

/* One collapse inside an open saga: WAIT -> TRACK (no event) -> WAIT (COLLAPSE).
 * 1 record. */
static void collapse(void)
{
	in.recovery_state = REC_TRACK;
	step();
	in.recovery_state = REC_WAIT;
	step();
}

/* Drain a session into an array of (event_id, event_type) views. */
typedef struct {
	uint16_t event_id;
	uint8_t  event_type;
} view_t;

static uint32_t collect(uint8_t session, view_t *out, uint32_t max)
{
	uint32_t n = 0;
	fw112_diag_record_t r;
	while (fw112_diag_queue_count_session(session) > 0U && n < max) {
		if (!fw112_diag_queue_peek_session(session, &r)) break;
		out[n].event_id = r.event_id;
		out[n].event_type = r.event_type;
		fw112_diag_queue_release_session(session);
		n++;
	}
	return n;
}

static int seq_ok(const view_t *v, uint32_t n, uint16_t first)
{
	for (uint32_t i = 0; i < n; i++) {
		if (v[i].event_id != (uint16_t)(first + i)) return 0;
	}
	return 1;
}

/* ---------------- S1 : capacity normal ------------------------------------------------------- */

static void test_capacity_normal(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	for (uint32_t i = 0; i < 6U; i++) saga_complete();   /* 24 records, never full */
	CHECK(fw112_diag_queue_enqueued() == 24U, "S1: 24 events accepted below capacity");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S1: queue holds all 24");
	CHECK(fw112_diag_queue_rejected() == 0U, "S1: nothing refused below capacity");
	CHECK(fw112_diag_queue_evicted_sagas() == 0U, "S1: nothing evicted below capacity");
	CHECK(fw112_diag_queue_evicted_records() == 0U, "S1: no records evicted");
}

/* ---------------- S2 : full + one completed old saga ------------------------------------------ */

static void test_full_evicts_oldest(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	for (uint32_t i = 0; i < 6U; i++) saga_complete();   /* sagas 0..5, ids 0..23, full */
	saga_complete();                                     /* saga 6 -> evicts saga 0 */
	CHECK(fw112_diag_queue_evicted_sagas() == 1U, "S2: one saga evicted on the 25th event");
	CHECK(fw112_diag_queue_evicted_records() == 4U, "S2: that saga held 4 records");
	CHECK(fw112_diag_queue_rejected() == 0U, "S2: the new event was accepted, not refused");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S2: queue still exactly full");
	CHECK(fw112_diag_queue_enqueued() == 28U, "S2: 28 events committed in total");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S2: 24 survivors");
	CHECK(v[0].event_id == 4U && v[0].event_type == FW112_EVT_PERMISSION_REVOKED,
	      "S2: oldest survivor is saga 1's REVOKED (saga 0 evicted)");
	CHECK(v[23].event_id == 27U && v[23].event_type == FW112_EVT_RECOVERY_EXIT,
	      "S2: newest survivor is saga 6's EXIT");
	CHECK(seq_ok(v, n, 4U), "S2: survivors stay strictly chronological");
}

/* ---------------- S3 : multiple sagas - only the oldest is evicted ---------------------------- */

static void test_evicts_only_oldest(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	for (uint32_t i = 0; i < 6U; i++) saga_complete();   /* full: sagas 0..5, ids 0..23 */
	for (uint32_t i = 0; i < 3U; i++) saga_complete();   /* sagas 6,7,8 - each evicts the oldest */
	CHECK(fw112_diag_queue_evicted_sagas() == 3U, "S3: sagas 0,1,2 evicted");
	CHECK(fw112_diag_queue_evicted_records() == 12U, "S3: 12 records evicted");
	CHECK(fw112_diag_queue_rejected() == 0U, "S3: no refusals");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S3: queue still full");
	CHECK(fw112_diag_queue_enqueued() == 36U, "S3: 36 events committed");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S3: 24 survivors");
	CHECK(seq_ok(v, n, 12U), "S3: survivors are exactly ids 12..35 (sagas 3..8)");
	CHECK(v[0].event_id == 12U && v[0].event_type == FW112_EVT_PERMISSION_REVOKED,
	      "S3: saga 3 preserved as the new oldest");
	CHECK(v[23].event_id == 35U && v[23].event_type == FW112_EVT_RECOVERY_EXIT,
	      "S3: saga 8 preserved as the newest");
	uint32_t revoked = 0;
	for (uint32_t i = 0; i < n; i++) if (v[i].event_type == FW112_EVT_PERMISSION_REVOKED) revoked++;
	CHECK(revoked == 6U, "S3: exactly 6 sagas remain (3..8), the evicted 0..2 are gone");
}

/* ---------------- S4 : active-saga protection ------------------------------------------------ */

static void test_active_saga_protected(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	saga_open();                                    /* REVOKED, GRANTED, ENTER */
	for (uint32_t i = 0; i < 21U; i++) collapse();  /* 21 COLLAPSE -> 24 records, full */
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S4: ring full with one open saga");
	CHECK(fw112_diag_queue_rejected() == 0U, "S4: filled without refusals");
	CHECK(fw112_diag_queue_evicted_sagas() == 0U, "S4: nothing evicted while filling");
	collapse();                                     /* another COLLAPSE -> must refuse */
	CHECK(fw112_diag_queue_rejected() == 1U, "S4: COLLAPSE refused (nothing completed to evict)");
	CHECK(fw112_diag_queue_evicted_sagas() == 0U, "S4: the in-flight saga was not evicted");
	in.recovery_state = REC_IDLE;
	step();                                         /* EXIT -> must also refuse */
	CHECK(fw112_diag_queue_rejected() == 2U, "S4: EXIT refused too (not a REVOKED closure)");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S4: ring unchanged after the refusals");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S4: all 24 records of the in-flight saga survive");
	CHECK(seq_ok(v, n, 0U), "S4: ids 0..23 intact, no corruption");
	CHECK(v[0].event_type == FW112_EVT_PERMISSION_REVOKED, "S4: saga still starts with REVOKED");
	CHECK(v[23].event_type == FW112_EVT_RECOVERY_COLLAPSE, "S4: saga still ends with COLLAPSE");
}

/* ---------------- S5 : a new REVOKED closes the unfinished saga ------------------------------- */

static void test_revoked_closes_open_saga(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	saga_open();
	for (uint32_t i = 0; i < 21U; i++) collapse();  /* 24 records, one open saga, full */
	in.session_state = ST_SUSPENDED;
	in.latched = false;
	in.fwd_run = 0;
	in.iq_setpoint = 0;
	step();                                         /* REVOKED closes it for retention */
	CHECK(fw112_diag_queue_evicted_sagas() == 1U, "S5: the unfinished saga was closed and evicted");
	CHECK(fw112_diag_queue_evicted_records() == 24U, "S5: all 24 of its records evicted");
	CHECK(fw112_diag_queue_rejected() == 0U, "S5: the new REVOKED was accepted");
	CHECK(fw112_diag_queue_count_session(1) == 1U, "S5: only the new REVOKED remains");
	CHECK(fw112_diag_queue_enqueued() == 25U, "S5: 25 events committed");
	view_t v[1];
	uint32_t n = collect(1, v, 1U);
	CHECK(n == 1U, "S5: one survivor");
	CHECK(v[0].event_id == 24U && v[0].event_type == FW112_EVT_PERMISSION_REVOKED,
	      "S5: the survivor is the new REVOKED, starting a fresh saga");
}

/* ---------------- S6 : wraparound ------------------------------------------------------------ */

static void test_wraparound(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	for (uint32_t i = 0; i < 6U; i++) saga_complete();   /* sagas 0..5, ids 0..23, full */
	for (uint32_t i = 0; i < 8U; i++) fw112_diag_queue_release_session(1); /* drop ids 0..7 */
	CHECK(fw112_diag_queue_count_session(1) == 16U, "S6: partial dump left 16 (sagas 2..5)");
	saga_complete();                                    /* sagas 6: ids 24..27 */
	saga_complete();                                    /* sagas 7: ids 28..31 -> full, wrapped */
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S6: refilled to capacity with head advanced");
	saga_complete();                                    /* saga 8: ids 32..35 -> evicts saga 2 across the wrap */
	CHECK(fw112_diag_queue_evicted_sagas() == 1U, "S6: eviction fired under wraparound");
	CHECK(fw112_diag_queue_evicted_records() == 4U, "S6: exactly one saga's 4 records evicted");
	CHECK(fw112_diag_queue_rejected() == 0U, "S6: no refusals");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S6: queue still full");
	CHECK(fw112_diag_queue_enqueued() == 36U, "S6: 36 events committed");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S6: 24 survivors");
	CHECK(seq_ok(v, n, 12U), "S6: survivors are ids 12..35 in order (sagas 3..8)");
	CHECK(v[0].event_id == 12U && v[0].event_type == FW112_EVT_PERMISSION_REVOKED,
	      "S6: saga 3 is the new oldest");
	CHECK(v[23].event_id == 35U && v[23].event_type == FW112_EVT_RECOVERY_EXIT,
	      "S6: saga 8 is the newest");
}

/* ---------------- S7 : >100 sagas, zero refusals --------------------------------------------- */

static void test_hundred_sagas(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	for (uint32_t i = 0; i < 100U; i++) saga_complete();
	CHECK(fw112_diag_queue_rejected() == 0U, "S7: zero refusals across 100 sagas");
	CHECK(fw112_diag_queue_evicted_sagas() == 94U, "S7: 94 evictions (100 sagas - 6 that fit)");
	CHECK(fw112_diag_queue_evicted_records() == 376U, "S7: 376 records evicted");
	CHECK(fw112_diag_queue_enqueued() == 400U, "S7: all 400 events committed");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S7: queue holds the newest 24");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S7: 24 survivors");
	CHECK(seq_ok(v, n, 376U), "S7: survivors are the last 6 sagas (ids 376..399) in order");
	CHECK(v[0].event_type == FW112_EVT_PERMISSION_REVOKED, "S7: oldest survivor starts a saga");
	CHECK(v[23].event_type == FW112_EVT_RECOVERY_EXIT, "S7: newest survivor ends a saga");
}

/* ---------------- S8 : single saga > ring capacity ------------------------------------------- */

static void test_single_saga_overflow(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	saga_open();                                    /* 3 records */
	for (uint32_t i = 0; i < 25U; i++) collapse();  /* 25 more -> 28 events, only 24 fit */
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S8: only the first 24 records fit");
	CHECK(fw112_diag_queue_enqueued() == 24U, "S8: 24 committed");
	CHECK(fw112_diag_queue_rejected() == 4U, "S8: the 4 overflow events were refused");
	CHECK(fw112_diag_queue_evicted_sagas() == 0U, "S8: nothing evicted (nothing completed)");
	CHECK(fw112_diag_queue_evicted_records() == 0U, "S8: no records evicted");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S8: all 24 survivors intact");
	CHECK(seq_ok(v, n, 0U), "S8: ids 0..23 in order, no corruption");
	CHECK(v[0].event_type == FW112_EVT_PERMISSION_REVOKED, "S8: starts with REVOKED");
	CHECK(v[23].event_type == FW112_EVT_RECOVERY_COLLAPSE, "S8: ends with COLLAPSE");
}

/* ---------------- S9 : dump order stays chronological ---------------------------------------- */

static void test_dump_order(void)
{
	reset();
	baseline();
	step();   /* prime the prev_* edge latches (no event fires) */
	for (uint32_t i = 0; i < 20U; i++) saga_complete();  /* 14 evictions, sagas 0..13 gone */
	CHECK(fw112_diag_queue_evicted_sagas() == 14U, "S9: 14 evictions");
	CHECK(fw112_diag_queue_evicted_records() == 56U, "S9: 56 records evicted");
	CHECK(fw112_diag_queue_rejected() == 0U, "S9: no refusals");
	CHECK(fw112_diag_queue_count_session(1) == 24U, "S9: queue holds the newest 24");
	view_t v[24];
	uint32_t n = collect(1, v, 24U);
	CHECK(n == 24U, "S9: 24 records drained");
	CHECK(seq_ok(v, n, 56U), "S9: drained order is strictly chronological (ids 56..79)");
	for (uint32_t i = 0; i < n; i++) {
		uint8_t expect = (uint8_t)((i % 4U == 0U) ? FW112_EVT_PERMISSION_REVOKED :
		                   (i % 4U == 1U) ? FW112_EVT_PERMISSION_GRANTED :
		                   (i % 4U == 2U) ? FW112_EVT_RECOVERY_ENTER :
		                                   FW112_EVT_RECOVERY_EXIT);
		if (v[i].event_type != expect) {
			CHECK(0, "S9: saga record pattern intact after evictions");
			break;
		}
	}
	CHECK(v[0].event_id == 56U && v[0].event_type == FW112_EVT_PERMISSION_REVOKED,
	      "S9: oldest survivor starts saga 14");
}

/* ---------------- S10 : logger OFF/ON equivalence (real torque_input) ------------------------- */

#define TRAJ_N 8192U

typedef struct {
	uint16_t filtered;
	uint16_t run;
	uint8_t  recovery_state;
	uint16_t load;
} traj_t;

static traj_t g_on[TRAJ_N];
static traj_t g_off[TRAJ_N];

/* Rapid churn: 60 native units for 60 ticks (well above the deadband -> TRACK)
 * then 5 (well below -> WAIT/collapse). Over a long run this makes many recovery
 * edges AND, with the session toggling in ret_diag_tick(), many closed sagas. */
static uint16_t profile_churn(uint32_t t)
{
	uint32_t cyc = t % 120U;
	return (uint16_t)(740U + (cyc < 60U ? 60U : 5U));
}

/* One fw112_diag observation for the current torque_input state, the same shape
 * main.c's diag block produces, PLUS a session ACTIVE/SUSPENDED toggle every 300
 * ticks so the retention policy has REVOKED/GRANTED saga boundaries to act on.
 * The session byte is a plain recorder input - torque_input never sees it. */
static void ret_diag_tick(uint32_t now)
{
	const torque_snapshot_t *snap = torque_input_get_snapshot();
	fw112_diag_input_t x;
	memset(&x, 0, sizeof(x));
	bool active = ((now / 300U) & 1U) == 0U;
	x.session_state = active ? ST_ACTIVE : ST_SUSPENDED;
	x.recovery_state = (uint8_t)torque_input_recovery_state();
	x.dir_state = 1U;
	x.fwd_run = active ? 6U : 0U;
	x.cadence_rpm = 80U;
	x.latched = active;
	x.pwm_on = true;
	x.torque_sensor_valid = true;
	x.wheel_valid = true;
	x.load_centikg = snap->load_centikg;
	x.load_threshold_centikg = 0U;
	x.recovery_stable_ticks = torque_input_recovery_stable_ticks();
	x.recovery_stable_ticks_at_edge = torque_input_recovery_stable_ticks_at_edge();
	x.assist_delta_filtered_native = snap->assist_delta_filtered_native;
	x.iq_request = 400;
	x.iq_pre_ramp = 400;
	x.iq_setpoint = 400;
	x.iq_actual = 400;
	x.reason_bits = 0U;
	fw112_diag_tick(&x, now);
}

static void run_retention_scenario(uint32_t n, bool log_enabled, traj_t *out)
{
	torque_input_init();
	torque_input_set_run_window_deg(180U);
	torque_input_startup_zero(740);   /* raw 740 == zero: 740 + d is a delta of d native */
	if (log_enabled) {
		fw112_diag_init();
		fw112_diag_set_session_id(1U);
	}
	for (uint32_t t = 0; t < n; t++) {
		uint16_t raw = profile_churn(t);
		int16_t corrected = torque_input_correct(raw);
		torque_input_update(raw, corrected, true);
		if ((t % 7U) == 0U) torque_input_run_filter_step();
		if (t == 0U) {
			if (log_enabled) ret_diag_tick(t);           /* primes prev_* states */
		} else if (t == 1U) {
			torque_input_begin_rolling_rearm();          /* RECOVERY_ENTER fires */
			if (log_enabled) ret_diag_tick(t);
		} else {
			if (log_enabled) ret_diag_tick(t);
		}
		const torque_snapshot_t *snap = torque_input_get_snapshot();
		out[t].filtered = snap->assist_delta_filtered_native;
		out[t].run = snap->assist_delta_run_native;
		out[t].recovery_state = (uint8_t)torque_input_recovery_state();
		out[t].load = snap->load_centikg;
	}
}

static void test_logger_equivalence(void)
{
	run_retention_scenario(TRAJ_N, false, g_off);  /* recorder OFF */
	run_retention_scenario(TRAJ_N, true, g_on);    /* recorder ON */
	CHECK(memcmp(g_on, g_off, sizeof(traj_t) * TRAJ_N) == 0,
	      "S10: torque_input output trajectory byte-identical (recorder on vs off)");
	CHECK(fw112_diag_queue_evicted_sagas() > 0U,
	      "S10: retention eviction engaged on the long ride");
	CHECK(fw112_diag_queue_rejected() == 0U,
	      "S10: zero refusals while completed sagas were evictable");
	CHECK(fw112_diag_queue_enqueued() > FW112_DIAG_RECORDS,
	      "S10: the ride exceeded the 24-record ceiling (recorder had to evict)");
}

int main(void)
{
	printf("FW-112-STABILITY retention: full-ring saga eviction in the real fw112_diag.c\n");

	test_capacity_normal();          /* S1 */
	test_full_evicts_oldest();       /* S2  (M3) */
	test_evicts_only_oldest();       /* S3  (M1) */
	test_active_saga_protected();    /* S4  (M2) */
	test_revoked_closes_open_saga(); /* S5  (M5) */
	test_wraparound();               /* S6  (M3, M4) */
	test_hundred_sagas();            /* S7  (M5) */
	test_single_saga_overflow();     /* S8 */
	test_dump_order();               /* S9 */
	test_logger_equivalence();       /* S10 */

	if (host_test_failures != 0) {
		printf("fw112_retention_host: %d FAILURES\n", host_test_failures);
		return 1;
	}
	printf("fw112_retention_host: ALL PASS\n");
	return 0;
}