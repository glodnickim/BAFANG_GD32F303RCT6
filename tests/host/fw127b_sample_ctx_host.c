/*
 * FW-127B: the atomic sample context, against the REAL module (src/current_sample_ctx.c).
 *
 * WHAT THIS PROTECTS. Before this card, everything needed to interpret a current sample lived in
 * four unrelated places - the sector in a MotorState field, the direct-phase ownership implicit
 * in a switch, the trigger only in a timer register, and the sequence in a separate counter.
 * Nothing tied them to the same conversion, so nothing could detect it when they disagreed, and
 * the pre-audit showed they systematically did.
 *
 * The tests that carry the weight are B3 (a context published for N+1 must not be readable as
 * the one describing N) and B7 (a rearm may not inherit a context from the previous run).
 */

#include "common/check.h"
#include "../../inc/current_sample_ctx.h"

#include <stdio.h>
#include <string.h>

#define DIRECT_BC (CURRENT_SAMPLE_DIRECT_B | CURRENT_SAMPLE_DIRECT_C)
#define DIRECT_AC (CURRENT_SAMPLE_DIRECT_A | CURRENT_SAMPLE_DIRECT_C)
#define DIRECT_AB (CURRENT_SAMPLE_DIRECT_A | CURRENT_SAMPLE_DIRECT_B)

int main(void)
{
	const current_sample_context_t *c;

	/* --- baseline: a fresh chain describes nothing --------------------------------------- */
	current_sample_ctx_reset();
	c = current_sample_ctx_latched();
	CHECK(c->seq == 0, "R1. a fresh chain has no transaction");
	CHECK(c->state == CURRENT_SAMPLE_INVALID, "R2. ...and is INVALID, not accidentally PRIMARY");
	CHECK(current_sample_ctx_next()->seq == 0, "R3. nothing is pending either");

	/* --- B1 + B2: the sample consumes the context published for it ------------------------ */
	current_sample_ctx_reset();
	{
		const uint32_t s1 = current_sample_ctx_publish(1, CURRENT_SAMPLE_PRIMARY, DIRECT_BC, 3740);
		c = current_sample_ctx_consume();
		CHECK(c->seq == s1, "B1a. the consumed context is the one that was published");
		CHECK(c->sector == 1 && c->direct_mask == DIRECT_BC && c->trigger_ccr == 3740,
		      "B1b. sector, direct ownership and trigger arrive together");
		CHECK(c == current_sample_ctx_latched(), "B1c. consume publishes it as the latched one");
	}

	/* --- B3: a context for N+1 must NOT be visible as the latched one ---------------------- */
	current_sample_ctx_reset();
	{
		(void)current_sample_ctx_publish(1, CURRENT_SAMPLE_PRIMARY, DIRECT_BC, 3740);
		c = current_sample_ctx_consume();                       /* now latched = N */
		const uint32_t seq_n = c->seq;
		const uint8_t sector_n = c->sector;

		/* the ISR computes geometry N+1 and publishes it - while the sample N is still in hand */
		(void)current_sample_ctx_publish(2, CURRENT_SAMPLE_ALTERNATE, DIRECT_AC, 3600);

		CHECK(current_sample_ctx_latched()->seq == seq_n,
		      "B3a. publishing N+1 does not change the context describing N");
		CHECK(current_sample_ctx_latched()->sector == sector_n,
		      "B3b. ...including its sector");
		CHECK(current_sample_ctx_next()->seq == seq_n + 1, "B3c. N+1 waits in its own slot");

		c = current_sample_ctx_consume();                       /* next ISR */
		CHECK(c->seq == seq_n + 1 && c->sector == 2 && c->trigger_ccr == 3600,
		      "B3d. and the next conversion consumes exactly that one");
	}

	/* --- B4: seq increments exactly once per transaction ----------------------------------- */
	current_sample_ctx_reset();
	{
		int ok = 1;
		/* published_count is deliberately power-cycle evidence and survives reset (see E1),
		 * so this checks the DELTA - an absolute would silently depend on test ordering. */
		const uint32_t published_before = current_sample_ctx_published_count();
		for (uint32_t i = 1; i <= 500; i++) {
			const uint32_t s = current_sample_ctx_publish((uint8_t)(i % 3), CURRENT_SAMPLE_PRIMARY,
			                                              DIRECT_AB, (uint16_t)(3000 + (i % 700)));
			if (s != i) ok = 0;
			if (current_sample_ctx_consume()->seq != i) ok = 0;
		}
		CHECK(ok, "B4a. 500 transactions produce seq 1..500 with no gap and no repeat");
		CHECK(current_sample_ctx_published_count() - published_before == 500,
		      "B4b. the publish count agrees");
	}

	/* --- B5: every field belongs to the same seq -------------------------------------------- */
	current_sample_ctx_reset();
	{
		int ok = 1;
		for (uint32_t i = 1; i <= 300; i++) {
			/* fields deliberately derived from i, so a mixture from two transactions is visible */
			const uint8_t sec = (uint8_t)(i % 7);
			const uint8_t msk = (uint8_t)(i % 8);
			const uint16_t trg = (uint16_t)(i * 3);
			(void)current_sample_ctx_publish(sec, CURRENT_SAMPLE_PRIMARY, msk, trg);
			c = current_sample_ctx_consume();
			if (c->sector != sec || c->direct_mask != msk || c->trigger_ccr != trg || c->seq != i) ok = 0;
		}
		CHECK(ok, "B5. sector, state, direct_mask and trigger never come from different transactions");
	}

	/* --- B6: a consume with nothing published is an orphan, not a stale reuse ---------------- */
	current_sample_ctx_reset();
	{
		(void)current_sample_ctx_publish(1, CURRENT_SAMPLE_PRIMARY, DIRECT_BC, 3740);
		c = current_sample_ctx_consume();
		CHECK(c->state == CURRENT_SAMPLE_PRIMARY, "B6a. the published transaction is consumed");

		/* a second conversion arrives with nothing prepared for it */
		const uint32_t orphans_before = current_sample_ctx_orphan_count();
		c = current_sample_ctx_consume();
		CHECK(c->state == CURRENT_SAMPLE_INVALID,
		      "B6b. an undescribed conversion is INVALID, not the previous context reused");
		CHECK(c->sector == 0 && c->direct_mask == 0,
		      "B6c. ...and its stale fields are cleared, so nothing can misread them");
		CHECK(current_sample_ctx_orphan_count() == orphans_before + 1, "B6d. ...and it is counted");
	}

	/* --- B7: a rearm cannot consume a context from the previous run -------------------------- */
	current_sample_ctx_reset();
	{
		(void)current_sample_ctx_publish(2, CURRENT_SAMPLE_PRIMARY, DIRECT_AC, 3500);
		(void)current_sample_ctx_consume();
		/* geometry for the next transaction was already prepared when the bridge went down */
		(void)current_sample_ctx_publish(3, CURRENT_SAMPLE_PRIMARY, DIRECT_AB, 3200);

		current_sample_ctx_reset();          /* shutdown / rearm boundary */

		CHECK(current_sample_ctx_latched()->seq == 0, "B7a. the reset clears the latched context");
		CHECK(current_sample_ctx_next()->seq == 0, "B7b. ...and the pending one");
		c = current_sample_ctx_consume();
		CHECK(c->state == CURRENT_SAMPLE_INVALID,
		      "B7c. the first conversion after a rearm is INVALID, never the pre-shutdown context");
		CHECK(c->sector == 0, "B7d. ...with no stale sector surviving");

		const uint32_t s = current_sample_ctx_publish(1, CURRENT_SAMPLE_PRIMARY, DIRECT_BC, 3740);
		CHECK(s == 1, "B7e. seq restarts at 1, so N/N+1 is unambiguous after a rearm");
	}

	/* --- the diagnostic snapshot reports a torn read rather than a mixture ------------------- */
	current_sample_ctx_reset();
	{
		current_sample_context_t snap;
		(void)current_sample_ctx_publish(4, CURRENT_SAMPLE_ALTERNATE, DIRECT_AB, 3300);
		(void)current_sample_ctx_consume();
		CHECK(current_sample_ctx_snapshot(&snap) != 0, "S1. a quiet snapshot is coherent");
		CHECK(snap.sector == 4 && snap.trigger_ccr == 3300 && snap.state == CURRENT_SAMPLE_ALTERNATE,
		      "S2. ...and carries the whole transaction");
		CHECK(current_sample_ctx_snapshot(NULL) == 0, "S3. a null target is refused");
	}

	/* --- evidence counters survive a bridge cycle, because a session total is the question ---- */
	{
		const uint32_t published = current_sample_ctx_published_count();
		current_sample_ctx_reset();
		CHECK(current_sample_ctx_published_count() == published,
		      "E1. the publish counter is power-cycle evidence, not per-start");
	}

	if (host_test_failures == 0) {
		printf("FW-127B sample context: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-127B sample context: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
