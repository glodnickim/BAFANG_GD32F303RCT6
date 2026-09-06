#ifndef FOC_CURRENT_LOOP_H_
#define FOC_CURRENT_LOOP_H_
#include <stdint.h>
typedef struct {
    int32_t i_d;
    int32_t i_d_setpoint;
    int32_t u_d, u_q, u_abs;
    int32_t u_d_req, u_q_req, u_abs_req;
    int32_t u_d_sat_err, u_q_sat_err;
} MotorState_t;
typedef struct {
    float gain_p, gain_i;
    int16_t limit_i, limit_output, recent_value;
    int32_t setpoint;
    float integral_part;
    int16_t max_step;
    int16_t shift;
    int32_t out;
    int32_t aw_sat_error;
    int32_t aw_inv_kp_q15;
} PI_control_t;
typedef struct { int32_t u_q_requested, u_d_requested; uint8_t saturated; } foc_current_loop_result_t;
void foc_current_loop_step(MotorState_t*, PI_control_t*, PI_control_t*, uint8_t, float, float, foc_current_loop_result_t*);
#endif
