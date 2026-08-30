#include "qs_transition_diag.h"
#include "diag_budget.h"
#include <string.h>

#if CAN_DIAGNOSTICS_ENABLE

enum { QS_ARMED = 0U, QS_CAPTURING = 1U, QS_COMPLETE = 2U };
static struct {
	qs_transition_sample_t slots[QS_TRANSITION_DIAG_SAMPLES];
	uint16_t write_pos, post_count, trigger_index;
	uint8_t state, generation, trigger_events;
	bool rearm_pending;
	bool primed, prev_run, prev_moe, prev_iq_ref_pos, prev_iq_requested_pos, prev_fault;
} R;

_Static_assert(sizeof(qs_transition_sample_t) == 44U, "QS-1 sample layout changed; update RAM evidence");
_Static_assert(sizeof(R) <= DIAG_BUDGET_QS_TRANSITION_BYTES, "QS-1 transition recorder exceeds RAM budget");

static uint8_t events_for(const qs_transition_input_t *in)
{
	uint8_t e = QS_EVT_NONE;
	bool iq_ref_pos = in->iq_ref != 0;
	bool iq_requested_pos = in->iq_requested != 0;
	if (in->run_request && !R.prev_run) e |= QS_EVT_RUN_RISE;
	if (in->moe && !R.prev_moe) e |= QS_EVT_MOE_RISE;
	if (iq_ref_pos && !R.prev_iq_ref_pos) e |= QS_EVT_FIRST_IQ_REF;
	if (!iq_requested_pos && R.prev_iq_requested_pos) e |= QS_EVT_IQ_TARGET_ZERO;
	if (!iq_ref_pos && R.prev_iq_ref_pos) e |= QS_EVT_IQ_REF_ZERO;
	if (!in->moe && R.prev_moe) e |= QS_EVT_MOE_FALL;
	if (in->fault_hard_off && !R.prev_fault) e |= QS_EVT_FAULT_HARD_OFF;
	R.prev_run = in->run_request; R.prev_moe = in->moe; R.prev_iq_ref_pos = iq_ref_pos;
	R.prev_iq_requested_pos = iq_requested_pos; R.prev_fault = in->fault_hard_off;
	return e;
}

void qs_transition_diag_init(void)
{
	memset(&R, 0, sizeof(R));
	/* Generation 1 preserves QS-1's original first-capture id. */
	R.generation = 1U;
}

void qs_transition_diag_fast_tick(const qs_transition_input_t *in)
{
	if (in == 0) return;
	if (R.rearm_pending) {
		uint8_t next = (uint8_t)(R.generation + 1U);
		memset(&R, 0, sizeof(R));
		R.generation = next;
	}
	if (R.state == QS_COMPLETE) return;
	if (!R.primed) { R.primed = true; (void)events_for(in); return; }
	uint8_t events = events_for(in);
	uint16_t pos = R.write_pos;
	qs_transition_sample_t *s = &R.slots[pos];
	s->foc_cycle=in->foc_cycle; s->iq_requested=in->iq_requested; s->iq_allowed=in->iq_allowed;
	s->iq_ref=in->iq_ref; s->iq_measured=in->iq_measured; s->id_ref=in->id_ref; s->id_measured=in->id_measured;
	s->theta_e_hi16=in->theta_e_hi16; s->ccr_a=in->ccr_a; s->ccr_b=in->ccr_b; s->ccr_c=in->ccr_c;
	s->lifecycle=in->lifecycle; s->hall=in->hall; s->sample_state=in->sample_state; s->events=events;
	s->flags=(in->run_request?QS_FLAG_RUN_REQUEST:0U)|(in->bridge_requested?QS_FLAG_BRIDGE_REQUESTED:0U)|
		(in->moe?QS_FLAG_MOE:0U)|(in->fault_hard_off?QS_FLAG_FAULT_HARD_OFF:0U)|
		(in->angle_static_legal?QS_FLAG_ANGLE_STATIC_LEGAL:0U);
	R.write_pos=(uint16_t)((pos+1U)%QS_TRANSITION_DIAG_SAMPLES);
	if (R.state == QS_ARMED && events != QS_EVT_NONE) {
		R.state=QS_CAPTURING; R.trigger_index=pos; R.post_count=0U; R.trigger_events=events;
	} else if (R.state == QS_CAPTURING) {
		R.post_count++;
		if (R.post_count >= QS_TRANSITION_DIAG_POST_SAMPLES) R.state=QS_COMPLETE;
	}
}

bool qs_transition_diag_is_complete(void) { return R.state == QS_COMPLETE; }
bool qs_transition_diag_sample_at(uint16_t i, qs_transition_sample_t *out)
{
	if (out == 0 || R.state != QS_COMPLETE || i >= QS_TRANSITION_DIAG_SAMPLES) return false;
	*out=R.slots[(uint16_t)((R.write_pos+i)%QS_TRANSITION_DIAG_SAMPLES)]; return true;
}
uint8_t qs_transition_diag_capture_id(void) { return R.generation; }
uint16_t qs_transition_diag_trigger_index(void) { return (uint16_t)((R.trigger_index + QS_TRANSITION_DIAG_SAMPLES - R.write_pos) % QS_TRANSITION_DIAG_SAMPLES); }
void qs_transition_diag_status(qs_transition_status_t *out)
{
	if (!out) return;
	out->state = (R.state == QS_COMPLETE) ? QS_TRANSITION_COMPLETE :
		(R.state == QS_CAPTURING) ? QS_TRANSITION_TRIGGERED : QS_TRANSITION_ARMED;
	out->generation = R.generation;
	out->sample_count = (R.state == QS_COMPLETE) ? QS_TRANSITION_DIAG_SAMPLES :
		(R.state == QS_CAPTURING) ? (uint8_t)(R.post_count + 1U) : 0U;
	out->trigger_events = R.trigger_events;
	out->trigger_index = qs_transition_diag_trigger_index();
	out->export_ready = R.state == QS_COMPLETE;
}
bool qs_transition_diag_request_new_capture(void)
{
	if (R.state != QS_COMPLETE || R.rearm_pending) return false;
	R.rearm_pending = true;
	return true;
}

#else
void qs_transition_diag_init(void) {}
void qs_transition_diag_fast_tick(const qs_transition_input_t *in) { (void)in; }
bool qs_transition_diag_is_complete(void) { return false; }
bool qs_transition_diag_sample_at(uint16_t i, qs_transition_sample_t *out) { (void)i; (void)out; return false; }
uint8_t qs_transition_diag_capture_id(void) { return 0U; }
uint16_t qs_transition_diag_trigger_index(void) { return 0U; }
void qs_transition_diag_status(qs_transition_status_t *out) { if(out) memset(out, 0, sizeof(*out)); }
bool qs_transition_diag_request_new_capture(void) { return false; }
#endif
