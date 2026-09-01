#ifndef MOTOR_CORE_H_
#define MOTOR_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#include "main.h"

/* Bind the shared state and establish the boot-only exact-zero current baseline. */
void motor_core_init(MotorState_t *motor_state);

/*
 * QS-3D-R1: Motor Core no longer accepts a dynamic Iq command. The 16 kHz fast
 * owner is the only normal writer of i_q_setpoint; this API retains the independent
 * Id target used by ride and calibration service paths.
 */
void motor_core_set_id_target(int32_t id_target);

#endif /* MOTOR_CORE_H_ */
