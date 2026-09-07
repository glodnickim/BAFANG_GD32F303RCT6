#include "stop_trace.h"
#include "can_tx_queue.h"
#include <string.h>

#if STOP_TRACE_ENABLE

/* This isolated recorder has one ISR writer and explicit publication barriers.
 * Optimize it independently: the rest of the hardware build remains -O0. */
#if defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif

#if defined(__arm__) || defined(__thumb__)
#define ST_BARRIER() __asm__ volatile("dmb" ::: "memory")
#else
#define ST_BARRIER() __asm__ volatile("" ::: "memory")
#endif

typedef struct {
    uint16_t write, count, trigger, after;
    uint8_t fired, done;
} ring_t;
static struct {
    stop_trace_sample_t overview[STOP_TRACE_OVERVIEW];
    stop_trace_sample_t fast[3][STOP_TRACE_FAST];
    ring_t ring[4];
    volatile uint8_t state, pending;
    uint8_t generation, reason, primed;
    uint32_t arm_tick, trigger_tick, last_overview, end_tick;
    int16_t prev_ref, min_iq, max_iq;
    uint16_t prev_theta, peak_id, peak_angle, events;
    uint8_t prev_quiet, prev_qzero;
    uint32_t max_irq_cycles, core_hz;
    uint8_t irq_overrun;
} R;
static struct {
    bool active, failed, loaded;
    uint8_t stage, fragment;
    uint16_t record, next;
    uint32_t crc, last_tick;
    can_tx_token_t token;
    uint8_t block[STOP_TRACE_BYTES];
} D;

_Static_assert(sizeof(stop_trace_sample_t) == STOP_TRACE_BYTES, "STOP-TRACE sample size");
_Static_assert(sizeof(R) + sizeof(D) <= 27500U, "STOP-TRACE RAM budget");

static uint16_t magnitude(int16_t v) { return (uint16_t)(v < 0 ? -(int32_t)v : v); }
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { put16(p,(uint16_t)v); put16(p+2,(uint16_t)(v>>16)); }

bool stop_trace_fast_needed(void) { return R.pending || R.state==ST_ARMED || R.state==ST_CAPTURING; }
bool stop_trace_dump_busy(void) { return D.active; }
bool stop_trace_arm(void)
{
    if (D.active || R.pending) return false;
    R.pending=1U;
    return true;
}

static void append(uint8_t stream, const stop_trace_sample_t *s, bool trigger)
{
    ring_t *r=&R.ring[stream];
    uint16_t capacity=stream ? STOP_TRACE_FAST : STOP_TRACE_OVERVIEW;
    uint16_t pre=stream ? STOP_TRACE_PRE : 16U;
    if (r->done) return;
    stop_trace_sample_t *slots=stream ? R.fast[stream-1U] : R.overview;
    slots[r->write]=*s;
    if (++r->write==capacity) r->write=0U;
    if (r->count<capacity) r->count++;
    if (!r->fired) {
        if (trigger) { r->fired=1U; r->trigger=(uint16_t)(r->count-1U); r->after=1U; }
        else if (r->count>pre) r->count=pre;
    } else r->after++;
    if (stream && r->fired && r->after>=STOP_TRACE_FAST-STOP_TRACE_PRE) r->done=1U;
}

static void finish(uint8_t reason, uint32_t tick)
{
    R.reason=reason; R.end_tick=tick;
    /* Untriggered fast rings are explicitly absent, never exported as an event. */
    for (unsigned j=1;j<4;j++) if (!R.ring[j].fired) R.ring[j].count=0U;
    ST_BARRIER();
    R.state=ST_FROZEN;
}

void stop_trace_tick(const stop_trace_sample_t *in)
{
    if (!in) return;
    if (R.pending) {
        R.state=ST_IDLE;
        /* O(1) metadata reset: never clear the sample storage in the ISR. */
        memset(R.ring,0,sizeof(R.ring));
        for (unsigned j=0;j<4;j++) R.ring[j].trigger=0xFFFFU;
        R.generation++; R.reason=ST_END_NONE; R.primed=0U;
        R.max_irq_cycles=0U; R.core_hz=0U; R.irq_overrun=0U;
        R.arm_tick=in->tick; R.last_overview=in->tick;
        R.trigger_tick=0U; R.end_tick=0U; R.events=0U;
        R.peak_id=0U; R.peak_angle=0U;
        R.min_iq=in->iq; R.max_iq=in->iq;
        R.state=ST_ARMED;
        ST_BARRIER(); R.pending=0U;
    }
    if (R.state!=ST_ARMED && R.state!=ST_CAPTURING) return;
    stop_trace_sample_t s=*in;
    if (R.primed && R.prev_qzero!=0U && s.qzero==0U) s.flags|=ST_EXIT;
    R.prev_qzero=s.qzero;
    bool quiet=(s.flags&ST_QUIET)!=0U;
    uint16_t delta=R.primed ? magnitude((int16_t)(s.theta-R.prev_theta)) : 0U;
    if (delta>=911U) s.flags|=ST_ANGLE_JUMP; /* 5 electrical degrees */
    bool trigger=R.primed && R.state==ST_ARMED && s.iq_ref==0 &&
        (R.prev_ref!=0 || (quiet && !R.prev_quiet));
    R.prev_ref=s.iq_ref; R.prev_quiet=(uint8_t)quiet;
    R.prev_theta=s.theta; R.primed=1U;
    if (trigger) { R.trigger_tick=s.tick; R.state=ST_CAPTURING; }
    s.iq_min=s.iq_max=s.iq; s.id_peak=magnitude(s.id); s.theta_step_peak=delta;
    if (s.iq<R.min_iq) R.min_iq=s.iq;
    if (s.iq>R.max_iq) R.max_iq=s.iq;
    if (s.id_peak>R.peak_id) R.peak_id=s.id_peak;
    if (delta>R.peak_angle) R.peak_angle=delta;
    /* Event OR plus bad-sample evidence for the WHOLE overview interval. */
    R.events|=s.flags & (ST_ENTRY|ST_HANDBACK|ST_EXIT|ST_ABORT|ST_ANGLE_JUMP|ST_BAD_SAMPLE);

    append(1U,&s,trigger);
    append(2U,&s,R.state==ST_CAPTURING && (s.flags&(ST_HANDBACK|ST_ABORT))!=0U);
    append(3U,&s,R.state==ST_CAPTURING && delta>=911U &&
        (s.hall_age>=80U || s.erps<=10U));

    bool stopped=R.state==ST_CAPTURING && s.iq_ref==0 && s.qzero==0U &&
        s.hall_age>=2000U && (uint32_t)(s.tick-R.trigger_tick)>=8000U;
    bool timeout=R.state==ST_CAPTURING && (uint32_t)(s.tick-R.trigger_tick)>=112000U;
    if (trigger || stopped || timeout || (s.flags&(ST_ENTRY|ST_HANDBACK|ST_EXIT|ST_ABORT)) ||
        (uint32_t)(s.tick-R.last_overview)>=STOP_TRACE_INTERVAL) {
        stop_trace_sample_t overview=s;
        overview.iq_min=R.min_iq; overview.iq_max=R.max_iq;
        overview.id_peak=R.peak_id; overview.theta_step_peak=R.peak_angle;
        overview.flags|=R.events;
        append(0U,&overview,trigger);
        R.last_overview=s.tick; R.min_iq=R.max_iq=s.iq;
        R.peak_id=R.peak_angle=R.events=0U;
    }
    if (stopped) finish(ST_END_STOPPED,s.tick);
    else if (timeout) finish(ST_END_TIMEOUT,s.tick);
    else if (R.state==ST_CAPTURING && R.ring[0].count==STOP_TRACE_OVERVIEW) finish(ST_END_FULL,s.tick);
    else if (R.state==ST_ARMED && (uint32_t)(s.tick-R.arm_tick)>=480000U) finish(ST_END_NO_TRIGGER,s.tick);
}

uint16_t stop_trace_count(uint8_t stream)
{
    if (R.state!=ST_FROZEN || R.pending || stream>=4U) return 0U;
    ST_BARRIER(); return R.ring[stream].count;
}
void stop_trace_timing(uint32_t cycles, uint32_t core_hz)
{
    if (!core_hz) return;
    if (cycles>R.max_irq_cycles) R.max_irq_cycles=cycles;
    R.core_hz=core_hz;
    if (cycles>=core_hz/16000U) R.irq_overrun=1U;
}
bool stop_trace_sample(uint8_t stream,uint16_t i,stop_trace_sample_t *out)
{
    if (!out || i>=stop_trace_count(stream)) return false;
    const ring_t *r=&R.ring[stream];
    uint16_t capacity=stream ? STOP_TRACE_FAST : STOP_TRACE_OVERVIEW;
    uint16_t pos=(uint16_t)((r->write+capacity-r->count+i)%capacity);
    *out=stream ? R.fast[stream-1U][pos] : R.overview[pos];
    return true;
}
void stop_trace_status(uint8_t d[8])
{
    memset(d,0,8); d[0]=STOP_TRACE_SCHEMA; d[1]=R.state; d[2]=R.generation;
    d[3]=4U|(D.active?1U:0U)|(D.failed?2U:0U)|(R.pending?8U:0U)|(R.irq_overrun?32U:0U);
    if (R.state==ST_FROZEN && !R.pending) {
        ST_BARRIER(); d[3]|=16U; put16(d+4,R.ring[0].count); d[6]=R.reason;
        for (unsigned j=1;j<4;j++) if (R.ring[j].done) d[7]|=(uint8_t)(1U<<(j-1));
    }
}

static void serialize(const stop_trace_sample_t *s,uint8_t *d)
{
    put32(d,s->tick);
    const uint16_t v[]={ (uint16_t)s->iq_ref,(uint16_t)s->iq,(uint16_t)s->id,
        (uint16_t)s->uq,(uint16_t)s->ud,(uint16_t)s->piq,(uint16_t)s->pid,
        s->theta,s->hall_age,s->hall_timer,s->erps,s->vbus_10mv,
        (uint16_t)s->iq_min,(uint16_t)s->iq_max,s->id_peak,s->theta_step_peak,s->flags };
    for (unsigned j=0;j<17;j++) put16(d+4+j*2,v[j]);
    d[38]=s->hall; d[39]=s->qzero;
}
static void metadata(uint8_t *d)
{
    memset(d,0,STOP_TRACE_BYTES);
    d[0]=STOP_TRACE_SCHEMA; d[1]=R.generation;
    d[2]=(QUIET_ZERO_ENABLE?1U:0U)|(CANONICAL_ANGLE_ENABLE?2U:0U); d[3]=R.reason;
    if (R.core_hz>=1000000U) {
        uint32_t per_us=R.core_hz/1000000U;
        uint32_t us=(R.max_irq_cycles+per_us-1U)/per_us;
        d[2]|=4U|(R.irq_overrun?8U:0U);
        put16(d+34,(uint16_t)(us>65535U?65535U:us));
    }
    put32(d+4,16000U); put16(d+8,STOP_TRACE_INTERVAL); put16(d+10,STOP_TRACE_BYTES);
    for (unsigned j=0;j<4;j++) { put16(d+12+j*2,R.ring[j].count); put16(d+24+j*2,R.ring[j].trigger); }
    put32(d+20,R.trigger_tick); put16(d+32,CAL_I); put32(d+36,R.end_tick);
}
static void crc_block(const uint8_t *data)
{
    for (unsigned j=0;j<STOP_TRACE_BYTES;j++) {
        D.crc^=data[j];
        for (unsigned b=0;b<8;b++) D.crc=(D.crc>>1)^((D.crc&1U)?0xEDB88320U:0U);
    }
}
bool stop_trace_dump_request(void)
{
    if (R.state!=ST_FROZEN || R.pending || D.active) return false;
    memset(&D,0,sizeof(D)); D.active=true; D.crc=0xFFFFFFFFU;
    return true;
}
void stop_trace_dump_step(uint32_t now,bool allow_tx)
{
    if (!D.active) return;
    if (D.token!=CANQ_TOKEN_INVALID) {
        can_tx_token_state_t state=can_tx_queue_token_state(D.token);
        if (state==CANQ_TOKEN_PENDING) return;
        D.token=CANQ_TOKEN_INVALID;
        if (state!=CANQ_TOKEN_DONE) { D.failed=true; D.active=false; return; }
        if (++D.fragment==8U) {
            D.fragment=0U; D.loaded=false;
            if (D.stage==2U) { D.active=false; return; }
            if (D.stage==0U) D.stage=1U;
        }
    }
    /* At most one low-priority frame, 5 ms apart; never during movement or
     * while a critical protocol frame is waiting. Host log loss is checked by CRC. */
    if (!allow_tx || can_tx_queue_depth()!=0U || (uint32_t)(now-D.last_tick)<20U) return;
    if (!D.loaded) {
        if (D.stage==0U) { metadata(D.block); D.record=0xFFFFU; }
        else {
            stop_trace_sample_t s; bool found=false;
            while (D.next<STOP_TRACE_OVERVIEW+3U*STOP_TRACE_FAST) {
                uint16_t n=D.next++;
                uint8_t stream=n<STOP_TRACE_OVERVIEW ? 0U : (uint8_t)(1U+(n-STOP_TRACE_OVERVIEW)/STOP_TRACE_FAST);
                uint16_t index=stream ? (uint16_t)((n-STOP_TRACE_OVERVIEW)%STOP_TRACE_FAST) : n;
                if (stop_trace_sample(stream,index,&s)) { serialize(&s,D.block); D.record=n; found=true; break; }
            }
            if (!found) {
                D.stage=2U; D.record=0xFFFEU; memset(D.block,0,STOP_TRACE_BYTES);
                memcpy(D.block,"DONE",4); put32(D.block+4,D.crc^0xFFFFFFFFU);
            }
        }
        if (D.stage!=2U) crc_block(D.block);
        D.loaded=true;
    }
    uint8_t data[8]; data[0]=R.generation; put16(data+1,D.record);
    memcpy(data+3,D.block+D.fragment*5U,5U);
    if (can_tx_queue_enqueue_tracked(STOP_TRACE_DATA_ID+D.fragment,8U,data,&D.token)) D.last_tick=now;
}

#if defined(__GNUC__)
#pragma GCC pop_options
#endif
#else
void stop_trace_timing(uint32_t c,uint32_t hz) { (void)c;(void)hz; }
bool stop_trace_fast_needed(void) { return false; }
void stop_trace_tick(const stop_trace_sample_t *in) { (void)in; }
bool stop_trace_arm(void) { return false; }
void stop_trace_status(uint8_t d[8]) { memset(d,0,8); d[0]=STOP_TRACE_SCHEMA; }
bool stop_trace_dump_request(void) { return false; }
bool stop_trace_dump_busy(void) { return false; }
void stop_trace_dump_step(uint32_t t,bool a) { (void)t;(void)a; }
uint16_t stop_trace_count(uint8_t s) { (void)s;return 0; }
bool stop_trace_sample(uint8_t s,uint16_t i,stop_trace_sample_t *o) { (void)s;(void)i;(void)o;return false; }
#endif
