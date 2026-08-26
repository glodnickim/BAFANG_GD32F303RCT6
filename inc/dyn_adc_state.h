#ifndef DYN_ADC_STATE_H_
#define DYN_ADC_STATE_H_

#include <stdint.h>

/*
 * FW-120.1: which two phase currents describe the sample that was JUST received.
 *
 * The 2-of-3 rule itself is unchanged and predates this card: the phase whose high side is on
 * longest has the shortest low-side conduction window, so its shunt is the one that cannot be
 * trusted, and it is rebuilt from the other two through Ia + Ib + Ic = 0. What FW-120.1 fixes is
 * WHICH PWM period that decision is made for.
 *
 * The pipeline is:
 *
 *   ISR n-1: FOC_calculation() -> switchtime[] -> CCR0/1/2 (shadow disabled, effective at once)
 *            |
 *            +-- those CCRs shape the whole next PWM period
 *                |
 *   counter top n: CH3 fires the injected trigger, the three shunts are sampled
 *                |
 *   ISR n:     JDR read -> RECONSTRUCTION -> Clarke
 *
 * So the sample read in ISR n was taken under switchtime[] as computed in ISR n-1 - which is
 * exactly what the switchtime[] array still holds when ISR n begins, because nothing writes it
 * between FOC_calculation() in ISR n-1 and FOC_calculation() in ISR n. Selecting from
 * switchtime[] at the TOP of the ISR therefore describes the sample in hand. Selecting anywhere
 * AFTER this ISR's own FOC_calculation() describes a period that has not been sampled yet, and
 * carrying that verdict to the next ISR is what the pre-FW-120.1 code did: it reconstructed with
 * the ranking from one period too early, so every sector crossing corrupted one Clarke input.
 *
 * The trigger side of the old dyn_adc_state() - programming CH3 for the NEXT acquisition - is a
 * genuinely different question and stayed in main.c as dyn_adc_trigger_update(). Two jobs, two
 * names, so neither can silently drift into the other's ordering again.
 */

/* No strict maximum (two or three CCRs tied): keep whatever the caller already had. All three
 * equal is the neutral/dwell/cutoff case, where every shunt conducts long enough anyway. */
#define DYN_ADC_STATE_UNDECIDED  0U
#define DYN_ADC_STATE_C_HIGH     1U   /* phase C longest high side -> A and B measured directly */
#define DYN_ADC_STATE_A_HIGH     2U   /* phase A longest high side -> A = -B - C               */
#define DYN_ADC_STATE_B_HIGH     3U   /* phase B longest high side -> B = -A - C               */

/*
 * switchtime[0]=phase A, [1]=phase B, [2]=phase C, as written to TIMER0 CH0/CH1/CH2.
 * `previous` is returned unchanged when no phase is a strict maximum.
 */
uint8_t dyn_adc_state_select(const uint16_t switchtime[3], uint8_t previous);

/*
 * Rebuilds the untrusted phase in place. i_c is by value: phase C is never a Clarke input
 * (arm_clarke_q31 takes Ia and Ib only) and must never be written back, so the type says so.
 * Offsets must already have been subtracted - the sum rule only holds around a true zero.
 */
void dyn_adc_state_reconstruct(uint8_t state, int16_t *i_a, int16_t *i_b, int16_t i_c);

#endif /* DYN_ADC_STATE_H_ */
