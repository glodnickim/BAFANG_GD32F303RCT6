#ifndef SAMPLE_WINDOW_H_
#define SAMPLE_WINDOW_H_

#include <stdint.h>
#include "current_sample_ctx.h"

/*
 * FW-127C: which phases can actually be measured under a given APPLIED PWM geometry, and what
 * trigger to use for them.
 *
 * WHY THIS REPLACES dyn_adc_trigger_update()
 * ------------------------------------------
 * The old function ran BEFORE FOC_calculation(), so it derived the trigger for transaction N+1
 * from the geometry of transaction N. It also had no notion of validity: it picked the phase
 * with the highest duty, always reconstructed that one, and programmed a trigger that - in the
 * over-modulation regime - could not match at all. This module answers the same question from
 * the geometry that will actually be applied, and returns INVALID instead of an illegal compare.
 *
 * THE PHYSICS, DERIVED FOR EVISTDRIVE - stock constants are NOT reused
 * -------------------------------------------------------------------
 * Measured and configured facts:
 *
 *   timer clock            120 MHz            -> 1 tick = 8.3333 ns
 *   TIMER0 ARR             _T = 3750, centre-aligned -> 62.5 us period, 16 kHz
 *   ADC clock              APB2/6 = 20 MHz    (rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV6))
 *   inserted sample time   55.5 ADC cycles    (adc_inserted_channel_config)
 *   dead time              DTCFG = 32, fDTS = fTIMER -> 32 ticks = 267 ns
 *   CH3 trigger half-cycle DOWN-count match   (FW-126.2, measured, HIGH confidence)
 *
 * Derivation:
 *
 *   T_ACQ    = 55.5 / 20 MHz = 2775 ns = 333 ticks     <- the sample-and-hold aperture
 *   T_CONV   = 12.5 / 20 MHz =  625 ns =  75 ticks     <- after the aperture; value already held
 *   T_ACQ + T_CONV = 408 ticks, and FW-126.2 measured CONV+L = 588, leaving ~180 ticks of ISR
 *   entry latency - an independent cross-check that these numbers describe the real hardware.
 *
 * A phase's low-side shunt only carries phase current while its low side conducts. On the DOWN
 * slope the counter falls from ARR toward 0, and phase i's low side is on while CNT > CCR_i;
 * it turns off as CNT crosses below CCR_i. The trigger fires at CNT = CH3 and the aperture then
 * spans CNT from CH3 down to CH3 - T_ACQ. For that whole aperture to sit inside the conducting
 * interval:
 *
 *      CH3 - T_ACQ - T_DEAD - T_SETTLE  >=  CCR_i
 *
 * so, with GUARD = T_ACQ + T_DEAD + T_SETTLE:
 *
 *      phase i is directly measurable  <=>  CCR_i + GUARD <= CH3
 *
 * T_SETTLE is the ONE value here that is not measured: it allows for ringing after the low side
 * turns on. One dead-time (32 ticks, 267 ns) is used. That choice errs in the SAFE direction -
 * too large only means a phase is reconstructed when it could have been measured, which costs
 * accuracy, never validity. Too small would feed a garbage direct reading into Clarke, so if the
 * hardware session shows ALTERNATE far more often than expected this is the number to revisit.
 *
 * WHY THE TRIGGER IS NOT MOVED
 * ----------------------------
 * The old code moved CH3 down to (highest duty - offset) when the highest duty was large. On
 * this hardware that makes the window WORSE, not better: a lower CH3 starts the aperture closer
 * to the CCR crossings, so FEWER phases qualify. The best possible position is as high as the
 * timer allows, which is where TRIGGER_DEFAULT (ARR - 10) already sits - and it is the position
 * every piece of hardware evidence in FW-126.x was taken with. So the trigger is a derived
 * constant and the decision is entirely about which phases that constant can reach.
 */

/* Sampling-window timing, in TIMER0 ticks. Every one of these is derived above. */
#define SAMPLE_WINDOW_T_ACQ_TICKS     333U   /* 55.5 ADC cycles @ 20 MHz                      */
#define SAMPLE_WINDOW_T_DEAD_TICKS     32U   /* DTCFG = 32 @ fDTS = fTIMER                    */
#define SAMPLE_WINDOW_T_SETTLE_TICKS   32U   /* ringing allowance: one dead-time (see above)   */
#define SAMPLE_WINDOW_GUARD_TICKS \
	(SAMPLE_WINDOW_T_ACQ_TICKS + SAMPLE_WINDOW_T_DEAD_TICKS + SAMPLE_WINDOW_T_SETTLE_TICKS)

/* Which phase, if any, has to be reconstructed. Derived from the APPLIED geometry - this is a
 * sampling sector and has nothing to do with the Hall/electrical sector. */
#define SAMPLE_WINDOW_RECONSTRUCT_NONE 3U

typedef struct {
	uint8_t  state;          /* current_sample_state_t                                     */
	uint8_t  sector;         /* index of the reconstructed phase, or _RECONSTRUCT_NONE      */
	uint8_t  direct_mask;    /* CURRENT_SAMPLE_DIRECT_*: phases measured directly           */
	uint16_t trigger_ccr;    /* the CH3 compare to program; only meaningful when != INVALID */
} sample_window_t;

/*
 * Decide the sampling window for the geometry that is about to be applied.
 *
 * `applied` must be the CLAMPED geometry (FW-127A), never the request: the decision has to
 * describe what the bridge will actually do.
 *
 * Guarantees, checked by the host tests:
 *   - the returned trigger_ccr is always inside [0, arr] when state != INVALID;
 *   - INVALID is returned rather than an illegal or unusable trigger;
 *   - direct_mask always has at least two bits set when state != INVALID, because 2-of-3
 *     reconstruction needs two trustworthy phases.
 */
void sample_window_decide(const uint16_t applied[CURRENT_SAMPLE_PHASES], uint16_t arr,
                          uint16_t trigger_ccr, sample_window_t *out);

/*
 * Reconstruct the third phase from the two that were measured directly, using the ownership
 * recorded in the context of the sample in hand. Kirchhoff: the three phase currents sum to 0.
 *
 * `i_a`/`i_b`/`i_c` are in/out; only the reconstructed one is written.
 */
void sample_window_reconstruct(uint8_t sector, int16_t *i_a, int16_t *i_b, int16_t *i_c);

#endif /* SAMPLE_WINDOW_H_ */
