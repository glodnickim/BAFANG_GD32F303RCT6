#ifndef ROLLING_NO_ASSIST_DIAG_H_
#define ROLLING_NO_ASSIST_DIAG_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * ROLLING NO-ASSIST DIAGNOSTIC: behavior-neutral 250 Hz pre/post-trigger
 * recorder for intermittent loss of assist while the bike is rolling.
 * It only observes, copies and serializes states which already exist.
 *
 * CASE A: rider/permission context exists, but final demand does not form.
 * CASE B: final demand exists, but PWM/MOE/bridge cannot enter operation.
 * CASE C: PWM + hardware MOE + bridge lifecycle are active, but measured Iq
 *         magnitude remains below one quarter of final Iq.
 *
 * MS.i_q_setpoint is positive. During normal forward drive MS.i_q is negative,
 * therefore CASE C compares magnitudes and always stores the signed raw Iq.
 *
 * Sampling: 4 kHz control timebase / 16 = 250 Hz = 4 ms/sample.
 * Ring: 100 PRE (400 ms) + 156 POST (624 ms) = 256 samples (1024 ms).
 * Trigger confirmation: 8 consecutive sampled observations = 32 ms.
 *
 * Build isolation: src/rolling_no_assist_diag.c compiles state/code only when
 * CAN_DIAGNOSTICS_ENABLE && ROLLING_NO_ASSIST_DIAG_ENABLE. NORMAL contributes
 * zero recorder RAM and zero recorder code.
 */

#define ROLLING_NO_ASSIST_DIAG_SCHEMA_VERSION 4U

#define ROLLING_NO_ASSIST_DIAG_SAMPLES       256U
#define ROLLING_NO_ASSIST_DIAG_DECIMATION    16U
#define ROLLING_NO_ASSIST_DIAG_PRE_SAMPLES   100U
#define ROLLING_NO_ASSIST_DIAG_POST_SAMPLES  156U
#define ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT 8U
#define ROLLING_NO_ASSIST_DIAG_SAMPLE_HZ     250U

/* FW-122.1: schema v3 reuses the six DATA fragments of v2 unchanged. Two of the four wire
 * padding bytes in DATA 5 become real fields (pwm_cutoff_progress, hall_timeout_progress);
 * the CAN frame count, EFIDs and per-sample payload size are therefore IDENTICAL to v2. */
#define ROLLING_NO_ASSIST_SAMPLE_WIRE_BYTES  48U
#define ROLLING_NO_ASSIST_DATA_FRAGMENTS     6U
#define ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE  (1U + ROLLING_NO_ASSIST_DATA_FRAGMENTS)

#define ROLLING_NO_ASSIST_CASE_NONE  0U
#define ROLLING_NO_ASSIST_CASE_A     1U
#define ROLLING_NO_ASSIST_CASE_B     2U
#define ROLLING_NO_ASSIST_CASE_C     3U

/* Header plus six data-fragment EFIDs: 0x10248..0x1024E. */
#define ROLLING_NO_ASSIST_EFID_HEADER    0x00010248U
#define ROLLING_NO_ASSIST_EFID_DATA_BASE 0x00010249U

/*
 * SCHEMA v3 WIRE FORMAT
 * =====================
 *
 * FW-122.1: v3 is additive over v2. It exists to make one fact directly visible on the wire
 * that v2 could only leave implicit: ui_8_PWM_ON_Flag==0 does NOT always mean the hardware
 * bridge is off. During SOFT_CUTOFF (src/main.c), the software flag drops to 0 while
 * hardware MOE (TIMER_CCHP.POEN) stays 1 for up to SOFT_CUTOFF_TICKS control ticks, and CCR
 * is being interpolated open-loop toward neutral. If MS.i_q_setpoint becomes positive again
 * inside that window, the start block sees "!ui_8_PWM_ON_Flag" and re-enters in full - see
 * FW-124 (documentation/FW-124_START_STOP_LIFECYCLE_AUDIT_PL.md) section 6/13 (finding D2).
 * v3 adds the two raw facts needed to see this on a captured trace, at zero extra CAN cost:
 *
 *   pwm_cutoff_active     (status_flags bit)         - true while soft-cutoff owns CCR
 *   pwm_cutoff_progress   (u8, DATA 5 byte 4)         - ticks elapsed in the cutoff window
 *   hall_timeout_progress (u8, DATA 5 byte 5)         - progress toward the ~1 s Hall-silence
 *                                                        cutoff trigger (uint16_half_rotation_
 *                                                        counter >> 4, saturated at 255)
 *
 * These are RAW, ALREADY-EXISTING firmware facts, copied the same read-only way every other
 * field in this recorder is copied. Classification (CASE A/B/C, src/rolling_no_assist_diag.c
 * classify_trigger()) is UNCHANGED: a sample with pwm_cutoff_active=1 and final_iq>0 already
 * satisfies CASE B's existing "!pwm_on" condition today, with no code change needed there.
 * "D2" (the quick-re-demand-during-soft-cutoff pattern) is a CASE B *subreason*, derived
 * off-device by the Trace Analyzer from these two raw facts plus final_iq and hardware MOE -
 * see tools/trace_analyzer_lib/core.py. Nothing in this module computes or stores a "D2"
 * verdict; it only makes the evidence visible.
 *
 * The HEADER is metadata. It is NOT part of rolling_no_assist_sample_t.
 * One logical 48-byte sample is serialized as SIX 8-byte DATA fragments (unchanged from v2).
 * Fragment 5 (DATA 5) now carries bytes 40..45 as real fields, followed by TWO explicit zero
 * padding bytes (was four in v2) - the CAN frame count, EFIDs and per-sample payload bytes on
 * the wire are therefore IDENTICAL to v2; only the meaning of DATA 5 bytes 4/5 changed from
 * padding to data.
 *
 * Per sample:
 *   logical sample              48 B
 *   CAN data fragments           6 x 8 B = 48 B (2 B wire padding)
 *   header frames                1 x 8 B
 *   total payload                56 B in 7 CAN frames
 * Full 256-sample dump: 1792 frames / 14336 payload bytes / 17.92 s at 10 ms/frame (unchanged
 * from v2 - see FW-122.1 report for the exact confirmation).
 *
 * HEADER 0x10248:
 *   [0] schema_version=3, [1] session_id, [2] data_fragment_count=6,
 *   [3] capture_id, [4] trigger_case, [5] logical_sample_bytes=48,
 *   [6] sample_rate_hz=250, [7] confirmation_samples=8.
 *
 * DATA 0 0x10249, logical bytes 0..7:
 *   tick_abs u32 BE; flags u8; status_flags u8; case_id u8; bridge_lifecycle u8.
 * DATA 1 0x1024A, logical bytes 8..15:
 *   hall u8; permission_bits u8; reason_bits u8; debug_flags u8;
 *   iq_before_pu i16 BE; iq_request i16 BE.
 * DATA 2 0x1024B, logical bytes 16..23:
 *   iq_after_latch_floor i16 BE; iq_pre_ramp i16 BE;
 *   iq_setpoint/final_iq i16 BE; iq_actual_raw i16 BE.
 * DATA 3 0x1024C, logical bytes 24..31:
 *   pi_q_int i16 BE; pi_d_int i16 BE; erps u16 BE; cadence_rpm i16 BE.
 * DATA 4 0x1024D, logical bytes 32..39:
 *   load_centikg u16 BE; load_threshold u16 BE;
 *   angle_hall u16 BE; angle_absolute u16 BE.
 * DATA 5 0x1024E, logical bytes 40..45 + 2 B padding:
 *   rotor_direction i8; neutral_dwell_counter u8;
 *   motor_voltage_utilization u16 BE;
 *   pwm_cutoff_progress u8 (v3); hall_timeout_progress u8 (v3); [6..7]=0 padding.
 *
 * angle_hall/angle_absolute are the retained bits 31..16 of the cyclic Q31
 * rotor angles. One full electrical revolution is 2^16 retained counts, so
 * resolution is 360/65536 = 0.005493164 degrees/LSB. This preserves Hall-sector,
 * direction and reseed evidence while dropping only sub-LSB interpolation bits.
 */

/* Existing compact trigger/context flags. */
#define RNA_FLAG_CASE_A        0x01U
#define RNA_FLAG_CASE_B        0x02U
#define RNA_FLAG_CASE_C        0x04U
#define RNA_FLAG_RIDER_ACTIVE  0x08U
#define RNA_FLAG_PERMISSION    0x10U
#define RNA_FLAG_LOAD_MET      0x20U
#define RNA_FLAG_NO_DEMAND     0x40U
#define RNA_FLAG_PWM_ON        0x80U

/* Independent facts; MOE is the real TIMER_CCHP.POEN bit. */
#define RNA_STATUS_HARDWARE_MOE            0x01U
#define RNA_STATUS_CURRENT_CAL_FOC_ALLOWED 0x02U
#define RNA_STATUS_NEUTRAL_DWELL_ACTIVE    0x04U
/* FW-122.1: true while src/main.c's soft-cutoff owns CCR (pwm_cutoff_active). Legal alongside
 * hardware MOE=1 and software PWM=0 - see FW-124 section 4 for the full legal-combination
 * table. NOT itself a fault indicator. */
#define RNA_STATUS_PWM_CUTOFF_ACTIVE       0x08U
/* FW-126: an Iq value with this bit clear is deliberately not a current-feedback measurement;
 * the recorder must never classify it as a current-tracking observation. */
#define RNA_STATUS_CURRENT_FEEDBACK_VALID   0x10U

/* Exactly 48 B. Every declared field is serialized; there is no compiler-inserted padding -
 * the two v3 trailing bytes are explicit reserved fields, not alignment slack. */
typedef struct {
	uint32_t tick_abs;
	uint8_t  flags;
	uint8_t  status_flags;
	uint8_t  case_id;
	uint8_t  bridge_lifecycle;
	uint8_t  hall;
	uint8_t  permission_bits;
	uint8_t  reason_bits;
	uint8_t  debug_flags;
	int16_t  iq_before_pu;
	int16_t  iq_request;
	int16_t  iq_after_latch_floor;
	int16_t  iq_pre_ramp;
	int16_t  iq_setpoint;
	int16_t  iq_actual;
	int16_t  pi_q_int;
	int16_t  pi_d_int;
	uint16_t erps;
	int16_t  rpm;
	uint16_t load_centikg;
	uint16_t load_threshold;
	uint16_t angle_hall;
	uint16_t angle_absolute;
	int8_t   rotor_direction;
	uint8_t  neutral_dwell_counter;
	uint16_t motor_voltage_utilization;
	uint8_t  pwm_cutoff_progress;      /* v3: min(pwm_cutoff_tick, 255) */
	uint8_t  hall_timeout_progress;    /* v3: min(half_rotation_counter >> 4, 255) */
	uint8_t  reserved0;                /* v3: explicit wire padding, always 0 */
	uint8_t  reserved1;                /* v3: explicit wire padding, always 0 */
} rolling_no_assist_sample_t;

/* One immutable local snapshot supplied by main.c at the sample point. */
typedef struct {
	uint32_t now_tick;
	int16_t  iq_request_raw;
	int16_t  iq_before_pu;
	int16_t  iq_after_latch_floor;
	int16_t  iq_pre_ramp;
	int16_t  iq_setpoint;
	int16_t  iq_actual;
	bool     pwm_on;
	bool     moe;
	uint8_t  bridge_lifecycle;
	uint8_t  hall;
	uint16_t angle_hall;
	uint16_t angle_absolute;
	int8_t   rotor_direction;
	int16_t  pi_q_int;
	int16_t  pi_d_int;
	uint16_t erps;
	int16_t  rpm;
	uint16_t motor_voltage_utilization;
	bool     neutral_dwell_active;
	uint8_t  neutral_dwell_counter;
	bool     current_cal_foc_allowed;
	bool     current_feedback_valid; /* foc_current_valid, sampled atomically with iq_actual */
	uint16_t load_centikg;
	uint16_t load_threshold;
	bool     permission_present;
	uint8_t  permission_bits;
	uint8_t  reason_bits;
	bool     load_met;
	bool     rider_latched;
	uint8_t  debug_flags;
	/* FW-122.1: raw soft-cutoff/Hall-timeout facts, copied as-is - see the schema v3 comment
	 * above for why these exist. pwm_cutoff_active/pwm_cutoff_tick are written exclusively
	 * from main-loop-context code (main()'s lifecycle block and reg_ADC_processing(), never
	 * from a real ISR) and are read outside IRQ protection in main.c, same as the other
	 * main-loop-owned RNA inputs. half_rotation_counter also has an ISR writer
	 * (TIMER2_IRQHandler resets it on two Hall transitions per electrical revolution - see
	 * FW-124 section 5/13 for the exact case-13/case-23 xrefs), so main.c copies it inside the
	 * same short critical section as the other ISR-touched fields (angles, Hall, direction). */
	bool     pwm_cutoff_active;
	uint16_t pwm_cutoff_tick;
	uint16_t half_rotation_counter;
} rolling_no_assist_input_t;

void rolling_no_assist_diag_init(void);
void rolling_no_assist_diag_set_session_id(uint8_t session_id);
void rolling_no_assist_diag_tick(const rolling_no_assist_input_t *in, uint32_t now_tick);

/* FROZEN capture access for the explicit/repeatable diagnostic dump. No accessor retires data. */
bool rolling_no_assist_diag_is_frozen(void);
uint16_t rolling_no_assist_diag_capture_sample_count(void);
bool rolling_no_assist_diag_capture_sample_at(uint16_t sample_index, rolling_no_assist_sample_t *out);
uint8_t rolling_no_assist_diag_capture_session_id(void);
uint32_t rolling_no_assist_diag_queue_enqueued(void);
uint32_t rolling_no_assist_diag_queue_rejected(void);
uint8_t rolling_no_assist_diag_capture_id(void);
uint8_t rolling_no_assist_diag_trigger_case(void);

/* Single real C serializer used by both the dump bridge and the C oracle. */
bool rolling_no_assist_diag_encode_fragment(
	const rolling_no_assist_sample_t *sample,
	uint8_t session_id,
	uint8_t capture_id,
	uint8_t trigger_case,
	uint16_t fragment,
	uint32_t *efid,
	uint8_t data[8],
	bool *last);

#endif /* ROLLING_NO_ASSIST_DIAG_H_ */
