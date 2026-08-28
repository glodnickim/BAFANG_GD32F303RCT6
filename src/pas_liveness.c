#include "pas_liveness.h"

/*
 * FW-112.1: the dedicated any-PAS-edge liveness timer and its REAL_STOP verdict. See the header
 * for why this is a separate question from direction permission.
 *
 * PRE-FW128: this module no longer keeps a counter of its own. It used to increment once per
 * call and be reset by pas_liveness_transition(), which made it exactly as main-loop dependent
 * as everything else in the PAS path - a stalled main loop under-counted the idle time and the
 * stop verdict arrived late, or a transition processed several ticks after it happened reset the
 * counter to 0 as though it had just occurred. The idle time is now handed in, measured against
 * the 4 kHz sampler's transition clock, so the verdict is the same whenever it is asked for.
 */
static uint32_t idle_ticks;
static bool     stopped;

void pas_liveness_init(void)
{
	idle_ticks = 0U;
	stopped = false;
}

void pas_liveness_update(uint32_t idle_ticks_real, uint16_t stop_timeout_ticks)
{
	idle_ticks = idle_ticks_real;
	stopped = (idle_ticks > (uint32_t)stop_timeout_ticks);
}

bool pas_liveness_stopped(void)
{
	return stopped;
}

uint32_t pas_liveness_idle_ticks(void)
{
	return idle_ticks;
}
