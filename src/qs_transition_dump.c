#include "qs_transition_dump.h"
#include "qs_transition_diag.h"
#include <string.h>
#if CAN_DIAGNOSTICS_ENABLE
#define QS_EFID_HEADER 0x00010250U
#define QS_EFID_DATA   0x00010251U
static struct { const diag_can_ops_t *ops; uint16_t index; uint8_t fragment, mailbox, pending, retry; bool armed, active; uint32_t last_tick; } D;
void qs_transition_dump_init(const diag_can_ops_t *ops) { memset(&D,0,sizeof(D)); D.ops=ops; }
bool qs_transition_dump_request(void) { if(!qs_transition_diag_is_complete() || D.armed || D.active) return false; D.armed=true; return true; }
bool qs_transition_dump_busy(void) { return D.armed || D.active; }
static void advance(void) { D.retry=0; if(++D.fragment < 7U) return; D.fragment=0; if(++D.index >= QS_TRANSITION_DIAG_SAMPLES) D.active=false; }
void qs_transition_dump_step(uint32_t now_tick, bool allow_new_tx, bool session_active)
{
	if(!D.ops || !qs_transition_diag_is_complete()) { D.armed=false; D.active=false; return; }
	if(D.pending) { uint8_t st=D.ops->state(D.mailbox); if(st==DIAG_CAN_PENDING)return; D.pending=0; if(st==DIAG_CAN_OK) advance(); else if(++D.retry>DIAG_TX_MAX_RETRY) D.active=false; }
	if(!D.active && D.armed && !session_active) { D.armed=false; D.active=true; D.index=0; D.fragment=0; D.retry=0; }
	if(!D.active || session_active || !allow_new_tx || (uint32_t)(now_tick-D.last_tick)<DIAG_TX_FRAME_INTERVAL_TICKS)return;
	qs_transition_sample_t s; uint8_t data[8]={0}; uint32_t id;
	if(!qs_transition_diag_sample_at(D.index,&s)) { D.active=false; return; }
	if(D.fragment==0U) { id=QS_EFID_HEADER; data[0]=1U; data[1]=qs_transition_diag_capture_id(); data[2]=(uint8_t)D.index; data[3]=(uint8_t)qs_transition_diag_trigger_index(); data[4]=44U; data[5]=6U; data[6]=48U; data[7]=0U; }
	else { id=QS_EFID_DATA+(uint32_t)(D.fragment-1U); memcpy(data, ((const uint8_t *)&s)+((uint16_t)(D.fragment-1U)*8U), 8U); }
	uint8_t box=D.ops->transmit(id,data); if(box==DIAG_CAN_NOMAILBOX)return; D.last_tick=now_tick; D.mailbox=box; D.pending=1;
}
#else
void qs_transition_dump_init(const diag_can_ops_t *ops) { (void)ops; }
bool qs_transition_dump_request(void) { return false; }
bool qs_transition_dump_busy(void) { return false; }
void qs_transition_dump_step(uint32_t now_tick, bool allow_new_tx, bool session_active) { (void)now_tick;(void)allow_new_tx;(void)session_active; }
#endif
