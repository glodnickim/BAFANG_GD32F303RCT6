#ifndef PWM_GEOMETRY_H_
#define PWM_GEOMETRY_H_

#include <stdint.h>

/*
 * FW-127A: the one place where a REQUESTED PWM geometry becomes an APPLIED one.
 *
 * WHY THIS LAYER EXISTS
 * ---------------------
 * Nothing between svpwm() and the compare write used to provide a final defensive range check.
 * FW-127 originally justified this layer with an analytical claim that the normal SVPWM path
 * exceeded ARR already around u_abs=1170. The later electrical SIL invalidated that claim: a
 * 360-degree sweep of the REAL svpwm() at _T=3750 stays inside the timer range all the way to
 * the configured circle limit _U_MAX=1920. At u_abs=1920 the observed requested compare range
 * is 103..3647, with zero clamp hits across 360 electrical degrees.
 *
 * The module is therefore a DEFENSIVE OUTPUT INVARIANT, not a normal modulation limiter and not
 * evidence that legal FOC output is routinely out of range. It still matters because malformed
 * state, a future SVPWM edit or an arithmetic defect must never become an impossible timer
 * compare or an impossible ADC-trigger geometry.
 *
 * WHAT THIS MODULE DOES AND DOES NOT DO
 * -------------------------------------
 * It does ONE thing: turn a requested geometry into a legal applied geometry, and count what it
 * had to change. It is deliberately not a saturation strategy - it does not scale the vector,
 * preserve its angle or reduce modulation. That is FW-128's ground. The invariant this card
 * owns is narrower and absolute:
 *
 *     whatever SVPWM asks for, what is written to the timer is inside [0, ARR],
 *     and every later sampling decision describes the APPLIED values.
 *
 * Clamping is therefore evidence, not a normal control action: a clamp hit means some producer
 * asked for geometry the timer cannot express. With the current real-SVPWM sweep, any hit inside
 * the normal _U_MAX envelope is itself a regression signal and should be investigated.
 */

#define PWM_GEOMETRY_PHASES 3

/*
 * Passive evidence. Free-running for the whole power cycle; nothing in the control path reads
 * them, so a lost update can never change behaviour.
 *
 *   PRODUCER : pwm_geometry_apply()
 *   CONSUMER : the 0x602F report (DIAG) and the host tests
 *   RESET    : pwm_geometry_init() at startup only
 *   PURPOSE  : how often, and how far, the request left the legal range
 */
typedef struct {
	uint32_t clamp_total;                        /* applies in which ANY phase was clamped   */
	uint32_t clamp_phase[PWM_GEOMETRY_PHASES];   /* per-phase clamp hits                     */
	int32_t  peak_requested;                     /* highest value ever asked for             */
	int32_t  min_requested;                      /* lowest  value ever asked for (may be <0) */
	uint16_t peak_applied;                       /* highest value ever written               */
	uint16_t min_applied;                        /* lowest  value ever written               */
	uint8_t  seen;                               /* 0 until the first apply, so the min/max  */
	                                             /* seeds are a measurement, not an assumption */
} pwm_geometry_stats_t;

/* Power-on. Also the only reset point: the counters describe a whole power cycle. */
void pwm_geometry_init(void);

/*
 * Clamp `requested` into [0, arr] and publish the result in `applied`.
 *
 * `requested` is SIGNED on purpose. svpwm()'s arithmetic can legitimately go negative, and a
 * negative value assigned straight into a uint16 becomes a large positive compare - the failure
 * would be invisible exactly where it matters most. Catching it needs the signed value.
 *
 * Returns a bit per clamped phase (bit0 = A, bit1 = B, bit2 = C); 0 when nothing was changed.
 */
uint8_t pwm_geometry_apply(const int32_t requested[PWM_GEOMETRY_PHASES],
                           uint16_t applied[PWM_GEOMETRY_PHASES],
                           uint16_t arr);

/* Read-only view of the evidence. */
const pwm_geometry_stats_t *pwm_geometry_get_stats(void);

/*
 * Is `value` a compare the timer can actually match inside a period? The trigger code uses this
 * so an illegal CH3 is never programmed in the first place, rather than computed and then
 * silently clamped into a different sampling instant than the one that was reasoned about.
 */
uint8_t pwm_geometry_compare_legal(int32_t value, uint16_t arr);

#endif /* PWM_GEOMETRY_H_ */
