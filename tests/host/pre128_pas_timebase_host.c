/*
 * PRE-FW128: the PAS timebase, against the REAL production modules.
 *
 * WHY THE OLD TESTS COULD NOT CATCH THIS. Everything that tested cadence until now started from
 * "a forward step happened" and handed that straight to the layer above. The defect was one
 * level below: the production code decided WHETHER a step had happened, and HOW LONG AGO, from
 * inside a main-loop function whose call rate is not the sampling rate. A test that supplies
 * steps cannot find a bug in the code that finds steps.
 *
 * So this harness starts at the pins. It generates a real quadrature waveform, samples it at
 * 4 kHz through the REAL src/pas_sampler.c, drains the events the way main.c drains them, and
 * feeds them to the REAL src/pas_cadence.c. Nothing about the decode or the period arithmetic is
 * re-implemented here - if it were, the test would agree with itself rather than with the
 * firmware.
 *
 * SCENARIO B IS THE POINT OF THE CARD. The same waveform is run with the "main loop" serviced
 * every tick, every 2, every 5, every 10 and at a pseudo-random rate. The cadence must come out
 * bit-identical. Under the old architecture it could not: the period was a count of main-loop
 * passes, so servicing it half as often halved the period and doubled the reported rpm.
 */

#include "common/check.h"
#include "../../inc/pas_sampler.h"
#include "../../inc/pas_cadence.h"
#include "../../inc/config.h"

#include <stdio.h>
#include <string.h>

/* --- the crank: a real quadrature generator ------------------------------------------------
 * Gray code, so exactly one line changes per step - which is what makes an illegal two-bit jump
 * detectable at all.
 *
 * The ORDER is 0,2,3,1 and not the textbook 0,1,3,2: with the shipped PAS_DIR_SIGN = -1 the
 * production table calls the latter REVERSE. Check 0b below asserts the direction against the
 * real decoder instead of trusting this comment - the first version of this fixture had it the
 * other way round and produced a run of perfectly clean reverse steps.
 */
static const uint8_t GRAY[4] = {0u, 2u, 3u, 1u};

typedef struct {
	int idx;          /* position in GRAY                                    */
	uint32_t acc;     /* fixed-point transition accumulator, 16 fractional bits */
	uint32_t step_q16;/* transitions per tick, Q16                            */
} crank_t;

static void crank_init(crank_t *c, uint32_t rpm)
{
	c->idx = 0;
	c->acc = 0u;
	/* transitions/s = rpm/60 * PAS_TRANSITIONS_PER_REV ; per tick = that / CONTROL_TIMEBASE_HZ */
	c->step_q16 = (uint32_t)(((uint64_t)rpm * PAS_TRANSITIONS_PER_REV * 65536u)
	                         / (60u * CONTROL_TIMEBASE_HZ));
}

/* Advance one 4 kHz tick; returns the line state to present to the sampler. */
static uint8_t crank_tick(crank_t *c)
{
	c->acc += c->step_q16;
	while (c->acc >= 65536u) { c->acc -= 65536u; c->idx = (c->idx + 1) & 3; }
	return GRAY[c->idx];
}

/* --- the consumer: exactly what main.c does with the drained events ------------------------- */
typedef struct {
	uint16_t cadence;      /* MS.cadence                                     */
	uint32_t measurements; /* how many raw readings were published           */
	uint32_t forwards, reverses, invalids;
	uint32_t idle_ticks;   /* main.c's derived pas_idle_ticks                */
} consumer_t;

/*
 * One "main loop pass" at tick `now`: drain every queued event and apply it. This mirrors
 * main.c's drain loop, including the FW-086 restart rule and the epoch breaks. It deliberately
 * does NOT re-implement the period arithmetic - that is pas_cadence.c's, called here.
 */
static void main_pass(consumer_t *m, uint32_t now, int fwd_run_is_zero)
{
	pas_step_event_t ev;
	while (pas_sampler_pop(&ev)) {
		if (ev.step > 0) {
			pas_cadence_step_t cad;
			m->forwards++;
			cad = pas_cadence_forward_step(ev.tick, (uint8_t)(fwd_run_is_zero ? 1 : 0));
			/* after the first forward step the run is no longer zero, exactly as fwd_run */
			fwd_run_is_zero = 0;
			if (cad.pulse && cad.measured) { m->cadence = cad.rpm; m->measurements++; }
		} else if (ev.step < 0) {
			m->reverses++;
			pas_cadence_break_epoch(0);
		} else {
			m->invalids++;
			pas_cadence_break_epoch(1);
		}
	}
	if (pas_sampler_take_overflow()) pas_cadence_break_epoch(2);
	m->idle_ticks = now - pas_sampler_last_transition_tick();
}

/* Run `ticks` of a steady crank, servicing "main" every `service` ticks. */
static void run_steady(consumer_t *m, uint32_t rpm, uint32_t ticks, uint32_t service,
                       uint32_t start_tick)
{
	crank_t c;
	uint32_t t;
	int fwd_run_zero = 1;
	crank_init(&c, rpm);
	memset(m, 0, sizeof(*m));
	for (t = 0; t < ticks; t++) {
		uint32_t now = start_tick + t;
		pas_sampler_isr_tick(crank_tick(&c), now);
		if ((t % service) == 0u) {
			main_pass(m, now, fwd_run_zero);
			if (m->forwards > 0) fwd_run_zero = 0;
		}
	}
}

static void reset_all(uint32_t tick)
{
	pas_sampler_init(tick);
	pas_cadence_reset();
}

int main(void)
{
	/* --- 0. the fixture itself is a legal forward waveform ---------------------------------- */
	{
		reset_all(0u);
		pas_sampler_isr_tick(GRAY[0], 0u);
		pas_sampler_isr_tick(GRAY[1], 10u);
		{
			pas_step_event_t ev;
			CHECK(pas_sampler_pop(&ev) == 1, "0a. a line change produces exactly one event");
			CHECK(ev.step > 0, "0b. the test's Gray order really is FORWARD for the shipped PAS_DIR_SIGN");
			CHECK(ev.tick == 10u, "0c. the event carries the tick it was OBSERVED on");
			CHECK(pas_sampler_pop(&ev) == 0, "0d. and nothing else is queued");
		}
	}

	/* --- 1. the formula is derived, not a magic number -------------------------------------- */
	{
		CHECK(PAS_CADENCE_RPM_NUMERATOR == 10000u,
		      "1a. steps*60*tick_rate/transitions_per_rev = 4*60*4000/96 = 10000, the shipped constant");
		CHECK(PAS_TRANSITIONS_PER_REV == 96u && PAS_STEPS_PER_PULSE == 4,
		      "1b. ...and it is derived from these two, so changing either cannot silently rescale rpm");
	}

	/* --- A. STEADY CADENCE ------------------------------------------------------------------ */
	{
		static const uint32_t rpms[5] = {40u, 60u, 80u, 100u, 120u};
		int i;
		for (i = 0; i < 5; i++) {
			consumer_t m;
			int err;
			reset_all(0u);
			/* four seconds is many pulses at every rate tested */
			run_steady(&m, rpms[i], 4u * CONTROL_TIMEBASE_HZ, 1u, 0u);
			err = (int)m.cadence - (int)rpms[i];
			if (err < 0) err = -err;
			CHECK(m.invalids == 0u, "A1. a clean quadrature waveform produces no illegal transitions");
			CHECK(m.reverses == 0u, "A2. ...and no reverse steps");
			CHECK(m.measurements > 0u, "A3. ...and real cadence measurements are published");
			CHECK(err <= 2, "A4. the raw rpm matches the crank within fixed-point rounding");
			printf("      [A] %3u rpm -> reported %3u  (fwd %u, meas %u)\n",
			       (unsigned)rpms[i], (unsigned)m.cadence,
			       (unsigned)m.forwards, (unsigned)m.measurements);
		}
	}

	/* --- B. MAIN-LOOP COALESCING - THE REGRESSION TEST FOR THIS CARD ------------------------ */
	{
		static const uint32_t services[5] = {1u, 2u, 5u, 10u, 37u};
		consumer_t ref;
		int i, all_same = 1, all_fwd_same = 1;
		reset_all(0u);
		run_steady(&ref, 75u, 4u * CONTROL_TIMEBASE_HZ, 1u, 0u);
		for (i = 0; i < 5; i++) {
			consumer_t m;
			reset_all(0u);
			run_steady(&m, 75u, 4u * CONTROL_TIMEBASE_HZ, services[i], 0u);
			if (m.cadence != ref.cadence) all_same = 0;
			if (m.forwards != ref.forwards) all_fwd_same = 0;
			printf("      [B] service every %2u ticks -> cadence %u (ref %u), fwd %u (ref %u)\n",
			       (unsigned)services[i], (unsigned)m.cadence, (unsigned)ref.cadence,
			       (unsigned)m.forwards, (unsigned)ref.forwards);
		}
		CHECK(all_same,
		      "B1. cadence is IDENTICAL at every main-loop service rate - the defect this card fixes");
		CHECK(all_fwd_same,
		      "B2. ...and not one physical transition is lost, however late the loop is");
	}

	/* --- C. ILLEGAL TRANSITION -------------------------------------------------------------- */
	{
		consumer_t m;
		uint32_t before;
		uint32_t t;
		crank_t c;
		reset_all(0u);
		memset(&m, 0, sizeof(m));
		/* settle a real 60 rpm cadence first */
		crank_init(&c, 60u);
		for (t = 0; t < 2u * CONTROL_TIMEBASE_HZ; t++) {
			pas_sampler_isr_tick(crank_tick(&c), t);
			main_pass(&m, t, (t == 0u) ? 1 : 0);
		}
		before = m.measurements;
		CHECK(m.cadence > 55u && m.cadence < 65u, "C0. a real 60 rpm reading is standing before the fault");

		/* now force a two-bit jump: from the current state to its diagonal opposite */
		{
			uint8_t cur = pas_sampler_state();
			uint8_t bad = (uint8_t)(cur ^ 0x03u);   /* both lines change at once - impossible */
			pas_sampler_isr_tick(bad, t);
			main_pass(&m, t, 0);
			t++;
		}
		CHECK(m.invalids == 1u, "C1. the illegal transition is counted, not silently dropped");
		CHECK(m.measurements == before,
		      "C2. ...and it publishes no cadence of its own - no invented value, high or low");
		CHECK(pas_cadence_get()->broken_invalid == 1u,
		      "C3. ...and the interval being assembled is voided, with the cause recorded");

		/* a clean forward sequence afterwards recovers a correct reading */
		{
			uint32_t recovered_at = 0u;
			crank_t c2;
			crank_init(&c2, 60u);
			/* resynchronise the generator to the line state the fault left behind */
			{
				int k; uint8_t cur = pas_sampler_state();
				for (k = 0; k < 4; k++) if (GRAY[k] == cur) c2.idx = k;
			}
			for (; t < 6u * CONTROL_TIMEBASE_HZ; t++) {
				pas_sampler_isr_tick(crank_tick(&c2), t);
				main_pass(&m, t, 0);
				if (m.measurements > before && recovered_at == 0u) recovered_at = t;
			}
			CHECK(m.measurements > before, "C4. a clean forward sequence recovers cadence");
			CHECK(m.cadence > 55u && m.cadence < 65u, "C5. ...and recovers it CORRECTLY, not high");
			printf("      [C] invalid at tick %u, cadence recovered at %u, reading %u\n",
			       (unsigned)(2u * CONTROL_TIMEBASE_HZ), (unsigned)recovered_at,
			       (unsigned)m.cadence);
		}
	}

	/* --- D. REVERSE IN THE MIDDLE OF AN INTERVAL -------------------------------------------- */
	{
		consumer_t m;
		crank_t c;
		uint32_t t;
		uint32_t before;
		reset_all(0u);
		memset(&m, 0, sizeof(m));
		crank_init(&c, 60u);
		for (t = 0; t < 2u * CONTROL_TIMEBASE_HZ; t++) {
			pas_sampler_isr_tick(crank_tick(&c), t);
			main_pass(&m, t, (t == 0u) ? 1 : 0);
		}
		before = m.measurements;

		/* one single backward step, then forward again from there */
		{
			int k; uint8_t cur = pas_sampler_state(); int idx = 0;
			for (k = 0; k < 4; k++) if (GRAY[k] == cur) idx = k;
			pas_sampler_isr_tick(GRAY[(idx + 3) & 3], t);   /* one step BACK */
			main_pass(&m, t, 0);
			t++;
			CHECK(m.reverses == 1u, "D1. the reverse step is decoded as reverse");
			CHECK(pas_cadence_get()->broken_reverse == 1u,
			      "D2. ...and the interval spanning it is discarded, with the cause recorded");
			CHECK(m.measurements == before, "D3. no rpm is published across the direction change");
			c.idx = (idx + 3) & 3;
		}
		/* and the next clean measurement is correct, not a blend of before and after */
		for (; t < 6u * CONTROL_TIMEBASE_HZ; t++) {
			pas_sampler_isr_tick(crank_tick(&c), t);
			main_pass(&m, t, 0);
		}
		CHECK(m.measurements > before, "D4. cadence resumes after the reverse");
		CHECK(m.cadence > 55u && m.cadence < 65u,
		      "D5. ...and the first reading after it is the true cadence, not one spanning the reverse");
	}

	/* --- E. STOP, on the real clock --------------------------------------------------------- */
	{
		consumer_t m;
		crank_t c;
		uint32_t t, stop_tick;
		reset_all(0u);
		memset(&m, 0, sizeof(m));
		crank_init(&c, 60u);
		for (t = 0; t < 2u * CONTROL_TIMEBASE_HZ; t++) {
			pas_sampler_isr_tick(crank_tick(&c), t);
			main_pass(&m, t, (t == 0u) ? 1 : 0);
		}
		stop_tick = pas_sampler_last_transition_tick();
		/* crank frozen; service main only every 50 ticks to prove the timeout does not care */
		for (; t < 4u * CONTROL_TIMEBASE_HZ; t++) {
			uint8_t held = pas_sampler_state();
			pas_sampler_isr_tick(held, t);
			if ((t % 50u) == 0u) main_pass(&m, t, 0);
		}
		CHECK(m.idle_ticks == (uint32_t)(((4u * CONTROL_TIMEBASE_HZ - 1u) / 50u) * 50u) - stop_tick,
		      "E1. idle time is measured from the last real edge to now, not counted in main");
		CHECK(m.idle_ticks > PAS_STOP_TICKS,
		      "E2. ...and it passes the stop threshold on real elapsed time");
		printf("      [E] last edge at tick %u, idle at last service = %u (threshold %u)\n",
		       (unsigned)stop_tick, (unsigned)m.idle_ticks, (unsigned)PAS_STOP_TICKS);
	}

	/* --- F. UINT32 WRAP ---------------------------------------------------------------------- */
	{
		consumer_t m;
		uint32_t base = 0xFFFFFF00u;   /* the run crosses 0xFFFFFFFF -> 0 */
		int err;
		reset_all(base);
		run_steady(&m, 60u, 4u * CONTROL_TIMEBASE_HZ, 1u, base);
		err = (int)m.cadence - 60;
		if (err < 0) err = -err;
		CHECK(err <= 2, "F1. cadence is unaffected by the tick counter wrapping through zero");
		CHECK(m.invalids == 0u, "F2. ...and the wrap invents no illegal transitions");
		printf("      [F] across the uint32 wrap: %u rpm\n", (unsigned)m.cadence);
	}

	/* --- G. A VERY BUSY MAIN LOOP ------------------------------------------------------------ */
	{
		/*
		 * The strongest form of B: main is serviced at an irregular, sometimes very long
		 * interval. The physical period must be untouched as long as the ISR keeps running -
		 * and where the stall is long enough to fill the queue, the loss must be REPORTED
		 * (overflow) rather than quietly changing the reading.
		 */
		consumer_t m;
		crank_t c;
		uint32_t t, seed = 0x1234567u;
		int fwd_run_zero = 1;
		reset_all(0u);
		memset(&m, 0, sizeof(m));
		crank_init(&c, 90u);
		for (t = 0; t < 6u * CONTROL_TIMEBASE_HZ; t++) {
			pas_sampler_isr_tick(crank_tick(&c), t);
			seed = seed * 1103515245u + 12345u;
			if (((seed >> 16) & 0x1Fu) == 0u) {         /* service ~1 tick in 32, at random */
				main_pass(&m, t, fwd_run_zero);
				if (m.forwards > 0) fwd_run_zero = 0;
			}
		}
		main_pass(&m, t, 0);
		CHECK(m.invalids == 0u,
		      "G1. an irregular, badly starved main loop still sees a legal quadrature sequence");
		{
			int err = (int)m.cadence - 90;
			if (err < 0) err = -err;
			CHECK(err <= 2, "G2. ...and the cadence is still the crank's, not the scheduler's");
		}
		printf("      [G] starved irregular service at 90 rpm -> %u rpm, overflow epochs %u\n",
		       (unsigned)m.cadence, (unsigned)pas_cadence_get()->broken_overflow);
	}

	/* --- H. the queue overflow path is real and reported ------------------------------------ */
	{
		/* Never drained: 40 edges into a 32-slot ring must overflow, and say so. */
		uint32_t t;
		crank_t c;
		reset_all(0u);
		crank_init(&c, 60u);
		for (t = 0; t < 4u * CONTROL_TIMEBASE_HZ; t++) pas_sampler_isr_tick(crank_tick(&c), t);
		CHECK(pas_sampler_get_stats()->overflow_count > 0u,
		      "H1. a main loop that never drains overflows the queue and counts it");
		CHECK(pas_sampler_take_overflow() == 1u, "H2. ...and reports it once to the consumer");
		CHECK(pas_sampler_take_overflow() == 0u, "H3. ...exactly once, so one epoch is voided per burst");
		CHECK(pas_sampler_get_stats()->forward_count > 32u,
		      "H4. the edges themselves are still counted - liveness must keep seeing them");
	}

	/* --- I. FW-086: the restart step is the ORIGIN, not the first count ---------------------- */
	{
		/*
		 * The property the old JS harness guarded by matching main.c's source text. It is
		 * asserted here against the real module instead: if the restart step were counted, the
		 * first pulse would span PAS_STEPS_PER_PULSE-1 gaps and read 4/3 too high.
		 */
		pas_cadence_step_t cad;
		int i;
		pas_cadence_reset();
		cad = pas_cadence_forward_step(1000u, 1u);          /* the restart step */
		CHECK(cad.pulse == 0u, "I1. the restart step itself never fires a pulse");
		for (i = 1; i <= PAS_STEPS_PER_PULSE; i++) {
			cad = pas_cadence_forward_step(1000u + (uint32_t)i * 42u, 0u);
		}
		CHECK(cad.pulse == 1u && cad.measured == 1u,
		      "I2. the pulse lands a full PAS_STEPS_PER_PULSE later");
		CHECK(cad.period_ticks == (uint16_t)(PAS_STEPS_PER_PULSE * 42u),
		      "I3. ...spanning a FULL interval - not one gap short, which would read 4/3 high");
	}

	/* --- J. validity is a fact, never a guess ------------------------------------------------ */
	{
		pas_cadence_reset();
		CHECK(pas_cadence_get()->valid == 0u, "J1. after a reset nothing is claimed as measured");
		CHECK(pas_cadence_get()->rpm == 0u, "J2. ...and no stale reading survives it");
		(void)pas_cadence_forward_step(0u, 1u);
		CHECK(pas_cadence_get()->valid == 0u,
		      "J3. starting an interval is not a measurement - validity waits for a real period");
	}

	if (host_test_failures == 0) {
		printf("PRE-FW128 PAS timebase: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("PRE-FW128 PAS timebase: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
