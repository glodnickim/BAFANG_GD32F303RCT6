#ifndef CURRENT_CAL_H_
#define CURRENT_CAL_H_

#include <stdint.h>

/*
 * FW-126.7: phase-current zero calibration, measured in the ONE electrical state that was
 * proven to produce a valid reading.
 *
 * WHY THE PREVIOUS CALIBRATION WAS REPLACED, not repaired
 * ------------------------------------------------------
 * Every calibration before this card sampled with the bridge DARK (MOE off). FW-126.6 showed
 * what that actually measured, by reconstructing the physical ADC result (raw = JDR + IOFF):
 *
 *     dark  : raw 3874 / 3910 / 3898  =  3.12-3.15 V of 3.3 V   -> amplifier AT ITS RAIL
 *     first conversion after MOE ON   :  raw ~4078              -> harder into the rail
 *     settled neutral bridge          :  raw 2004 / 2023 / 2020 -> ~1.62 V, MID-SCALE
 *     hardware IOFF constants         :      2020 / 2028 / 2012 -> agrees within 16 LSB
 *
 * The dark reading was a SATURATED amplifier output, not a zero-current offset. It was also
 * extremely quiet (spread 10 LSB, dump P2P 17-19), which is why it passed every noise check
 * for three cards running. THAT is the lesson encoded below: a small peak-to-peak proves
 * nothing on its own, because saturation is stable too. Validity is decided by the window
 * FIRST and by stability only SECOND.
 *
 * THE THREE DOMAINS - never mixed anywhere in this module or its report
 * --------------------------------------------------------------------
 *     physical ADC result  =  JDR + IOFF          (~2020 at zero current, mid-scale)
 *     JDR                  =  what adc_inserted_data_read() returns, i.e. raw - IOFF in
 *                             silicon (a small signed residual around 0)
 *     software offset      =  current_cal.offset[], subtracted by the FOC ISR from JDR
 *
 * Hardware IOFF stays a FIXED coarse centering (2012/2028/2020, written once in adc_config()
 * and nowhere else). This module calibrates only the small residual JDR zero on top of it.
 *
 * WHAT THIS MODULE OWNS, AND WHAT IT DOES NOT
 * -------------------------------------------
 * It owns the DATA state of the calibration: settling, eligibility, the estimator, validation
 * and the resulting offsets. It is deliberately hardware-free - it never reads or writes a
 * timer, ADC or GPIO register, which is what makes it testable on the host.
 *
 * It does NOT own the bridge. MOE, the neutral compares and the start progression belong to
 * exactly one place: the motor-start lifecycle in main.c. This module can ASK for the existing
 * neutral dwell to be held (current_cal_wants_dwell()); the lifecycle decides. There is no
 * second state machine driving the bridge.
 *
 * THE DEADLOCK THIS CARD ALSO FIXES
 * ---------------------------------
 * Two different questions used to share one flag:
 *
 *     "may the controller enter the safe neutral bridge state?"   and
 *     "may the controller run active FOC?"
 *
 * Gating the first on calibration validity is a deadlock: an uncalibrated controller could
 * never reach the only state in which it can be calibrated. They are now separate. Entering
 * the neutral dwell needs only a legitimate torque/walk request; current_cal_foc_allowed()
 * gates ONLY the transition to active FOC.
 */

/* Phase index into every three-element array below. */
#define CURRENT_CAL_PHASE_A  0
#define CURRENT_CAL_PHASE_B  1
#define CURRENT_CAL_PHASE_C  2
#define CURRENT_CAL_PHASES   3

/*
 * Data state. This is the calibration's own status - it says nothing about the bridge.
 * The numbering is stable because it goes on the wire in the 0x602D report.
 */
typedef enum {
	CURRENT_CAL_ST_UNCALIBRATED = 0,  /* power-on, or after an invalidate                  */
	CURRENT_CAL_ST_SETTLING     = 1,  /* in the dwell, waiting for a valid, steady reading */
	CURRENT_CAL_ST_COLLECTING   = 2,  /* gate satisfied, accumulating eligible samples     */
	CURRENT_CAL_ST_VALID        = 3,  /* offsets installed; active FOC may be released     */
	CURRENT_CAL_ST_FAILED       = 4   /* this attempt gave up; FOC stays forbidden         */
} current_cal_state_t;

/*
 * Why an attempt ended. UNCALIBRATED means "no failure recorded yet".
 * Also on the wire - keep the numbering stable.
 */
typedef enum {
	CURRENT_CAL_FAIL_NONE        = 0,
	CURRENT_CAL_FAIL_TIMEOUT     = 1,  /* never got a settled reading inside the budget     */
	CURRENT_CAL_FAIL_OUT_OF_RANGE= 2,  /* final mean outside the residual window            */
	CURRENT_CAL_FAIL_TOO_NOISY   = 3,  /* eligible samples still too spread out             */
	CURRENT_CAL_FAIL_NOT_NEUTRAL = 4,  /* the caller reported the bridge was not neutral    */
	CURRENT_CAL_FAIL_ATTEMPTS    = 5   /* attempt budget for this power cycle is spent      */
} current_cal_fail_t;

/* The only source that can produce valid offsets. There is deliberately no second one. */
typedef enum {
	CURRENT_CAL_SRC_NONE          = 0,
	CURRENT_CAL_SRC_NEUTRAL_DWELL = 1
} current_cal_source_t;

/*
 * Per-phase measurement of the accepted sample set. Streaming - no sample array is kept, so
 * the collection length can be raised without costing RAM.
 *
 *   PRODUCER  : current_cal_sample(), while state == COLLECTING
 *   CONSUMER  : the validator, and the 0x602D report
 *   RESET     : cal_restart_collection(), and current_cal_begin_attempt()
 *   PURPOSE   : the estimator and the evidence behind its verdict
 */
typedef struct {
	int32_t  sum;
	int16_t  min;
	int16_t  max;
} current_cal_acc_t;

typedef struct {
	/* --- what the FOC ISR consumes. Nothing outside this module may write these. --- */
	int16_t  offset[CURRENT_CAL_PHASES];
	uint8_t  valid;            /* 1 = ISR subtracts offset[]. The ONE source of that truth. */

	/* --- data state --- */
	current_cal_state_t  state;
	current_cal_fail_t   failure_reason;
	current_cal_source_t source;
	uint8_t  attempts;         /* attempts consumed this power cycle                        */

	/* --- gate progress, all in dwell ISR cycles (one per PWM period) --- */
	uint16_t cycles;           /* cycles since this attempt began holding the dwell         */
	uint16_t stable_count;     /* consecutive cycles satisfying window AND stability        */
	uint16_t eligible;         /* samples accepted into the estimator                       */
	uint16_t restarts;         /* collection restarts caused by losing the gate             */
	uint8_t  timeout_hit;      /* the bounded budget ran out                                */
	/*
	 * Positive evidence that the electrical state held for EVERY sample of this attempt, rather
	 * than something the report infers. FOC-blocked is not a separate field because it is
	 * structural: the only call site is the dwell branch of the FOC ISR, which is mutually
	 * exclusive with FOC_calculation().
	 *   PRODUCER : current_cal_begin_attempt() sets it, current_cal_sample() can only clear it
	 *   CONSUMER : the 0x602D report
	 *   RESET    : current_cal_begin_attempt()
	 *   PURPOSE  : "MOE on and compares neutral, proven, not assumed"
	 */
	uint8_t  neutral_ok;
	uint8_t  seen_sample;      /* a previous sample exists, so a delta can be formed        */
	int16_t  prev[CURRENT_CAL_PHASES];

	/* --- result of the accepted set (diagnostic evidence, JDR domain) --- */
	current_cal_acc_t acc[CURRENT_CAL_PHASES];
	int16_t  mean[CURRENT_CAL_PHASES];
	uint16_t p2p[CURRENT_CAL_PHASES];
} current_cal_t;

/*
 * Power-on. Leaves UNCALIBRATED with no offsets applied and active FOC forbidden, which is the
 * only honest state before anything has been measured.
 */
void current_cal_init(current_cal_t *s);

/* 1 while a calibration is still wanted and the attempt budget allows one. */
uint8_t current_cal_needs_calibration(const current_cal_t *s);

/*
 * A legitimate neutral bridge start has begun and calibration may use it. Called by the
 * lifecycle BEFORE the dwell can produce its first ISR cycle. Does nothing once VALID.
 */
void current_cal_begin_attempt(current_cal_t *s);

/*
 * One dwell ISR cycle. `jdr` is the RAW inserted result of phases A/B/C, read before any
 * software offset, reconstruction or filtering - the same values the FOC ISR consumes.
 * `bridge_neutral` is the caller's own verification that MOE is on, the compares are neutral
 * and FOC is blocked; a false here fails the attempt rather than quietly measuring garbage.
 *
 * Returns the state after this cycle.
 */
current_cal_state_t current_cal_sample(current_cal_t *s, const int16_t jdr[CURRENT_CAL_PHASES],
                                       uint8_t bridge_neutral);

/* 1 while the calibration still needs the existing neutral dwell held open. */
uint8_t current_cal_wants_dwell(const current_cal_t *s);

/*
 * THE gate for active FOC, and nothing else. True only when calibration is VALID.
 * Never use it to decide whether the neutral bridge state may be entered - that is what the
 * deadlock above was.
 */
uint8_t current_cal_foc_allowed(const current_cal_t *s);

#endif /* CURRENT_CAL_H_ */
