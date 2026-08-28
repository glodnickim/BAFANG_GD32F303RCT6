#ifndef PAS_LIVENESS_H_
#define PAS_LIVENESS_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * FW-112.1: PAS liveness / real-stop separation.
 *
 * One question, asked once: has the crank genuinely stopped producing ANY physical PAS
 * transition? DIRECTION PERMISSION is a different question (pas_direction.c) and must never
 * leak into it.
 *
 * Before FW-112.1, main.c's pas_idle_ticks served as the real-stop timer, and it is refreshed
 * by a forward step OR a reverse step but NOT by an illegal two-bit (INVALID) transition, so a
 * session held in SUSPENDED_BY_DIRECTION by reverse/invalid activity could still be collapsed
 * to COLD by real_stop even though the PAS lines were physically toggling. This module owns a
 * dedicated liveness counter refreshed by ANY qualified physical edge - forward, reverse and
 * invalid alike - and answers the stop question from that counter alone. Reverse/invalid is
 * therefore still never treated as permission or cadence; it is only ever evidence that the
 * crank is still physically moving, so "direction suspended" stays distinct from "real stop".
 */

void pas_liveness_init(void);

/*
 * Call once per control pass with the REAL idle time - ticks since the last physical PAS edge
 * of any direction, measured against the 4 kHz sampler clock (pas_sampler_last_transition_tick).
 *
 * PRE-FW128: this replaces pas_liveness_tick()/pas_liveness_transition(). The module used to
 * hold the counter itself and advance it once per call, which made the stop verdict a function
 * of how often the main loop ran. Handing the elapsed time in makes it a function of the clock,
 * which is what "the crank has stopped" was always supposed to mean. Refreshed by ANY qualified
 * edge - forward, reverse and invalid alike - because the anchor it is measured from is.
 */
void pas_liveness_update(uint32_t idle_ticks_real, uint16_t stop_timeout_ticks);

/* This tick's verdict: true when no PAS transition has occurred for longer than the stop
 * threshold - the crank has genuinely stopped. */
bool pas_liveness_stopped(void);

/* Diagnostic readback: ticks since the last physical PAS transition. */
uint32_t pas_liveness_idle_ticks(void);

#endif /* PAS_LIVENESS_H_ */