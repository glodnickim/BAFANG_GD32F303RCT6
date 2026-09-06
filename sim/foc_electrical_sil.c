#include "main.h"
#include "FOC.h"
#include "foc_current_loop.h"
#include "pwm_geometry.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define FAST_HZ 16000.0
#define DT (1.0/FAST_HZ)
#define CURRENT_A_PER_COUNT 0.095
#define VBUS 40.0
#define SQRT3 1.7320508075688772

typedef struct {
    double r,l_d,l_q,flux;
    double id,iq;
    double theta_e;
    double erps;
} pmsm_t;

typedef struct {
    double iq_peak_abs;
    double id_peak_abs;
    double iq_err_sum;
    double iq_err_sq_sum;
    int samples;
    int geometry_clamp_ticks;
    int saturated_ticks;
} metrics_t;

MotorState_t MS;
MotorParams_t MP;
PI_control_t PI_iq,PI_id;
uint8_t ui_8_PWM_ON_Flag=1U;
uint8_t bridge_lifecycle=5U;
int32_t switchtime[3];
uint16_t pwm_applied[3];
static int32_t g_iq_target;
static uint8_t g_sat;
static int pwm_polarity=-1;

void timer_channel_output_pulse_value_config(uint32_t timer,uint16_t ch,uint32_t value){(void)timer;(void)ch;(void)value;}
void timer_primary_output_config(uint32_t timer,uint32_t enable){(void)timer;(void)enable;}
extern void svpwm(q31_t q31_u_alpha, q31_t q31_u_beta);

static void pi_init(PI_control_t *p,int limit_i){
    memset(p,0,sizeof(*p));p->gain_p=1.5f;p->gain_i=.01f;p->limit_i=(int16_t)limit_i;
    p->limit_output=1920;p->max_step=15;p->shift=11;p->aw_inv_kp_q15=21845;
}
void runPIcontrol(void){
    foc_current_loop_result_t r;
    PI_iq.recent_value=(int16_t)MS.i_q;
    PI_iq.setpoint=g_iq_target;
    foc_current_loop_step(&MS,&PI_iq,&PI_id,0U,0.0f,0.0f,&r);
    g_sat=r.saturated;
}

static double wrap_pi(double a){while(a>=M_PI)a-=2*M_PI;while(a< -M_PI)a+=2*M_PI;return a;}
static q31_t angle_q31(double a){
    a=wrap_pi(a); double x=a/M_PI*2147483648.0;
    if(x>=2147483647.0) x=2147483647.0;
    if(x< -2147483648.0) x=-2147483648.0;
    return (q31_t)llround(x);
}
static void dq_to_ab(double d,double q,double th,double *a,double *b){double c=cos(th),s=sin(th);*a=d*c-q*s;*b=d*s+q*c;}
static void ab_to_dq(double a,double b,double th,double *d,double *q){double c=cos(th),s=sin(th);*d=a*c+b*s;*q=-a*s+b*c;}
static void dq_current_to_phase_counts(const pmsm_t *p,int16_t *ia,int16_t *ib){
    double a,b;dq_to_ab(p->id,p->iq,p->theta_e,&a,&b);
    double ib_a=(-a+SQRT3*b)*0.5;
    long ca=llround(a/CURRENT_A_PER_COUNT),cb=llround(ib_a/CURRENT_A_PER_COUNT);
    if(ca>32767) ca=32767;
    if(ca<-32768) ca=-32768;
    if(cb>32767) cb=32767;
    if(cb<-32768) cb=-32768;
    *ia=(int16_t)ca;*ib=(int16_t)cb;
}
static void pwm_to_ab(const uint16_t p[3],double *alpha,double *beta){
    double da=(double)p[0]/3750.0,db=(double)p[1]/3750.0,dc=(double)p[2]/3750.0;
    double mean=(da+db+dc)/3.0;
    double va=pwm_polarity*VBUS*(da-mean),vb=pwm_polarity*VBUS*(db-mean);
    *alpha=va;*beta=(va+2.0*vb)/SQRT3;
}
static void plant_step(pmsm_t *p,double vd,double vq){
    /* Sub-step the electrical plant. The production controller still runs at 16 kHz;
     * only the virtual motor integration is finer so a voltage-limited high-speed case
     * cannot be mistaken for a numerical Euler instability. */
    const int substeps=8;
    const double h=DT/(double)substeps;
    double we=2.0*M_PI*p->erps;
    for(int k=0;k<substeps;k++){
        double did=(vd-p->r*p->id+we*p->l_q*p->iq)/p->l_d;
        double diq=(vq-p->r*p->iq-we*(p->l_d*p->id+p->flux))/p->l_q;
        p->id += did*h;
        p->iq += diq*h;
        p->theta_e=wrap_pi(p->theta_e+we*h);
    }
    if(fabs(p->id)>500.0||fabs(p->iq)>500.0){
        fprintf(stderr,"plant out of characterized envelope id=%.1f iq=%.1f\n",p->id,p->iq);
        exit(4);
    }
}
static void reset_all(pmsm_t *p,double theta,double erps){
    memset(&MS,0,sizeof(MS));memset(&MP,0,sizeof(MP));memset(switchtime,0,sizeof(switchtime));memset(pwm_applied,0,sizeof(pwm_applied));
    pi_init(&PI_iq,1920);pi_init(&PI_id,1800);MS.hall_angle_detect_flag=1;MS.i_d_setpoint=0;MP.com_mode=Hallsensor;
    memset(p,0,sizeof(*p));p->r=.060;p->l_d=80e-6;p->l_q=80e-6;p->flux=.015;p->theta_e=wrap_pi(theta);p->erps=erps;
    foc_current_feedback_reset(&MS);pwm_geometry_init();g_sat=0;
}
static void one_tick(pmsm_t *p,int iq_target,double angle_error,metrics_t *m){
    int16_t ia,ib;dq_current_to_phase_counts(p,&ia,&ib);g_iq_target=iq_target;
    FOC_calculation(ia,ib,angle_q31(p->theta_e+angle_error),(int16_t)iq_target,&MS,&MP);
    uint8_t cl=pwm_geometry_apply(switchtime,pwm_applied,3750);
    double a,b,vd,vq;pwm_to_ab(pwm_applied,&a,&b);ab_to_dq(a,b,p->theta_e,&vd,&vq);plant_step(p,vd,vq);
    if(m){double iqc=p->iq/CURRENT_A_PER_COUNT,err=(double)iq_target-iqc;if(fabs(iqc)>m->iq_peak_abs)m->iq_peak_abs=fabs(iqc);if(fabs(p->id/CURRENT_A_PER_COUNT)>m->id_peak_abs)m->id_peak_abs=fabs(p->id/CURRENT_A_PER_COUNT);m->iq_err_sum+=fabs(err);m->iq_err_sq_sum+=err*err;m->samples++;if(cl)m->geometry_clamp_ticks++;if(g_sat)m->saturated_ticks++;}
}
static int determine_polarity(void){
    pmsm_t p;reset_all(&p,0,0);g_iq_target=100;int16_t ia=0,ib=0;FOC_calculation(ia,ib,angle_q31(0),100,&MS,&MP);pwm_geometry_apply(switchtime,pwm_applied,3750);
    double vals[2];for(int k=0;k<2;k++){pwm_polarity=k?1:-1;double a,b,d,q;pwm_to_ab(pwm_applied,&a,&b);ab_to_dq(a,b,0,&d,&q);vals[k]=q;}
    int chosen=(MS.u_q>=0)?(vals[0]>vals[1]?-1:1):(vals[0]<vals[1]?-1:1);pwm_polarity=chosen;
    printf("PWM polarity calibration: u_q=%ld vq(-)=%+.4fV vq(+)=%+.4fV -> polarity=%+d\n",(long)MS.u_q,vals[0],vals[1],pwm_polarity);
    return ((MS.u_q>0 && ((chosen<0?vals[0]:vals[1])>0))||(MS.u_q<0 && ((chosen<0?vals[0]:vals[1])<0)))?0:1;
}
static int locked_step_test(int target,double angle_error_deg){
    pmsm_t p;metrics_t m={0};reset_all(&p,0,0);double err=angle_error_deg*M_PI/180.0;
    for(int i=0;i<800;i++) one_tick(&p,target,err,&m); /* 50 ms */
    double physical_q=p.iq/CURRENT_A_PER_COUNT,physical_d=p.id/CURRENT_A_PER_COUNT;
    /* Controller axes are rotated +err from the physical rotor axes. */
    double ctrl_q=-physical_d*sin(err)+physical_q*cos(err);
    double expected=target;
    double e=fabs(ctrl_q-expected);
    printf("locked target=%d angle=%+.0f: physical dq=(%+.1f,%+.1f) ctrl_q=%.1f u=%ld clamps=%d sat=%d\n",target,angle_error_deg,physical_d,physical_q,ctrl_q,(long)MS.u_abs,m.geometry_clamp_ticks,m.saturated_ticks);
    return (e>12.0 || fabs(physical_q)>900.0 || fabs(physical_d)>500.0);
}
static int moving_test(double erps,int target){
    pmsm_t p;metrics_t m={0};reset_all(&p,0,erps);
    for(int i=0;i<1600;i++) one_tick(&p,target,0,&m); /* 100 ms */
    double iqc=p.iq/CURRENT_A_PER_COUNT,idc=p.id/CURRENT_A_PER_COUNT;
    printf("moving erps=%5.1f target=%3d: iq=%+.1f id=%+.1f u=%ld clamp=%.1f%% sat=%.1f%%\n",erps,target,iqc,idc,(long)MS.u_abs,100.0*m.geometry_clamp_ticks/m.samples,100.0*m.saturated_ticks/m.samples);
    if(erps<=150.0) return fabs(iqc-target)>18.0 || fabs(idc)>20.0;
    return fabs(iqc)>900.0 || fabs(idc)>300.0;
}
static int six_sector_test(void){int fail=0;for(int s=0;s<6;s++){double center=s*M_PI/3.0;for(int off=-25;off<=25;off+=25){pmsm_t p;reset_all(&p,center+off*M_PI/180.0,0);for(int i=0;i<640;i++)one_tick(&p,80,-off*M_PI/180.0,NULL);double tq=p.iq/CURRENT_A_PER_COUNT;if(tq<45.0||tq>130.0)fail++;}}printf("6-sector standstill sweep: %s\n",fail?"FAIL":"PASS");return fail?1:0;}

static int svpwm_geometry_sweep(void){
    int fail=0;
    const int magnitudes[]={1170,1305,1600,1887,1920};
    puts("SVPWM requested-geometry 360deg sweep:");
    for(size_t m=0;m<sizeof(magnitudes)/sizeof(magnitudes[0]);m++){
        int32_t lo=INT32_MAX,hi=INT32_MIN;
        int clamp_angles=0;
        for(int deg=0;deg<360;deg++){
            double th=(double)deg*M_PI/180.0;
            q31_t s,c,a,b;
            arm_sin_cos_q31(angle_q31(th),&s,&c);
            arm_inv_park_q31(0,magnitudes[m],&a,&b,-s,c);
            svpwm(a,b);
            for(int p=0;p<3;p++){
                if(switchtime[p]<lo)lo=switchtime[p];
                if(switchtime[p]>hi)hi=switchtime[p];
            }
            uint16_t applied[3];
            if(pwm_geometry_apply(switchtime,applied,3750)) clamp_angles++;
        }
        printf("  u=%4d -> requested [%ld,%ld], clamp angles=%d/360\n",
               magnitudes[m],(long)lo,(long)hi,clamp_angles);
        if(magnitudes[m]<=1920 && clamp_angles!=0) fail++;
    }
    printf("SVPWM geometry within configured _U_MAX: %s\n",fail?"FAIL":"PASS");
    return fail?1:0;
}
int main(void){
    int fail=0;puts("EVistDrive electrical FOC SIL (real FOC.c + PI + SVPWM + PWM geometry)");fail+=determine_polarity();
    fail+=svpwm_geometry_sweep();
    fail+=locked_step_test(100,0);fail+=locked_step_test(200,0);fail+=locked_step_test(100,20);fail+=locked_step_test(100,-20);fail+=locked_step_test(100,30);fail+=locked_step_test(100,-30);
    fail+=six_sector_test();fail+=moving_test(50,100);fail+=moving_test(100,100);fail+=moving_test(150,100);fail+=moving_test(180,100);fail+=moving_test(200,100);fail+=moving_test(220,100);
    printf("electrical FOC SIL: %s\n",fail?"FAIL":"PASS");return fail?1:0;
}
