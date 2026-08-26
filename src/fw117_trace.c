#include "fw117_trace.h"
#include "config.h"
#include "diag_budget.h"

#if CAN_DIAGNOSTICS_ENABLE && FW117_TRACE_ENABLE

#include <string.h>

/*
 * FW-117 (TEMPORARY): bridge lifecycle trace - see inc/fw117_trace.h for the record layout, the
 * RAM budget note, and the FW-117.1 first-observation/warm-up/trigger-selector notes. This
 * comment covers implementation only.
 *
 * All mutable state lives in the single struct R below so the RAM budget check measures the whole
 * module, and the module is driven entirely through fw117_trace_input_t (main.c reads the TIMER
 * registers and the MS/PI globals and passes values - nothing here links against main.c).
 *
 * FSM:
 *   IDLE      - rolling ring, sample every FW117_TRACE_DECIMATION-th tick, watching for the one
 *               edge FW117_TRACE_TRIGGER_EVENT selects (the other edge is watched for nothing -
 *               it is computed for motor_state_of()'s STARTING/RUNNING classification but never
 *               tested as a trigger).
 *   CAPTURING - the selected edge latched; keep writing, count post-trigger samples.
 *   FROZEN    - post window done; the ring holds the capture and waits for the dump to drain it.
 *
 * Trigger edges: bridge-on (pwm_on 0->1, the START event) and bridge-off (MOE 1->0, the STOP
 * event) are both DETECTED every tick, but only the one FW117_TRACE_TRIGGER_EVENT names can open
 * a capture. Only the first qualifying trigger after a drain (and only once the ring is warm -
 * see FW-117.1 WARM-UP in the header) opens one; a qualifying edge while FROZEN is refused. A
 * non-qualifying edge is never refused - "refused" specifically means "the edge this build
 * captures happened, but a frozen capture was still queued".
 */

#define FW117_FSM_IDLE      0U
#define FW117_FSM_CAPTURING 1U
#define FW117_FSM_FROZEN    2U

/* The module's STARTING/RUNNING boundary: 250 ms of continuous positive Iq demand. */
#define FW117_STARTING_TICKS 250U

/* FW-117.1: which of the two detected edges this build treats as the trigger - see
 * FW117_TRACE_TRIGGER_EVENT in inc/fw117_trace.h. The other edge is still detected (motor_state_
 * of() uses start_edge either way) but never tested against the FSM below. */
#if FW117_TRACE_TRIGGER_EVENT
#define FW117_TRIGGER_EVENT_TYPE FW117_EVENT_STOP
#else
#define FW117_TRIGGER_EVENT_TYPE FW117_EVENT_START
#endif

static struct {
	fw117_trace_sample_t slots[FW117_TRACE_SAMPLES];
	uint8_t  fsm;            /* FW117_FSM_* */
	uint16_t write_pos;      /* next ring slot to write (mod FW117_TRACE_SAMPLES) */
	uint16_t post_count;     /* samples written since the trigger sample */
	uint16_t dump_pos;       /* samples of the frozen capture already handed to the dump */
	uint32_t trigger_tick;   /* absolute tick of the trigger sample */
	uint8_t  capture_id;     /* monotonic capture counter (wraps at 256 - a key, never a count) */
	uint8_t  event_type;     /* FW117_EVENT_* of the open/frozen capture */
	uint16_t decim;          /* decimation counter (0 .. FW117_TRACE_DECIMATION-1) */
	uint8_t  session_id;     /* stamped into the capture at the trigger tick */
	uint8_t  prev_pwm_on;    /* previous tick's pwm_on (edge detection); meaningless until primed */
	uint8_t  prev_moe;       /* previous tick's moe (edge detection); meaningless until primed */
	bool     primed;         /* FW-117.1: false until one tick() call has recorded prev_pwm_on/
	                           * prev_moe - that first call ONLY records them; it never evaluates
	                           * an edge, so no false START/STOP is possible on it regardless of
	                           * the initial MOE/PWM state. See fw117_trace_tick(). */
	uint16_t warm_samples;   /* FW-117.1: real samples written since init, saturating at
	                           * FW117_TRACE_SAMPLES. A trigger only opens a capture once this
	                           * reaches FW117_TRACE_SAMPLES, so a dumped capture's pre-trigger
	                           * window is always real data, never a memset-zeroed unwritten
	                           * slot. One-time latch: it is never reset by a drain/re-arm, only
	                           * by fw117_trace_init(), because the ring itself is never zeroed
	                           * again after its first full pass - every slot always holds real
	                           * (if possibly stale) data from then on. */
	uint8_t  running_ticks;  /* consecutive samples with pwm_on (STARTING vs RUNNING) */
	uint32_t accepted_total; /* captures committed to the queue (frozen) */
	uint32_t rejected_total; /* captures refused because a frozen capture was still queued */
} R;

_Static_assert(sizeof(R) <= DIAG_BUDGET_FW117_TRACE_BYTES,
	"FW-117: fw117_trace's total state exceeds its RAM line item");

_Static_assert(sizeof(fw117_trace_sample_t) == 48U,
	"FW-126: the compact start-trace sample must stay exactly 48 B");

/* The module's view of the bridge/FOC state, derived from the inputs only. */
static uint8_t motor_state_of(const fw117_trace_input_t *in, bool start_edge)
{
	if (in->pwm_on && in->cutoff_active) return FW117_STATE_SOFT_OFF;
	if (!in->pwm_on)                     return FW117_STATE_IDLE;
	if (!in->moe)                        return FW117_STATE_FAULT;
	if (start_edge)                      R.running_ticks = 0U;
	if (in->iq_setpoint == 0)            return FW117_STATE_COASTING;
	if (R.running_ticks < FW117_STARTING_TICKS) return FW117_STATE_STARTING;
	return FW117_STATE_RUNNING;
}

static void write_sample(const fw117_trace_input_t *in, uint32_t now_tick, uint8_t state)
{
	uint16_t pos = R.write_pos;
	fw117_trace_sample_t *s = &R.slots[pos];

	s->tick_abs = now_tick;
	s->flags = (uint8_t)(
		(in->moe  ? FW117_FLAG_MOE      : 0U) |
		(in->cen  ? FW117_FLAG_CEN      : 0U) |
		(in->pwm_on ? FW117_FLAG_PWM_ON : 0U) |
		(in->cutoff_active ? FW117_FLAG_CUTOFF_ACT : 0U) |
		(in->reverse ? FW117_FLAG_REVERSE : 0U) |
		(in->current_fresh ? FW117_FLAG_CURRENT_FRESH : 0U) |
		(in->current_valid ? FW117_FLAG_CURRENT_VALID : 0U) |
		((R.fsm == FW117_FSM_CAPTURING && R.post_count == 0U) ? FW117_FLAG_START_CAP : 0U));
	s->state_and_hall = (uint8_t)((state & 0x07U) | ((in->hall & 0x07U) << 3)
		| (in->neutral_dwell_active ? FW117_STATE_DWELL_BIT : 0U));
	s->pi_q_int = in->pi_q_int;
	s->pi_d_int = in->pi_d_int;
	s->iq_request = in->iq_request;
	s->iq_setpoint = in->iq_setpoint;
	s->iq_meas = in->iq_meas;
	s->id_meas = in->id_meas;
	s->vq = in->vq;
	s->vd = in->vd;
	s->angle = in->angle;
	s->ccr1 = in->ccr1;
	s->ccr2 = in->ccr2;
	s->ccr3 = in->ccr3;
	s->phase_a = in->phase_a;
	s->phase_b = in->phase_b;
	s->phase_c = in->phase_c;
	s->adc_ch3 = in->adc_ch3;
	s->current_seq = in->current_seq;
	s->current_age = in->current_age;
	s->u_abs = in->u_abs;
	s->bridge_lifecycle = in->bridge_lifecycle;
	s->reserved = 0U;

	R.write_pos = (uint16_t)((pos + 1U) % FW117_TRACE_SAMPLES);
	if (R.warm_samples < FW117_TRACE_SAMPLES) R.warm_samples++;  /* FW-117.1: one more real sample */
	if (R.fsm == FW117_FSM_CAPTURING) {
		R.post_count++;
		if (R.post_count >= FW117_TRACE_POST_SAMPLES) {
			R.fsm = FW117_FSM_FROZEN;
			R.accepted_total++;
		}
	}
}

void fw117_trace_init(void)
{
	/* FW-117.1: primed and warm_samples both start at 0/false from the memset - no sentinel
	 * value needed any more. primed=false means the first tick() call below only records the
	 * starting pwm_on/moe (see there); warm_samples=0 means no trigger can open a capture until
	 * FW117_TRACE_SAMPLES real samples have been written. */
	memset(&R, 0, sizeof(R));
}

void fw117_trace_set_session_id(uint8_t session_id)
{
	R.session_id = session_id;
}

void fw117_trace_tick(const fw117_trace_input_t *in, uint32_t now_tick)
{
	if (in == 0) {
		/* FW-117.1: re-prime rather than poking a sentinel - the next real call will just
		 * record its state with no edge evaluated, same as the very first call ever. */
		R.primed = false;
		return;
	}

	if (!R.primed) {
		/* FW-117.1: THE FIRST OBSERVATION. Only the starting pwm_on/moe are recorded - no edge
		 * is evaluated, no sample is written, no capture can open, regardless of whether MOE/
		 * PWM read on or off right now. Before this fix, prev_pwm_on/prev_moe started at a
		 * sentinel 0xFF that is truthy in the boolean edge tests below, so a first call with
		 * MOE=0 (the ordinary case: the bridge is off before a ride starts) satisfied
		 * `!in->moe && R.prev_moe` and opened a false STOP capture. */
		R.prev_pwm_on = in->pwm_on ? 1U : 0U;
		R.prev_moe = in->moe ? 1U : 0U;
		R.primed = true;
		return;
	}

	/* Edge detection happens on every call; sampling happens every DECIMATION-th call. */
	bool start_edge = in->pwm_on && !R.prev_pwm_on;
	bool stop_edge  = !in->moe && R.prev_moe;
	R.prev_pwm_on = in->pwm_on ? 1U : 0U;
	R.prev_moe = in->moe ? 1U : 0U;

	/* FW-117.1: only the edge FW117_TRACE_TRIGGER_EVENT selects is a trigger candidate. The
	 * other edge is not tested here at all - it neither opens a capture nor counts as refused.
	 * (The plain #if here, rather than a macro that drops the unused parameter, keeps whichever
	 * of start_edge/stop_edge this build does NOT use referenced in the source text, so it is
	 * never an -Wunused-variable error in either build.) */
#if FW117_TRACE_TRIGGER_EVENT
	bool trigger_edge = stop_edge;
#else
	bool trigger_edge = start_edge;
	(void)stop_edge;
#endif

	if (R.fsm == FW117_FSM_FROZEN) {
		/* A capture is queued and undrained - a new trigger edge cannot be stored. Refuse it. */
		if (trigger_edge) R.rejected_total++;
		return;
	}

	/* FW-117.1: a trigger only opens a capture once the ring is warm (see `warm_samples`) - an
	 * early edge is silently ignored so a dumped capture's pre-trigger window is always real
	 * samples, never a memset-zeroed slot nothing has written yet. */
	if (trigger_edge && R.fsm == FW117_FSM_IDLE && R.warm_samples >= FW117_TRACE_SAMPLES) {
		R.trigger_tick = now_tick;
		R.capture_id = (uint8_t)(R.capture_id + 1U);
		R.event_type = FW117_TRIGGER_EVENT_TYPE;
		R.fsm = FW117_FSM_CAPTURING;
		R.post_count = 0U;
		R.running_ticks = 0U;
	}

	R.decim = (uint16_t)((R.decim + 1U) % FW117_TRACE_DECIMATION);
	if (R.decim != 0U) return;   /* not a sample tick */

	uint8_t state = motor_state_of(in, start_edge);
	if (in->pwm_on && R.running_ticks < FW117_STARTING_TICKS) {
		R.running_ticks++;
	}
	write_sample(in, now_tick, state);
}

/* --- diag_record_source bridge, same shape as fw112_ab's ------------------------------------ */

uint16_t fw117_trace_queue_count_session(uint8_t session_id)
{
	if (R.fsm != FW117_FSM_FROZEN || R.session_id != session_id) return 0U;
	return (uint16_t)(FW117_TRACE_SAMPLES - R.dump_pos);
}

bool fw117_trace_queue_peek_session(uint8_t session_id, fw117_trace_sample_t *out)
{
	if (out == 0) return false;
	if (R.fsm != FW117_FSM_FROZEN || R.session_id != session_id) return false;
	if (R.dump_pos >= FW117_TRACE_SAMPLES) return false;
	/* The capture is the SAMPLES slots ending at (write_pos-1); chronological order starts at
	 * write_pos (mod SAMPLES). */
	uint16_t idx = (uint16_t)((R.write_pos + R.dump_pos) % FW117_TRACE_SAMPLES);
	*out = R.slots[idx];
	return true;
}

void fw117_trace_queue_release_session(uint8_t session_id)
{
	if (R.fsm != FW117_FSM_FROZEN || R.session_id != session_id) return;
	R.dump_pos++;
	if (R.dump_pos >= FW117_TRACE_SAMPLES) {
		/* Fully drained: re-arm the ring. write_pos already points at the next slot to write
		 * (it has not advanced since the freeze), so rolling restarts by overwriting the
		 * just-dumped capture.
		 *
		 * FW-117.1: clear THIS capture's trigger-sample (FW117_FLAG_START_CAP) slot before
		 * re-arming. The next trigger can fire again well before the ring has rolled all the
		 * way around (nothing requires a fresh warm-up between captures - see `warm_samples`),
		 * so that slot is not guaranteed to be overwritten before the NEXT capture freezes. Left
		 * alone, its stale flag would surface a second time inside that next capture's dump - a
		 * false extra "this is the trigger sample" marker a consumer (the decoder) would
		 * otherwise take at face value. The slot's exact position is deterministic: the trigger
		 * write is always exactly FW117_TRACE_POST_SAMPLES writes before the write_pos this
		 * capture froze at (write_sample() freezes on the POST_SAMPLES-th write since
		 * CAPTURING opened, no more, no less). */
		uint16_t trigger_pos = (uint16_t)((R.write_pos + FW117_TRACE_SAMPLES - FW117_TRACE_POST_SAMPLES)
			% FW117_TRACE_SAMPLES);
		R.slots[trigger_pos].flags &= (uint8_t)~FW117_FLAG_START_CAP;
		R.dump_pos = 0U;
		R.fsm = FW117_FSM_IDLE;
	}
}

uint32_t fw117_trace_queue_enqueued(void) { return R.accepted_total; }
uint32_t fw117_trace_queue_rejected(void) { return R.rejected_total; }

uint8_t fw117_trace_capture_id(void)   { return R.capture_id; }
uint8_t fw117_trace_event_type(void)   { return R.event_type; }

#else  /* !CAN_DIAGNOSTICS_ENABLE || !FW117_TRACE_ENABLE */

/* FW-117: like every other diagnostic module, the recorder costs ZERO RAM in the production
 * build - see pas_raw.c for why this is a #if rather than a reliance on --gc-sections. */
typedef int fw117_trace_not_compiled_in;

#endif /* CAN_DIAGNOSTICS_ENABLE */
