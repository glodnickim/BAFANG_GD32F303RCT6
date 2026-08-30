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
	qs_transition_diag_init();
	for (uint32_t n=0; n<80; n++) tick(in(n)); /* T1: ring wraps before trigger */
	qs_transition_input_t x=in(80); x.run_request=true; x.iq_requested=10; tick(x); /* T3/T7 */
	for (uint32_t n=81; n<81+QS_TRANSITION_DIAG_POST_SAMPLES; n++) tick(in(n));
	CHECK(qs_transition_diag_is_complete(), "T4 complete exactly after post capacity");
	CHECK(qs_transition_diag_sample_at(0,&s) && s.foc_cycle==69, "T1/T2 chronological wrapped prehistory");
	CHECK(qs_transition_diag_sample_at(qs_transition_diag_trigger_index(),&s) && (s.events&QS_EVT_RUN_RISE), "T3 trigger cycle marked");
	qs_transition_diag_sample_at(QS_TRANSITION_DIAG_SAMPLES-1,&s); uint32_t frozen=s.foc_cycle; tick(in(1000)); CHECK(qs_transition_diag_sample_at(QS_TRANSITION_DIAG_SAMPLES-1,&s) && s.foc_cycle==frozen, "T5 complete freezes writer");

	qs_transition_diag_init(); tick(in(0)); x=in(1); x.iq_requested=5; x.iq_ref=5; tick(x); x=in(2); x.iq_requested=0; x.iq_ref=0; tick(x);
	for(uint32_t n=3;n<3+QS_TRANSITION_DIAG_POST_SAMPLES;n++) tick(in(n));
	CHECK(qs_transition_diag_sample_at(qs_transition_diag_trigger_index()+1,&s) && (s.events&(QS_EVT_IQ_TARGET_ZERO|QS_EVT_IQ_REF_ZERO)), "T8 stop markers");

	qs_transition_diag_init(); tick(in(0)); x=in(1); x.fault_hard_off=true; tick(x);
	for(uint32_t n=2;n<2+QS_TRANSITION_DIAG_POST_SAMPLES;n++) tick(in(n));
	CHECK(qs_transition_diag_sample_at(qs_transition_diag_trigger_index(),&s) && (s.events&QS_EVT_FAULT_HARD_OFF), "T9 fault distinct");
	qs_transition_diag_init(); CHECK(!qs_transition_diag_is_complete() && qs_transition_diag_capture_id()==0, "T6 rearm resets metadata");
	printf("qs_transition_diag_host: %s\n", failed ? "FAIL" : "PASS"); return failed != 0;
}
