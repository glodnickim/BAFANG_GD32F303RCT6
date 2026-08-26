/*
 * FOC.h
 *
 *  Created on: 25.01.2019
 *      Author: stancecoke
 */

#ifndef FOC_H_
#define FOC_H_

#include "main.h"
//exportetd functions
void FOC_calculation(int16_t int16_i_as, int16_t int16_i_bs, q31_t q31_teta, int16_t int16_i_q_target, MotorState_t* MS_FOC, MotorParams_t* MP_FOC);
q31_t PI_control (PI_control_t* PI_c);

/*
 * FW-126: q31_i_q_fil/q31_i_d_fil (this file, file-scope) are the ONLY place MS_FOC->i_q/i_d are
 * ever written - FOC_calculation() runs them through a first-order IIR every call and nowhere
 * else touches them. Neither the filter state nor MS_FOC->i_q/i_d are ever reset on bridge OFF ->
 * ON, so a bridge restart's very first FOC_calculation() call blends a brand new Clarke/Park
 * sample into whatever the filter held at the end of the PREVIOUS run - and until that first call
 * happens at all, MS_FOC->i_q/i_d are still exactly that previous run's last value. Call this
 * ONCE, at the single bridge-start gate in main.c, before the first FOC_calculation() of a new
 * run: it zeroes q31_i_q_fil/q31_i_d_fil and MS_FOC->i_q/i_d so the new run's filter builds up
 * from a defined zero, not a historical value that happens to look plausible.
 *
 * Resets ONLY the current-feedback filter state and MS_FOC->i_q/i_d. Does not touch Hall/rotor
 * angle, assist state, ride session, calibration offsets, or PI state - those have their own
 * reset points in the existing lifecycle and this function must not duplicate or race them.
 */
void foc_current_feedback_reset(MotorState_t* MS_FOC);

/* FW-126: truthfulness of measured-current publication. `fresh` means a new ADC1 injected
 * conversion raised EOIC; it says nothing about physical PWM-window validity. Until FW-127 has
 * a measured window model, `valid` remains 0 so no diagnostic may call MS.i_q live feedback.
 * The sequence never changes merely because a control tick or a JDR re-read happened. */
extern volatile uint32_t foc_current_sample_seq;
extern volatile uint32_t foc_current_sample_tick;
extern volatile uint8_t  foc_current_fresh;
extern volatile uint8_t  foc_current_valid;
extern volatile uint16_t foc_current_sample_age;
void foc_current_feedback_invalidate(void);
void foc_current_feedback_note_fresh(uint32_t control_tick);
//q31_t PI_control_i_q (q31_t ist, q31_t soll);
//q31_t PI_control_i_d (q31_t ist, q31_t soll);

// Maximum Voltage applyed

#ifdef DISABLE_DYNAMIC_ADC
#define _U_MAX	2000L
#else
#define _U_MAX	1920L //scaling ud, uq, u_alpha and u_beta to 2^11 = 2048, stay a little below to have a minimum time for ADC conversion in the middle of the PWM cycle
#endif



// Square Root of 3
#define _SQRT3	28  //1.73205081*16

#define ADC_DUR 250//minimal duration for proper ADC reading deadtime + noise subsiding + sample time


//globals
extern q31_t temp1;
extern q31_t temp2;
extern q31_t temp3;
extern q31_t temp4;
extern q31_t temp5;
extern q31_t temp6;
extern char PI_flag;



extern q31_t e_log[300][6];
extern char Obs_flag;
extern uint8_t ui8_debug_state;



#endif /* FOC_H_ */
