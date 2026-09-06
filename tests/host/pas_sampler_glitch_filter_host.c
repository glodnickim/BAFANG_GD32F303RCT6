/*
 * PAS sampler plausibility filter: real pas_sampler + quadrature + direction automaton.
 *
 * Regression target comes from the production FW-097 diagnostic comment in main.c:
 * during normal forward pedalling the real bike recorded reverse events with 1..3 control-tick
 * gaps, while a genuine quadrature step at 52 rpm is about 48 ticks. A 1..3 tick immediate
 * reversal is therefore electrical/contact bounce, not physically possible crank reversal.
 *
 * The safety requirement is two-sided:
 *   - an implausibly-fast reverse/invalid transition must NOT cut assist or corrupt cadence;
 *   - a reverse/invalid transition after the plausibility window must still fail safe immediately.
 */
#include "../common/check.h"

#include "pas_sampler.h"
#include "pas_direction.h"

static pas_step_event_t require_event(const char *label)
{
    pas_step_event_t ev = {0};
    CHECK(pas_sampler_pop(&ev) == 1, label);
    return ev;
}

static void drain_to_direction(void)
{
    pas_step_event_t ev;
    while (pas_sampler_pop(&ev)) {
        pas_direction_on_step(ev.step);
    }
}

static void reset_all(uint8_t initial, uint32_t tick)
{
    pas_sampler_init(tick);
    pas_direction_init();
    pas_sampler_isr_tick(initial, tick);
}

int main(void)
{
    printf("PAS sampler glitch plausibility filter - real sampler + direction automaton\n");

    /* A. Clean forward edge establishes ordinary operation. With PAS_DIR_SIGN=-1 on this
     * hardware, raw 00->10 is the production-forward transition (the pure decoder test proves
     * the sign separately). */
    reset_all(0U, 0U);
    pas_sampler_isr_tick(2U, 20U); /* 00 -> 10: forward after sign */
    {
        pas_step_event_t ev = require_event("A1: clean forward edge is queued");
        CHECK(ev.step > 0, "A2: 00->10 is production-forward");
        pas_direction_on_step(ev.step);
        CHECK(!pas_direction_direction_inhibit_active(), "A3: clean forward remains direction-safe");
        CHECK(pas_sampler_last_transition_tick() == 20U, "A4: clean edge owns transition timestamp");
    }

    /* B. Same line bounces back one tick later, then returns. It must disappear below the
     * physical-event layer: no reverse event, no qstate retreat, no direction inhibit, and the
     * genuine edge timestamp remains the cadence anchor. */
    pas_sampler_isr_tick(0U, 21U); /* 10 -> 00 would decode reverse, gap=1 */
    CHECK(pas_sampler_pop(&(pas_step_event_t){0}) == 0, "B1: 1-tick reverse bounce is not published");
    CHECK(pas_sampler_state() == 2U, "B2: rejected bounce does not move authoritative qstate");
    CHECK(pas_sampler_last_transition_tick() == 20U, "B3: rejected bounce does not reset period anchor");
    CHECK(pas_sampler_get_stats()->reverse_count == 0U, "B4: rejected bounce is not counted as physical reverse");
    CHECK(pas_sampler_get_stats()->glitch_count == 1U, "B5: rejected bounce is counted separately as a glitch");
    CHECK(!pas_direction_direction_inhibit_active(), "B6: bounce cannot cut assist through direction safety");

    pas_sampler_isr_tick(2U, 22U); /* line settles back to authoritative state */
    CHECK(pas_sampler_pop(&(pas_step_event_t){0}) == 0, "B7: bounce settling generates no event");
    CHECK(pas_sampler_get_stats()->glitch_count == 1U, "B8: settling does not double-count the glitch");

    /* The next real forward edge is measured from the previous REAL edge, not from bounce. */
    pas_sampler_isr_tick(3U, 40U); /* 10 -> 11 forward */
    {
        pas_step_event_t ev = require_event("B9: next real forward is published");
        CHECK(ev.step > 0, "B10: next real edge remains forward");
        CHECK(ev.gap == 20U, "B11: real period spans real-edge to real-edge, ignoring bounce");
        pas_direction_on_step(ev.step);
    }

    /* C. A physically plausible reverse remains a hard safety event. */
    pas_sampler_isr_tick(2U, 60U); /* 11 -> 10 = reverse, gap=20 */
    {
        pas_step_event_t ev = require_event("C1: plausible reverse is published");
        CHECK(ev.step < 0, "C2: plausible reverse still decodes reverse");
        pas_direction_on_step(ev.step);
        CHECK(pas_direction_direction_inhibit_active(), "C3: plausible reverse still inhibits immediately");
        CHECK(pas_sampler_get_stats()->reverse_count == 1U, "C4: genuine reverse count remains intact");
    }

    /* D. Recovery safety is unchanged: two clean forward steps are still required. */
    pas_sampler_isr_tick(3U, 80U); /* 10 -> 11 forward #1 */
    drain_to_direction();
    CHECK(pas_direction_direction_inhibit_active(), "D1: first recovery forward step is still inhibited");
    pas_sampler_isr_tick(1U, 100U); /* 11 -> 01 forward #2 */
    drain_to_direction();
    CHECK(!pas_direction_direction_inhibit_active(), "D2: configured second forward step restores direction-safe");

    /* E. Tiny illegal two-bit jump is also physically impossible and must be rejected. */
    reset_all(0U, 1000U);
    pas_sampler_isr_tick(2U, 1020U); /* clean forward */
    drain_to_direction();
    pas_sampler_isr_tick(1U, 1021U); /* 10 -> 01, diagonal illegal, gap=1 */
    CHECK(pas_sampler_pop(&(pas_step_event_t){0}) == 0, "E1: 1-tick illegal jump is filtered");
    CHECK(pas_sampler_state() == 2U, "E2: filtered illegal jump cannot desynchronise qstate");
    CHECK(pas_sampler_get_stats()->invalid_count == 0U, "E3: filtered illegal jump is not a physical invalid event");
    CHECK(pas_sampler_get_stats()->glitch_count == 1U, "E4: filtered illegal jump increments glitch counter");
    CHECK(!pas_direction_direction_inhibit_active(), "E5: impossible 1-tick illegal jump cannot cut assist");

    /* F. Same illegal transition after enough elapsed time remains fail-safe. */
    pas_sampler_isr_tick(1U, 1024U); /* still different; gap now reaches plausibility threshold */
    {
        pas_step_event_t ev = require_event("F1: persistent illegal state becomes a real fault event at threshold");
        CHECK(ev.step == 0, "F2: persistent diagonal remains INVALID, not invented direction");
        pas_direction_on_step(ev.step);
        CHECK(pas_direction_direction_inhibit_active(), "F3: persistent invalid state still fails safe");
        CHECK(pas_sampler_get_stats()->invalid_count == 1U, "F4: persistent invalid is counted once accepted");
    }

    /* G. uint32 wrap must not bypass the plausibility window. */
    reset_all(0U, 0xFFFFFFE0U);
    pas_sampler_isr_tick(2U, 0xFFFFFFFEU); /* real forward */
    drain_to_direction();
    pas_sampler_isr_tick(0U, 0xFFFFFFFFU); /* reverse bounce gap=1 */
    CHECK(pas_sampler_pop(&(pas_step_event_t){0}) == 0, "G1: pre-wrap 1-tick bounce filtered");
    pas_sampler_isr_tick(0U, 1U); /* still present: unsigned gap from FFFFFFFE is 3 */
    CHECK(pas_sampler_pop(&(pas_step_event_t){0}) == 0, "G2: wrapped gap=3 is still filtered");
    pas_sampler_isr_tick(0U, 2U); /* gap=4, now persistent input is accepted */
    {
        pas_step_event_t ev = require_event("G3: wrapped gap=4 persistent reverse is accepted");
        CHECK(ev.step < 0, "G4: accepted wrapped event remains reverse");
        CHECK(ev.gap == 4U, "G5: wrap-safe elapsed time is exact");
    }

    if (host_test_failures == 0) {
        printf("All PAS glitch-filter checks passed.\n");
        return 0;
    }
    printf("%d PAS glitch-filter check(s) FAILED.\n", host_test_failures);
    return 1;
}
