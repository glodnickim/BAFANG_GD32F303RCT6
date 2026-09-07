#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "rotor_motion.h"
#include "rotor_angle.h"
#include "quiet_zero.h"
#undef assert
#define assert(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while (0)

static void motion_checks(void)
{
    rotor_motion_t m={0};
    assert(rotor_motion_angle_stale(&m,0));
    rotor_motion_note_edge(&m,64000,2000);
    assert(!rotor_motion_speed_fresh(&m,0)); /* first edge is just the anchor */
    rotor_motion_note_edge(&m,16,2000);
    assert(m.edge_erps==41 && m.period_ticks==16);
    assert(rotor_motion_speed_fresh(&m,33));
    assert(!rotor_motion_speed_fresh(&m,34));
    assert(!rotor_motion_angle_stale(&m,65));
    assert(rotor_motion_angle_stale(&m,66));
    uint16_t age=0, speed=41;
    /* Run actual production helpers with NO foreground or Hall calls. */
    for(unsigned i=0;i<8000;i++) {
        age=rotor_motion_age_next(age);
        uint16_t previous=speed;
        speed=rotor_motion_speed_ceiling(speed,age);
        assert(speed<=previous);
    }
    assert(age==8000 && speed==0 && m.edge_erps==41);
    assert(rotor_motion_age_next(64000)==64000);
    rotor_motion_note_edge(&m,age,1234); /* wrapped timer after standstill */
    assert(!m.valid);
    rotor_motion_note_edge(&m,20,2500);
    assert(m.valid && m.edge_erps==33);
    rotor_motion_note_edge(&m,524,80); assert(!m.valid);
    rotor_motion_note_edge(&m,10,0); assert(!m.valid);
    for(unsigned v=3;v<=400;v++) {
        unsigned interval=4000/(v*6);
        if(!interval) interval=1;
        for(unsigned a=0;a<=interval;a++) assert(rotor_motion_speed_ceiling(v,a)==v);
    }
}
static int32_t delta(int32_t a,int32_t b) { return (int32_t)((uint32_t)a-(uint32_t)b); }
static void angle_checks(void)
{
    for(int direction=-1;direction<=1;direction+=2) {
        for(unsigned fraction=0;fraction<=100;fraction++) {
            rotor_angle_state_t s; rotor_angle_reset(&s);
            rotor_angle_input_t in={0};
            in.direction=direction; in.fallback_sign=direction;
            in.hall_angle=(int32_t)0x7ffff000U; in.angle_correction=1000;
            in.tim2_recent=20000*fraction/100; in.tics_filtered_8=20000*8;
            in.hall_sequence_valid=true;
            s.trusted=1; s.edges=2;
            int32_t previous=rotor_angle_update(&s,&in);
            in.want_untrusted=true; in.stalled=true;
            int32_t out=rotor_angle_update(&s,&in);
            assert(out==previous && s.edges==0 && !s.trusted);
            for(unsigned t=0;t<400;t++) {
                previous=out; out=rotor_angle_update(&s,&in);
                assert(llabs((long long)delta(out,previous))<=ROTOR_ANGLE_TRANSFER_STEP);
            }
            assert(s.transfer_offset==0);
            /* No real Hall edge: many hardware wraps must not create history. */
            for(unsigned i=0;i<20;i++) {
                in.tim2_recent=65500; rotor_angle_update(&s,&in);
                in.tim2_recent=10; rotor_angle_update(&s,&in);
                assert(s.edges==0);
            }
            in.stalled=false; in.want_untrusted=false; in.tim2_recent=10010;
            in.hall_sequence++; rotor_angle_update(&s,&in);
            assert(s.edges==1 && !s.trusted);
            in.hall_sequence++; rotor_angle_update(&s,&in);
            assert(s.edges==2 && s.trusted);
            /* Losing trust earlier must not prevent a later stall reset. */
            in.want_untrusted=true; rotor_angle_update(&s,&in);
            assert(!s.trusted);
            in.stalled=true; rotor_angle_update(&s,&in); assert(s.edges==0);
        }
    }
}
static void qzero_checks(void)
{
    for(int sign=-1;sign<=1;sign+=2) {
        for(unsigned fade_tick=1;fade_tick<=160;fade_tick++) {
            quiet_zero_t q; quiet_zero_reset(&q);
            quiet_zero_input_t in={.iq_ref=100,.zero_policy_quiet=true,.rotor_erps=100,
                .speed_fresh=true,.min_brake_erps=10,.abort_current=1000,
                .iq_integral=sign*800,.id_integral=sign*160};
            quiet_zero_action_t out;
            quiet_zero_tick(&q,&in,&out); in.iq_ref=0;
            for(unsigned i=0;i<fade_tick;i++) {
                quiet_zero_tick(&q,&in,&out);
                if(out.apply_integral) { in.iq_integral=out.iq_integral; in.id_integral=out.id_integral; }
            }
            in.rotor_erps=40;
            float previous=in.iq_integral, previous_d=in.id_integral;
            quiet_zero_tick(&q,&in,&out);
            assert(out.state==QZERO_HANDBACK);
            assert(fabsf(out.iq_integral-previous)<=1.001f);
            assert(fabsf(out.id_integral-previous_d)<=0.201f);
            for(unsigned i=0;i<100;i++) {
                previous=out.iq_integral; previous_d=out.id_integral;
                in.iq_integral=previous; in.id_integral=previous_d;
                in.rotor_erps=(i&1)?5:39;
                quiet_zero_tick(&q,&in,&out);
                assert(fabsf(out.iq_integral-previous)<=1.001f);
                assert(fabsf(out.id_integral-previous_d)<=0.201f);
            }
            in.speed_fresh=false;
            quiet_zero_tick(&q,&in,&out);
            assert(out.state==QZERO_INACTIVE && !out.apply_integral && !out.freeze_aw && out.exited);
            quiet_zero_tick(&q,&in,&out); assert(out.state==QZERO_INACTIVE && !out.apply_integral);
            in.iq_ref=10; quiet_zero_tick(&q,&in,&out); assert(!out.apply_integral);
        }
    }
}
int main(void) {
    motion_checks(); angle_checks(); qzero_checks();
    puts("STOP-CLICK PASS: real time, freshness, no timer-wrap edges, 2-edge relearning, bounded angle, interrupted handback and stale PI handoff.");
    return 0;
}
