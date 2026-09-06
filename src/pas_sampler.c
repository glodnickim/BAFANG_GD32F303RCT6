#include "pas_sampler.h"
#include "pas_quadrature.h"

/*
 * Single-producer (TIMER1 ISR) / single-consumer (main loop) ring. head is written only by the
 * ISR, tail only by main, both are single bytes, and a byte store on this core is atomic - so
 * no critical section is needed and the ISR is never blocked by the main loop. That matters:
 * the entire purpose of this module is that its timing cannot be influenced by main.
 */
static pas_step_event_t ring[PAS_SAMPLER_RING];
static volatile uint8_t head;
static volatile uint8_t tail;

static volatile uint32_t last_transition_tick;
static volatile uint8_t  qstate = 0xFFU;   /* 0xFF = not yet seeded */
static volatile uint8_t  overflow_flag;

static pas_sampler_stats_t stats;

void pas_sampler_init(uint32_t tick)
{
	head = 0U;
	tail = 0U;
	qstate = 0xFFU;
	overflow_flag = 0U;
	last_transition_tick = tick;
	stats.tick_total = 0U;
	stats.forward_count = 0U;
	stats.reverse_count = 0U;
	stats.invalid_count = 0U;
	stats.glitch_count = 0U;
	stats.overflow_count = 0U;
}

void pas_sampler_isr_tick(uint8_t ab, uint32_t tick)
{
	ab &= 0x03U;
	stats.tick_total++;

	if (qstate == 0xFFU) {
		/* First sample of this power cycle: adopt the lines as they stand. There is no
		 * transition to report - the previous state is unknown, and inventing one would
		 * manufacture a step the crank never took. */
		qstate = ab;
		last_transition_tick = tick;
		return;
	}

	if (ab == qstate) return;   /* the common case, and it must stay this cheap */

	{
		const uint8_t from = qstate;
		const int8_t step = pas_quadrature_step(from, ab);
		/* Unsigned subtraction, so a uint32 wrap of the tick counter yields the correct
		 * elapsed count with no special case - see the host test's wrap scenario. */
		const uint32_t gap32 = tick - last_transition_tick;
		uint8_t next;

		/* FW-139: the real-bike FW-097 capture showed 1..3-tick reverse events while the
		 * rider was physically pedalling forward. A genuine 52 rpm quadrature step is about
		 * 48 ticks; even at the supported high-cadence end a new physical crank transition
		 * cannot reverse in under 1 ms. Treat only a NON-FORWARD transition in that refractory
		 * window as line/contact bounce. Crucially, do not move qstate and do not move the
		 * transition timestamp: if the input settles back, the glitch vanishes completely; if
		 * it persists, the same state is reconsidered each ISR tick and becomes an accepted
		 * reverse/INVALID at PAS_SAMPLER_GLITCH_TICKS, preserving the fail-safe behavior for a
		 * real direction change or sustained wiring fault. Clean forward edges are never held. */
		if (step <= 0 && gap32 < PAS_SAMPLER_GLITCH_TICKS) {
			if (stats.glitch_count < 0xFFFFFFFFU) stats.glitch_count++;
			return;
		}

		qstate = ab;
		last_transition_tick = tick;

		if (step > 0) stats.forward_count++;
		else if (step < 0) stats.reverse_count++;
		else stats.invalid_count++;

		next = (uint8_t)((head + 1U) & (PAS_SAMPLER_RING - 1U));
		if (next == tail) {
			/*
			 * Main has not drained for 32 physical edges. The edge itself is still counted
			 * above - the line activity is real and liveness must keep seeing it - but the
			 * ORDER of the sequence is now broken, and an out-of-order quadrature stream
			 * decodes into nonsense. Report it instead of dropping it quietly.
			 */
			stats.overflow_count++;
			overflow_flag = 1U;
			return;
		}

		ring[head].tick = tick;
		ring[head].gap = (gap32 > 0xFFFFU) ? 0xFFFFU : (uint16_t)gap32;
		ring[head].states = (uint8_t)((from << 4) | ab);
		ring[head].step = step;
		head = next;
	}
}

int pas_sampler_pop(pas_step_event_t *out)
{
	uint8_t t = tail;
	if (!out) return 0;
	if (t == head) return 0;
	*out = ring[t];
	tail = (uint8_t)((t + 1U) & (PAS_SAMPLER_RING - 1U));
	return 1;
}

uint32_t pas_sampler_last_transition_tick(void)
{
	return last_transition_tick;
}

uint8_t pas_sampler_state(void)
{
	return qstate;
}

uint8_t pas_sampler_seeded(void)
{
	return (qstate == 0xFFU) ? 0U : 1U;
}

uint8_t pas_sampler_take_overflow(void)
{
	uint8_t f = overflow_flag;
	if (f) overflow_flag = 0U;
	return f;
}

const pas_sampler_stats_t *pas_sampler_get_stats(void)
{
	return &stats;
}
