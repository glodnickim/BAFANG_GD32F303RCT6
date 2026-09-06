#include "foc_current_loop.h"
#include "FOC.h"
#include "common/check.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t PI_control(PI_control_t *p)
{
    float Delta = (float)(p->setpoint - p->recent_value);
    float p_part = Delta * p->gain_p;
    float aw_part = (float)((p->aw_sat_error * p->aw_inv_kp_q15) >> 15);
    p->integral_part += (Delta - aw_part) * p->gain_i;
    if (p->integral_part > p->limit_i) p->integral_part = p->limit_i;
    if (p->integral_part < -p->limit_i) p->integral_part = -p->limit_i;
    if (p_part + p->integral_part > p->out + p->max_step) p->out += p->max_step;
    else if (p_part + p->integral_part < p->out - p->max_step) p->out -= p->max_step;
    else p->out = (int32_t)(p_part + p->integral_part);
    if (p->out > p->limit_output) p->out = p->limit_output;
    if (p->out < -p->limit_output) p->out = -p->limit_output;
    return p->out;
}

typedef struct { MotorState_t ms; PI_control_t q,d; uint8_t sat; uint32_t sat_ticks; } legacy_t;
static int32_t clamp_err(int32_t v){ if(v>4096)return 4096; if(v<-4096)return -4096; return v; }
static void legacy_step(legacy_t *m, uint8_t reassert, float qi, float di)
{
    int32_t uq = PI_control(&m->q);
    m->d.recent_value=m->ms.i_d; m->d.setpoint=m->ms.i_d_setpoint;
    int32_t ud = -PI_control(&m->d);
    if(reassert){ m->q.integral_part=qi; m->d.integral_part=di; }
    m->ms.u_q_req=uq; m->ms.u_d_req=ud;
    m->ms.u_abs=(int32_t)sqrtf((float)(ud*ud+uq*uq)); m->ms.u_abs_req=m->ms.u_abs;
    if(m->ms.u_abs>_U_MAX){m->ms.u_q=uq*_U_MAX/m->ms.u_abs;m->ms.u_d=ud*_U_MAX/m->ms.u_abs;m->ms.u_abs=_U_MAX;m->sat=1;m->sat_ticks++;}
    else {m->ms.u_q=uq;m->ms.u_d=ud;m->sat=0;}
    m->ms.u_q_sat_err=clamp_err(m->ms.u_q_req-m->ms.u_q);
    m->ms.u_d_sat_err=clamp_err(m->ms.u_d_req-m->ms.u_d);
    m->q.aw_sat_error=m->ms.u_q_sat_err; m->d.aw_sat_error=-m->ms.u_d_sat_err;
}
static uint32_t rng=0x5a17d3c1u;
static int32_t rr(int32_t lo,int32_t hi){rng=rng*1664525u+1013904223u;return lo+(int32_t)(rng%(uint32_t)(hi-lo+1));}
static void init_pi(PI_control_t *p,int li){memset(p,0,sizeof(*p));p->gain_p=1.5f;p->gain_i=.01f;p->limit_i=li;p->limit_output=1920;p->max_step=15;p->aw_inv_kp_q15=21845;}
int main(void)
{
    printf("FOC current-loop extraction parity\n");
    for(int case_i=0;case_i<2000;case_i++){
        legacy_t a; memset(&a,0,sizeof(a)); init_pi(&a.q,1920); init_pi(&a.d,1800);
        a.q.integral_part=(float)rr(-1800,1800);a.d.integral_part=(float)rr(-1700,1700);
        a.q.out=rr(-1900,1900);a.d.out=rr(-1900,1900);
        a.q.aw_sat_error=rr(-3000,3000);a.d.aw_sat_error=rr(-3000,3000);
        a.q.setpoint=rr(-700,700);a.q.recent_value=(int16_t)rr(-700,700);
        a.ms.i_d=rr(-500,500);a.ms.i_d_setpoint=rr(-100,100);
        legacy_t b=a;
        uint8_t hold=(uint8_t)(rr(0,9)==0); float qi=(float)rr(-1500,1500),di=(float)rr(-1500,1500);
        legacy_step(&a,hold,qi,di);
        foc_current_loop_result_t out;
        foc_current_loop_step(&b.ms,&b.q,&b.d,hold,qi,di,&out);
        CHECK(memcmp(&a.ms,&b.ms,sizeof(a.ms))==0,"parity: MotorState outputs match legacy block exactly");
        CHECK(memcmp(&a.q,&b.q,sizeof(a.q))==0,"parity: Q PI state matches legacy block exactly");
        CHECK(memcmp(&a.d,&b.d,sizeof(a.d))==0,"parity: D PI state matches legacy block exactly");
        CHECK(out.saturated==a.sat,"parity: saturation result matches legacy block");
        CHECK(out.u_q_requested==a.ms.u_q_req && out.u_d_requested==a.ms.u_d_req,"parity: requested vector result matches legacy block");
        if(host_test_failures) break;
    }
    CHECK(host_test_failures==0,"2000 randomized states preserve the legacy PI/vector-limit behavior");
    return host_test_failures?1:0;
}
