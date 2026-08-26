/*
 * FW-112 PATCH B0a: pedal-assist gate.
 *
 * Single-purpose module: the SOLE owner of pedal permission.
 * Session events (cold_arm, fast_rearm) trigger the session lifecycle but
 * do NOT independently open the gate — the gate enforces the load condition.
 *
 * SET:   forward valid AND (start_load == 0 OR load >= start_load)
 * KEEP:  once open, stays open until stop_or_reverse
 * RESET: real_stop OR direction_inhibit OR session not latched (terminal event)
 * OUTPUT OVERRIDES (independent of gate):
 *   brake zeros final iq, fault zeros final iq, level-0 zeros final iq
 */

#ifndef PEDAL_ASSIST_GATE_H
#define PEDAL_ASSIST_GATE_H

#include <stdbool.h>

typedef struct {
	bool forward_valid;
	bool load_met;
	bool stop_or_reverse;
} pedal_assist_gate_input_t;

typedef struct {
	bool open;
} pedal_assist_gate_output_t;

void pedal_assist_gate_init(void);
void pedal_assist_gate_update(const pedal_assist_gate_input_t *in,
			      pedal_assist_gate_output_t *out);
bool pedal_assist_gate_is_open(void);

#endif
