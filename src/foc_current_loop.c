#include "foc_current_loop.h"
#include "FOC.h"

#include <math.h>

static int32_t clamp_sat_error(int32_t v)
{
    if (v > FOC_AW_SAT_ERROR_MAX) return FOC_AW_SAT_ERROR_MAX;
    if (v < -FOC_AW_SAT_ERROR_MAX) return -FOC_AW_SAT_ERROR_MAX;
    return v;
}

void foc_current_loop_step(MotorState_t *ms,
                           PI_control_t *pi_iq,
                           PI_control_t *pi_id,
                           uint8_t reassert_integral,
                           float iq_integral,
                           float id_integral,
                           foc_current_loop_result_t *result)
{
    int32_t u_q_requested;
    int32_t u_d_requested;
    int32_t sat_q;
    int32_t sat_d;
    uint8_t saturated = 0U;

    if (!ms || !pi_iq || !pi_id) {
        if (result) {
            result->u_q_requested = 0;
            result->u_d_requested = 0;
            result->saturated = 0U;
        }
        return;
    }

    u_q_requested = PI_control(pi_iq);

    pi_id->recent_value = ms->i_d;
    pi_id->setpoint = ms->i_d_setpoint;
    u_d_requested = -PI_control(pi_id);

    /* QZERO intentionally owns the held/faded integral trajectory. PI_control() still executes
     * so the proportional path remains live; only its one integration increment is discarded. */
    if (reassert_integral) {
        pi_iq->integral_part = iq_integral;
        pi_id->integral_part = id_integral;
    }

    ms->u_q_req = u_q_requested;
    ms->u_d_req = u_d_requested;
    ms->u_abs = (int32_t)sqrtf((float)(u_d_requested * u_d_requested +
                                      u_q_requested * u_q_requested));
    ms->u_abs_req = ms->u_abs;

    if (ms->u_abs > _U_MAX) {
        ms->u_q = (u_q_requested * _U_MAX) / ms->u_abs;
        ms->u_d = (u_d_requested * _U_MAX) / ms->u_abs;
        ms->u_abs = _U_MAX;
        saturated = 1U;
    } else {
        ms->u_q = u_q_requested;
        ms->u_d = u_d_requested;
    }

    /* Tracking anti-windup residual is published in the PI controllers' own output domains.
     * Vd is -PI_id.out, hence the one sign inversion for the D controller. */
    sat_q = clamp_sat_error(ms->u_q_req - ms->u_q);
    sat_d = clamp_sat_error(ms->u_d_req - ms->u_d);
    ms->u_q_sat_err = sat_q;
    ms->u_d_sat_err = sat_d;
    pi_iq->aw_sat_error = sat_q;
    pi_id->aw_sat_error = -sat_d;

    if (result) {
        result->u_q_requested = u_q_requested;
        result->u_d_requested = u_d_requested;
        result->saturated = saturated;
    }
}
