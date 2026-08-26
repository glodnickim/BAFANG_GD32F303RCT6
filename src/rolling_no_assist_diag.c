#include "rolling_no_assist_diag.h"
#include "diag_budget.h"

#if CAN_DIAGNOSTICS_ENABLE && ROLLING_NO_ASSIST_DIAG_ENABLE

#include <stddef.h>
#include <string.h>

/*
 * ROLLING NO-ASSIST DIAGNOSTIC: captures pre/post-trigger data when the rider is pedalling but
 * the motor provides no assist during rolling re-enable.
 *
 * Three trigger classes (exactly one fires per trigger):
 *   CASE A — PAS/RIDE_CONTROL: permission present, demand zero (latch not firing).
 *   CASE B — BRIDGE/SAFETY: demand exists but bridge not starting.
 *   CASE C — FOC/ROTOR: bridge ON but no motor current (iq_actual < iq_setpoint/4).
 *
 * FSM:
 *   IDLE      - rolling ring, sample every DECIMATION-th tick, looking for trigger confirmation.
 *   CAPTURING - trigger confirmed; keep writing, count post-trigger samples.
 *   FROZEN    - post window done; ring holds capture until reset/power cycle. Explicit CAN
 *               replays use a separate cursor and never drain this ring.
 *
 * All mutable state in the single struct R below. RAM budget measured against
 * DIAG_BUDGET_ROLLING_NO_ASSIST_DIAG_BYTES.
 */

#define RNA_FSM_IDLE      0U
#define RNA_FSM_CAPTURING 1U
#define RNA_FSM_FROZEN    2U

static struct {
	rolling_no_assist_sample_t slots[ROLLING_NO_ASSIST_DIAG_SAMPLES];
	uint8_t  fsm;
	uint16_t write_pos;
	uint16_t post_count;
	uint32_t trigger_tick;
	uint8_t  capture_id;
	uint8_t  trigger_case;
	uint8_t  session_id;
	bool     primed;
	uint16_t warm_samples;
	/* trigger confirmation */
	uint8_t  confirm_count;
	uint8_t  confirm_case;
	uint32_t accepted_total;
	uint32_t rejected_total;
} R;

_Static_assert(sizeof(R) <= DIAG_BUDGET_ROLLING_NO_ASSIST_DIAG_BYTES,
	"rolling_no_assist_diag: total state exceeds RAM line item");

_Static_assert(sizeof(rolling_no_assist_sample_t) == ROLLING_NO_ASSIST_SAMPLE_WIRE_BYTES,
	"rolling_no_assist_diag: schema v3 sample must stay exactly 48 B");
_Static_assert(offsetof(rolling_no_assist_sample_t, tick_abs) == 0U
	&& offsetof(rolling_no_assist_sample_t, hall) == 8U
	&& offsetof(rolling_no_assist_sample_t, iq_before_pu) == 12U
	&& offsetof(rolling_no_assist_sample_t, iq_after_latch_floor) == 16U
	&& offsetof(rolling_no_assist_sample_t, pi_q_int) == 24U
	&& offsetof(rolling_no_assist_sample_t, load_centikg) == 32U
	&& offsetof(rolling_no_assist_sample_t, rotor_direction) == 40U
	&& offsetof(rolling_no_assist_sample_t, motor_voltage_utilization) == 42U
	&& offsetof(rolling_no_assist_sample_t, pwm_cutoff_progress) == 44U
	&& offsetof(rolling_no_assist_sample_t, hall_timeout_progress) == 45U
	&& offsetof(rolling_no_assist_sample_t, reserved0) == 46U
	&& offsetof(rolling_no_assist_sample_t, reserved1) == 47U,
	"rolling_no_assist_diag: schema v3 field offsets changed");

static uint8_t classify_trigger(const rolling_no_assist_input_t *in)
{
	/* CASE C first: bridge ON, no motor current (most specific).
	 * MS.i_q is NEGATIVE during forward drive (MP.reverse=-1, Park transform convention).
	 * MS.i_q_setpoint is POSITIVE (0..700). Compare magnitudes: */
	if (in->iq_setpoint > 0 && in->pwm_on && in->moe &&
	    in->bridge_lifecycle >= 2U && in->current_cal_foc_allowed && in->current_feedback_valid) {
		int32_t threshold = (int32_t)in->iq_setpoint / 4;
		int32_t mag_iq = (in->iq_actual < 0) ? -(int32_t)in->iq_actual : (int32_t)in->iq_actual;
		if (mag_iq < threshold) return ROLLING_NO_ASSIST_CASE_C;
	}
	/* CASE B: demand exists, but a software/hardware/safety bridge fact is false. */
	if (in->iq_setpoint > 0 &&
	    (!in->pwm_on || !in->moe || in->bridge_lifecycle < 2U || !in->current_cal_foc_allowed)) {
		return ROLLING_NO_ASSIST_CASE_B;
	}
	/* CASE A: permission present, demand zero. */
	if (in->rider_latched && in->permission_present && in->load_met
	    && in->iq_setpoint == 0 && in->iq_before_pu == 0) {
		return ROLLING_NO_ASSIST_CASE_A;
	}
	return ROLLING_NO_ASSIST_CASE_NONE;
}

static uint8_t make_status_flags(const rolling_no_assist_input_t *in)
{
	uint8_t f = 0U;
	if (in->moe) f |= RNA_STATUS_HARDWARE_MOE;
	if (in->current_cal_foc_allowed) f |= RNA_STATUS_CURRENT_CAL_FOC_ALLOWED;
	if (in->neutral_dwell_active) f |= RNA_STATUS_NEUTRAL_DWELL_ACTIVE;
	if (in->pwm_cutoff_active) f |= RNA_STATUS_PWM_CUTOFF_ACTIVE;
	if (in->current_feedback_valid) f |= RNA_STATUS_CURRENT_FEEDBACK_VALID;
	return f;
}

static uint8_t make_flags(const rolling_no_assist_input_t *in, uint8_t case_id)
{
	uint8_t f = 0;
	if (case_id == ROLLING_NO_ASSIST_CASE_A) f |= RNA_FLAG_CASE_A;
	if (case_id == ROLLING_NO_ASSIST_CASE_B) f |= RNA_FLAG_CASE_B;
	if (case_id == ROLLING_NO_ASSIST_CASE_C) f |= RNA_FLAG_CASE_C;
	if (in->rider_latched && in->permission_present && in->load_met) f |= RNA_FLAG_RIDER_ACTIVE;
	if (in->permission_present) f |= RNA_FLAG_PERMISSION;
	if (in->load_met)          f |= RNA_FLAG_LOAD_MET;
	if (in->iq_setpoint == 0 && in->iq_before_pu == 0) f |= RNA_FLAG_NO_DEMAND;
	if (in->pwm_on)            f |= RNA_FLAG_PWM_ON;
	return f;
}

static void write_sample(const rolling_no_assist_input_t *in, uint32_t now_tick, uint8_t case_id)
{
	uint16_t pos = R.write_pos;
	rolling_no_assist_sample_t *s = &R.slots[pos];

	s->tick_abs = now_tick;
	s->flags = make_flags(in, case_id);
	s->status_flags = make_status_flags(in);
	s->case_id = case_id;
	s->bridge_lifecycle = in->bridge_lifecycle;
	s->hall = in->hall;
	s->permission_bits = in->permission_bits;
	s->reason_bits = in->reason_bits;
	s->debug_flags = in->debug_flags;
	s->iq_before_pu = in->iq_before_pu;
	s->iq_request = in->iq_request_raw;
	s->iq_after_latch_floor = in->iq_after_latch_floor;
	s->iq_pre_ramp = in->iq_pre_ramp;
	s->iq_setpoint = in->iq_setpoint;
	s->iq_actual = in->iq_actual;
	s->pi_q_int = in->pi_q_int;
	s->pi_d_int = in->pi_d_int;
	s->erps = in->erps;
	s->rpm = in->rpm;
	s->load_centikg = in->load_centikg;
	s->load_threshold = in->load_threshold;
	s->angle_hall = in->angle_hall;
	s->angle_absolute = in->angle_absolute;
	s->rotor_direction = in->rotor_direction;
	s->neutral_dwell_counter = in->neutral_dwell_counter;
	s->motor_voltage_utilization = in->motor_voltage_utilization;
	/* FW-122.1: saturating compaction of two u16 counters into one wire byte each. Neither
	 * counter is ever negative or signed, so plain clamp-at-max is exact for every value the
	 * firmware can produce today (pwm_cutoff_tick is bounded at SOFT_CUTOFF_TICKS=40 by
	 * production code) and degrades gracefully (saturates, does not wrap) if either bound ever
	 * changes. */
	s->pwm_cutoff_progress = (in->pwm_cutoff_tick > 255U) ? 255U : (uint8_t)in->pwm_cutoff_tick;
	{
		uint16_t scaled = (uint16_t)(in->half_rotation_counter >> 4);
		s->hall_timeout_progress = (scaled > 255U) ? 255U : (uint8_t)scaled;
	}
	s->reserved0 = 0U;
	s->reserved1 = 0U;

	R.write_pos = (uint16_t)((pos + 1U) % ROLLING_NO_ASSIST_DIAG_SAMPLES);
	if (R.warm_samples < ROLLING_NO_ASSIST_DIAG_SAMPLES) R.warm_samples++;
	if (R.fsm == RNA_FSM_CAPTURING) {
		R.post_count++;
		if (R.post_count >= ROLLING_NO_ASSIST_DIAG_POST_SAMPLES) {
			R.fsm = RNA_FSM_FROZEN;
			R.accepted_total++;
		}
	}
}

void rolling_no_assist_diag_init(void)
{
	memset(&R, 0, sizeof(R));
}

void rolling_no_assist_diag_set_session_id(uint8_t session_id)
{
	R.session_id = session_id;
}

void rolling_no_assist_diag_tick(const rolling_no_assist_input_t *in, uint32_t now_tick)
{
	if (in == 0) {
		R.primed = false;
		return;
	}
	/* Confirmation and storage share the same 250 Hz sample clock. The previous
	 * implementation incremented confirmation at 4 kHz and only decimated writes,
	 * turning the documented 8 samples / 32 ms into ~2 ms. */
	if ((now_tick % ROLLING_NO_ASSIST_DIAG_DECIMATION) != 0U) return;

	if (!R.primed) {
		R.primed = true;
		R.confirm_count = 0;
		R.confirm_case = ROLLING_NO_ASSIST_CASE_NONE;
		return;
	}

	if (R.fsm == RNA_FSM_FROZEN) {
		/* A capture is queued and undrained - a new trigger cannot be stored. Count it. */
		uint8_t this_case = classify_trigger(in);
		if (this_case != ROLLING_NO_ASSIST_CASE_NONE) R.rejected_total++;
		return;
	}

	/* Classify this tick. */
	uint8_t this_case = classify_trigger(in);

	/* Confirmation counter: count consecutive ticks of the same case. */
	if (this_case != ROLLING_NO_ASSIST_CASE_NONE && this_case == R.confirm_case) {
		if (R.confirm_count < ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT) R.confirm_count++;
	} else {
		R.confirm_case = this_case;
		R.confirm_count = (this_case != ROLLING_NO_ASSIST_CASE_NONE) ? 1U : 0U;
	}

	/* Open capture once confirmation threshold met (ring must be warm). */
	if (R.confirm_count >= ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT
	    && R.fsm == RNA_FSM_IDLE
	    && R.warm_samples >= ROLLING_NO_ASSIST_DIAG_SAMPLES) {
		R.trigger_tick = now_tick;
		R.capture_id = (uint8_t)(R.capture_id + 1U);
		R.trigger_case = R.confirm_case;
		R.fsm = RNA_FSM_CAPTURING;
		R.post_count = 0U;
	}

	write_sample(in, now_tick, this_case);
}

bool rolling_no_assist_diag_is_frozen(void)
{
	return R.fsm == RNA_FSM_FROZEN;
}

uint16_t rolling_no_assist_diag_capture_sample_count(void)
{
	return rolling_no_assist_diag_is_frozen() ? ROLLING_NO_ASSIST_DIAG_SAMPLES : 0U;
}

bool rolling_no_assist_diag_capture_sample_at(uint16_t sample_index, rolling_no_assist_sample_t *out)
{
	if (out == 0 || !rolling_no_assist_diag_is_frozen() ||
	    sample_index >= ROLLING_NO_ASSIST_DIAG_SAMPLES) return false;
	uint16_t idx = (uint16_t)((R.write_pos + sample_index) % ROLLING_NO_ASSIST_DIAG_SAMPLES);
	*out = R.slots[idx];
	return true;
}

uint8_t rolling_no_assist_diag_capture_session_id(void)
{
	return R.session_id;
}

uint32_t rolling_no_assist_diag_queue_enqueued(void) { return R.accepted_total; }
uint32_t rolling_no_assist_diag_queue_rejected(void)  { return R.rejected_total; }

uint8_t rolling_no_assist_diag_capture_id(void)  { return R.capture_id; }
uint8_t rolling_no_assist_diag_trigger_case(void) { return R.trigger_case; }

static void put_u16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)(value & 0xFFU);
}

static void put_i16(uint8_t *data, int16_t value)
{
	put_u16(data, (uint16_t)value);
}

bool rolling_no_assist_diag_encode_fragment(
	const rolling_no_assist_sample_t *s,
	uint8_t session_id,
	uint8_t capture_id,
	uint8_t trigger_case,
	uint16_t fragment,
	uint32_t *efid,
	uint8_t data[8],
	bool *last)
{
	if (s == 0 || efid == 0 || data == 0 || last == 0 ||
	    fragment >= ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE) return false;
	for (uint8_t i = 0U; i < 8U; i++) data[i] = 0U;
	*last = (fragment + 1U) >= ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE;

	if (fragment == 0U) {
		*efid = ROLLING_NO_ASSIST_EFID_HEADER;
		data[0] = ROLLING_NO_ASSIST_DIAG_SCHEMA_VERSION;
		data[1] = session_id;
		data[2] = ROLLING_NO_ASSIST_DATA_FRAGMENTS;
		data[3] = capture_id;
		data[4] = trigger_case;
		data[5] = ROLLING_NO_ASSIST_SAMPLE_WIRE_BYTES;
		data[6] = ROLLING_NO_ASSIST_DIAG_SAMPLE_HZ;
		data[7] = ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT;
		return true;
	}

	*efid = ROLLING_NO_ASSIST_EFID_DATA_BASE + (uint32_t)(fragment - 1U);
	switch (fragment) {
	case 1U:
		data[0] = (uint8_t)(s->tick_abs >> 24);
		data[1] = (uint8_t)(s->tick_abs >> 16);
		data[2] = (uint8_t)(s->tick_abs >> 8);
		data[3] = (uint8_t)(s->tick_abs & 0xFFU);
		data[4] = s->flags;
		data[5] = s->status_flags;
		data[6] = s->case_id;
		data[7] = s->bridge_lifecycle;
		break;
	case 2U:
		data[0] = s->hall;
		data[1] = s->permission_bits;
		data[2] = s->reason_bits;
		data[3] = s->debug_flags;
		put_i16(&data[4], s->iq_before_pu);
		put_i16(&data[6], s->iq_request);
		break;
	case 3U:
		put_i16(&data[0], s->iq_after_latch_floor);
		put_i16(&data[2], s->iq_pre_ramp);
		put_i16(&data[4], s->iq_setpoint);
		put_i16(&data[6], s->iq_actual);
		break;
	case 4U:
		put_i16(&data[0], s->pi_q_int);
		put_i16(&data[2], s->pi_d_int);
		put_u16(&data[4], s->erps);
		put_i16(&data[6], s->rpm);
		break;
	case 5U:
		put_u16(&data[0], s->load_centikg);
		put_u16(&data[2], s->load_threshold);
		put_u16(&data[4], s->angle_hall);
		put_u16(&data[6], s->angle_absolute);
		break;
	default: /* fragment 6 */
		data[0] = (uint8_t)s->rotor_direction;
		data[1] = s->neutral_dwell_counter;
		put_u16(&data[2], s->motor_voltage_utilization);
		data[4] = s->pwm_cutoff_progress;      /* v3 */
		data[5] = s->hall_timeout_progress;    /* v3 */
		/* data[6..7] remains explicit zero wire padding (was data[4..7] in schema v2). */
		break;
	}
	return true;
}

#else  /* !CAN_DIAGNOSTICS_ENABLE */

typedef int rolling_no_assist_diag_not_compiled_in;

#endif /* CAN_DIAGNOSTICS_ENABLE */
