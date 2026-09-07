#include "stop_trace.h"
#include "can_tx_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, sent, token;
static bool pending, fail_tx, busy;
static FILE *log_file;
#define CHECK(c) do { if(!(c)) {printf("FAIL line %d: %s\n",__LINE__,#c);failures++;} } while(0)
uint16_t can_tx_queue_depth(void) { return busy?1U:0U; }
bool can_tx_queue_enqueue_tracked(uint32_t id,uint8_t len,const uint8_t d[8],can_tx_token_t *out)
{
    CHECK(len==8U); CHECK(id>=STOP_TRACE_DATA_ID && id<STOP_TRACE_DATA_ID+8U);
    *out=++token; sent++;
    if(log_file){
        fprintf(log_file,"[12:00:00] [INFO] %u ID:%08X DLC:8 Data:",sent*5000U,(unsigned)(id|0x80000000U));
        for(unsigned j=0;j<8;j++) fprintf(log_file,"%02X%s",d[j],j==7?"\n":" ");
    }
    return true;
}
can_tx_token_state_t can_tx_queue_token_state(can_tx_token_t t)
{ (void)t; return pending?CANQ_TOKEN_PENDING:fail_tx?CANQ_TOKEN_FAILED:CANQ_TOKEN_DONE; }

static stop_trace_sample_t input(uint32_t tick,int16_t ref)
{
    stop_trace_sample_t s={0}; s.tick=tick; s.iq_ref=ref;
    s.flags=ST_MOE|ST_PWM|ST_FRESH|ST_VALID|ST_FOC_RAN;
    s.erps=80U; s.vbus_10mv=4200U; return s;
}

static void capture(void)
{
    CHECK(stop_trace_arm()); CHECK(!stop_trace_arm());
    stop_trace_sample_t s=input(100U,0); stop_trace_tick(&s);
    uint8_t status[8]; stop_trace_status(status); CHECK(status[1]==ST_ARMED);
    /* Initial zero must never trigger; a real positive or negative -> zero does. */
    for(unsigned t=101;t<9000;t++){
        s=input(t,t<1000?100:0);
        if(t>=1000) s.flags|=ST_QUIET;
        if(t==1000){s.flags|=ST_ENTRY;s.qzero=1;}
        else if(t>1000 && t<1200)s.qzero=2;
        if(t==1200){s.flags|=ST_HANDBACK;s.qzero=3;}
        if(t==1250){s.theta=10000;s.hall_age=100;}
        if(t==1237){s.iq=-211;s.id=-77;}
        if(t>8900){s.hall_age=2000;s.erps=0;}
        stop_trace_tick(&s);
    }
    s=input(9000,0);s.hall_age=2000;stop_trace_tick(&s);
    stop_trace_status(status);CHECK(status[1]==ST_FROZEN);CHECK(status[6]==ST_END_STOPPED);
    CHECK(stop_trace_count(1)==96);CHECK(stop_trace_count(2)==96);CHECK(stop_trace_count(3)==96);
    stop_trace_sample_t got; CHECK(!stop_trace_sample(4,0,&got));
    CHECK(stop_trace_sample(1,32,&got));CHECK(got.tick==1000);CHECK(got.iq_ref==0);
    CHECK(stop_trace_sample(2,32,&got));CHECK(got.tick==1200);
    CHECK(stop_trace_sample(3,32,&got));CHECK(got.tick==1250);
    bool peak=false;
    for(unsigned j=0;j<stop_trace_count(0);j++){
        CHECK(stop_trace_sample(0,j,&got));if(got.iq_min==-211 && got.id_peak==77)peak=true;
    }
    CHECK(peak);
    s=input(10000,300);stop_trace_tick(&s);
    CHECK(stop_trace_sample(1,32,&got));CHECK(got.tick==1000); /* frozen is immutable */
}

int main(int argc,char **argv)
{
    uint8_t status[8];stop_trace_status(status);CHECK(status[1]==ST_IDLE);CHECK(!stop_trace_dump_request());
    capture();
    CHECK(stop_trace_dump_request());CHECK(!stop_trace_arm());
    unsigned start=sent;
    for(unsigned t=0;t<200;t++)stop_trace_dump_step(t,false);
    CHECK(sent==start);
    busy=true;stop_trace_dump_step(200,true);CHECK(sent==start);busy=false;
    pending=true;stop_trace_dump_step(200,true);CHECK(sent==start+1);
    for(unsigned t=201;t<240;t++)stop_trace_dump_step(t,true);
    CHECK(sent==start+1);
    pending=false;fail_tx=true;stop_trace_dump_step(240,true);fail_tx=false;
    CHECK(!stop_trace_dump_busy());stop_trace_status(status);CHECK(status[3]&2U);
    /* Timing extension is independent of CRC/transport success. */
    stop_trace_timing(5000U,120000000U);
    stop_trace_status(status);CHECK(!(status[3]&32U));
    stop_trace_timing(8000U,120000000U);
    stop_trace_status(status);CHECK(status[3]&32U);
    /* Repeat the same frozen generation after a failed transmission. */
    if(argc>1){log_file=fopen(argv[1],"w");CHECK(log_file!=NULL);}
    CHECK(stop_trace_dump_request());start=sent;
    for(unsigned t=0;t<200000 && stop_trace_dump_busy();t++)stop_trace_dump_step(t,true);
    CHECK(!stop_trace_dump_busy());stop_trace_status(status);CHECK(!(status[3]&2U));
    unsigned records=2;for(unsigned j=0;j<4;j++)records+=stop_trace_count(j);
    CHECK(sent-start==records*8U);
    if(log_file){fclose(log_file);log_file=NULL;}
    /* Re-arm resets only metadata; stale samples are not made visible. */
    CHECK(stop_trace_arm());CHECK(stop_trace_count(0)==0);
    stop_trace_sample_t s=input(0xFFFFF000U,-100);stop_trace_tick(&s);
    s=input(0xFFFFF001U,0);stop_trace_tick(&s);
    stop_trace_status(status);CHECK(status[1]==ST_CAPTURING);
    /* Fixed timeout works across counter wrap and does not depend on erps. */
    for(unsigned j=2;j<=120001;j++){s=input(0xFFFFF000U+j,0);s.qzero=2;stop_trace_tick(&s);}
    stop_trace_status(status);CHECK(status[1]==ST_FROZEN);CHECK(status[6]==ST_END_TIMEOUT);
    CHECK(stop_trace_arm());s=input(1,0);stop_trace_tick(&s);s.tick=480001;stop_trace_tick(&s);
    stop_trace_status(status);CHECK(status[6]==ST_END_NO_TRIGGER);CHECK(stop_trace_count(1)==0);
    printf("STOP-TRACE %s (%u failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
