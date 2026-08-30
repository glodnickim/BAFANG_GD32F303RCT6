#ifndef QS_TRANSITION_DIAG_H_
#define QS_TRANSITION_DIAG_H_

#include <stdbool.h>
#include <stdint.h>

/* QS-1: passive, ISR-owned transition microscope.  It has no control outputs. */
#define QS_TRANSITION_DIAG_SAMPLES       48U
#define QS_TRANSITION_DIAG_PRE_SAMPLES   12U
#define QS_TRANSITION_DIAG_POST_SAMPLES  (QS_TRANSITION_DIAG_SAMPLES - QS_TRANSITION_DIAG_PRE_SAMPLES)

typedef enum {
	QS_EVT_NONE = 0U,
	QS_EVT_RUN_RISE = 1U,
	QS_EVT_MOE_RISE = 2U,
	QS_EVT_FIRST_IQ_REF = 4U,
	QS_EVT_IQ_TARGET_ZERO = 8U,
	QS_EVT_IQ_REF_ZERO = 16U,
	QS_EVT_MOE_FALL = 32U,
	QS_EVT_FAULT_HARD_OFF = 64U
} qs_transition_event_t;

typedef enum {
	QS_TRANSITION_IDLE = 0U,
	QS_TRANSITION_ARMED = 1U,
	QS_TRANSITION_TRIGGERED = 2U,
	QS_TRANSITION_COMPLETE = 3U
} qs_transition_state_t;

typedef struct {
	qs_transition_state_t state;
	uint8_t generation;
	uint8_t sample_count;
	uint8_t trigger_events;
	uint8_t trigger_index;
	bool export_ready;
} qs_transition_status_t;

typedef struct {
	uint32_t foc_cycle;
	int32_t iq_requested;
	int32_t iq_allowed;
	int32_t iq_ref;
	int32_t iq_measured;
	int32_t id_ref;
	int32_t id_measured;
	int16_t theta_e_hi16; /* q31 theta, high word: lossless view of the CAN-era angle domain. */
	uint16_t ccr_a;
	uint16_t ccr_b;
	uint16_t ccr_c;
	uint16_t flags;
	uint8_t lifecycle;
	uint8_t hall;
	uint8_t sample_state;
	uint8_t events;
} qs_transition_sample_t;

typedef struct {
	uint32_t foc_cycle;
	int32_t iq_requested, iq_allowed, iq_ref, iq_measured, id_ref, id_measured;
	int16_t theta_e_hi16;
	uint16_t ccr_a, ccr_b, ccr_c;
	uint8_t lifecycle, hall, sample_state;
	bool run_request, bridge_requested, moe, fault_hard_off, angle_static_legal;
} qs_transition_input_t;

#define QS_FLAG_RUN_REQUEST        0x0001U
#define QS_FLAG_BRIDGE_REQUESTED   0x0002U
#define QS_FLAG_MOE                0x0004U
#define QS_FLAG_FAULT_HARD_OFF     0x0008U
#define QS_FLAG_ANGLE_STATIC_LEGAL 0x0010U

void qs_transition_diag_init(void);
void qs_transition_diag_fast_tick(const qs_transition_input_t *in);
bool qs_transition_diag_is_complete(void);
bool qs_transition_diag_sample_at(uint16_t chronological_index, qs_transition_sample_t *out);
uint8_t qs_transition_diag_capture_id(void);
uint16_t qs_transition_diag_trigger_index(void);
void qs_transition_diag_status(qs_transition_status_t *out);
/* Request only: the ISR performs the complete reset, so it can never observe a partial ring. */
bool qs_transition_diag_request_new_capture(void);

#endif
