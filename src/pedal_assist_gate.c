/*
 * FW-112 PATCH B0a: pedal-assist gate implementation.
 *
 * A latch — the SOLE owner of pedal permission:
 *   OPEN:  forward_valid AND load_met (on the tick both become true)
 *   CLOSE: stop_or_reverse OR session not latched (immediate, unconditional)
 *   KEEP:  once open, stays open (no close on forward_valid dropping alone)
 *
 * Session lifecycle events (cold_arm, fast_rearm) do NOT independently open
 * the gate — the gate enforces the load condition on every path.
 */

#include "pedal_assist_gate.h"

static bool gate_open;

void pedal_assist_gate_init(void)
{
	gate_open = false;
}

void pedal_assist_gate_update(const pedal_assist_gate_input_t *in,
			      pedal_assist_gate_output_t *out)
{
	if (in->stop_or_reverse) {
		gate_open = false;
	} else if (!gate_open && in->forward_valid && in->load_met) {
		gate_open = true;
	}
	if (out != 0) {
		out->open = gate_open;
	}
}

bool pedal_assist_gate_is_open(void)
{
	return gate_open;
}
