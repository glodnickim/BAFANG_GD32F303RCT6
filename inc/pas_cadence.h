#ifndef PAS_CADENCE_H_
#define PAS_CADENCE_H_

#include <stdint.h>

/*
 * PRE-FW128: the cadence measurement itself - the period, the epoch, and the validity.
 *
 * WHY THIS IS A MODULE. The period used to be a counter incremented inside reg_ADC_processing(),
 * which meant the number could only ever be tested by testing main.c, and main.c cannot be
 * linked on a host. The one regression that matters here - "the cadence is the same whether the
 * main loop runs every tick or every tenth tick" - was therefore untestable, which is a large
 * part of why the defect survived. It is testable now.
 *
 * WHAT IT OWNS, AND ONLY THIS:
 *   - the anchor tick a cadence interval starts from,
 *   - whether that interval is still intact,
 *   - the forward-step count inside it,
 *   - the resulting raw rpm and its validity.
 *
 * WHAT IT DOES NOT OWN. Filtering, p_human, the start phase, the assist gates, the direction
 * policy, the stop timeout. Those stay in main.c, unchanged, and are driven by the result below.
 * This module makes no decision about assist and never touches a current.
 *
 * THE EPOCH RULE, which is the heart of it. A published rpm must span an uninterrupted run of
 * forward steps and nothing else. Three things break that run and all three void it:
 *
 *   REVERSE   the crank turned back; an interval spanning it would be measured across a
 *             direction change and report a period the pedals never took.
 *   INVALID   an illegal two-bit jump. The step count assembling the interval is now wrong by
 *             an unknown amount, so the interval would be divided by the wrong number of steps.
 *   OVERFLOW  the sampler queue filled, so the ORDER of the events is no longer the order the
 *             crank produced.
 *
 * In all three cases the honest answer is to publish nothing until a clean interval completes.
 * There is deliberately no substitute value, no clamp and no held-over reading: the previous
 * measurement simply stands until a new one is earned, and the stop timeout in main.c is what
 * eventually zeroes it.
 */

typedef struct {
	uint8_t  pulse;          /* a cadence pulse boundary occurred on this step                 */
	uint8_t  measured;       /* ...and the interval was intact and long enough to divide       */
	uint16_t rpm;            /* the raw measurement, valid only when `measured`                */
	uint16_t period_ticks;   /* the interval in REAL 4 kHz ticks, 0 when not measured          */
} pas_cadence_step_t;

typedef struct {
	uint16_t rpm;                 /* last raw measurement (0 after a reset)                    */
	uint8_t  valid;               /* a real measurement stands - never set by a seed or a guess*/
	uint8_t  epoch_valid;         /* the interval currently being assembled is intact          */
	uint16_t last_period_ticks;   /* diagnostic readback of the last measured interval         */
	uint32_t anchor_tick;         /* the tick that interval started on                         */
	uint8_t  fwd_steps;           /* forward steps accumulated inside it                       */
	uint32_t broken_reverse;      /* epochs voided, by cause - all three are ride-log evidence */
	uint32_t broken_invalid;
	uint32_t broken_overflow;
	uint32_t measurements;        /* raw rpm values published since reset                      */
} pas_cadence_state_t;

/* Power-on and every genuine crank stop. Clears the measurement AND the epoch. */
void pas_cadence_reset(void);

/* Void the interval being assembled. `cause` is 0 reverse, 1 invalid, 2 sampler overflow -
 * counted separately because a ride log that cannot tell them apart cannot tell bounce from a
 * busy main loop. */
void pas_cadence_break_epoch(uint8_t cause);

/*
 * One forward quadrature step, at its REAL tick.
 *
 * `interval_restart` is main.c's existing FW-086 condition (the first forward step of a new
 * engagement): that step is the interval's ORIGIN, not its first count, or the pulse would fire
 * one step early and span PAS_STEPS_PER_PULSE-1 gaps - a reading 4/3 too high.
 */
pas_cadence_step_t pas_cadence_forward_step(uint32_t tick, uint8_t interval_restart);

const pas_cadence_state_t *pas_cadence_get(void);

#endif /* PAS_CADENCE_H_ */
