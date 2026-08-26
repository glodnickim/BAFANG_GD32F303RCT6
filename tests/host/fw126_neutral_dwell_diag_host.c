/*
 * FW-126.2 host proof for the neutral-dwell CH3 recorder.
 *
 * The 2026-08-25 17:44 ride is the specification here: the sweep ran, produced slopes of -4
 * and +1.1, and could not decide. Every check below encodes one of the reasons that log gave
 * for the failure, so a regression cannot quietly bring it back:
 *
 *   - the first interrupt after MOE ON is discarded (it was the only DIR=up sample and it
 *     alone produced the -4);
 *   - an interrupt whose ADC1 group had not finished never enters the median (ADC1_LATE=1 in
 *     that log skipped an interrupt and split the first two points by two PWM periods);
 *   - a point is several conversions, reported as a MEDIAN, so one disturbed reading can no
 *     longer move the answer;
 *   - a point below SAMPLES_MIN is never marked complete.
 */
#include "check.h"

#include <stdio.h>
#include <string.h>

#include "adc_trigger_diag.h"

#if !CAN_DIAGNOSTICS_ENABLE
#error "FW-126.2 host suite requires CAN_DIAGNOSTICS_ENABLE=1"
#endif

#define SAFE_BOTH (ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR | ADC_TRIGGER_DIAG_SAFE_FOC_HELD)
#define EOIC_ALL  (ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC1_EOIC | \
                   ADC_TRIGGER_DIAG_F_ADC2_EOIC)

static adc_trigger_diag_env_t env_at(uint32_t tick, uint16_t cnt, int dir_down)
{
	adc_trigger_diag_env_t e;
	e.tick = tick;
	e.cnt = cnt;
	e.iq_setpoint_abs = 14U;
	e.lifecycle = 2U;
	e.flags = ADC_TRIGGER_DIAG_ENV_POEN | ADC_TRIGGER_DIAG_ENV_PWM_ON |
		ADC_TRIGGER_DIAG_ENV_DWELL | ADC_TRIGGER_DIAG_ENV_RUNNING;
	if (dir_down) e.flags |= ADC_TRIGGER_DIAG_ENV_DIR_DOWN;
	return e;
}

/* One interrupt. Returns 1 when the module asked the caller to program a new CH3. */
static int isr(uint32_t tick, uint16_t cnt, uint8_t adc_flags, uint8_t safety,
               uint16_t ccr3_readback, int dir_down, uint16_t *next)
{
	adc_trigger_diag_sample_t s;
	memset(&s, 0, sizeof(s));
	s.env = env_at(tick, cnt, dir_down);
	s.adc_flags = adc_flags;
	s.safety_flags = safety;
	s.ccr3_readback = ccr3_readback;
	s.raw_a = 100U; s.raw_b = 101U; s.raw_c = 102U;
	return adc_trigger_diag_neutral_dwell_isr(&s, next) ? 1 : 0;
}

/* Feed one full point: `n` accepted conversions at `cnt_base` (+jitter on one of them). */
static uint16_t feed_point(uint16_t cnt_base, uint16_t ccr3, int16_t jitter_on)
{
	uint16_t next = 0U;
	uint8_t k;
	for (k = 0U; k < ADC_TRIGGER_DIAG_SAMPLES_TARGET; k++) {
		uint16_t cnt = (uint16_t)(cnt_base + ((k == (uint8_t)jitter_on) ? 300 : 0));
		(void)isr(200U + k, cnt, EOIC_ALL, SAFE_BOTH, ccr3, 1, &next);
	}
	return next;
}

static void test_first_isr_after_moe_is_discarded(void)
{
	adc_trigger_diag_env_t e = env_at(100U, 0U, 1);
	uint16_t next = 0U;

	CHECK(adc_trigger_diag_arm_neutral_dwell(&e, SAFE_BOTH, &next), "A1: arms on the gate");
	CHECK(next == 3740U, "A2: first CH3 is the production value");
	CHECK(!adc_trigger_diag_first_isr_discarded(), "A3: nothing discarded before any interrupt");

	/* The 17:44 shape exactly: a perfectly valid-looking sample, entered while counting UP. */
	(void)isr(101U, 2956U, EOIC_ALL, SAFE_BOTH, 3740U, 0, &next);
	CHECK(adc_trigger_diag_first_isr_discarded(), "A4: the MOE-ON interrupt is discarded");
	CHECK(adc_trigger_diag_get_npoint(0U)->accepted == 0U,
		"A5: ...and contributes no sample, however healthy it looked");
	CHECK(adc_trigger_diag_get_npoint(0U)->rejected_late == 0U,
		"A6: ...and is not miscounted as a late conversion either");
	CHECK(adc_trigger_diag_arm_isr_count() == 1U, "A7: it is still counted as an interrupt");
}

static void test_median_and_slope_evidence(void)
{
	adc_trigger_diag_env_t e = env_at(100U, 0U, 1);
	uint16_t next = 0U;
	const adc_trigger_diag_npoint_t *p0, *p1, *p2;

	CHECK(adc_trigger_diag_arm_neutral_dwell(&e, SAFE_BOTH, &next), "B1: arms");
	(void)isr(101U, 9999U, EOIC_ALL, SAFE_BOTH, 3740U, 0, &next);   /* discarded */

	/* Three points on the DOWN-count hypothesis: CNT = CCR3 - (CONV+L), CONV+L = 500.
	 * Each point carries ONE badly disturbed reading (+300) to prove the median ignores it. */
	next = feed_point(3240U, 3740U, 2);
	CHECK(next == 3700U, "B2: point 0 completes at SAMPLES_TARGET and programs the next CH3");
	next = feed_point(3200U, 3700U, 0);
	CHECK(next == 3660U, "B3: point 1 completes and programs the last CH3");
	next = feed_point(3160U, 3660U, 6);
	CHECK(next == ADC_TRIGGER_DIAG_CCR3_MAX,
		"B4: the last point restores the production CH3 before FOC is released");
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_DONE && !adc_trigger_diag_owns_ch3(),
		"B5: the sweep is DONE and no longer owns CH3");
	CHECK(!adc_trigger_diag_needs_dwell(), "B6: the extended dwell is no longer needed");

	p0 = adc_trigger_diag_get_npoint(0U);
	p1 = adc_trigger_diag_get_npoint(1U);
	p2 = adc_trigger_diag_get_npoint(2U);

	CHECK(p0->accepted == ADC_TRIGGER_DIAG_SAMPLES_TARGET &&
		p1->accepted == ADC_TRIGGER_DIAG_SAMPLES_TARGET &&
		p2->accepted == ADC_TRIGGER_DIAG_SAMPLES_TARGET, "B7: every point took its full sample set");
	CHECK(p0->complete && p1->complete && p2->complete, "B8: every point is complete");

	/* THE POINT OF THE CARD: one +300 outlier per point, and the median is still the true
	 * value. With FW-126.0's single sample this is exactly where the answer went wrong. */
	CHECK(p0->cnt_median == 3240U, "B9: point 0 median ignores the outlier");
	CHECK(p1->cnt_median == 3200U, "B10: point 1 median ignores the outlier");
	CHECK(p2->cnt_median == 3160U, "B11: point 2 median ignores the outlier");

	/* The slope the decoder will compute is now consistent on BOTH segments. */
	CHECK((int)p1->cnt_median - (int)p0->cnt_median == -40 &&
		(int)p2->cnt_median - (int)p1->cnt_median == -40,
		"B12: both segments give the same -40 for a -40 CH3 step (slope +1, DOWN-count)");

	/* min/max/spread expose the outlier instead of hiding it. */
	CHECK(p0->cnt_min == 3240U && p0->cnt_max == 3540U, "B13: min/max carry the real range");
	CHECK(p0->ccr3_readback == 3740U && p1->ccr3_readback == 3700U && p2->ccr3_readback == 3660U,
		"B14: the CH3 readback proves which compare each point actually used");
	CHECK(p0->isr_seq_first == 2U, "B15: the first accepted sample is interrupt 2, not 1");
	CHECK(p0->dir_up_count == 0U && p0->eoic_all_count == ADC_TRIGGER_DIAG_SAMPLES_TARGET,
		"B16: DIR and EOIC are counted per accepted sample");
}

static void test_late_adc1_never_enters_the_median(void)
{
	adc_trigger_diag_env_t e = env_at(100U, 0U, 1);
	uint16_t next = 0U;
	const adc_trigger_diag_npoint_t *p0;
	uint8_t k;

	CHECK(adc_trigger_diag_arm_neutral_dwell(&e, SAFE_BOTH, &next), "C1: arms");
	(void)isr(101U, 9999U, EOIC_ALL, SAFE_BOTH, 3740U, 0, &next);   /* discarded */

	/* An interrupt without ADC1's group: rejected, counted, and the point simply waits. */
	(void)isr(102U, 1U, ADC_TRIGGER_DIAG_F_ADC0_EOIC, SAFE_BOTH, 3740U, 1, &next);
	p0 = adc_trigger_diag_get_npoint(0U);
	CHECK(p0->accepted == 0U, "C2: a late ADC1 interrupt contributes no sample");
	CHECK(p0->rejected_late == 1U, "C3: ...it is counted against the point");
	CHECK(adc_trigger_diag_adc1_late_total() == 1U, "C4: ...and in the sweep total");

	for (k = 0U; k < ADC_TRIGGER_DIAG_SAMPLES_TARGET; k++)
		(void)isr(200U + k, 3240U, EOIC_ALL, SAFE_BOTH, 3740U, 1, &next);
	CHECK(p0->accepted == ADC_TRIGGER_DIAG_SAMPLES_TARGET, "C5: the point still fills up");
	CHECK(p0->cnt_median == 3240U, "C6: the rejected reading never reached the median");
	CHECK(p0->rejected_late == 1U, "C7: the rejection count survives into the report");
}

static void test_short_point_is_not_evidence(void)
{
	adc_trigger_diag_env_t e = env_at(100U, 0U, 1);
	uint16_t next = 0U;
	uint8_t k;

	CHECK(adc_trigger_diag_arm_neutral_dwell(&e, SAFE_BOTH, &next), "D1: arms");
	(void)isr(101U, 9999U, EOIC_ALL, SAFE_BOTH, 3740U, 0, &next);

	/* Fewer than SAMPLES_MIN, then the dwell is lost - the point must not read as complete. */
	for (k = 0U; k < (ADC_TRIGGER_DIAG_SAMPLES_MIN - 1U); k++)
		(void)isr(200U + k, 3240U, EOIC_ALL, SAFE_BOTH, 3740U, 1, &next);
	CHECK(adc_trigger_diag_get_npoint(0U)->accepted == ADC_TRIGGER_DIAG_SAMPLES_MIN - 1U,
		"D2: the samples are recorded");
	CHECK(!adc_trigger_diag_get_npoint(0U)->complete,
		"D3: but the point is NOT complete below SAMPLES_MIN");
}

static void test_safety_abort_restores_ch3(void)
{
	adc_trigger_diag_env_t e = env_at(100U, 0U, 1);
	uint16_t next = 0U;
	int wrote;

	CHECK(adc_trigger_diag_arm_neutral_dwell(&e, SAFE_BOTH, &next), "E1: arms");
	(void)isr(101U, 9999U, EOIC_ALL, SAFE_BOTH, 3740U, 0, &next);

	/* The dwell is gone: ownership must end in this very interrupt, before FOC can see it. */
	wrote = isr(102U, 3240U, EOIC_ALL, ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR, 3740U, 1, &next);
	CHECK(wrote && next == ADC_TRIGGER_DIAG_CCR3_MAX,
		"E2: a broken interlock restores the production CH3 immediately");
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_ABORTED_MOE && !adc_trigger_diag_owns_ch3(),
		"E3: the sweep aborts and releases CH3");
	CHECK(!adc_trigger_diag_needs_dwell(), "E4: an aborted sweep no longer holds the dwell");
}

static void test_wire_contract_schema8(void)
{
	adc_trigger_diag_env_t e = env_at(100U, 0U, 1);
	uint16_t next = 0U;
	uint32_t efid;
	uint8_t d[8];

	CHECK(adc_trigger_diag_arm_neutral_dwell(&e, SAFE_BOTH, &next), "F1: arms");
	(void)isr(101U, 9999U, EOIC_ALL, SAFE_BOTH, 3740U, 0, &next);
	(void)feed_point(3240U, 3740U, 2);
	(void)feed_point(3200U, 3700U, -1);
	(void)feed_point(3160U, 3660U, -1);

	CHECK(adc_trigger_diag_aggregate_frame(0U, &efid, d) && efid == 0x00010240U &&
		d[0] == ADC_TRIGGER_DIAG_DONE && d[1] == ADC_TRIGGER_DIAG_NEUTRAL_POINTS,
		"F2: status frame reports DONE and three completed points");

	/* Point 0 frame A. */
	CHECK(adc_trigger_diag_aggregate_frame(1U, &efid, d) && efid == 0x00010241U, "F3: point 0 A id");
	CHECK(d[0] == 0xC0U, "F4: frame A tag");
	CHECK(d[1] == 240U && d[2] == 240U, "F5: CH3 requested and readback both 3740");
	CHECK(d[3] == ADC_TRIGGER_DIAG_SAMPLES_TARGET, "F6: accepted count");
	CHECK((((uint16_t)d[4] << 8) | d[5]) == 3240U, "F7: MEDIAN CNT on the wire");
	CHECK(d[6] == 0U, "F8: no rejected samples here");
	CHECK((d[7] & 0x01U) && !(d[7] & 0x02U), "F9: every accepted sample entered counting DOWN");
	CHECK((d[7] & 0x04U) && (d[7] & 0x08U), "F10: all-EOIC and complete bits");

	/* Point 0 frame B carries the range. */
	CHECK(adc_trigger_diag_aggregate_frame(2U, &efid, d) && efid == 0x00010242U, "F11: point 0 B id");
	CHECK(d[0] == 0xB0U, "F12: frame B tag");
	CHECK((((uint16_t)d[1] << 8) | d[2]) == 3240U, "F13: CNT min");
	CHECK((((uint16_t)d[3] << 8) | d[4]) == 3540U, "F14: CNT max (the outlier is visible)");
	CHECK(d[5] == 255U, "F15: spread clamps at 255 rather than wrapping");

	/* 0x10244 is what the decoder uses to tell schema 8 from every other layout. */
	CHECK(adc_trigger_diag_aggregate_frame(4U, &efid, d) && efid == 0x00010244U &&
		d[0] == 0xB1U, "F16: 0x10244 Data0 = 0xB1 identifies schema 8");
	CHECK(adc_trigger_diag_aggregate_frame(6U, &efid, d) && efid == 0x00010246U &&
		d[0] == 0xB2U, "F17: the block still ends at 0x10246");
	CHECK(!adc_trigger_diag_aggregate_frame(ADC_TRIGGER_DIAG_AGG_FRAMES, &efid, d),
		"F18: nothing past the end of the block");
}

int main(void)
{
	/* Every test starts from a clean module: adc_trigger_diag_init() clears armed_once, which
	 * is what makes the one-sweep-per-power-cycle rule testable more than once. */
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
	test_first_isr_after_moe_is_discarded();
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
	test_median_and_slope_evidence();
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
	test_late_adc1_never_enters_the_median();
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
	test_short_point_is_not_evidence();
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
	test_safety_abort_restores_ch3();
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
	test_wire_contract_schema8();

	if (host_test_failures == 0) {
		printf("\nAll FW-126.2 neutral-dwell CH3 checks passed "
			"(%u samples/point, min %u, discard-first-ISR on).\n",
			(unsigned)ADC_TRIGGER_DIAG_SAMPLES_TARGET,
			(unsigned)ADC_TRIGGER_DIAG_SAMPLES_MIN);
		return 0;
	}
	printf("\n%d FW-126.2 check(s) FAILED.\n", host_test_failures);
	return 1;
}
