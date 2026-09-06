#ifndef PAS_SAMPLER_H_
#define PAS_SAMPLER_H_

#include <stdint.h>

/*
 * PRE-FW128: the PAS lines are sampled HERE, in the 4 kHz timer interrupt, and nowhere else.
 *
 * WHAT WAS WRONG. The production decoder used to live in reg_ADC_processing(), which the main
 * loop calls when a plain one-bit flag is set. A flag cannot count. Four timer interrupts
 * between two main-loop passes still set it once, so the decoder ran once and saw one GPIO
 * sample. Two separate defects followed from that:
 *
 *   TIME     pas_cycle_ticks and pas_idle_ticks were incremented once per DECODER RUN, so they
 *            measured how often the main loop got round to PAS, not how much time had passed.
 *            Every skipped pass shortened the measured cadence period, and since the reading is
 *            10000/period, a shorter period reads as a HIGHER cadence. One missed pass at
 *            60 rpm is already ~1.5 rpm of error, and a burst is a visible jump.
 *
 *   ORDER    the A/B lines were only read when the decoder ran. A crank step that started and
 *            finished inside a skipped window was never seen, and a two-step gap (00 -> 01 -> 11
 *            observed as 00 -> 11) is an ILLEGAL transition the table cannot classify.
 *
 * Both are the same root cause: the measurement lived downstream of a scheduler. This module
 * moves the two things that must happen in real time - reading the pins and stamping the clock -
 * into the timer interrupt, and leaves everything else exactly where it was.
 *
 * WHAT THIS MODULE DELIBERATELY IS NOT. It is not a second ride-policy decoder. It calls the SAME
 * pas_quadrature_step() the main loop used to call and owns only the physical plausibility layer:
 * an immediate reverse/diagonal transition less than PAS_SAMPLER_GLITCH_TICKS after the last
 * accepted edge is electrical/contact bounce, not a physically possible crank step, and is not
 * published downstream. It makes no cadence, ride-direction, latch or assist decision. Every
 * consequence of an ACCEPTED step stays in main.c, which drains the events below in order.
 *
 * THE RING IS THE WHOLE POINT. Replaying the decoder N times against a stale GPIO reading would
 * have reproduced the same sample N times; queueing the real events reproduces the real
 * sequence. If the queue ever overflows, ORDER has been lost, not just time - so overflow is
 * reported to main as an event in its own right and invalidates the cadence epoch, rather than
 * being silently dropped.
 */

/* Power of two. One physical edge per 4 kHz tick is the absolute ceiling, and a real crank at
 * 120 rpm produces one roughly every 21 ticks, so 32 slots covers a main-loop stall an order of
 * magnitude longer than anything observed. Beyond that the overflow path takes over. */
#define PAS_SAMPLER_RING 32U

/* FW-139: physical plausibility/refractory window for PAS A/B. Production ride diagnostics
 * measured genuine 52 rpm quadrature gaps around 48 control ticks, but false reverse events
 * while pedalling forward arrived 1..3 ticks after the preceding edge. At 4 kHz, 4 ticks = 1 ms;
 * even at the supported high cadence this is far shorter than a physical 3.75-degree crank step.
 *
 * Only non-forward transitions inside this window are suppressed. A clean forward sequence is
 * never delayed. If a reverse/invalid state persists until this many ticks have elapsed from the
 * last accepted edge, it is accepted and the existing fail-safe direction logic remains intact. */
#define PAS_SAMPLER_GLITCH_TICKS 4U

typedef struct {
	uint32_t tick;    /* the 4 kHz tick this edge was OBSERVED on - the real clock, not main's */
	uint16_t gap;     /* ticks since the previous edge of any direction, saturated at 65535     */
	uint8_t  states;  /* (from << 4) | to, both 0..3                                            */
	int8_t   step;    /* +1 forward, -1 reverse, 0 = illegal two-bit jump (INVALID)             */
} pas_step_event_t;

typedef struct {
	uint32_t tick_total;       /* 4 kHz ticks seen by the sampler since init                  */
	uint32_t forward_count;    /* physical edges classified forward                           */
	uint32_t reverse_count;    /* ...reverse                                                  */
	uint32_t invalid_count;    /* accepted illegal two-bit jumps - see pas_sampler_isr_tick() */
	uint32_t glitch_count;     /* implausibly-fast reverse/invalid transitions rejected         */
	uint32_t overflow_count;   /* events lost because main did not drain in time               */
} pas_sampler_stats_t;

/* Call once before the timer interrupt is enabled. `tick` seeds the transition clock so the
 * first idle reading is 0 rather than a large number derived from an uninitialised anchor. */
void pas_sampler_init(uint32_t tick);

/*
 * THE 4 kHz sampling point. Called from TIMER1_IRQHandler and from nowhere else.
 *
 * `ab` is the two PAS lines already read by the caller (bit0 = A/PC12, bit1 = B/PD2) - the pins
 * are read once in the ISR and the same value is handed to every consumer, so the production
 * decoder and the FW-106 raw recorder can never disagree about what the lines did.
 *
 * Cost on a tick with no edge, which is the overwhelming majority: one increment, one compare,
 * return. On an edge: one table lookup, one counter, one ring store.
 */
void pas_sampler_isr_tick(uint8_t ab, uint32_t tick);

/* Drain one event, oldest first. Returns 0 when the queue is empty. Main-loop side only. */
int pas_sampler_pop(pas_step_event_t *out);

/* The tick of the most recent physical edge of ANY direction, invalid included. Idle time is
 * `now - this`, which is true no matter when the main loop asks. */
uint32_t pas_sampler_last_transition_tick(void);

/* Current decoded line state (0..3), for diagnostics that tag a snapshot with it. */
uint8_t pas_sampler_state(void);

/* 0 until the first sample establishes a starting state - the PAS-sensor-present check. */
uint8_t pas_sampler_seeded(void);

/*
 * Consume the overflow marker. Returns non-zero once per overflow burst and clears it, so the
 * caller can invalidate exactly one cadence epoch per loss of ordering. The monotone count in
 * the stats below is kept separately for the ride log.
 */
uint8_t pas_sampler_take_overflow(void);

const pas_sampler_stats_t *pas_sampler_get_stats(void);

#endif /* PAS_SAMPLER_H_ */
