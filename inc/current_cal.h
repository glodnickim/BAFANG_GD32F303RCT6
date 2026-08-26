#ifndef CURRENT_CAL_H_
#define CURRENT_CAL_H_

#include <stdint.h>

/*
 * FW-119: safety policy around the FW-118 phase-current zero calibration.
 * FW-125: same-path measurement fix - see below and documentation/
 * FW-125_PHASE_CURRENT_SAME_PATH_CALIBRATION_PL.md.
 *
 * FW-118 measures the per-phase ADC zero once at startup and, on success, installs a software
 * correction the ISR subtracts from the injected readings. What it did NOT have was a policy:
 * one bad attempt simply left the firmware on the legacy hardware-offset path with a status
 * byte nobody acts on, and there was no way to distinguish "never calibrated" from "calibrated,
 * then a later attempt failed".
 *
 * This module owns that policy and nothing else:
 *
 *     retry  ->  last-known-good  ->  legacy fallback / hard fail
 *
 * It is deliberately hardware-free: main.c does the sampling (it is the only place that may
 * touch the ADC registers) and hands each attempt's raw accumulation in here as a plain struct.
 * That is what makes the policy testable on the host, and it is the same split FW-106 used for
 * diag_session.c.
 *
 * FW-125 CHANGED THE SAMPLING DOMAIN (the policy above is otherwise untouched):
 *   FW-118/119 sampled the ADC0 REGULAR scan (adc_value[7]/[8]/[4], raw ADC12 counts around
 *   2048) and computed the software offset as (that mean - the hardware IOFFx constant). But the
 *   FOC ISR reads phase current from adc_inserted_data_read() on each phase's OWN ADC instance
 *   (A=ADC2, B=ADC1, C=ADC0) - a DIFFERENT silicon ADC than ADC0's regular scan for phases A and
 *   B, with its own independent zero-current offset. Measuring one ADC and correcting the other
 *   is a domain mismatch; it was the confirmed root cause of FW-122 CASE C.
 *
 *   The fix: calibration now accumulates i16_ph1/2/3_current - the exact adc_inserted_data_read()
 *   values the FOC ISR consumes, captured inside ADC0_1_IRQHandler itself before any software
 *   offset or sector reconstruction is applied (main.c owns this; see phase_cal_acc_t there).
 *   Because adc_inserted_data_read() already returns (raw - IOFFx) in hardware, that accumulated
 *   mean IS the residual software offset directly - no second subtraction of the hardware offset
 *   constant. attempt_t.sum/min/max are therefore SIGNED (int32_t/int16_t): the residual domain
 *   is a small value centred on 0, not a ~2048 unsigned ADC12 code.
 *
 * WHAT THIS MODULE DOES NOT DO:
 *   - it does not change the provisional residual/P2P limits - those stay HW_PENDING
 *   - it does not persist anything to flash. LKG is RAM/session-level only; see below.
 *
 * SCOPE OF "LAST-KNOWN-GOOD" (read this before relying on it):
 *   LKG lives in RAM and dies with power. It is NOT a value carried over from a previous ride.
 *   Within one power cycle it means exactly this: the last attempt that passed validation. Its
 *   value today is that (a) a failing retry can never overwrite a set that already passed, and
 *   (b) if calibration is ever re-run while the bike is live - it is not, today, but the
 *   lifecycle now supports it - the running FOC keeps the good offsets instead of dropping to
 *   legacy mid-ride. Adding flash persistence would need storage infrastructure this card
 *   deliberately does not introduce.
 */

/* Phase index into every three-element array below. */
#define CURRENT_CAL_PHASE_A  0
#define CURRENT_CAL_PHASE_B  1
#define CURRENT_CAL_PHASE_C  2
#define CURRENT_CAL_PHASES   3

/*
 * Status codes. 0..3 keep the exact numeric meaning FW-118 shipped with, so any log or decoder
 * already reading current_calibration_status keeps reading it correctly; 4..8 are new.
 */
typedef enum {
	CURRENT_CAL_UNCALIBRATED    = 0, /* no attempt has been evaluated yet */
	CURRENT_CAL_OK              = 1, /* this attempt passed; runtime offsets installed */
	CURRENT_CAL_OUT_OF_RANGE    = 2, /* a phase mean fell outside the provisional window */
	CURRENT_CAL_TOO_NOISY       = 3, /* a phase P2P exceeded the provisional noise limit */
	CURRENT_CAL_SAMPLE_TIMEOUT         = 4, /* the sampling loop never got its samples */
	CURRENT_CAL_USING_LKG       = 5, /* every attempt failed; a previously accepted set is active */
	CURRENT_CAL_LEGACY_FALLBACK = 6, /* no usable set; running the pre-FW-118 hardware-offset path */
	CURRENT_CAL_HARD_FAILED     = 7, /* no usable set and STRICT policy: FOC start is inhibited */
	CURRENT_CAL_MOE_ON          = 8  /* calibration observed TIMER0 POEN; reject without touching PWM */
} current_cal_status_t;

/* Where the offsets the ISR applies right now came from. */
typedef enum {
	CURRENT_CAL_SRC_NONE    = 0, /* nothing applied, and FOC is inhibited (STRICT hard fail) */
	CURRENT_CAL_SRC_RUNTIME = 1, /* offsets from the attempt that just passed */
	CURRENT_CAL_SRC_LKG     = 2, /* offsets from an earlier attempt that passed */
	CURRENT_CAL_SRC_LEGACY  = 3  /* no software offsets; hardware offset only, as before FW-118 */
} current_cal_source_t;

/*
 * Start policy when calibration produced nothing usable.
 *
 * LEGACY_FALLBACK is the default and the only one that may be ridden until the FW-118 limits
 * have been measured on the bench. The provisional +-200 LSB window and the 200 LSB P2P limit
 * are guesses; STRICT on top of a guessed threshold is a way to immobilise a working bike over
 * a number nobody has verified. STRICT exists so the policy can be switched over in one place
 * once those limits are real.
 */
typedef enum {
	CURRENT_CAL_POLICY_LEGACY_FALLBACK = 0,
	CURRENT_CAL_POLICY_STRICT          = 1
} current_cal_policy_t;

/*
 * One attempt's raw accumulation, filled in by the caller's sampling loop.
 *
 * `timed_out` means the loop gave up waiting for the ISR accumulator. `samples` is how many
 * samples were actually accumulated - the mean is computed from THIS, not from a hardcoded
 * shift, so a changed CURRENT_CAL_SAMPLES cannot silently produce a wrong mean.
 *
 * FW-125: sum/min/max are SIGNED. The source is now adc_inserted_data_read() (already
 * hardware-offset-corrected, see current_cal.h's top comment), a small residual around 0 that
 * can legitimately go negative - unlike the old regular-ADC raw counts, which never could.
 */
typedef struct {
	uint8_t  timed_out;
	uint8_t  moe_on;  /* sampling observed TIMER0 CCHP.POEN; a valid startup attempt requires 0 */
	uint16_t samples;
	int32_t  sum[CURRENT_CAL_PHASES];
	int16_t  min[CURRENT_CAL_PHASES];
	int16_t  max[CURRENT_CAL_PHASES];
} current_cal_attempt_t;

typedef struct {
	/* --- what the ISR uses. Nothing else may write these. --- */
	int16_t              offset[CURRENT_CAL_PHASES];
	uint8_t              valid;   /* 1 = ISR subtracts offset[]; 0 = legacy hardware-offset path */

	/* --- result of the most recent attempt (diagnostic). FW-125: signed residual domain, see
	 * above - `residual_mean` replaces FW-118's `zero_adc` name to make that domain change
	 * explicit at every call site. --- */
	int16_t               residual_mean[CURRENT_CAL_PHASES];
	uint16_t              p2p[CURRENT_CAL_PHASES];
	current_cal_status_t  attempt_status;

	/* --- last accepted set. RAM/session only - see the header comment. --- */
	int16_t              lkg_offset[CURRENT_CAL_PHASES];
	int16_t              lkg_residual_mean[CURRENT_CAL_PHASES];
	uint8_t              lkg_valid;

	/* --- policy outcome --- */
	current_cal_policy_t policy;
	current_cal_status_t status;         /* final, policy-level status */
	current_cal_status_t failure_reason; /* last attempt failure; UNCALIBRATED if none */
	current_cal_source_t source;
	uint8_t              attempts;       /* attempts consumed so far */
	uint8_t              foc_allowed;    /* 0 only on STRICT hard fail */
	uint8_t              degraded;       /* 1 = running, but not on a freshly calibrated set */
	uint16_t             last_sample_count; /* accepted or rejected most recent attempt */
	uint8_t              last_moe_on;       /* most recent attempt observed POEN */
} current_cal_t;

/*
 * Full reset, including LKG. This is the power-on entry point: after it the state reads
 * UNCALIBRATED / SRC_LEGACY, FOC is allowed, and no software offset is applied - i.e. exactly
 * the pre-FW-118 behaviour, which is the only safe thing to be doing before any measurement.
 */
void current_cal_init(current_cal_t *s, current_cal_policy_t policy);

/*
 * Begin a calibration sequence: clears the attempt counter and the current attempt's result,
 * KEEPS the LKG set and keeps the offsets the ISR is using. Calling this mid-ride therefore
 * never disturbs a running FOC.
 */
void current_cal_begin(current_cal_t *s);

/* 1 while another attempt is both permitted and useful (not yet successful, budget left). */
uint8_t current_cal_attempt_allowed(const current_cal_t *s);

/*
 * Evaluate one attempt. Returns that attempt's status.
 *
 * On OK the runtime offsets are installed and become the new LKG. On any failure NOTHING that
 * FOC or LKG depends on is written - a failed retry cannot degrade a set that already passed.
 */
current_cal_status_t current_cal_submit(current_cal_t *s, const current_cal_attempt_t *a);

/*
 * Apply the start policy once the attempt budget is spent (or an attempt succeeded). Sets
 * status, source, degraded and foc_allowed. Safe to call more than once.
 */
void current_cal_finalize(current_cal_t *s);

/* 0 only when the policy is STRICT and there is no usable offset set. */
uint8_t current_cal_foc_allowed(const current_cal_t *s);

#endif /* CURRENT_CAL_H_ */
