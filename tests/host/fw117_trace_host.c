/*
 * FW-117.1 host suite: prove the bridge lifecycle trace's three reliability fixes, against the
 * REAL src/fw117_trace.c module (built for the host, CAN_DIAGNOSTICS_ENABLE=1).
 *
 * This file is compiled TWICE by run-host-tests.ps1 - once with the default
 * FW117_TRACE_TRIGGER_EVENT (0 = START) and once with -DFW117_TRACE_TRIGGER_EVENT=1 (STOP) - so
 * every scenario below runs against both bench-image variants. Tests that do not depend on which
 * edge is the trigger (the two "first observation" tests) are unconditional; the rest use the
 * BASE_PWM/BASE_MOE/toggle_ignored_edge()/fire_trigger_edge() seam below, which is the ONLY
 * thing that differs between the two builds.
 *
 * A fourth, separate check - that an out-of-range FW117_TRACE_TRIGGER_EVENT value is a compile
 * error, not a runtime one - cannot be expressed as a CHECK() in a program that has to link and
 * run; it is instead its own suite entry in run-host-tests.ps1 (fw117_trace_bad_selector_host.c,
 * ExpectBuildFailure = $true) that compiles a tiny probe file with
 * -DFW117_TRACE_TRIGGER_EVENT=2 and asserts the BUILD fails.
 *
 * Scenario coverage (the FW-117.1 card's acceptance list):
 *
 *   test_first_observation_no_false_stop()   the very first tick() call, with MOE=0/PWM=0 (the
 *                                             ordinary case: the bridge is off before a ride
 *                                             starts), opens no capture. Before this fix,
 *                                             prev_moe started at a sentinel 0xFF that is
 *                                             truthy in `!in->moe && R.prev_moe`, so this exact
 *                                             observation produced a false STOP.
 *   test_first_observation_no_false_start()  the very first tick() call, with MOE=1/PWM=1,
 *                                             likewise opens no capture.
 *   test_ignored_edge_never_opens_or_rejects()
 *                                             the edge FW117_TRACE_TRIGGER_EVENT did NOT select
 *                                             never opens a capture and is never counted in
 *                                             rejected_total, whether it happens before or
 *                                             after the ring is warm.
 *   test_early_trigger_before_warm_is_ignored()
 *                                             the selected edge, fired long before
 *                                             FW117_TRACE_SAMPLES real samples exist, opens
 *                                             nothing (silently - not counted as rejected
 *                                             either); the SAME edge fired again once the ring
 *                                             is warm opens a real capture, with the correct
 *                                             event_type, exactly FW117_TRACE_SAMPLES queued
 *                                             samples, all of them real (drain_and_check's
 *                                             floor-tick check), in chronological order, with
 *                                             the trigger-sample flag set exactly once.
 *   test_frozen_rejects_further_trigger_edges()
 *                                             a second real trigger edge while the first
 *                                             capture is still FROZEN (undrained) is refused -
 *                                             rejected_total rises by exactly one, capture_id
 *                                             and accepted_total are unchanged; draining the
 *                                             first capture re-arms the ring (fsm -> IDLE) and
 *                                             a fresh trigger completes a second, independent
 *                                             capture (capture_id advances, no re-warm needed -
 *                                             the ring's warm-up latch is one-time, not
 *                                             per-capture).
 *
 * Wire/serialization (frame layout, missing-fragment handling) is NOT this suite's concern - see
 * tools/decode_fw117_trace.ps1's own synthetic-log check for that.
 */

#include "check.h"

#include "fw117_trace.h"

#include <string.h>

static uint32_t tick;
static fw117_trace_input_t in;

static void feed_n(bool pwm_on, bool moe, uint32_t n)
{
	in.pwm_on = pwm_on;
	in.moe = moe;
	in.cen = moe;
	for (uint32_t i = 0; i < n; i++) {
		fw117_trace_tick(&in, tick);
		tick++;
	}
}

static void reset(void)
{
	tick = 1000U;   /* nonzero base: a leaked memset-zeroed tick_abs would read as < 1000 */
	memset(&in, 0, sizeof(in));
	fw117_trace_init();
	fw117_trace_set_session_id(1U);
}

/* Comfortably more real ticks than FW117_TRACE_SAMPLES*FW117_TRACE_DECIMATION of constant,
 * non-edge state - guarantees warm_samples saturates regardless of how many samples any earlier
 * feed_n() call in the same test already wrote, with no need to track exact counts by hand. */
static void warm_up(bool pwm_on, bool moe)
{
	feed_n(pwm_on, moe,
	       (uint32_t)FW117_TRACE_SAMPLES * FW117_TRACE_DECIMATION + FW117_TRACE_DECIMATION * 4U);
}

static void test_first_observation_no_false_stop(void)
{
	reset();
	feed_n(false, false, 1U);   /* the priming call: MOE=0/PWM=0 */
	/* Same state, no edge possible - if the priming call had wrongly opened a capture, it would
	 * have frozen well before this many more ticks. */
	feed_n(false, false, (FW117_TRACE_POST_SAMPLES + FW117_TRACE_SAMPLES) * FW117_TRACE_DECIMATION);
	CHECK(fw117_trace_queue_enqueued() == 0U, "priming MOE=0/PWM=0: no capture ever completed");
	CHECK(fw117_trace_queue_rejected() == 0U, "priming MOE=0/PWM=0: nothing was rejected either");
	CHECK(fw117_trace_capture_id() == 0U, "priming MOE=0/PWM=0: capture_id never advanced");
	CHECK(fw117_trace_queue_count_session(1U) == 0U, "priming MOE=0/PWM=0: nothing queued");
}

static void test_first_observation_no_false_start(void)
{
	reset();
	feed_n(true, true, 1U);   /* the priming call: MOE=1/PWM=1 */
	feed_n(true, true, (FW117_TRACE_POST_SAMPLES + FW117_TRACE_SAMPLES) * FW117_TRACE_DECIMATION);
	CHECK(fw117_trace_queue_enqueued() == 0U, "priming MOE=1/PWM=1: no capture ever completed");
	CHECK(fw117_trace_queue_rejected() == 0U, "priming MOE=1/PWM=1: nothing was rejected either");
	CHECK(fw117_trace_capture_id() == 0U, "priming MOE=1/PWM=1: capture_id never advanced");
}

/*
 * The only mode-dependent seam. BASE = the quiescent state the module sits in between events for
 * this build (idle/off for a START-triggered image, running/on for a STOP-triggered image).
 * toggle_ignored_edge() fires the edge this build does NOT capture and returns to BASE.
 * fire_trigger_edge() fires the edge this build DOES capture, leaving the state at POST.
 */
#if FW117_TRACE_TRIGGER_EVENT
#define BASE_PWM true
#define BASE_MOE true
#define POST_PWM true
#define POST_MOE false
static const uint8_t EXPECTED_EVENT = FW117_EVENT_STOP;
static void toggle_ignored_edge(void) { feed_n(false, true, 1U); feed_n(true, true, 1U); }
static void fire_trigger_edge(void) { feed_n(true, false, 1U); }
#else
#define BASE_PWM false
#define BASE_MOE false
#define POST_PWM true
#define POST_MOE true
static const uint8_t EXPECTED_EVENT = FW117_EVENT_START;
static void toggle_ignored_edge(void) { feed_n(false, true, 1U); feed_n(false, false, 1U); }
static void fire_trigger_edge(void) { feed_n(true, true, 1U); }
#endif

static void test_ignored_edge_never_opens_or_rejects(void)
{
	reset();
	feed_n(BASE_PWM, BASE_MOE, 1U);   /* priming */

	for (int i = 0; i < 30; i++) toggle_ignored_edge();   /* well before warm-up */
	CHECK(fw117_trace_queue_enqueued() == 0U, "ignored edges before warm-up open nothing");
	CHECK(fw117_trace_queue_rejected() == 0U, "ignored edges before warm-up are not rejected");

	warm_up(BASE_PWM, BASE_MOE);

	for (int i = 0; i < 30; i++) toggle_ignored_edge();   /* now well after warm-up */
	CHECK(fw117_trace_queue_enqueued() == 0U, "ignored edges after warm-up still open nothing");
	CHECK(fw117_trace_queue_rejected() == 0U, "ignored edges after warm-up are still not rejected");
	CHECK(fw117_trace_capture_id() == 0U, "capture_id never advanced from an ignored edge");
}

/* Drains a completed capture, checking that every sample is real data (never a memset-zeroed
 * unwritten slot), in chronological tick_abs order, with the trigger-sample flag set exactly
 * once, and that the drain hands back exactly FW117_TRACE_SAMPLES records. */
static void drain_and_check(uint32_t floor_tick)
{
	fw117_trace_sample_t s;
	uint16_t n = 0U;
	uint32_t prev = 0U;
	int have_prev = 0;
	int start_cap = 0;

	while (fw117_trace_queue_peek_session(1U, &s)) {
		CHECK(s.tick_abs >= floor_tick, "no unwritten/zeroed slot leaked into the drained capture");
		if (have_prev) CHECK(s.tick_abs > prev, "drained samples come out in chronological order");
		prev = s.tick_abs;
		have_prev = 1;
		if ((s.flags & FW117_FLAG_START_CAP) != 0U) start_cap++;
		fw117_trace_queue_release_session(1U);
		n++;
	}
	CHECK(n == FW117_TRACE_SAMPLES, "a drained capture holds exactly FW117_TRACE_SAMPLES real samples");
	CHECK(start_cap == 1, "the trigger-sample flag is set exactly once");
}

static void test_early_trigger_before_warm_is_ignored(void)
{
	reset();
	feed_n(BASE_PWM, BASE_MOE, 1U);    /* priming */
	feed_n(BASE_PWM, BASE_MOE, 40U);   /* a little history - nowhere near FW117_TRACE_SAMPLES */

	fire_trigger_edge();               /* the real trigger, but far too early */
	feed_n(POST_PWM, POST_MOE, (FW117_TRACE_POST_SAMPLES + 8U) * FW117_TRACE_DECIMATION);
	CHECK(fw117_trace_queue_enqueued() == 0U, "an early trigger before the ring is warm opens nothing");
	CHECK(fw117_trace_queue_rejected() == 0U, "an early trigger before the ring is warm is not rejected");
	CHECK(fw117_trace_capture_id() == 0U, "capture_id never advanced from the early trigger");

	/* Back to BASE, warm the ring the rest of the way, then fire the SAME edge for real. */
	feed_n(BASE_PWM, BASE_MOE, 1U);
	warm_up(BASE_PWM, BASE_MOE);

	fire_trigger_edge();
	feed_n(POST_PWM, POST_MOE, (FW117_TRACE_POST_SAMPLES + 4U) * FW117_TRACE_DECIMATION);
	CHECK(fw117_trace_queue_enqueued() == 1U, "the trigger fires once the ring is warm");
	CHECK(fw117_trace_event_type() == EXPECTED_EVENT, "event_type matches the compiled selector");
	CHECK(fw117_trace_queue_count_session(1U) == FW117_TRACE_SAMPLES, "queued count is a full capture");
	drain_and_check(1000U);
}

static void test_frozen_rejects_further_trigger_edges(void)
{
	reset();
	feed_n(BASE_PWM, BASE_MOE, 1U);
	warm_up(BASE_PWM, BASE_MOE);

	fire_trigger_edge();
	feed_n(POST_PWM, POST_MOE, (FW117_TRACE_POST_SAMPLES + 4U) * FW117_TRACE_DECIMATION);
	CHECK(fw117_trace_queue_enqueued() == 1U, "first capture completed");
	uint8_t first_capture_id = fw117_trace_capture_id();

	/* Capture is FROZEN and undrained. Back to BASE (the reverse edge - not this build's
	 * trigger - must not be counted here either), then fire the real trigger again: it must be
	 * refused, not silently accepted as a second capture. */
	feed_n(BASE_PWM, BASE_MOE, 1U);
	fire_trigger_edge();
	CHECK(fw117_trace_queue_rejected() == 1U, "a trigger edge while FROZEN is refused, counted once");
	CHECK(fw117_trace_queue_enqueued() == 1U, "the refusal does not create a second accepted capture");
	CHECK(fw117_trace_capture_id() == first_capture_id, "capture_id unchanged by a refused edge");

	/* Draining the first capture re-arms the ring; a fresh trigger completes a second capture
	 * with no re-warm needed (the warm-up latch is one-time, not per-capture). */
	drain_and_check(1000U);
	feed_n(BASE_PWM, BASE_MOE, 1U);
	fire_trigger_edge();
	feed_n(POST_PWM, POST_MOE, (FW117_TRACE_POST_SAMPLES + 4U) * FW117_TRACE_DECIMATION);
	CHECK(fw117_trace_queue_enqueued() == 2U, "re-armed after drain, a second capture completes");
	CHECK(fw117_trace_capture_id() == (uint8_t)(first_capture_id + 1U),
	      "capture_id advanced on the second capture");
	drain_and_check(1000U);
}

int main(void)
{
#if FW117_TRACE_TRIGGER_EVENT
	printf("FW-117.1 fw117_trace.c (bridge lifecycle trace), TRIGGER_EVENT=STOP\n");
#else
	printf("FW-117.1 fw117_trace.c (bridge lifecycle trace), TRIGGER_EVENT=START\n");
#endif

	test_first_observation_no_false_stop();
	test_first_observation_no_false_start();
	test_ignored_edge_never_opens_or_rejects();
	test_early_trigger_before_warm_is_ignored();
	test_frozen_rejects_further_trigger_edges();

	if (host_test_failures != 0) {
		printf("fw117_trace_host: %d FAILURES\n", host_test_failures);
		return 1;
	}
	printf("fw117_trace_host: ALL PASS\n");
	return 0;
}
