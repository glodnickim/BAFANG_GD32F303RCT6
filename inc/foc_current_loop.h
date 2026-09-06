#ifndef FOC_CURRENT_LOOP_H_
#define FOC_CURRENT_LOOP_H_

#include <stdint.h>
#include "main.h"

/*
 * Shared production D/Q current-loop stage.
 *
 * runPIcontrol() owns reference preparation (final Iq slew, zero-torque policy/QZERO).
 * FOC_calculation() owns Clarke/Park and SVPWM. This module owns the one piece that must be
 * identical in target firmware and electrical SIL: PI Id/Iq execution, optional QZERO integral
 * re-assertion, shared voltage-vector limiting, and tracking anti-windup residual publication.
 *
 * The caller must prepare pi_iq->setpoint/recent_value before this call. The D-axis inputs come
 * from MotorState_t because production always regulates measured Id to MS.i_d_setpoint.
 */
typedef struct {
    int32_t u_q_requested;
    int32_t u_d_requested;
    uint8_t saturated;
} foc_current_loop_result_t;

void foc_current_loop_step(MotorState_t *ms,
                           PI_control_t *pi_iq,
                           PI_control_t *pi_id,
                           uint8_t reassert_integral,
                           float iq_integral,
                           float id_integral,
                           foc_current_loop_result_t *result);

#endif /* FOC_CURRENT_LOOP_H_ */
