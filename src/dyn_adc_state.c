#include "dyn_adc_state.h"

/*
 * FW-120.1. The selection rule and the reconstruction arithmetic are lifted verbatim from the
 * ISR they used to live in - same comparisons, same tie behaviour, same two subtractions. Only
 * the place where they are CALLED changed; see the header for the pipeline proof.
 */

uint8_t dyn_adc_state_select(const uint16_t switchtime[3], uint8_t previous)
{
	if (switchtime[2] > switchtime[0] && switchtime[2] > switchtime[1]) return DYN_ADC_STATE_C_HIGH;
	if (switchtime[0] > switchtime[1] && switchtime[0] > switchtime[2]) return DYN_ADC_STATE_A_HIGH;
	if (switchtime[1] > switchtime[0] && switchtime[1] > switchtime[2]) return DYN_ADC_STATE_B_HIGH;
	return previous;
}

void dyn_adc_state_reconstruct(uint8_t state, int16_t *i_a, int16_t *i_b, int16_t i_c)
{
	switch (state) {
	case DYN_ADC_STATE_A_HIGH:
		*i_a = (int16_t)(-(*i_b) - i_c);
		break;
	case DYN_ADC_STATE_B_HIGH:
		*i_b = (int16_t)(-(*i_a) - i_c);
		break;
	case DYN_ADC_STATE_C_HIGH:
	case DYN_ADC_STATE_UNDECIDED:
	default:
		/* C_HIGH: A and B were both sampled in a valid window, and C is not a Clarke input.
		 * UNDECIDED: no strict maximum, every shunt conducted long enough. Nothing to rebuild
		 * in either case. */
		break;
	}
}
