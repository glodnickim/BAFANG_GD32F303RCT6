#ifndef CURRENT_FEEDBACK_H_
#define CURRENT_FEEDBACK_H_

#include <stdint.h>
#include "current_sample_ctx.h"

/*
 * FW-127D: what the control loop is allowed to believe about the current it was just handed.
 *
 * THE DISTINCTION THIS CARD MAKES
 * ------------------------------
 * Until now the only predicate anywhere was "the conversion completed" (ADC1 EOIC). That proves
 * the ADC finished. It proves nothing about whether the conversion happened while the shunts
 * were conducting - which is the only thing that makes the number a current. A sample taken
 * outside the conduction window went straight into Clarke, Park and the PI regulators with no
 * marker of any kind.
 *
 * FW-127C decides that question per transaction (PRIMARY / ALTERNATE / INVALID). This module is
 * what acts on the answer:
 *
 *     PRIMARY or ALTERNATE -> the reading is trustworthy. Use it, remember it, age = 0.
 *     INVALID              -> do NOT let it reach the control loop. Substitute the last
 *                             trustworthy reading and count how long that has been going on.
 *
 * WHY LAST-VALID RATHER THAN ZERO OR A HOLD-IN-PLACE
 * -------------------------------------------------
 * Feeding zero would tell the PI the motor is drawing no current and invite it to wind up.
 * Leaving MS.i_q untouched is what the firmware did by accident before FW-126 (the ISR simply
 * did not run), and it hid the staleness completely. Substituting the last trustworthy sample is
 * explicit, bounded and, above all, COUNTED - so the one hardware session can show how often it
 * really happens instead of the question staying open.
 *
 * NO MAXIMUM AGE IS DEFINED HERE, deliberately. Choosing a limit before the real distribution is
 * known would be inventing a threshold, which is the mistake FW-125 made with its acceptance
 * window. The age is instrumented; the limit is a later decision with evidence behind it.
 */

/* Compact age histogram. Buckets are powers of two so a long tail is visible without a big array. */
#define CURRENT_FEEDBACK_AGE_BUCKETS 4   /* age 1 | 2-3 | 4-7 | 8+ */

typedef struct {
	/* --- the substitute the control loop gets when a sample cannot be trusted --- */
	int16_t  last_a, last_b, last_c;
	uint8_t  have_last;        /* 0 until the first trustworthy sample of this run       */
	uint32_t sample_age;       /* consecutive INVALID samples since the last good one    */

	/* --- passive evidence for the single hardware session --- */
	uint32_t primary_count;
	uint32_t alternate_count;
	uint32_t invalid_count;
	uint32_t reuse_count;      /* INVALID samples that were served from last-valid        */
	uint32_t starved_count;    /* INVALID with no last-valid at all - nothing usable      */
	uint32_t max_sample_age;
	uint32_t age_bucket[CURRENT_FEEDBACK_AGE_BUCKETS];
} current_feedback_state_t;

/*
 * Bridge start / rearm. Drops last-valid ownership so a new run can never be steered by a
 * current measured before the previous shutdown.
 *
 *   PRODUCER : current_feedback_reset()
 *   CONSUMER : current_feedback_update()
 *   RESET    : every bridge start, alongside current_sample_ctx_reset()
 *   PURPOSE  : a deterministic first sample, so rolling start does not inherit stale current
 *
 * The evidence counters deliberately survive: a session total is what the hardware test asks for.
 */
void current_feedback_reset(void);

/*
 * Decide what the control loop may use, given the state of the transaction that produced this
 * sample. `i_a`/`i_b`/`i_c` are in/out: on an INVALID sample they are REPLACED by the last
 * trustworthy reading.
 *
 * Returns 1 when the values may be used, 0 when there is nothing trustworthy at all - which can
 * only happen before the first good sample of a run. The caller must not run current control on
 * a 0.
 */
uint8_t current_feedback_update(uint8_t sample_state, int16_t *i_a, int16_t *i_b, int16_t *i_c);

/* Read-only view of the evidence. */
const current_feedback_state_t *current_feedback_get(void);

#endif /* CURRENT_FEEDBACK_H_ */
