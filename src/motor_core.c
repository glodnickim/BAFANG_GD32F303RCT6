#include "motor_core.h"

static MotorState_t *state;

void motor_core_init(MotorState_t *motor_state)
{
	state = motor_state;
	if (state != 0) {
		/* STARTUP classification only; no dynamic command enters through Motor Core. */
		state->i_q_setpoint = 0;
		state->i_d_setpoint = 0;
	}
}

void motor_core_set_id_target(int32_t id_target)
{
	if (state == 0) {
		return;
	}
	state->i_d_setpoint = id_target;
}
