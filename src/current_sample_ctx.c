#include "current_sample_ctx.h"

#include <string.h>

/*
 * FW-127B. See inc/current_sample_ctx.h for the model and the honest statement about what
 * "atomic" means here. Pure state: no register is touched, which is what makes it host-testable.
 */

static current_sample_context_t ctx_latched;
static current_sample_context_t ctx_next;
static uint8_t  next_pending;      /* 1 while ctx_next holds a transaction not yet consumed */
static uint32_t seq_counter;       /* the ONLY producer of seq                              */
static uint32_t published_count;
static uint32_t orphan_count;

void current_sample_ctx_reset(void)
{
	memset(&ctx_latched, 0, sizeof(ctx_latched));
	memset(&ctx_next, 0, sizeof(ctx_next));
	ctx_latched.state = (uint8_t)CURRENT_SAMPLE_INVALID;
	ctx_next.state = (uint8_t)CURRENT_SAMPLE_INVALID;
	next_pending = 0U;
	seq_counter = 0U;
	/* published_count / orphan_count are power-cycle evidence and deliberately survive a
	 * bridge-disable: a session's total is the question the hardware test asks. */
}

uint32_t current_sample_ctx_publish(uint8_t sector, uint8_t state, uint8_t direct_mask,
                                    uint16_t trigger_ccr)
{
	/* seq is assigned here and nowhere else, so "one publish = one transaction" is structural
	 * rather than a convention someone has to remember. */
	seq_counter++;

	ctx_next.sector = sector;
	ctx_next.state = state;
	ctx_next.direct_mask = direct_mask;
	ctx_next.trigger_ccr = trigger_ccr;
	ctx_next.seq = seq_counter;      /* written LAST: a diagnostic reader keys on seq */

	next_pending = 1U;
	if (published_count < 0xFFFFFFFFu) published_count++;
	return seq_counter;
}

const current_sample_context_t *current_sample_ctx_consume(void)
{
	if (next_pending) {
		ctx_latched = ctx_next;
		next_pending = 0U;
	} else {
		/*
		 * A conversion arrived that nobody had described. That is not a crash - it happens
		 * legitimately on the very first conversion after a bridge enable, before any SVPWM has
		 * run - but it must never be silently interpreted as if a context existed. The latched
		 * context is forced INVALID so a consumer cannot mistake stale fields for a description
		 * of the sample in hand.
		 */
		ctx_latched.state = (uint8_t)CURRENT_SAMPLE_INVALID;
		ctx_latched.sector = 0U;
		ctx_latched.direct_mask = 0U;
		if (orphan_count < 0xFFFFFFFFu) orphan_count++;
	}
	return &ctx_latched;
}

const current_sample_context_t *current_sample_ctx_latched(void) { return &ctx_latched; }
const current_sample_context_t *current_sample_ctx_next(void)    { return &ctx_next; }

uint8_t current_sample_ctx_snapshot(current_sample_context_t *out)
{
	uint32_t before, after;

	if (!out) return 0U;
	before = ctx_latched.seq;
	*out = ctx_latched;
	after = ctx_latched.seq;
	/* seq is written last on publish, so an unchanged seq around the copy means the copy did not
	 * straddle a publish. A changed seq is reported as a torn read rather than returned as a
	 * mixture of two transactions. */
	return (uint8_t)(before == after);
}

uint32_t current_sample_ctx_published_count(void) { return published_count; }
uint32_t current_sample_ctx_orphan_count(void)    { return orphan_count; }
