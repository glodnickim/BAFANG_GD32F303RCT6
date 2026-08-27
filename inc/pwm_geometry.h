#ifndef PWM_GEOMETRY_H_
#define PWM_GEOMETRY_H_

#include <stdint.h>

/*
 * FW-127A: the one place where a REQUESTED PWM geometry becomes an APPLIED one.
 *
 * WHY THIS LAYER EXISTS
 * ---------------------
 * Nothing between svpwm() and the compare write ever bounded the value. The FW-127 pre-audit
 * proved that a legal controller output can ask for a compare outside the timer's range:
 *
 *     deviation from centre = 1.602 * u_abs      (binding sector 2&5, switchtime[0];
 *                                                 swept over a full electrical revolution
 *                                                 across all three sector formulas)
 *     |deviation| > _T/2 = 1875   <=>   u_abs > 1170
 *
 * and the circle limiter in runPIcontrol() allows u_abs up to _U_MAX = 1920, i.e. 61 % of its
 * own ceiling. At u_abs = 1920 the request reaches 4952 against ARR = 3750.
 *
 * Two consequences, both bad and both silent:
 *   - the compare is truncated into uint16 and handed to the timer as if it were meaningful;
 *   - the old trigger code then derived CH3 from that same out-of-range number, producing a
 *     compare that can never match - no CC3 event, no conversion, no ISR, and with the rotor
 *     turning nothing resets the soft-cutoff counter, so there is no recovery path.
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
 * Clamping is therefore evidence, not a fix: a clamp hit means the voltage controller asked for
 * something the bridge cannot express. The counters below exist so the single FW-127 hardware
 * session can say how often that really happens - which is the practical-reachability question
 * the pre-audit deliberately left open rather than spending a bike test on.
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
