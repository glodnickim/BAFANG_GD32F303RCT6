#ifndef CADENCE_FILTER_H
#define CADENCE_FILTER_H

#include <stdint.h>

/*
 * FW-140 control-cadence conditioner.
 *
 * PAS cadence is measured over only PAS_STEPS_PER_PULSE transitions (15 crank degrees on the
 * M820 configuration). That is an excellent raw diagnostic but it also contains the rider's
 * normal within-revolution speed ripple. Feeding that local speed directly into Power/eMTB,
 * cadence compensation and the adaptive final-Iq trajectory turns normal leg strokes into a
 * moving motor-current target.
 *
 * This module owns the already-existing 1/8 IIR. The first REAL cadence sample seeds the state
 * directly, so using the filtered value for control adds no artificial startup ramp from 0 rpm.
 * Raw cadence remains available separately for diagnostics/HMI.
 */
void cadence_filter_reset(void);
uint8_t cadence_filter_update(uint8_t raw_rpm);
uint8_t cadence_filter_get(void);
uint16_t cadence_filter_get_x8(void);

#endif
