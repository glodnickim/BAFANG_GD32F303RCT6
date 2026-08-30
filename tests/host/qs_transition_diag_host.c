#include <stdio.h>
#include <string.h>
#include "qs_transition_diag.h"

static int failed;
#define CHECK(x, n) do { if (!(x)) { printf("FAIL %s\n", n); failed++; } } while (0)

static qs_transition_input_t in(uint32_t cycle)
{
	qs_transition_input_t x; memset(&x, 0, sizeof(x)); x.foc_cycle=cycle;
	x.hall=1; x.angle_static_legal=true; x.sample_state=1; x.ccr_a=100; x.ccr_b=101; x.ccr_c=102;
	return x;
}
static void tick(qs_transition_input_t x) { qs_transition_diag_fast_tick(&x); }

int main(void)
{
	qs_transition_sample_t s;
	qs_transition_status_t status;
	qs_transition_diag_init();
	for (uint32_t n=0; n<80; n++) tick(in(n)); /* T1: ring wraps before trigger */
	qs_transition_input_t x=in(80); x.run_request=true; x.iq_requested=10; tick(x); /* T3/T7 */
	for (uint32_t n=81; n<81+QS_TRANSITION_DIAG_POST_SAMPLES; n++) tick(in(n));
	CHECK(qs_transition_diag_is_complete(), "T4 complete exactly after post capacity");
	CHECK(qs_transition_diag_sample_at(0,&s) && s.foc_cycle==69, "T1/T2 chronological wrapped prehistory");
	CHECK(qs_transition_diag_sample_at(qs_transition_diag_trigger_index(),&s) && (s.events&QS_EVT_RUN_RISE), "T3 trigger cycle marked");
	qs_transition_diag_sample_at(QS_TRANSITION_DIAG_SAMPLES-1,&s); uint32_t frozen=s.foc_cycle; tick(in(1000)); CHECK(qs_transition_diag_sample_at(QS_TRANSITION_DIAG_SAMPLES-1,&s) && s.foc_cycle==frozen, "T5 complete freezes writer");
	qs_transition_diag_status(&status);
	CHECK(status.state==QS_TRANSITION_COMPLETE && status.sample_count==48 && status.generation==1 && status.export_ready, "T5 status complete and first generation");
	CHECK(qs_transition_diag_request_new_capture(), "T7 new capture accepted only after complete");
	tick(in(1001)); qs_transition_diag_status(&status);
	CHECK(status.state==QS_TRANSITION_ARMED && status.sample_count==0 && status.generation==2 && !status.export_ready, "T8 ISR-owned rearm clears metadata");
	x=in(1002); x.run_request=true; x.iq_requested=20; tick(x);
	CHECK(!qs_transition_diag_request_new_capture(), "T10 rearm refused while triggered");
	for(uint32_t n=1003;n<1003+QS_TRANSITION_DIAG_POST_SAMPLES;n++) tick(in(n));
	CHECK(qs_transition_diag_is_complete() && qs_transition_diag_sample_at(qs_transition_diag_trigger_index(),&s) && s.foc_cycle==1002, "T9 old capture cannot leak into new generation");

	qs_transition_diag_init(); tick(in(0)); x=in(1); x.iq_requested=5; x.iq_ref=5; tick(x); x=in(2); x.iq_requested=0; x.iq_ref=0; tick(x);
	for(uint32_t n=3;n<3+QS_TRANSITION_DIAG_POST_SAMPLES;n++) tick(in(n));
	CHECK(qs_transition_diag_sample_at(qs_transition_diag_trigger_index()+1,&s) && (s.events&(QS_EVT_IQ_TARGET_ZERO|QS_EVT_IQ_REF_ZERO)), "T8 stop markers");

	qs_transition_diag_init(); tick(in(0)); x=in(1); x.fault_hard_off=true; tick(x);
	for(uint32_t n=2;n<2+QS_TRANSITION_DIAG_POST_SAMPLES;n++) tick(in(n));
	CHECK(qs_transition_diag_sample_at(qs_transition_diag_trigger_index(),&s) && (s.events&QS_EVT_FAULT_HARD_OFF), "T9 fault distinct");
	qs_transition_diag_init(); CHECK(!qs_transition_diag_is_complete() && qs_transition_diag_capture_id()==1, "T6 power-on auto-arms generation one");
	printf("qs_transition_diag_host: %s\n", failed ? "FAIL" : "PASS"); return failed != 0;
}
