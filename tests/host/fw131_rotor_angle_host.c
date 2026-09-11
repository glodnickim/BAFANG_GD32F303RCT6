/*
 * FW-131 / FW-131.1: the rotor angle must be CONTINUOUS across the change of formula, and it must
 * not pretend to know things it has not measured.
 *
 * Links the real src/rotor_angle.c and sweeps a rotor through the speed range where the legacy
 * code switched between its two angle formulas. The legacy behaviour is modelled alongside, so the
 * improvement is a measured number rather than a claim.
 *
 *   L0  LEGACY BASELINE   the old pair of formulas really does step, and by how much. This is the
 *                         click, quantified, before anything is fixed.
 *   T1  ONE ZERO          every state is (hall + angle_correction) + an offset; no state invents
 *                         its own zero, and angle_correction reaches all of them.
 *   T2  CENTRE, NOT EDGE  (FW-131.1) the untrusted answer is the sector CENTRE. A standstill on the
 *                         boundary is wrong by up to a whole sector; the centre halves that, which
 *                         is why the manufacturer uses it and why the pre-FW-131 code did too.
 *   T3  LEARNING GATE     (FW-131.1) interpolation is refused until two Hall edges have been seen
 *                         since the anchor. One stale period left over from before a standstill is
 *                         not a speed.
 *   T4  BUMPLESS BOTH WAYS  gaining and losing trust produce no step.
 *   T5  BOUNDED           the offset never runs past its own sector, however late the edge is.
 *   T6  STALL             a stopped rotor drops to the centre at once and must re-earn trust.
 *   T7  DIRECTION         the centre follows the MEASURED direction once one has been seen, and the
 *                         caller's fallback sign only ever applies before that.
 */

#include "../common/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rotor_angle.h"

#define HALL_ANGLE        1000000000
#define ANGLE_CORRECTION    71582790   /* 6 deg, the FW-022 calibration result */
#define PERIOD                  8000   /* Hall-timer ticks per sector */

static double q31_to_deg(int32_t v)
{
	return (double)v * 360.0 / 4294967296.0;
}

static int32_t abs32(int32_t v) { return (v < 0) ? -v : v; }

/*
 * The LEGACY pair, transcribed from main.c before FW-131. reverse is the configured MP.reverse;
 * direction is the measured one. The whole point is that these two are independent.
 */
static int32_t legacy_angle(uint32_t tim2, uint32_t period, int32_t direction,
	int32_t reverse, bool sixstep)
{
	if (sixstep) {
		return HALL_ANGLE - reverse * ROTOR_ANGLE_DEG_30;   /* NOTE: no angle_correction */
	}
	int32_t mag = (int32_t)(((uint32_t)10923 * tim2) / period) << 16;
	return HALL_ANGLE + ANGLE_CORRECTION + direction * mag;
}

static rotor_angle_input_t make_input(uint32_t tim2, uint32_t period, int32_t direction,
	bool want_untrusted, bool stalled)
{
	rotor_angle_input_t in;
	memset(&in, 0, sizeof in);
	in.hall_angle = HALL_ANGLE;
	in.angle_correction = ANGLE_CORRECTION;
	in.direction = direction;
	in.tim2_recent = tim2;
	in.tics_filtered_8 = period << 3;
	in.want_untrusted = want_untrusted;
	in.stalled = stalled;
	in.fallback_sign = 1;      /* legacy -MP.reverse for the usual configuration */
	return in;
}

/*
 * Run whole sectors so the module can count the Hall edges it needs. tim2 sweeps 0..period and
 * restarts, which is exactly what the hardware timer does at every edge.
 */

static void spin(rotor_angle_state_t *st, int sectors, int32_t direction,
	bool want_untrusted, int32_t *last_theta)
{
	for (int s = 0; s < sectors; s++) {
		for (uint32_t t = 0; t <= PERIOD; t += 100) {
			rotor_angle_input_t in =
				make_input(t, PERIOD, direction, want_untrusted, false);
			int32_t theta = rotor_angle_update(st, &in);
			if (last_theta) {
				*last_theta = theta;
			}
		}
	}
}

/* Largest single-tick jump while sweeping one sector across a change of trust. */
static int32_t sweep_max_step(bool want_untrusted_after, int32_t direction)
{
	rotor_angle_state_t st;
	rotor_angle_reset(&st);

	/* Earn trust and settle into the state we are switching FROM. */
	spin(&st, 4, direction, !want_untrusted_after, NULL);

	int32_t prev = 0, max_step = 0;
	bool have_prev = false;
	for (uint32_t t = 0; t <= PERIOD; t += 10) {
		rotor_angle_input_t in =
			make_input(t, PERIOD, direction, want_untrusted_after, false);
		int32_t theta = rotor_angle_update(&st, &in);
		if (have_prev) {
			int32_t step = abs32(theta - prev);
			if (step > max_step) {
				max_step = step;
			}
		}
		prev = theta;
		have_prev = true;
	}
	return max_step;
}

int main(void)
{
	printf("FW-131/131.1 canonical rotor angle - continuity, sector centre, learning gate\n");

	/* ==================================================================
	 * L0: what the legacy pair actually did. This is the click, measured.
	 * ================================================================== */
	{
		int32_t worst = 0;
		uint32_t worst_at = 0;
		for (uint32_t t = 0; t <= PERIOD; t += 10) {
			int32_t fast = legacy_angle(t, PERIOD, +1, -1, false);
			int32_t slow = legacy_angle(t, PERIOD, +1, -1, true);
			int32_t step = abs32(fast - slow);
			if (step > worst) { worst = step; worst_at = t; }
		}
		int32_t at_edge = abs32(legacy_angle(0, PERIOD, +1, -1, false) -
			legacy_angle(0, PERIOD, +1, -1, true));
		printf("  L0 legacy step: worst %.1f deg (at %u/%u), and %.1f deg even ON a Hall edge\n",
			q31_to_deg(worst), worst_at, (unsigned)PERIOD, q31_to_deg(at_edge));
		CHECK(worst > ROTOR_ANGLE_DEG_30,
			"L0: the legacy pair steps by more than 30 deg somewhere in the sector");
		CHECK(at_edge > 0,
			"L0: and the two do NOT agree even on a Hall edge - there was no safe moment to switch");
	}

	/* ==================================================================
	 * T1: one zero, shared by every state.
	 * ================================================================== */
	{
		rotor_angle_state_t st;
		rotor_angle_reset(&st);
		rotor_angle_input_t in = make_input(0, PERIOD, +1, true, false);
		int32_t theta = rotor_angle_update(&st, &in);

		rotor_angle_input_t no_corr = in;
		no_corr.angle_correction = 0;
		rotor_angle_state_t st2;
		rotor_angle_reset(&st2);
		int32_t theta2 = rotor_angle_update(&st2, &no_corr);
		CHECK(theta - theta2 == ANGLE_CORRECTION,
			"T1: angle_correction moves the untrusted state exactly as much as any other");
	}

	/* ==================================================================
	 * T2 (FW-131.1): the untrusted answer is the sector CENTRE, not the boundary.
	 * ================================================================== */
	{
		rotor_angle_state_t st;
		rotor_angle_reset(&st);
		rotor_angle_input_t in = make_input(0, PERIOD, +1, true, false);
		int32_t theta = rotor_angle_update(&st, &in);
		int32_t offset = theta - (HALL_ANGLE + ANGLE_CORRECTION);
		printf("  T2 standstill offset: %.1f deg (centre is %.1f, boundary is 0.0)\n",
			q31_to_deg(offset), q31_to_deg(ROTOR_ANGLE_DEG_30));
		CHECK(offset == ROTOR_ANGLE_DEG_30,
			"T2: a fresh state answers with the sector CENTRE - halves the worst-case error");
		CHECK(offset != 0,
			"T2: and specifically NOT the boundary, which FW-131 v1 used and which can be a whole sector out");
	}

	/* ==================================================================
	 * T3 (FW-131.1): the learning gate. One stale period is not a speed.
	 * ================================================================== */
	{
		rotor_angle_state_t st;
		rotor_angle_reset(&st);
		/*
		 * Exactly the situation after a standstill: the speed hysteresis is happy (a stale period
		 * from before the stop looks fast) and the interpolation would sweep a whole sector - but
		 * no edge has been seen yet, so none of it may be believed.
		 */
		int32_t theta_at_half = 0;
		for (uint32_t t = 0; t <= PERIOD; t += 100) {
			rotor_angle_input_t in = make_input(t, PERIOD, +1, false, false);
			int32_t theta = rotor_angle_update(&st, &in);
			if (t == PERIOD / 2U) {
				theta_at_half = theta;
			}
		}
		CHECK(st.trusted == 0U,
			"T3: after a reset the first sector is NOT interpolated - no edges have been counted");
		CHECK(theta_at_half == HALL_ANGLE + ANGLE_CORRECTION + ROTOR_ANGLE_DEG_30,
			"T3: it answers with the centre throughout, whatever the stale period suggests");

		/* Second sector: one edge counted. Still not enough. */
		for (uint32_t t = 0; t <= PERIOD; t += 100) {
			rotor_angle_input_t in = make_input(t, PERIOD, +1, false, false);
			(void)rotor_angle_update(&st, &in);
		}
		CHECK(st.edges >= 1U, "T3: the module counted a Hall edge (setup)");

		/* By the third sector two edges have been seen and trust is allowed. */
		bool became_trusted = false;
		for (uint32_t t = 0; t <= PERIOD; t += 100) {
			rotor_angle_input_t in = make_input(t, PERIOD, +1, false, false);
			(void)rotor_angle_update(&st, &in);
			if (st.trusted) {
				became_trusted = true;
			}
		}
		CHECK(became_trusted,
			"T3: once two edges have been seen the interpolation is finally allowed");
	}

	/* ==================================================================
	 * T4: bumpless in both directions.
	 * ================================================================== */
	{
		int32_t down = sweep_max_step(true, +1);
		int32_t up = sweep_max_step(false, +1);
		int32_t tolerance = ROTOR_ANGLE_DEG_60 / 60;   /* 1 electrical degree */
		printf("  T4 losing trust: %.3f deg   gaining trust: %.3f deg\n",
			q31_to_deg(down), q31_to_deg(up));
		CHECK(down < tolerance,
			"T4: losing trust at the midpoint produces no step");
		CHECK(up < tolerance,
			"T4: gaining trust at the midpoint produces no step either");
	}

	/* ==================================================================
	 * T5: bounded extrapolation.
	 * ================================================================== */
	{
		rotor_angle_input_t late = make_input(PERIOD * 50U, PERIOD, +1, false, false);
		CHECK(rotor_angle_sector_offset(&late) == ROTOR_ANGLE_DEG_60,
			"T5: an edge 50 sectors late still leaves the offset at exactly one sector");
		rotor_angle_input_t half = make_input(PERIOD / 2U, PERIOD, +1, false, false);
		CHECK(abs32(rotor_angle_sector_offset(&half) - ROTOR_ANGLE_DEG_30) <
			ROTOR_ANGLE_DEG_60 / 100,
			"T5: half a sector of time is half a sector of angle - the bumpless point");
	}

	/* ==================================================================
	 * T6: a stall drops to the centre at once and trust must be re-earned.
	 * ================================================================== */
	{
		rotor_angle_state_t st;
		rotor_angle_reset(&st);
		spin(&st, 4, +1, false, NULL);
		CHECK(st.trusted == 1U, "T6: interpolating before the stall (setup)");

		rotor_angle_input_t stalled = make_input(PERIOD / 4U, PERIOD, +1, true, true);
		int32_t theta = rotor_angle_update(&st, &stalled);
		CHECK(st.trusted == 0U,
			"T6: a stall gives up at once - no edge is coming to reach the midpoint");
		int32_t before = theta;
		for (int i = 0; i < 400; ++i) {
			theta = rotor_angle_update(&st, &stalled);
			CHECK(abs32((int32_t)((uint32_t)theta - (uint32_t)before)) <= ROTOR_ANGLE_TRANSFER_STEP,
				"T6: fallback is rate limited even when no midpoint is coming");
			before = theta;
		}
		CHECK(theta == HALL_ANGLE + ANGLE_CORRECTION + ROTOR_ANGLE_DEG_30,
			"T6: fallback settles at the sector centre without an instantaneous step");
		CHECK(st.edges == 0U,
			"T6: the edge count is cleared, so the next spin-up has to earn trust again");
	}

	/* ==================================================================
	 * T7: the centre follows the measured direction; the fallback is cold-start only.
	 * ================================================================== */
	{
		rotor_angle_state_t st;
		rotor_angle_reset(&st);
		rotor_angle_input_t rev = make_input(0, PERIOD, -1, true, false);
		int32_t theta = rotor_angle_update(&st, &rev);
		int32_t offset = theta - (HALL_ANGLE + ANGLE_CORRECTION);
		CHECK(offset == -ROTOR_ANGLE_DEG_30,
			"T7: a measured reverse direction flips the centre, overriding the fallback sign");

		/* Direction goes unknown (standstill): the LATCHED one must still rule. */
		rotor_angle_input_t unknown = make_input(0, PERIOD, 0, true, false);
		int32_t held = rotor_angle_update(&st, &unknown) - (HALL_ANGLE + ANGLE_CORRECTION);
		CHECK(held == -ROTOR_ANGLE_DEG_30,
			"T7: a standstill keeps the sign it last measured, it does not revert to the fallback");

		/* A cold state that has never measured anything uses the caller's sign. */
		rotor_angle_state_t cold;
		rotor_angle_reset(&cold);
		rotor_angle_input_t cold_in = make_input(0, PERIOD, 0, true, false);
		cold_in.fallback_sign = -1;
		int32_t cold_off = rotor_angle_update(&cold, &cold_in) -
			(HALL_ANGLE + ANGLE_CORRECTION);
		CHECK(cold_off == -ROTOR_ANGLE_DEG_30,
			"T7: before any measurement the caller's legacy sign is used, so a cold boot is unchanged");
	}

	/* ==================================================================
	 * T8: THE EDGE RACE (FW-131.2). The production path supplies Hall sequence numbers, and the
	 * timer restart reaches the FOC loop from the HARDWARE while the sequence number and the
	 * sector angle are published by the Hall ISR. A FOC tick landing between the two must not be
	 * read as a lost measurement.
	 *
	 * Ideal Hall signals, ~208 ERPS, every fifth edge served late. Before FW-131.2 this cost 48
	 * losses of trust and a worst angle error of 179.75 degrees, because each spurious flip
	 * latched a transfer_offset for a sector that was about to arrive anyway.
	 * ================================================================== */
	{
		#define RACE_SECTORS      60U
		#define TICKS_PER_SECTOR  13U    /* 801us sector at 16 kHz FOC = 208 ERPS */
		#define RACE_DELAY_TICKS   2U    /* how late the Hall ISR is on the delayed edges */

		rotor_angle_state_t st;
		rotor_angle_reset(&st);

		const uint32_t warmup = 3U * TICKS_PER_SECTOR;   /* the learning gate needs its edges */
		int losses = 0;
		int32_t worst = 0;
		uint32_t held_ticks = 0U;

		for (uint32_t n = 0; n < RACE_SECTORS * TICKS_PER_SECTOR; n++) {
			const uint32_t phys_sector = n / TICKS_PER_SECTOR;
			const uint32_t k = n % TICKS_PER_SECTOR;
			const uint32_t tim2 = (PERIOD * k) / TICKS_PER_SECTOR;

			/* The hardware timer has already restarted; on every fifth edge the ISR has not
			 * caught up yet, so the sequence and the angle still name the previous sector. */
			const bool late = (phys_sector % 5U) == 0U && phys_sector > 0U;
			const uint32_t pub = (late && k < RACE_DELAY_TICKS) ? phys_sector - 1U : phys_sector;
			if (late && k < RACE_DELAY_TICKS) held_ticks++;

			rotor_angle_input_t in = make_input(tim2, PERIOD, +1, false, false);
			in.hall_angle = (int32_t)((uint32_t)HALL_ANGLE +
				pub * (uint32_t)ROTOR_ANGLE_DEG_60);
			in.hall_sequence = pub;
			in.hall_sequence_valid = true;

			const uint8_t before = st.trusted;
			const int32_t theta = rotor_angle_update(&st, &in);

			if (n >= warmup) {
				if (before == 1U && st.trusted == 0U) losses++;
				if (st.trusted) {
					/* Where the rotor actually is. q31 wraps, so this is uint32 arithmetic. */
					const uint32_t truth = (uint32_t)HALL_ANGLE + (uint32_t)ANGLE_CORRECTION +
						phys_sector * (uint32_t)ROTOR_ANGLE_DEG_60 +
						(uint32_t)(((uint64_t)ROTOR_ANGLE_DEG_60 * k) / TICKS_PER_SECTOR);
					const int32_t err = abs32((int32_t)((uint32_t)theta - truth));
					if (err > worst) worst = err;
				}
			}
		}

		printf("    T8: %u raced edges, %d trust losses, worst angle error %.2f deg\n",
			(unsigned)held_ticks, losses, q31_to_deg(worst));

		CHECK(losses == 0,
			"T8: an ISR that has not yet published the new sector is a race, not a lost measurement");
		CHECK(worst < ROTOR_ANGLE_DEG_30,
			"T8: holding across the race keeps the angle inside half a sector, not a sector away");

		/*
		 * The protection the original condition was written for must survive. A restart that no
		 * sequence ever explains is a 16-bit rollover or a dead Hall, and past the bound it still
		 * loses trust - it just waits long enough to be sure.
		 */
		rotor_angle_input_t roll = make_input(1U, PERIOD, +1, false, false);
		roll.hall_sequence = st.prev_hall_sequence;   /* nothing new is ever published */
		roll.hall_sequence_valid = true;
		CHECK(st.trusted == 1U, "T8: interpolating before the rollover (setup)");
		for (uint32_t i = 0; i <= ROTOR_ANGLE_EDGE_PENDING_TICKS + 1U; i++) {
			roll.tim2_recent = 1U;   /* restarted and staying restarted: no edge explains it */
			(void)rotor_angle_update(&st, &roll);
		}
		CHECK(st.trusted == 0U,
			"T8: a restart no sequence ever explains still loses trust once the bound is spent");
		CHECK(st.edges == 0U,
			"T8: and it clears the edge history, so the next spin-up re-earns trust");

		/*
		 * T9: A MEASURED STALL OUTRANKS A PENDING EDGE. The hold may only ever cover the ABSENCE
		 * of information. input->stalled is the opposite: the 4 kHz layer has measured that the
		 * rotor stopped. The first version of this patch held across it, so a standing rotor kept
		 * trusted = 1 and its edge history - precisely what the learning gate exists to refuse.
		 */
		rotor_angle_state_t sst;
		rotor_angle_reset(&sst);
		for (uint32_t n = 0; n < 5U * TICKS_PER_SECTOR; n++) {
			const uint32_t ps = n / TICKS_PER_SECTOR;
			const uint32_t k = n % TICKS_PER_SECTOR;
			rotor_angle_input_t in = make_input((PERIOD * k) / TICKS_PER_SECTOR,
				PERIOD, +1, false, false);
			in.hall_angle = (int32_t)((uint32_t)HALL_ANGLE + ps * (uint32_t)ROTOR_ANGLE_DEG_60);
			in.hall_sequence = ps;
			in.hall_sequence_valid = true;
			(void)rotor_angle_update(&sst, &in);
		}
		CHECK(sst.trusted == 1U, "T9: interpolating before the stall (setup)");

		/* The rotor stops INSIDE a race window: the timer has restarted, the ISR has not published
		 * the new sector, and the stall is reported in the same tick. */
		rotor_angle_input_t stall = make_input(1U, PERIOD, +1, false, true);
		stall.hall_angle = (int32_t)((uint32_t)HALL_ANGLE +
			sst.prev_hall_sequence * (uint32_t)ROTOR_ANGLE_DEG_60);
		stall.hall_sequence = sst.prev_hall_sequence;   /* unchanged: the ISR is late */
		stall.hall_sequence_valid = true;
		(void)rotor_angle_update(&sst, &stall);
		CHECK(sst.trusted == 0U,
			"T9: a measured stall is information, so it is never held across");
		CHECK(sst.edges == 0U,
			"T9: and it clears the edge history exactly like any other lost timing");

		#undef RACE_SECTORS
		#undef TICKS_PER_SECTOR
		#undef RACE_DELAY_TICKS
	}

	if (host_test_failures == 0) {
		printf("All FW-131/131.1/131.2 rotor angle checks passed.\n");
		return 0;
	}
	printf("\n%d FW-131/131.1/131.2 rotor angle check(s) FAILED.\n", host_test_failures);
	return 1;
}
