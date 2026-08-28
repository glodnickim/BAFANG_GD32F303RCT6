#include "battery_current.h"

/*
 * Single producer (TIMER1 ISR), many consumers (main loop, FOC ISR through MS.Battery_Current).
 * `acc` is private to the producer and is never read outside it; `published` is the only word
 * that crosses, and a 32-bit aligned store is single-copy atomic here, so the whole exchange
 * needs no critical section. That matters: the point of this card is that the sampler's timing
 * cannot be influenced by anything the main loop does, and a lock would reintroduce exactly that.
 */
static int32_t          acc;          /* private IIR accumulator, fixed point 64x */
static int32_t          offset;       /* the startup zero, in ADC counts          */
static volatile int32_t published;    /* filtered ADC delta, signed               */
static volatile uint8_t armed;

static battery_current_stats_t stats;

void battery_current_init(void)
{
	acc = 0;
	offset = 0;
	published = 0;
	armed = 0U;
	stats.sample_count = 0U;
	stats.update_count = 0U;
	stats.unarmed_count = 0U;
	stats.late_scan_count = 0U;
}

void battery_current_set_offset(int32_t new_offset)
{
	/* Order matters: the accumulator and the published value are cleared BEFORE arming, so the
	 * sampler can never observe an armed module holding state from the wrong offset domain. */
	acc = 0;
	published = 0;
	offset = new_offset;
	armed = 1U;
}

void battery_current_sample(uint16_t raw, uint8_t scan_complete)
{
	stats.sample_count++;

	if (!scan_complete) {
		/* Evidence, not permission. Rank 0 is the first transfer of the regular scan and is
		 * written ~12.6 us after the trigger, roughly 190 us before this point - so the value
		 * in hand is a real, fresh PA0 reading even when a later rank is still converting.
		 * Counted because a scan that routinely overruns would mean the timing proof in the
		 * card no longer holds, and that should be visible rather than assumed. */
		stats.late_scan_count++;
	}

	if (!armed) {
		stats.unarmed_count++;
		return;
	}

	/* The production law, unchanged. Arithmetic shift: >>6 floors, which biases the result low
	 * by less than one ADC count - the same sub-count bias the old code had. */
	acc -= acc >> 6;
	acc += (int32_t)raw - offset;
	published = acc >> 6;
	stats.update_count++;
}

int32_t battery_current_filtered_adc(void)
{
	return published;
}

const battery_current_stats_t *battery_current_get_stats(void)
{
	return &stats;
}
