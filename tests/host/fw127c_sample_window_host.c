/*
 * FW-127C: the sampling-window decision, against the REAL module (src/sample_window.c).
 *
 * WHAT THIS REPLACES. dyn_adc_trigger_update() ran before FOC_calculation(), so it derived the
 * trigger for transaction N+1 from the geometry of transaction N; it always reconstructed the
 * highest-duty phase even when that phase was perfectly measurable; and in the over-modulation
 * regime it programmed a compare that could never match. This module answers the same question
 * from the geometry that will actually be applied, and returns INVALID instead of an illegal
 * trigger.
 *
 * The tests that carry the weight are C6-C8 (a legal trigger or an honest INVALID, never an
 * illegal compare) and C3-C5 (which phases a sector owns).
 */

#include "common/check.h"
#include "../../inc/sample_window.h"
#include "../../inc/config.h"

#include <stdio.h>

#define ARR      ((uint16_t)_T)                 /* 3750 */
#define TRIG     ((uint16_t)TRIGGER_DEFAULT)    /* 3740 = ARR - 10 */
#define GUARD    ((int32_t)SAMPLE_WINDOW_GUARD_TICKS)
#define LATEST   ((int32_t)TRIG - GUARD)        /* the highest CCR still measurable */

#define D_A CURRENT_SAMPLE_DIRECT_A
#define D_B CURRENT_SAMPLE_DIRECT_B
#define D_C CURRENT_SAMPLE_DIRECT_C

static sample_window_t decide(uint16_t a, uint16_t b, uint16_t c, uint16_t trig)
{
	const uint16_t applied[CURRENT_SAMPLE_PHASES] = { a, b, c };
	sample_window_t w;
	sample_window_decide(applied, ARR, trig, &w);
	return w;
}

int main(void)
{
	/* --- the derived geometry is what the header claims --------------------------------- */
	CHECK(SAMPLE_WINDOW_T_ACQ_TICKS == 333, "G1. acquisition = 55.5 ADC cycles @ 20 MHz = 333 ticks");
	CHECK(SAMPLE_WINDOW_T_DEAD_TICKS == 32, "G2. dead time = DTCFG 32 @ fDTS = fTIMER");
	CHECK(GUARD == 397, "G3. guard = acquisition + dead time + settle");
	CHECK(LATEST == 3343, "G4. the highest measurable CCR at the default trigger");

	/* --- C2 + PRIMARY: ordinary duty measures all three phases directly ------------------ */
	{
		const sample_window_t w = decide(1875, 1875, 1875, TRIG);
		CHECK(w.state == CURRENT_SAMPLE_PRIMARY, "C2a. neutral 50 % duty is PRIMARY");
		CHECK(w.direct_mask == (D_A | D_B | D_C), "C2b. all three phases are measured directly");
		CHECK(w.sector == SAMPLE_WINDOW_RECONSTRUCT_NONE, "C2c. nothing is reconstructed");
		CHECK(w.trigger_ccr == TRIG, "C2d. the derived trigger is used");
	}
	{
		/* exactly at the boundary: still measurable, because the comparison is <= */
		const sample_window_t w = decide((uint16_t)LATEST, 1000, 1000, TRIG);
		CHECK(w.state == CURRENT_SAMPLE_PRIMARY, "C2e. a phase exactly at the limit still qualifies");
	}

	/* --- C3/C4/C5: which phases a sector owns --------------------------------------------- */
	{
		const sample_window_t w = decide((uint16_t)(LATEST + 1), 1000, 1000, TRIG);
		CHECK(w.state == CURRENT_SAMPLE_ALTERNATE, "C3a. one blocked phase is ALTERNATE");
		CHECK(w.sector == 0, "C3b. phase A is the reconstructed one");
		CHECK(w.direct_mask == (D_B | D_C), "C3c. B and C are direct - A = -(B+C)");
	}
	{
		const sample_window_t w = decide(1000, (uint16_t)(LATEST + 1), 1000, TRIG);
		CHECK(w.sector == 1, "C4a. phase B is the reconstructed one");
		CHECK(w.direct_mask == (D_A | D_C), "C4b. A and C are direct - B = -(A+C)");
	}
	{
		const sample_window_t w = decide(1000, 1000, (uint16_t)(LATEST + 1), TRIG);
		CHECK(w.sector == 2, "C5a. phase C is the reconstructed one");
		CHECK(w.direct_mask == (D_A | D_B), "C5b. A and B are direct - C = -(A+B)");
	}

	/* --- C8: two blocked phases -> INVALID, never a fabricated trigger --------------------- */
	{
		const sample_window_t w = decide((uint16_t)(LATEST + 1), (uint16_t)(LATEST + 1), 1000, TRIG);
		CHECK(w.state == CURRENT_SAMPLE_INVALID, "C8a. two unusable phases cannot be recovered");
		CHECK(w.sector == SAMPLE_WINDOW_RECONSTRUCT_NONE,
		      "C8b. ...and no reconstruction is claimed");
	}
	{
		const sample_window_t w = decide(ARR, ARR, ARR, TRIG);
		CHECK(w.state == CURRENT_SAMPLE_INVALID, "C8c. full duty on every phase is INVALID");
	}

	/* --- C6/C7: the returned trigger is ALWAYS legal, whatever the geometry ----------------- */
	{
		int illegal = 0, primary = 0, alternate = 0, invalid = 0;
		for (int32_t a = 0; a <= ARR; a += 37) {
			for (int32_t b = 0; b <= ARR; b += 53) {
				const sample_window_t w = decide((uint16_t)a, (uint16_t)b,
				                                 (uint16_t)((a + b) / 2), TRIG);
				if (w.state != CURRENT_SAMPLE_INVALID && w.trigger_ccr > ARR) illegal = 1;
				if (w.state == CURRENT_SAMPLE_PRIMARY) primary++;
				else if (w.state == CURRENT_SAMPLE_ALTERNATE) alternate++;
				else invalid++;
			}
		}
		CHECK(illegal == 0, "C6. across the whole geometry space, no illegal trigger is ever returned");
		CHECK(primary > 0 && alternate > 0 && invalid > 0,
		      "C7. all three states are reachable, so none of them is dead code");
	}

	/* --- an illegal trigger request is refused rather than clamped -------------------------- */
	{
		const sample_window_t w = decide(1000, 1000, 1000, (uint16_t)(ARR + 1));
		CHECK(w.state == CURRENT_SAMPLE_INVALID,
		      "C6b. a trigger the timer cannot match gives INVALID, not a silently moved sample");
	}
	{
		/* The value the old over-modulation branch would have programmed. */
		const sample_window_t w = decide(1000, 1000, 1000, 4942);
		CHECK(w.state == CURRENT_SAMPLE_INVALID, "C6c. 4942 - the old illegal CH3 - is refused");
	}
	{
		/* A trigger so low that no aperture fits before the counter reaches 0. */
		const sample_window_t w = decide(0, 0, 0, 100);
		CHECK(w.state == CURRENT_SAMPLE_INVALID,
		      "C6d. a trigger with no room for the acquisition aperture is INVALID");
	}

	/* --- C10: equal duties are deterministic ------------------------------------------------ */
	{
		const sample_window_t w1 = decide(2000, 2000, 2000, TRIG);
		const sample_window_t w2 = decide(2000, 2000, 2000, TRIG);
		CHECK(w1.state == w2.state && w1.sector == w2.sector && w1.direct_mask == w2.direct_mask,
		      "C10a. identical geometry gives an identical verdict - no history, no drift");
		CHECK(w1.state == CURRENT_SAMPLE_PRIMARY,
		      "C10b. a three-way tie below the limit is PRIMARY, not an arbitrary pick");
		const sample_window_t w3 = decide((uint16_t)(LATEST + 1), (uint16_t)(LATEST + 1),
		                                  (uint16_t)(LATEST + 1), TRIG);
		CHECK(w3.state == CURRENT_SAMPLE_INVALID,
		      "C10c. a three-way tie ABOVE the limit is INVALID, not a coin flip");
	}

	/* --- reconstruction: Kirchhoff, and only on the blocked phase ---------------------------- */
	{
		int16_t a = 0, b = 40, c = -30;
		sample_window_reconstruct(0, &a, &b, &c);
		CHECK(a == -10 && b == 40 && c == -30, "K1. sector 0 rebuilds A = -(B+C), others untouched");
	}
	{
		int16_t a = 40, b = 0, c = -30;
		sample_window_reconstruct(1, &a, &b, &c);
		CHECK(b == -10 && a == 40 && c == -30, "K2. sector 1 rebuilds B = -(A+C)");
	}
	{
		int16_t a = 40, b = -30, c = 0;
		sample_window_reconstruct(2, &a, &b, &c);
		CHECK(c == -10 && a == 40 && b == -30, "K3. sector 2 rebuilds C = -(A+B)");
	}
	{
		/* RECONSTRUCT_NONE must leave a good direct reading alone - overwriting it would replace
		 * a measurement with an arithmetic identity. */
		int16_t a = 11, b = 22, c = 33;
		sample_window_reconstruct(SAMPLE_WINDOW_RECONSTRUCT_NONE, &a, &b, &c);
		CHECK(a == 11 && b == 22 && c == 33, "K4. PRIMARY reconstructs nothing");
	}

	/* --- defensive ---------------------------------------------------------------------------- */
	{
		sample_window_t w;
		sample_window_decide(NULL, ARR, TRIG, &w);
		CHECK(w.state == CURRENT_SAMPLE_INVALID, "N1. a null geometry gives INVALID");
		sample_window_decide(NULL, ARR, TRIG, NULL);   /* must not crash */
		sample_window_reconstruct(0, NULL, NULL, NULL);
	}

	if (host_test_failures == 0) {
		printf("FW-127C sampling window: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-127C sampling window: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
