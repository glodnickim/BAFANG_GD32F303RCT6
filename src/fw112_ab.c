#include "fw112_ab.h"
#include "diag_budget.h"

#if CAN_DIAGNOSTICS_ENABLE

#include <string.h>

/*
 * FW-112 A/B: see the header for what this records and why. Implementation notes only.
 *
 * All mutable state lives in the single struct R below so the RAM budget check measures the
 * whole module, and the module is driven entirely through fw112_ab_input_t - the session /
 * recovery / direction states arrive as plain bytes, the chain and the active configuration as
 * plain scalars, nothing is linked from ride_session.c, ride_control.c, assist_modes.c or
 * main.c (same discipline as fw112_diag.c).
 *
 * The episode automaton:
 *
 *   IDLE --REVOKED edge (session left ACTIVE)--> WAIT_GRANTED   (CONFIG record, arm_tick set)
 *   WAIT_GRANTED --GRANTED edge (entered ACTIVE)--> RUNNING     (GRANTED sample, offset 0)
 *   RUNNING --SETPOINT_POSITIVE edge--> IDLE                    (SETPOINT sample recorded first)
 *   RUNNING / WAIT_GRANTED --window (FW112_AB_WINDOW_TICKS)--> IDLE
 *   any active state --another REVOKED edge--> close, then IDLE --REVOKED--> WAIT_GRANTED again
 *
 * Every milestone is a RISING EDGE captured on the first tick it is true, and each has its own
 * latched "seen" flag so it fires exactly once per episode (fw112_diag's stretch/edge pattern).
 * Milestones are detected in every active state - a rider who resumes pushing while the session
 * is still WAIT_GRANTED produces a TORQUE_POSITIVE with a negative tick_offset, which is exactly
 * the D1 .. D5 material the card is about.
 *
 * The queue is per-session and refuses rather than overwrites when full, exactly like
 * fw112_diag.c: a dump must never lose a session's history, and the refusal is surfaced
 * (rejected_total -> DIAG_ERR_CAPTURES_FULL + DIAG_TRAILER_F_FW112_AB_REJECTED) so recorder
 * saturation is visible in the log rather than silent. When the CONFIG itself is refused the
 * episode is aborted (it cannot be reconstructed from fragments anyway) and rejected_total counts
 * the lost episode.
 */

#define FW112_AB_ST_IDLE        0U
#define FW112_AB_ST_WAIT_GRANTED 1U
#define FW112_AB_ST_RUNNING     2U

/* Post-GRANTED schedule offsets in ticks (GRANTED itself is sample_idx 0 at offset 0). */
static const uint16_t sched_offsets[] = {
	1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8000U
};
#define FW112_AB_N_SCHED (uint8_t)(sizeof(sched_offsets) / sizeof(sched_offsets[0]))

/*
 * ALL of this module's mutable state, in one object (same discipline as fw112_diag.c's R and
 * diag_session.c's D - see inc/diag_budget.h for why).
 */
static struct {
	/* The queue of records awaiting dump. In-place: the slot being appended NOW is the free
	 * queue slot (q_head + q_depth) % FW112_AB_RECORDS. */
	fw112_ab_record_t slots[FW112_AB_RECORDS];
	uint8_t q_head;         /* oldest queued record */
	uint8_t q_depth;        /* number of records awaiting dump */
	uint32_t accepted_total;   /* records committed to the queue */
	uint32_t rejected_total;   /* records refused because every slot held a queued record */

	uint8_t  session_id;       /* stamped into every record opened now */
	uint16_t episode_id;       /* id of the next episode to open, wraps at 65536 - a key, never a count */
	uint16_t ep_cur;           /* id of the episode currently open (CONFIG and its samples share it) */

	/* Episode automaton. */
	uint8_t  fsm;              /* FW112_AB_ST_* */
	uint32_t open_tick;        /* the REVOKED tick that opened the episode (CONFIG arm_tick) */
	uint32_t granted_tick;     /* the GRANTED tick - zero until granted */
	uint8_t  schedule_idx;     /* how many post-GRANTED schedule points have been emitted */

	/* Pre-grant milestone samples are queued with an open-relative offset and their absolute
	 * tick remembered here, so the GRANTED edge can re-stamp them grant-relative (negative). */
	uint8_t  pre_grant_count;
	uint32_t pre_grant_ticks[FW112_AB_MAX_PRE_GRANT];

	/* Edge detection across ticks. */
	uint8_t  prev_session_state;  /* 0xFFU until the first tick */
	bool     ms_torque_pos;       /* previous tick: torque_for_assist_mv > 0 */
	bool     ms_mode_demand_pos;  /* previous tick: iq_request > 0 */
	bool     ms_iq_request_pos;   /* previous tick: iq_after_latch_floor > 0 */
	bool     ms_pre_ramp_pos;     /* previous tick: iq_pre_ramp > 0 */
	bool     ms_setpoint_pos;     /* previous tick: iq_setpoint > 0 */
} R;

_Static_assert(sizeof(R) <= DIAG_BUDGET_FW112_AB_BYTES,
	"FW-112 A/B: fw112_ab's total state exceeds its RAM line item");

_Static_assert(sizeof(fw112_ab_record_t) == 32U,
	"FW-112 A/B: the record must stay exactly 32 B so 4 data frames serialize it with no padding");

static void append_sample(const fw112_ab_input_t *in, uint32_t now_tick,
	uint8_t milestone_id, uint8_t sample_idx)
{
	if (R.q_depth >= FW112_AB_RECORDS) {
		R.rejected_total++;
		return;
	}
	uint8_t idx = (uint8_t)((R.q_head + R.q_depth) % FW112_AB_RECORDS);
	fw112_ab_sample_record_t *rec = &R.slots[idx].smp;
	memset(rec, 0, sizeof(*rec));

	rec->episode_id = R.ep_cur;
	rec->record_type = (uint8_t)FW112_AB_REC_SAMPLE;
	rec->session_id = R.session_id;
	rec->ms_and_idx = FW112_AB_MS_AND_IDX(milestone_id, sample_idx);
	if (R.granted_tick == 0U) {
		/* No permission yet: open-relative now, re-stamped grant-relative at the GRANTED edge. */
		rec->tick_offset = (int16_t)(now_tick - R.open_tick);
		if (R.pre_grant_count < FW112_AB_MAX_PRE_GRANT) {
			R.pre_grant_ticks[R.pre_grant_count++] = now_tick;
		} else {
			/* Defensive: only four pre-grant milestones exist, this cannot happen. */
			R.rejected_total++;
			return;
		}
	} else {
		rec->tick_offset = (int16_t)(now_tick - R.granted_tick);
	}
	rec->torque_for_assist_mv = in->torque_for_assist_mv;
	rec->load_centikg = in->load_centikg;
	rec->cadence_rpm = in->cadence_rpm;
	rec->session_state = in->session_state;
	rec->recovery_state = in->recovery_state;
	rec->dir_state = in->dir_state;
	rec->flags = (uint8_t)(
		(in->latched ? FW112_AB_FLAG_LATCHED : 0U) |
		(in->pwm_on ? FW112_AB_FLAG_PWM_ON : 0U) |
		(in->torque_sensor_valid ? FW112_AB_FLAG_SENSOR_VALID : 0U) |
		(in->wheel_valid ? FW112_AB_FLAG_WHEEL_VALID : 0U) |
		(in->rolling_coast ? FW112_AB_FLAG_ROLLING_COAST : 0U) |
		/* FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2): pure restatement of recovery_state,
		 * see the flag's own comment in inc/fw112_ab.h - never a second source of truth. */
		((in->recovery_state != 0U) ? FW112_AB_FLAG_RECOVERY_ACTIVE : 0U));
	rec->assist_hold_ticks = (uint8_t)(in->assist_hold_ticks & 0xFFU);
	rec->iq_request = in->iq_request;
	rec->iq_after_latch_floor = in->iq_after_latch_floor;
	rec->iq_pre_ramp = in->iq_pre_ramp;
	rec->iq_setpoint = in->iq_setpoint;
	rec->controller_temperature_c = (int8_t)(
		(in->controller_temperature_c > 127) ? 127 :
		(in->controller_temperature_c < -128) ? -128 : in->controller_temperature_c);
	/* FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2): direct afilt/arun, see the fields' own
	 * comments in inc/fw112_ab.h. arun saturates at 255 native instead of wrapping - a silently
	 * wrapped 256 reading back to 0 would look exactly like "arun is low", the one reading this
	 * diagnostic must never produce by accident. */
	rec->arun_native_clamped = (uint8_t)((in->arun_native > 255U) ? 255U : in->arun_native);
	rec->afilt_native = in->afilt_native;
	rec->battery_voltage_mv_div10 = (uint16_t)(in->battery_voltage_mv / 10U);
	rec->assist_level = in->assist_level;

	R.q_depth++;
	R.accepted_total++;
}

static bool append_config(const fw112_ab_input_t *in, uint32_t now_tick)
{
	if (R.q_depth >= FW112_AB_RECORDS) {
		R.rejected_total++;
		return false;
	}
	uint8_t idx = (uint8_t)((R.q_head + R.q_depth) % FW112_AB_RECORDS);
	fw112_ab_config_record_t *rec = &R.slots[idx].cfg;
	memset(rec, 0, sizeof(*rec));

	rec->episode_id = R.episode_id;
	rec->record_type = (uint8_t)FW112_AB_REC_CONFIG;
	rec->session_id = R.session_id;
	rec->assist_level = in->assist_level;
	rec->bank_index = in->bank_index;
	rec->mode_type = in->mode_type;
	rec->emtb_parameter = in->emtb_parameter;
	rec->support_ratio_pct = in->support_ratio_pct;
	rec->max_iq_pct = in->max_iq_pct;
	rec->flags = (uint8_t)(
		(in->emtb_based_on_power ? FW112_AB_CFG_BASED_ON_POWER : 0U) |
		(in->cadence_comp_enabled ? FW112_AB_CFG_CADENCE_COMP : 0U) |
		(in->assist_without_rotation ? FW112_AB_CFG_ASSIST_WITHOUT_ROT : 0U) |
		((in->assist_level == 0U) ? FW112_AB_CFG_LEVEL_ZERO : 0U));
	rec->emtb_reference_voltage_mv = in->emtb_reference_voltage_mv;
	rec->max_motor_power_w = in->max_motor_power_w;
	rec->controller_temperature_c = in->controller_temperature_c;
	rec->battery_voltage_mv_div10 = (uint16_t)(in->battery_voltage_mv / 10U);
	rec->load_threshold_centikg = in->load_threshold_centikg;
	rec->required_steps = in->required_steps;
	rec->start_steps = in->start_steps;
	rec->cadence_rpm_at_arm = in->cadence_rpm;
	rec->arm_tick = now_tick;

	R.q_depth++;
	R.accepted_total++;
	return true;
}

static void open_episode(const fw112_ab_input_t *in, uint32_t now_tick)
{
	R.fsm = FW112_AB_ST_WAIT_GRANTED;
	R.open_tick = now_tick;
	R.granted_tick = 0U;
	R.schedule_idx = 0U;
	R.pre_grant_count = 0U;
	R.ms_torque_pos = false;
	R.ms_mode_demand_pos = false;
	R.ms_iq_request_pos = false;
	R.ms_pre_ramp_pos = false;
	R.ms_setpoint_pos = false;

	uint16_t ep = R.episode_id;
	if (append_config(in, now_tick)) {
		R.ep_cur = ep;
		R.episode_id = ep + 1U;
	} else {
		/* The CONFIG was refused - the episode cannot be reconstructed; abort it. */
		R.episode_id = ep;
		R.fsm = FW112_AB_ST_IDLE;
	}
}

static void close_episode(void)
{
	/* Pre-grant samples already carry open-relative offsets; the episode closed without a grant,
	 * so they stay open-relative (the reader keys on the absence of a GRANTED record). */
	R.pre_grant_count = 0U;
	R.fsm = FW112_AB_ST_IDLE;
}

void fw112_ab_init(void)
{
	memset(&R, 0, sizeof(R));
	R.prev_session_state = 0xFFU;
}

void fw112_ab_set_session_id(uint8_t session_id)
{
	R.session_id = session_id;
}

void fw112_ab_tick(const fw112_ab_input_t *in, uint32_t now_tick)
{
	if (in == 0) {
		R.prev_session_state = 0xFFU;
		R.ms_torque_pos = false;
		R.ms_mode_demand_pos = false;
		R.ms_iq_request_pos = false;
		R.ms_pre_ramp_pos = false;
		R.ms_setpoint_pos = false;
		return;
	}

	uint8_t st = in->session_state;
	bool active_now = (st == 1U);

	/* --- session edges ---------------------------------------------------------------- */
	if (R.prev_session_state != 0xFFU) {
		if (!active_now && R.prev_session_state == 1U) {
			/* Left ACTIVE: a REVOKED edge. Close any open episode, then open a new one. */
			if (R.fsm != FW112_AB_ST_IDLE) {
				close_episode();
			}
			open_episode(in, now_tick);
		} else if (active_now && R.prev_session_state != 1U) {
			/* Entered ACTIVE: a GRANTED edge. Only relevant to an open episode. */
			if (R.fsm == FW112_AB_ST_WAIT_GRANTED) {
				R.granted_tick = now_tick;
				R.fsm = FW112_AB_ST_RUNNING;
				/* Re-stamp every pre-grant milestone sample grant-relative (negative offset). */
				if (R.pre_grant_count > 0U && R.pre_grant_count <= R.q_depth) {
					for (uint8_t i = 0; i < R.pre_grant_count; i++) {
						uint8_t slot = (uint8_t)((R.q_head + R.q_depth - R.pre_grant_count + i) %
							FW112_AB_RECORDS);
						R.slots[slot].smp.tick_offset =
							(int16_t)(R.pre_grant_ticks[i] - R.granted_tick);
					}
				}
				R.pre_grant_count = 0U;
				append_sample(in, now_tick, (uint8_t)FW112_AB_MS_GRANTED, 0U);
			}
		}
	}

	/* --- the chain's stages turning positive (every active state; edges only) ---------- */
	if (R.fsm != FW112_AB_ST_IDLE) {
		bool tq_pos = in->torque_for_assist_mv > 0U;
		if (tq_pos && !R.ms_torque_pos) {
			append_sample(in, now_tick, (uint8_t)FW112_AB_MS_TORQUE_POSITIVE, 0U);
		}
		R.ms_torque_pos = tq_pos;

		bool md_pos = in->iq_request > 0;
		if (md_pos && !R.ms_mode_demand_pos) {
			append_sample(in, now_tick, (uint8_t)FW112_AB_MS_MODE_DEMAND_POSITIVE, 0U);
		}
		R.ms_mode_demand_pos = md_pos;

		bool iq_pos = in->iq_after_latch_floor > 0;
		if (iq_pos && !R.ms_iq_request_pos) {
			append_sample(in, now_tick, (uint8_t)FW112_AB_MS_IQ_REQUEST_POSITIVE, 0U);
		}
		R.ms_iq_request_pos = iq_pos;

		bool pre_pos = in->iq_pre_ramp > 0;
		if (pre_pos && !R.ms_pre_ramp_pos) {
			append_sample(in, now_tick, (uint8_t)FW112_AB_MS_PRE_RAMP_POSITIVE, 0U);
		}
		R.ms_pre_ramp_pos = pre_pos;

		bool setp_pos = in->iq_setpoint > 0;
		if (setp_pos && !R.ms_setpoint_pos) {
			/* Something reached the motor command - the restart is over. */
			append_sample(in, now_tick, (uint8_t)FW112_AB_MS_SETPOINT_POSITIVE, 0U);
			R.ms_setpoint_pos = true;
			close_episode();
		}
	}

	/* --- the schedule and window, once permission is back ------------------------------ */
	if (R.fsm == FW112_AB_ST_RUNNING) {
		uint32_t off = now_tick - R.granted_tick;
		while (R.schedule_idx < FW112_AB_N_SCHED && off >= sched_offsets[R.schedule_idx]) {
			append_sample(in, now_tick, (uint8_t)FW112_AB_MS_SCHEDULED,
				(uint8_t)(R.schedule_idx + 1U));
			R.schedule_idx++;
		}
		if (off > FW112_AB_WINDOW_TICKS) {
			close_episode();
		}
	} else if (R.fsm == FW112_AB_ST_WAIT_GRANTED) {
		if ((now_tick - R.open_tick) > FW112_AB_WINDOW_TICKS) {
			close_episode();
		}
	}

	R.prev_session_state = st;
}

/* --- diag_record_source bridge, same shape as fw112_diag's -------------------------------- */

static int32_t find_session_index(uint8_t session_id)
{
	for (uint8_t i = 0; i < R.q_depth; i++) {
		uint8_t idx = (uint8_t)((R.q_head + i) % FW112_AB_RECORDS);
		if (R.slots[idx].smp.session_id == session_id) return (int32_t)i;
	}
	return -1;
}

uint16_t fw112_ab_queue_count_session(uint8_t session_id)
{
	uint16_t n = 0;
	for (uint8_t i = 0; i < R.q_depth; i++) {
		uint8_t idx = (uint8_t)((R.q_head + i) % FW112_AB_RECORDS);
		if (R.slots[idx].smp.session_id == session_id) n++;
	}
	return n;
}

bool fw112_ab_queue_peek_session(uint8_t session_id, fw112_ab_record_t *out)
{
	if (out == 0) return false;
	int32_t off = find_session_index(session_id);
	if (off < 0) return false;
	*out = R.slots[(uint8_t)((R.q_head + (uint8_t)off) % FW112_AB_RECORDS)];
	return true;
}

void fw112_ab_queue_release_session(uint8_t session_id)
{
	int32_t off = find_session_index(session_id);
	if (off < 0) return;
	for (int32_t i = off; i > 0; i--) {
		uint8_t dst = (uint8_t)((R.q_head + (uint8_t)i) % FW112_AB_RECORDS);
		uint8_t src = (uint8_t)((R.q_head + (uint8_t)i - 1U) % FW112_AB_RECORDS);
		R.slots[dst] = R.slots[src];
	}
	R.q_head = (uint8_t)((R.q_head + 1U) % FW112_AB_RECORDS);
	R.q_depth--;
}

uint32_t fw112_ab_queue_enqueued(void) { return R.accepted_total; }
uint32_t fw112_ab_queue_rejected(void) { return R.rejected_total; }

#else  /* !CAN_DIAGNOSTICS_ENABLE */

/* FW-112 A/B: like every other diagnostic module, the recorder costs ZERO RAM in the production
 * build - see pas_raw.c for why this is a #if rather than a reliance on --gc-sections. */
typedef int fw112_ab_not_compiled_in;

#endif /* CAN_DIAGNOSTICS_ENABLE */
