#ifndef IQ_CHAIN_H_
#define IQ_CHAIN_H_

#include <stdint.h>

/*
 * FW-128A: names and observability for the q-current demand chain.
 *
 * WHAT THE AUDIT ACTUALLY FOUND
 * -----------------------------
 * The pre-audit expected several unrelated variables silently representing different versions
 * of "requested q current". That is NOT what the source shows. The chain is already one-way and
 * already has a single final owner:
 *
 *     ride_control.c holds ONE local int32_t iq_target and overwrites it in place at each
 *     stage; MS.i_q_setpoint has exactly ONE writer in the whole firmware (motor_core.c:22).
 *
 * So there is no split ownership to repair here. What is missing is that the stages have no
 * NAMES and no OBSERVABILITY: "the demand before the limiters" and "the demand after the
 * limiters" exist only as two transient values of the same local, which is why the pre-audit
 * could not say which limiter was binding, and why no test could pin limiter behaviour without
 * reaching into ride_control's internals.
 *
 * This module therefore adds exactly two recorded stages and nothing else. It is deliberately
 * NOT a new owner of the demand - ride_control still computes it, in the same order, with the
 * same arithmetic. FW-128A changes no control behaviour by design.
 *
 * THE THREE CANONICAL STAGES
 * --------------------------
 *     Iq_requested   the rider/assist demand, BEFORE any dynamic limiter
 *                    producer: ride_control_update(), after the mode/walk/boost branches
 *
 *     Iq_allowed     the demand after every limiter still active in FW-128A
 *                    (assist_limits_apply voltage/thermal/speed, assist_start smoothing,
 *                    the gear preload cap), BEFORE the ramp
 *                    producer: ride_control_update(), immediately before final mailbox publish
 *
 *     Iq_ref         the ramped reference PI_iq actually follows
 *                    producer: fast_iq_slew_tick() @ 16 kHz -> MS.i_q_setpoint
 *
 * Iq_ref IS NOT STORED HERE, on purpose. MS.i_q_setpoint already holds it and already has one
 * writer; copying it into this struct would create exactly the duplicate state this card exists
 * to avoid. Read MS.i_q_setpoint for the reference - that is the one source of truth.
 *
 * UNITS. All three are in the same domain: the phase-current demand scale that
 * MP.phase_current_max is expressed in (config.h PH_CURRENT_MAX = 700), signed, int32_t while in
 * flight and int16_t once stored in MS.i_q_setpoint. Nothing here rescales anything.
 *
 * NOT AN OWNERSHIP GATE. Recording a value cannot change it. If a future card wants the chain
 * enforced rather than observed, that is FW-128D's arbitration owner, not this.
 */

typedef struct {
	int32_t requested;   /* rider/assist demand, before any dynamic limiter */
	int32_t allowed;     /* after every FW-128A-era limiter, before the ramp */
	uint8_t valid;       /* 0 until the first note of this run - so a reader can tell    */
	                     /* "no demand recorded yet" from "a demand of zero"             */
} iq_chain_t;

/*
 * Lifecycle reset. Called alongside the other per-start resets so a new run can never be read
 * as carrying the previous run's demand.
 *
 *   PRODUCER : iq_chain_reset()
 *   CONSUMER : iq_chain_get(), diagnostics, host tests
 *   RESET    : every bridge start, with current_sample_ctx_reset()/current_feedback_reset()
 *   PURPOSE  : a deterministic first state
 */
void iq_chain_reset(void);

/* Record the demand before the limiters. One producer: ride_control_update(). */
void iq_chain_note_requested(int32_t requested);

/* Record the demand after the limiters and before the ramp. Same single producer. */
void iq_chain_note_allowed(int32_t allowed);

/* Read-only view. */
const iq_chain_t *iq_chain_get(void);

#endif /* IQ_CHAIN_H_ */
