#include "fw112_diag.h"
#include "diag_budget.h"

#if CAN_DIAGNOSTICS_ENABLE

#include <string.h>

/*
 * FW-112-DIAG: see the header for what this records and why. Implementation notes only.
 *
 * All mutable state lives in the single struct R below so the RAM budget check measures the
 * whole module, and the module is driven entirely through fw112_diag_input_t - the session /
 * recovery / direction states arrive as plain bytes, the torque/Iq chain as plain scalars,
 * nothing is linked from ride_session.c, ride_control.c or main.c.
 *
 * Every event is an EDGE captured on the tick it first fires - never a per-tick spam. A record
 * is appended only when the event's contiguous stretch begins (BLOCKED / ZEROED) or the state
 * machine transitions (session / recovery / hold). The queue is per-session, exactly like
 * rearm_delay_diag.c.
 *
 * RETENTION (FW-112-STABILITY): a full queue no longer refuses every later event. A recovery
 * saga runs from a PERMISSION_REVOKED (its start) to RECOVERY_EXIT (its terminal), or is closed
 * by the next PERMISSION_REVOKED arriving before it terminated - a REVOKED/EXIT/REVOKED
 * boundary is what a reader joins to a fw111 rearm record anyway. When the ring is full, the
 * OLDEST COMPLETED saga is evicted (its records removed, evicted_* counters incremented) and
 * the new event is appended - the current in-flight saga is never evicted - so late-ride
 * recovery evidence is kept instead of lost to the 24-record ceiling. Only when NO completed
 * saga exists (a single open saga fills the whole ring and the incoming event is not a REVOKED
 * that closes it) is the event still refused: rejected_total -> DIAG_ERR_CAPTURES_FULL +
 * DIAG_TRAILER_F_FW112_REJECTED, so a refusal is always honest saturation, never silent loss.
 */

/*
 * FW-112-STABILITY: one queued slot = the 32 B wire record plus 4 B of edge metadata that does
 * NOT fit in the record (it is exactly 32 B, asserted below) and is written into the record's
 * spare wire bytes at dump time instead - schema 3 (see fw112_diag.h).
 */
typedef struct {
	fw112_diag_record_t rec;
	uint16_t edge_stable_ticks;   /* COLLAPSE: the streak before the collapse; EXIT: at close */
	uint16_t event_value;         /* COLLAPSE: filtered assist; EXIT: the episode collapse count */
} fw112_diag_slot_t;

/*
 * ALL of this module's mutable state, in one object (same discipline as rearm_delay_diag.c's
 * R and diag_session.c's D - see inc/diag_budget.h for why).
 */
static struct {
	/* The queue of records awaiting dump. In-place: the slot being appended NOW is the free
	 * queue slot (q_head + q_depth) % FW112_DIAG_RECORDS. */
	fw112_diag_slot_t slots[FW112_DIAG_RECORDS];
	uint8_t q_head;         /* oldest queued record */
	uint8_t q_depth;        /* number of records awaiting dump */
	uint32_t accepted_total;   /* records committed to the queue */
	uint32_t rejected_total;   /* events refused because no completed saga could be evicted */
	/* Retention eviction counters - internal only, never on the wire: a full queue evicts the
	 * oldest completed recovery saga (see the module note), and these count how often. */
	uint32_t evicted_saga_total;
	uint32_t evicted_record_total;

	uint8_t  session_id;       /* stamped into every record opened now */
	uint16_t event_id;         /* monotonic per-record id, wraps at 65536 - a key, never a count */

	/* Edge detection: the previous tick's observations. */
	uint8_t  prev_session_state;
	uint8_t  prev_recovery_state;
	uint16_t prev_hold_ticks;
	bool     blocked_stretch;  /* fwd_run > 0 && !latched */
	bool     zeroed_stretch;   /* fwd_run > 0 && latched && iq_setpoint == 0 */

	/* FW-112-STABILITY: the current recovery EPISODE's tracked state, reset on RECOVERY_ENTER and
	 * closed on RECOVERY_EXIT (the module's own copy - the production streak is cleared on the
	 * transition tick, so this is what survives to be stamped on the EXIT record). */
	uint16_t episode_stable_current;
	uint16_t episode_stable_max;
	uint16_t episode_collapse_count;
	bool     episode_active;

	/* The tick of the previously recorded event, so each record carries the gap to its
	 * predecessor. The first-ever event has no predecessor (ever_recorded false) and records 0. */
	uint32_t last_event_tick;
	bool     ever_recorded;
} R;

_Static_assert(sizeof(R) <= DIAG_BUDGET_FW112_DIAG_BYTES,
	"FW-112-DIAG: fw112_diag's total state exceeds its RAM line item");

_Static_assert(sizeof(fw112_diag_record_t) == 32U,
	"FW-112-DIAG: the record must stay exactly 32 B so 4 data frames serialize it with no padding");

/* Remove count records at logical offset off (0 = oldest) from the ring, shifting the later
 * records down toward the head so the survivors keep their chronological order. Equivalent to
 * calling release_session() count times on the same block, but without re-scanning for a
 * session id and without touching q_head - an eviction target is a record BLOCK, not a
 * session. */
static void remove_block(uint8_t off, uint8_t count)
{
	uint8_t n = R.q_depth;
	for (uint8_t i = (uint8_t)(off + count); i < n; i++) {
		uint8_t dst = (uint8_t)((R.q_head + i - count) % FW112_DIAG_RECORDS);
		uint8_t src = (uint8_t)((R.q_head + i) % FW112_DIAG_RECORDS);
		R.slots[dst] = R.slots[src];
	}
	R.q_depth = (uint8_t)(n - count);
}

/* Evict the oldest COMPLETED recovery saga so a full ring can accept the incoming event. A
 * saga starts at a PERMISSION_REVOKED and ends at the first RECOVERY_EXIT (inclusive) or at the
 * next PERMISSION_REVOKED (exclusive), whichever comes first in ring order. A saga that runs to
 * the tail has not terminated: it is the in-flight saga, and is only evicted when the incoming
 * event is a new PERMISSION_REVOKED, which closes it for retention at that boundary. Returns
 * false when nothing is evictable (the ring holds no saga, or holds only the in-flight saga and
 * the incoming event is not a REVOKED). */
static bool evict_oldest_completed_saga(uint8_t incoming_event_type)
{
	uint8_t n = R.q_depth;

	uint8_t start = 0xFFU;
	for (uint8_t i = 0U; i < n; i++) {
		uint8_t idx = (uint8_t)((R.q_head + i) % FW112_DIAG_RECORDS);
		if (R.slots[idx].rec.event_type == FW112_EVT_PERMISSION_REVOKED) { start = i; break; }
	}
	if (start == 0xFFU) return false;        /* no saga in the ring at all */

	uint8_t end = 0xFFU;
	for (uint8_t i = (uint8_t)(start + 1U); i < n; i++) {
		uint8_t idx = (uint8_t)((R.q_head + i) % FW112_DIAG_RECORDS);
		uint8_t t = R.slots[idx].rec.event_type;
		if (t == FW112_EVT_PERMISSION_REVOKED) { end = i; break; }         /* next saga: exclusive */
		if (t == FW112_EVT_RECOVERY_EXIT) { end = (uint8_t)(i + 1U); break; } /* terminal: inclusive */
	}
	if (end == 0xFFU) {
		if (incoming_event_type != FW112_EVT_PERMISSION_REVOKED) return false;
		end = n;                             /* the incoming REVOKED closes the in-flight saga */
	}

	remove_block(start, (uint8_t)(end - start));
	R.evicted_saga_total++;
	R.evicted_record_total += (uint32_t)(end - start);
	return true;
}

static void append_record(const fw112_diag_input_t *in, uint32_t now_tick, uint8_t event_type)
{
	if (R.q_depth >= FW112_DIAG_RECORDS) {
		/* Retention: evict the oldest completed saga so this event is still captured. Only when
		 * nothing is evictable is the event refused. */
		if (!evict_oldest_completed_saga(event_type)) {
			R.rejected_total++;
			return;
		}
	}
	uint8_t idx = (uint8_t)((R.q_head + R.q_depth) % FW112_DIAG_RECORDS);
	fw112_diag_slot_t *slot = &R.slots[idx];
	fw112_diag_record_t *rec = &slot->rec;
	memset(slot, 0, sizeof(*slot));

	rec->event_id = R.event_id;
	R.event_id++;
	rec->session_id = R.session_id;
	rec->event_type = event_type;
	rec->reason_bits = in->reason_bits;
	rec->flags = (uint8_t)(
		(in->latched ? FW112_FLAG_LATCHED : 0U) |
		(in->pwm_on ? FW112_FLAG_PWM_ON : 0U) |
		(in->torque_sensor_valid ? FW112_FLAG_SENSOR_VALID : 0U) |
		(in->cal_user ? FW112_FLAG_CAL_USER : 0U) |
		(in->wheel_valid ? FW112_FLAG_WHEEL_VALID : 0U) |
		(in->rolling_coast ? FW112_FLAG_ROLLING_COAST : 0U));
	/* A GRANTED record's own edge (which state ACTIVE was re-entered from) tells whether it
	 * was a fast rearm (SUSPENDED_BY_DIRECTION) or a cold start (COLD) - computed here from the
	 * previous session_state, before this tick's update overwrites it. */
	if (event_type == FW112_EVT_PERMISSION_GRANTED) {
		if (R.prev_session_state == 2U) rec->flags |= FW112_FLAG_FAST_REARM;
		else if (R.prev_session_state == 0U) rec->flags |= FW112_FLAG_COLD_ARM;
	}

	/* C0-PROOF (schema 4): packed state — session[1:0]|dir[3:2]|recovery[5:4]|spare[7:6] */
	rec->packed_state = (uint8_t)(
		((in->session_state & 0x03U) << 0) |
		((in->dir_state & 0x03U) << 2) |
		((in->recovery_state & 0x03U) << 4));
	rec->fwd_run = in->fwd_run;
	rec->cadence_rpm = in->cadence_rpm;
	rec->permission_bits = in->permission_bits;
	/* C0-PROOF: PU_CLAMPED — set here from the diagnostic snapshot. FW-129 moved the clamp
	 * point by exactly one limiter: the P/U conversion is now the REQUEST rather than a
	 * ceiling on a separately generated one, so iq_before_pu is the blended request BEFORE
	 * max_iq_pct. iq_before_pu > iq_request therefore now means "the LEVEL's own Iq ceiling
	 * trimmed it", still one specific limiter and still not any other. The wire bit and its
	 * position are unchanged; only which limiter it names has moved. */
	rec->flags2 = in->flags2;
	if (in->iq_before_pu > in->iq_request) {
		rec->flags2 |= FW112_FLAG2_PU_CLAMPED;
	}
	/* hold_ticks_sat: saturate u16 assist_hold_ticks to u8 for wire */
	rec->hold_ticks_sat = (in->assist_hold_ticks > 255U) ?
		255U : (uint8_t)in->assist_hold_ticks;
	/* elapsed_ticks: saturate u32 tick delta to u16 */
	{
		uint32_t delta = R.ever_recorded ? (now_tick - R.last_event_tick) : 0U;
		if (delta > 0xFFFFU) {
			delta = 0xFFFFU;
			rec->flags2 |= FW112_FLAG2_ELAPSED_SAT;
		}
		if (in->assist_hold_ticks > 255U) {
			rec->flags2 |= FW112_FLAG2_HOLD_SAT;
		}
		rec->elapsed_ticks = (uint16_t)delta;
	}
	rec->load_centikg = in->load_centikg;
	rec->load_threshold_centikg = in->load_threshold_centikg;
	rec->motor_voltage_utilization = in->motor_voltage_utilization;
	rec->iq_before_pu = in->iq_before_pu;
	rec->iq_request = in->iq_request;
	rec->iq_pre_ramp = in->iq_pre_ramp;
	rec->iq_setpoint = in->iq_setpoint;
	rec->iq_actual = in->iq_actual;

	/* FW-112-STABILITY (schema 3/4): the recovery-boundary evidence goes beside the record
	 * (it does not fit in the 32 B record's data area) and is written into the record's spare
	 * header bytes at dump time. COLLAPSE: the streak before the collapse + the filtered assist
	 * that drove it; EXIT: the streak at close + the episode's collapse count. Every other event
	 * leaves both zero. */
	if (event_type == FW112_EVT_RECOVERY_COLLAPSE) {
		slot->edge_stable_ticks = in->recovery_stable_ticks_at_edge;
		slot->event_value = in->assist_delta_filtered_native;
		rec->recovery_ev_lo = (uint8_t)(in->recovery_stable_ticks_at_edge & 0xFF);
		rec->recovery_ev_hi = (uint8_t)(in->recovery_stable_ticks_at_edge >> 8);
	} else if (event_type == FW112_EVT_RECOVERY_EXIT) {
		slot->edge_stable_ticks = in->recovery_stable_ticks_at_edge;
		slot->event_value = R.episode_collapse_count;
		rec->recovery_ev_lo = (uint8_t)(in->recovery_stable_ticks_at_edge & 0xFF);
		rec->recovery_ev_hi = (uint8_t)(in->recovery_stable_ticks_at_edge >> 8);
	}

	R.q_depth++;
	R.accepted_total++;
	R.last_event_tick = now_tick;
	R.ever_recorded = true;
}

void fw112_diag_init(void)
{
	memset(&R, 0, sizeof(R));
	R.prev_session_state = 0xFFU;
	R.prev_recovery_state = 0xFFU;
}

void fw112_diag_set_session_id(uint8_t session_id)
{
	R.session_id = session_id;
}

void fw112_diag_tick(const fw112_diag_input_t *in, uint32_t now_tick)
{
	if (in == 0) {
		R.prev_session_state = 0xFFU;
		R.prev_recovery_state = 0xFFU;
		R.blocked_stretch = false;
		R.zeroed_stretch = false;
		return;
	}

	uint8_t st = in->session_state;
	uint8_t rec = in->recovery_state;

	/* --- session edges ---------------------------------------------------------------- */
	if (st != R.prev_session_state && R.prev_session_state != 0xFFU) {
		if (st == 1U) {
			/* Entered ACTIVE: cold start or fast rearm - the flag field tells which. */
			append_record(in, now_tick, (uint8_t)FW112_EVT_PERMISSION_GRANTED);
		} else if (R.prev_session_state == 1U) {
			/* Left ACTIVE: a suspension (direction hold, or FW-112.2 rolling coast - the flags
			 * byte's ROLLING_COAST / reason_bits tell which) or a terminal event (COLD). */
			append_record(in, now_tick, (uint8_t)FW112_EVT_PERMISSION_REVOKED);
		}
	}

	/* --- recovery automaton edges ------------------------------------------------------ */
	if (rec != R.prev_recovery_state && R.prev_recovery_state != 0xFFU) {
		if (rec != 0U && R.prev_recovery_state == 0U) {
			/* IDLE -> WAIT/TRACK: begin_rolling_rearm() opened the recovery - a new episode. */
			R.episode_active = true;
			R.episode_stable_current = 0U;
			R.episode_stable_max = 0U;
			R.episode_collapse_count = 0U;
			append_record(in, now_tick, (uint8_t)FW112_EVT_RECOVERY_ENTER);
		} else if (rec == 0U && R.prev_recovery_state != 0U) {
			/* non-IDLE -> IDLE: recovery completed or was cancelled - close the episode AFTER the
			 * EXIT record has stamped its collapse count. */
			append_record(in, now_tick, (uint8_t)FW112_EVT_RECOVERY_EXIT);
			R.episode_active = false;
			R.episode_stable_current = 0U;
			R.episode_stable_max = 0U;
			R.episode_collapse_count = 0U;
		} else if (rec == 1U && R.prev_recovery_state == 2U) {
			/* TRACK_FAST -> WAIT_FRESH_LOAD: pressure lost mid-recovery. */
			R.episode_collapse_count++;
			append_record(in, now_tick, (uint8_t)FW112_EVT_RECOVERY_COLLAPSE);
		}
	}

	/* FW-112-STABILITY: track the episode's live streak each tick (the module's own copy, since
	 * the production counter is cleared on the transition tick). */
	if (R.episode_active) {
		R.episode_stable_current = in->recovery_stable_ticks;
		if (R.episode_stable_current > R.episode_stable_max) {
			R.episode_stable_max = R.episode_stable_current;
		}
	}

	/* --- ride-latch hold grace --------------------------------------------------------- */
	if (in->latched) {
		if (R.prev_hold_ticks == 0U && in->assist_hold_ticks > 0U) {
			/* Armed (or refreshed from a zero state) by a positive mode demand. */
			append_record(in, now_tick, (uint8_t)FW112_EVT_HOLD_ARMED);
		} else if (R.prev_hold_ticks > 0U && in->assist_hold_ticks == 0U) {
			/* Counted down to zero while still latched: the grace expired. */
			append_record(in, now_tick, (uint8_t)FW112_EVT_HOLD_EXPIRED);
		}
	}

	/* --- the card's two questions, as contiguous stretches ------------------------------ */
	bool blocked_now = in->fwd_run > 0U && !in->latched;
	if (blocked_now && !R.blocked_stretch) {
		/* Entered "pedalling forward but no permission". reason_bits names the holding stage. */
		append_record(in, now_tick, (uint8_t)FW112_EVT_BLOCKED);
	}
	R.blocked_stretch = blocked_now;

	bool zeroed_now = in->fwd_run > 0U && in->latched && in->iq_setpoint == 0;
	if (zeroed_now && !R.zeroed_stretch) {
		/* Entered "permission present, nothing reaching the motor" - WHO-ZEROED, live. */
		append_record(in, now_tick, (uint8_t)FW112_EVT_ZEROED);
	}
	R.zeroed_stretch = zeroed_now;

	R.prev_session_state = st;
	R.prev_recovery_state = rec;
	R.prev_hold_ticks = in->assist_hold_ticks;
}

/* --- diag_record_source bridge, same shape as rearm_delay_diag's ---------------------------- */

static int32_t find_session_index(uint8_t session_id)
{
	for (uint8_t i = 0; i < R.q_depth; i++) {
		uint8_t idx = (uint8_t)((R.q_head + i) % FW112_DIAG_RECORDS);
		if (R.slots[idx].rec.session_id == session_id) return (int32_t)i;
	}
	return -1;
}

uint16_t fw112_diag_queue_count_session(uint8_t session_id)
{
	uint16_t n = 0;
	for (uint8_t i = 0; i < R.q_depth; i++) {
		uint8_t idx = (uint8_t)((R.q_head + i) % FW112_DIAG_RECORDS);
		if (R.slots[idx].rec.session_id == session_id) n++;
	}
	return n;
}

bool fw112_diag_queue_peek_session(uint8_t session_id, fw112_diag_record_t *out)
{
	if (out == 0) return false;
	int32_t off = find_session_index(session_id);
	if (off < 0) return false;
	*out = R.slots[(uint8_t)((R.q_head + (uint8_t)off) % FW112_DIAG_RECORDS)].rec;
	return true;
}

bool fw112_diag_queue_peek_meta(uint8_t session_id, uint16_t *edge_stable_ticks, uint16_t *event_value)
{
	if (edge_stable_ticks == 0 || event_value == 0) return false;
	int32_t off = find_session_index(session_id);
	if (off < 0) return false;
	uint8_t idx = (uint8_t)((R.q_head + (uint8_t)off) % FW112_DIAG_RECORDS);
	*edge_stable_ticks = R.slots[idx].edge_stable_ticks;
	*event_value = R.slots[idx].event_value;
	return true;
}

uint16_t fw112_diag_episode_stable_current(void) { return R.episode_stable_current; }
uint16_t fw112_diag_episode_stable_max(void)     { return R.episode_stable_max; }
uint16_t fw112_diag_episode_collapse_count(void) { return R.episode_collapse_count; }

void fw112_diag_queue_release_session(uint8_t session_id)
{
	int32_t off = find_session_index(session_id);
	if (off < 0) return;
	for (int32_t i = off; i > 0; i--) {
		uint8_t dst = (uint8_t)((R.q_head + (uint8_t)i) % FW112_DIAG_RECORDS);
		uint8_t src = (uint8_t)((R.q_head + (uint8_t)i - 1U) % FW112_DIAG_RECORDS);
		R.slots[dst] = R.slots[src];
	}
	R.q_head = (uint8_t)((R.q_head + 1U) % FW112_DIAG_RECORDS);
	R.q_depth--;
}

uint32_t fw112_diag_queue_enqueued(void) { return R.accepted_total; }
uint32_t fw112_diag_queue_rejected(void) { return R.rejected_total; }
uint32_t fw112_diag_queue_evicted_sagas(void)   { return R.evicted_saga_total; }
uint32_t fw112_diag_queue_evicted_records(void) { return R.evicted_record_total; }

#else  /* !CAN_DIAGNOSTICS_ENABLE */

/* FW-112-DIAG: like every other diagnostic module, the recorder costs ZERO RAM in the production
 * build - see pas_raw.c for why this is a #if rather than a reliance on --gc-sections. */
typedef int fw112_diag_not_compiled_in;

#endif /* CAN_DIAGNOSTICS_ENABLE */
