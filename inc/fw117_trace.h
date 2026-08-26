#ifndef FW117_TRACE_H_
#define FW117_TRACE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * FW-126 FOC START TRACE, implemented on the existing FW-117 recorder transport. It observes
 * the normal bridge start and current-acquisition facts; it changes no FOC, PWM, ADC trigger,
 * or torque decision.
 *
 * WHY. The stock M820 keys the bridge off the internal torque ramp and keeps the outputs driven
 * to defined idle states through OSSR/OSSI; our lifecycle matches the ORDER (neutral CCR before
 * MOE, ramp to zero before MOE off, CEN never touched) but OSSR/OSSI were DISABLE (high-Z) and the
 * bridge was enabled from noise-level demand. This module records the real bridge/FOC state
 * around every bridge-on (START) and bridge-off (STOP) edge so the bench can verify the fix:
 * no CCR/Vq jump at MOE ON/OFF, no inverted Iq impulse, CCR equal before MOE ON, ramp 0 before
 * MOE OFF, CEN never disabled, and the PI integrals' behaviour at both edges.
 *
 * SAMPLING. One sample per 4th control tick (FW117_TRACE_DECIMATION = 4 -> 1 kHz from the 4 kHz
 * control tick) into a 480 x 32 B ring. The ring rolls continuously; on the first trigger edge
 * (bridge-on 0->1 = START, or MOE 1->0 = STOP) it keeps recording for FW117_TRACE_POST_SAMPLES
 * samples (240 ms) and then freezes. The frozen ring therefore holds 240 ms pre-trigger + 240 ms
 * post-trigger. The full 500 ms windows requested on the card do not fit the ~16 KB SRAM left in
 * this build; 240+240 at 1 kHz captures the edge transient (the click lives in the first few ms)
 * and the immediate ramp/decay, which is the diagnostic content that matters. RAM budget
 * (temporary, see inc/diag_budget.h): 480 x 32 B + bookkeeping.
 *
 * QUEUE. The frozen capture is handed to the dump layer as FW117_TRACE_SAMPLES queued records
 * (one per sample, header + 4 data fragments each), keyed by the session id stamped at the
 * trigger tick, exactly like fw112_ab's per-session queue. A new edge while a capture is frozen
 * is refused (rejected_total -> DIAG_TRAILER_F_FW117_REJECTED); re-arm happens only when the
 * dump has drained the frozen capture. Dump pacing (1 frame / 10 ms) means one capture takes
 * about 24 s to drain: on the bench, do one event, wait for the dump, then the next event.
 *
 * The module is driven entirely through fw117_trace_input_t - the bridge/FOC state arrives as
 * plain scalars (main.c reads the TIMER registers and the MS/PI globals and passes values), so
 * nothing here links against main.c or the motor core (same discipline as fw112_ab.c).
 *
 * BUILD GATING. Everything below - the 15 KB ring included - is compiled ONLY when
 * CAN_DIAGNOSTICS_ENABLE=1 (see the #if at the top of src/fw117_trace.c, and main.c's tick site,
 * dump bridge and init call, all inside the same guard). In the production build
 * (CAN_DIAGNOSTICS_ENABLE=0) this module contributes ZERO bytes of RAM and no code; only these
 * declarations remain, and declarations allocate nothing.
 *
 * The diagnostic image is a BENCH image, not a riding one. It carries ~33 KB of recorder RAM on
 * a 48 KB part and floods the bus while dumping. Ride the normal build; flash the diagnostic one
 * only to capture a trace, and separately from the FW117_BRIDGE_TIMING_TEST flag in config.h -
 * that flag is a different experiment and combining the two reproduces exactly the mixed-change
 * problem this card exists to undo. CAN_DIAGNOSTICS_ENABLE=1 together with
 * FW117_BRIDGE_TIMING_TEST=1 is refused by a compile-time #error in config.h for the same
 * reason - see the check there.
 *
 * FW-117.1: FIRST OBSERVATION. fw117_trace_tick()'s first call after fw117_trace_init() (or
 * after an `in == 0` reset call) only records the starting pwm_on/moe - it never evaluates an
 * edge, so it can never open a false capture no matter what MOE/PWM read on that first call.
 * Edge detection begins on the SECOND call onward, once there is a real previous sample to
 * compare against. See the `primed` field in src/fw117_trace.c.
 *
 * FW-117.1: WARM-UP. A trigger can only OPEN a capture once the ring holds
 * FW117_TRACE_SAMPLES samples of real (written) history - so every dumped capture's pre-trigger
 * window is real data, never a slot fw117_trace_init()'s memset zeroed and nothing has written
 * since. A trigger edge that arrives before the ring is warm is silently ignored: it neither
 * opens a capture nor counts against rejected_total (that counter means "a frozen capture was
 * still queued", a different condition). Warm-up is a one-time latch - once the ring has been
 * fully written once, every slot holds real (if possibly stale) data forever after, so later
 * captures never need to wait again. See `warm_samples` in src/fw117_trace.c.
 */

/*
 * FW-117.1: WHICH EDGE IS THE TRIGGER. The card asked for two SEPARATE bench images - one that
 * captures the START edge and one that captures the STOP edge - rather than one image racing
 * both, so a capture can never be ambiguous about which edge produced it.
 * FW117_TRACE_TRIGGER_EVENT selects which edge arms a capture; the OTHER edge is ignored
 * entirely by fw117_trace_tick() - it neither opens a capture nor counts against
 * rejected_total. Set ONLY on the compiler command line, e.g. -DFW117_TRACE_TRIGGER_EVENT=1;
 * deliberately not exposed as a build-script switch, for the same reason
 * FW117_BRIDGE_TIMING_TEST in config.h is not one - an unintended default must never silently
 * pick which edge a bench session is capturing.
 *   0 = START (bridge-on, pwm_on 0->1)   - the default when the macro is left undefined.
 *   1 = STOP  (bridge-off, MOE 1->0)
 */
#ifndef FW117_TRACE_TRIGGER_EVENT
#define FW117_TRACE_TRIGGER_EVENT 0
#endif
#if (FW117_TRACE_TRIGGER_EVENT != 0) && (FW117_TRACE_TRIGGER_EVENT != 1)
#error "FW117_TRACE_TRIGGER_EVENT must be 0 (START) or 1 (STOP)"
#endif

#define FW117_TRACE_SCHEMA_VERSION 2U

/* 70 x 48 B = 3360 B. At 250 Hz this retains 72 ms pre + 208 ms post start evidence. */
#define FW117_TRACE_SAMPLES       70U
#define FW117_TRACE_DECIMATION    16U     /* sample every 16th control tick = 250 Hz */
#define FW117_TRACE_POST_SAMPLES  52U     /* 208 ms after a captured start */

/* Record wire frames (0x10234/0x10235..0x1023A are free; fw112_ab ends at 0x10233). */
#define FW117_TRACE_EFID_HEADER    0x00010234U  /* one per sample: schema/session/capture/event */
#define FW117_TRACE_EFID_DATA_BASE 0x00010235U  /* +0..+5: 6 x 8 B sample fragments */
#define FW117_TRACE_DATA_FRAGMENTS 6U

/* Trigger event types (carried in the header frame). */
#define FW117_EVENT_START 0U   /* bridge-on edge: ui_8_PWM_ON_Flag 0->1 (with MOE=1) */
#define FW117_EVENT_STOP  1U   /* bridge-off edge: MOE 1->0 */

/* Motor state machine (bits 0-2 of the state_and_hall byte). */
#define FW117_STATE_IDLE     0U   /* bridge off, no soft-cutoff in progress */
#define FW117_STATE_STARTING 1U   /* bridge on, Iq demand positive, within the first 250 ms */
#define FW117_STATE_RUNNING  2U   /* bridge on, Iq demand positive, established */
#define FW117_STATE_COASTING 3U   /* bridge on, Iq demand zero (FOC holding neutral) */
#define FW117_STATE_SOFT_OFF 4U   /* soft-cutoff active: CCR ramp to neutral before MOE off */
#define FW117_STATE_FAULT    5U   /* bridge claimed on but MOE off (transient or fault) */

/* Sample flags byte. */
#define FW117_FLAG_MOE         0x01U  /* TIMER0 primary output enable (POEN/BDTR bit 15) */
#define FW117_FLAG_CEN         0x02U  /* TIMER0 counter enable (CTL0 bit 0) */
#define FW117_FLAG_PWM_ON      0x04U  /* ui_8_PWM_ON_Flag (FOC running) */
#define FW117_FLAG_CUTOFF_ACT  0x08U  /* soft-cutoff ramp in progress */
#define FW117_FLAG_START_CAP   0x10U  /* THIS sample is the trigger sample (offset 0) */
#define FW117_FLAG_REVERSE     0x20U  /* i8_reverse_flag */
#define FW117_FLAG_CURRENT_FRESH 0x40U /* ADC1 EOIC proved a new injected conversion */
#define FW117_FLAG_CURRENT_VALID 0x80U /* validity is never inferred from EOIC */
#define FW117_STATE_DWELL_BIT  0x40U  /* state_and_hall bit: neutral dwell active */

/* One queued/ring sample - exactly 48 B, no padding (asserted in fw117_trace.c). */
typedef struct {
	uint32_t tick_abs;      /* absolute control tick (CONTROL_TIMEBASE_HZ) */
	uint32_t current_seq;   /* current conversion sequence (freshness proof) */
	uint8_t  flags;         /* FW117_FLAG_* */
	uint8_t  state_and_hall;/* bits 0-2 = FW117_STATE_*, bits 3-5 = hall state (0..7) */
	int16_t  pi_q_int;      /* PI_iq.integral_part, i16-clamped (float source) */
	int16_t  pi_d_int;      /* PI_id.integral_part, i16-clamped */
	int16_t  iq_request;    /* assist_modes demand (iq_request) */
	int16_t  iq_setpoint;   /* final commanded current reaching the PI (ramp / Iq_ref) */
	int16_t  iq_meas;       /* measured MS.i_q */
	int16_t  id_meas;       /* measured MS.i_d (id_ref is 0 in normal FOC - omitted) */
	int16_t  vq;            /* MS.u_q (SVPWM voltage output) */
	int16_t  vd;            /* MS.u_d */
	uint16_t angle;         /* electrical angle: q31_rotorposition_absolute >> 16 (0..65535) */
	uint16_t ccr1;          /* TIMER_CH0CV (channel 0 compare value) */
	uint16_t ccr2;          /* TIMER_CH1CV */
	uint16_t ccr3;          /* TIMER_CH2CV */
	int16_t  phase_a;       /* corrected/reconstructed phase A current used by FOC */
	int16_t  phase_b;       /* corrected/reconstructed phase B current used by FOC */
	int16_t  phase_c;       /* corrected/reconstructed phase C current used by FOC */
	uint16_t adc_ch3;       /* TIMER0 CH3 for the next injected conversion */
	uint16_t current_age;   /* feedback age state */
	uint16_t u_abs;         /* voltage-vector magnitude */
	uint8_t  bridge_lifecycle;
	uint8_t  reserved;      /* explicit wire padding */
} fw117_trace_sample_t;

typedef struct {
	uint32_t now_tick;      /* control_time_ticks */
	bool     pwm_on;        /* ui_8_PWM_ON_Flag */
	bool     moe;           /* TIMER_CCHP & TIMER_CCHP_POEN */
	bool     cen;           /* TIMER_CTL0 & TIMER_CTL0_CEN */
	bool     cutoff_active; /* pwm_cutoff_active */
	bool     reverse;       /* i8_reverse_flag */
	uint8_t  hall;          /* ui8_hall_state (0..7) */
	int16_t  pi_q_int;
	int16_t  pi_d_int;
	int16_t  iq_request;
	int16_t  iq_pre_ramp;
	int16_t  iq_setpoint;
	int16_t  iq_meas;
	int16_t  id_meas;
	int16_t  vq;
	int16_t  vd;
	uint16_t angle;
	uint16_t ccr1;
	uint16_t ccr2;
	uint16_t ccr3;
	bool     neutral_dwell_active;
	bool     current_fresh;
	bool     current_valid;
	uint32_t current_seq;
	uint16_t current_age;
	int16_t  phase_a;
	int16_t  phase_b;
	int16_t  phase_c;
	uint16_t adc_ch3;
	uint16_t u_abs;
	uint8_t  bridge_lifecycle;
} fw117_trace_input_t;

void fw117_trace_init(void);
/* The session id stamped into the capture opened from now on - call with diag_session_current_id(). */
void fw117_trace_set_session_id(uint8_t session_id);
/* One observation per control tick; decimation and edge detection are internal. */
void fw117_trace_tick(const fw117_trace_input_t *in, uint32_t now_tick);

/* diag_record_source bridge - same shape as fw112_ab's. */
uint16_t fw117_trace_queue_count_session(uint8_t session_id);
bool fw117_trace_queue_peek_session(uint8_t session_id, fw117_trace_sample_t *out);
void fw117_trace_queue_release_session(uint8_t session_id);
uint32_t fw117_trace_queue_enqueued(void);
uint32_t fw117_trace_queue_rejected(void);

/* Capture-level metadata for the frame serializer (constant across all samples of a capture). */
uint8_t fw117_trace_capture_id(void);
uint8_t fw117_trace_event_type(void);

#endif /* FW117_TRACE_H_ */
