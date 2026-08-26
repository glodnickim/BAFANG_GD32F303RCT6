/*
 * FW-121.0: the trigger-timing sweep, against the real src/adc_trigger_diag.c.
 *
 * The module is deliberately hardware-free - main.c reads TIMER0/ADC and writes CH3, the module
 * only decides - so the whole safety interlock and the whole sweep sequence are executable here.
 * This harness plays the ISR at 16 kHz against a 4 kHz control tick, exactly the ratio the real
 * hardware has, and drives MOE the way a bridge start would.
 *
 * WHAT THIS PROVES (S1-S8):
 *   S1: it does not arm while the bridge is live, and does not arm before standstill is
 *       confirmed - neither condition alone is enough.
 *   S2: CCR3 never leaves [ADC_TRIGGER_DIAG_CCR3_MIN, ADC_TRIGGER_DIAG_CCR3_MAX], the sweep
 *       visits exactly the six specified values in descending order, and it ends on the floor.
 *   S3: each point is held for exactly ADC_TRIGGER_DIAG_POINT_TICKS control ticks.
 *   S4: on finish, CCR3 is handed back to the production value and ownership is released.
 *   S5: THE INTERLOCK - MOE going high mid-sweep aborts on the VERY NEXT ISR, restores the
 *       production CCR3 in that same call, and releases ownership. Tested at every point.
 *   S6: it arms once per power cycle only - after DONE or after an abort it never re-arms, so it
 *       can never start while the rider is out on the road.
 *   S7: the recorded statistics are the ones the hardware log will be read from: ISR count per
 *       point separates 4-per-tick from 8-per-tick, CNT min/max carry the slope and the jitter,
 *       and the two ADC-late counters carry the phase-C skew question.
 *   S8: an aborted point is marked incomplete, so a partial sweep cannot be misread as data.
 *
 * WHAT THIS DOES NOT PROVE: anything about the hardware. Which CC3 edge actually fires, the real
 * interrupt latency and the analogue settling time are exactly what the measurement is FOR.
 */

#include "../common/check.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "adc_trigger_diag.h"

#if !CAN_DIAGNOSTICS_ENABLE
#error "FW-121.0 host suite must be built with -DCAN_DIAGNOSTICS_ENABLE=1"
#endif

/* The real ratio: PWM 16 kHz, control tick 4 kHz. */
#define ISR_PER_TICK 4U

typedef struct {
	uint16_t ccr3;          /* what main.c would have programmed into TIMER0 CH3 */
	uint32_t tick;
	uint32_t isr_in_tick;
	uint32_t isr_per_tick;   /* how many injected ISRs the model puts in one control tick */
	uint32_t writes;        /* how many times the module asked for a CCR3 write  */
	uint16_t ccr3_history[64];
	uint32_t ccr3_history_n;
} bench_t;

static void bench_init(bench_t *b)
{
	memset(b, 0, sizeof(*b));
	b->ccr3 = ADC_TRIGGER_DIAG_CCR3_MAX;   /* production TRIGGER_DEFAULT */
	b->isr_per_tick = ISR_PER_TICK;
	adc_trigger_diag_init(ADC_TRIGGER_DIAG_CCR3_MAX);
}

static void bench_note_ccr3(bench_t *b, uint16_t v)
{
	b->ccr3 = v;
	b->writes++;
	if (b->ccr3_history_n < (uint32_t)(sizeof(b->ccr3_history) / sizeof(b->ccr3_history[0])))
		b->ccr3_history[b->ccr3_history_n++] = v;
}

/* One injected-ADC interrupt, wired exactly as src/main.c wires it. */
static void bench_isr(bench_t *b, bool moe_on, uint16_t cnt, bool dir_down, uint8_t flags)
{
	if (adc_trigger_diag_owns_ch3()) {
		adc_trigger_diag_sample_t s;
		uint16_t next;
		s.env.tick = b->tick;
		s.env.cnt = cnt;
		s.env.iq_setpoint_abs = 0U;
		s.env.lifecycle = 0U;
		s.env.flags = (uint8_t)((dir_down ? ADC_TRIGGER_DIAG_ENV_DIR_DOWN : 0U)
		                     | (moe_on ? ADC_TRIGGER_DIAG_ENV_POEN : 0U));
		s.adc_flags = flags;
		if (adc_trigger_diag_isr(&s, &next)) bench_note_ccr3(b, next);
	}
	if (++b->isr_in_tick >= b->isr_per_tick) { b->isr_in_tick = 0; b->tick++; }
}

/* One control iteration, where main.c offers the module its chance to arm. */
static void bench_arm(bench_t *b, bool standstill, bool bridge_live)
{
	uint16_t req;
	adc_trigger_diag_env_t env;
	env.tick = b->tick; env.cnt = 0U; env.iq_setpoint_abs = 0U;
	env.lifecycle = 0U; env.flags = standstill ? ADC_TRIGGER_DIAG_ENV_STANDSTILL : 0U;
	if (adc_trigger_diag_arm(standstill, bridge_live, &env, &req)) bench_note_ccr3(b, req);
}

/*
 * A hardware model good enough to exercise the statistics: under hypothesis A the counter
 * position at ISR entry is 2*_T - CCR3 - CONV - L. The module never looks at the value, so this
 * only has to be self-consistent, not true - which hypothesis is real is the measurement.
 */
#define MODEL_T    3750
#define MODEL_CONV 408
#define MODEL_L    40
static uint16_t model_cnt_for(uint16_t ccr3)
{
	int32_t cnt = 2 * MODEL_T - (int32_t)ccr3 - MODEL_CONV - MODEL_L;
	if (cnt < 0) cnt = 0;
	return (uint16_t)cnt;
}

/* Run a full undisturbed sweep; returns the number of ISRs it took. */
static uint32_t run_full_sweep(bench_t *b, uint8_t flags)
{
	uint32_t isrs = 0;
	bench_arm(b, true, false);
	while (adc_trigger_diag_owns_ch3() && isrs < 200000U) {
		bench_isr(b, false, model_cnt_for(b->ccr3), true, flags);
		isrs++;
	}
	return isrs;
}

/* --------------------------------------------------------------------------------- S1 / S6 */

static void s1_arming_gate(void)
{
	bench_t b;

	bench_init(&b);
	bench_arm(&b, false, false);
	CHECK(!adc_trigger_diag_owns_ch3(), "S1a: no standstill -> does not arm");
	CHECK(b.ccr3 == ADC_TRIGGER_DIAG_CCR3_MAX, "S1b: ... and CCR3 is untouched");

	bench_init(&b);
	bench_arm(&b, true, true);
	CHECK(!adc_trigger_diag_owns_ch3(), "S1c: standstill but the bridge is live -> does not arm");
	CHECK(b.ccr3 == ADC_TRIGGER_DIAG_CCR3_MAX, "S1d: ... and CCR3 is untouched");

	bench_init(&b);
	for (int i = 0; i < 50; i++) bench_arm(&b, true, true);
	CHECK(!adc_trigger_diag_owns_ch3(), "S1e: a live bridge held for many iterations still never arms");

	bench_init(&b);
	bench_arm(&b, true, false);
	CHECK(adc_trigger_diag_owns_ch3(), "S1f: standstill AND a dark bridge -> arms");
	CHECK(b.ccr3 == ADC_TRIGGER_DIAG_CCR3_MAX,
		"S1g: the first point IS the production value - the baseline changes nothing");
}

static void s6_once_per_power_cycle(void)
{
	bench_t b;

	bench_init(&b);
	(void)run_full_sweep(&b, ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC);
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_DONE, "S6a: the undisturbed sweep completes");
	for (int i = 0; i < 100; i++) bench_arm(&b, true, false);
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_DONE && !adc_trigger_diag_owns_ch3(),
		"S6b: after DONE it never re-arms, however long the bike then stands still");

	/* and the same after an abort */
	bench_init(&b);
	bench_arm(&b, true, false);
	bench_isr(&b, true, 0, true, 0);
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_ABORTED_MOE, "S6c: MOE aborts");
	for (int i = 0; i < 100; i++) bench_arm(&b, true, false);
	CHECK(!adc_trigger_diag_owns_ch3(), "S6d: an aborted sweep never re-arms either");
}

/* --------------------------------------------------------------------------- S2 / S3 / S4 */

static void s2_s3_s4_sweep_shape(void)
{
	bench_t b;
	uint32_t isrs;
	uint8_t i;
	int range_ok = 1, order_ok = 1, span_ok = 1, count_ok = 1;

	bench_init(&b);
	isrs = run_full_sweep(&b, ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC);

	for (i = 0; i < ADC_TRIGGER_DIAG_POINTS; i++) {
		const adc_trigger_diag_point_t *p = adc_trigger_diag_get_point(i);
		uint16_t expect = (uint16_t)(ADC_TRIGGER_DIAG_CCR3_MAX - i * ADC_TRIGGER_DIAG_CCR3_STEP);
		if (p->ccr3 < ADC_TRIGGER_DIAG_CCR3_MIN || p->ccr3 > ADC_TRIGGER_DIAG_CCR3_MAX) range_ok = 0;
		if (p->ccr3 != expect) order_ok = 0;
		if (p->tick_span != ADC_TRIGGER_DIAG_POINT_TICKS) span_ok = 0;
		if (!p->complete) span_ok = 0;
		/* ~4 ISRs per tick, allowing the boundary ISR either side */
		if (p->isr_count < ADC_TRIGGER_DIAG_POINT_TICKS * ISR_PER_TICK ||
		    p->isr_count > ADC_TRIGGER_DIAG_POINT_TICKS * ISR_PER_TICK + ISR_PER_TICK) count_ok = 0;
	}

	CHECK(range_ok, "S2a: every swept CCR3 stays inside [MIN, MAX]");
	CHECK(order_ok, "S2b: the sweep is 3740/3700/3660/3620/3580/3540, in that order");
	CHECK(adc_trigger_diag_get_point(ADC_TRIGGER_DIAG_POINTS - 1)->ccr3 == ADC_TRIGGER_DIAG_CCR3_MIN,
		"S2c: it ends exactly on the floor, 3540 - the point where the two hypotheses diverge");
	CHECK(span_ok, "S3: every point ran exactly ADC_TRIGGER_DIAG_POINT_TICKS control ticks");
	CHECK(count_ok, "S7a: the per-point ISR count matches the modelled 4-per-tick rate");
	CHECK(b.ccr3 == ADC_TRIGGER_DIAG_CCR3_MAX,
		"S4a: on finish CCR3 is back at the production value");
	CHECK(!adc_trigger_diag_owns_ch3(),
		"S4b: ... and ownership of CH3 is released, so dyn_adc_trigger_update() runs again");
	CHECK(b.writes == ADC_TRIGGER_DIAG_POINTS + 1,
		"S4c: exactly one CCR3 write per point plus the restore - nothing writes CH3 per ISR");
	CHECK(isrs > 0 && isrs < 200000U, "S4d: the sweep terminates");
	CHECK(b.ccr3_history_n >= ADC_TRIGGER_DIAG_POINTS,
		"S4e: the whole CCR3 history was captured");
}

/* --------------------------------------------------------------------------------------- S5 */

static void s5_moe_interlock(void)
{
	uint8_t target;
	int restored_ok = 1, released_ok = 1, immediate_ok = 1, incomplete_ok = 1;

	/* abort at each point in turn: run into point `target`, then raise MOE */
	for (target = 0; target < ADC_TRIGGER_DIAG_POINTS; target++) {
		bench_t b;
		uint32_t guard = 0;
		bench_init(&b);
		bench_arm(&b, true, false);

		while (adc_trigger_diag_index() < target && adc_trigger_diag_owns_ch3() && guard++ < 200000U)
			bench_isr(&b, false, model_cnt_for(b.ccr3), true, ADC_TRIGGER_DIAG_F_ADC0_EOIC);

		/* a few clean ISRs inside the target point, then the bridge comes up */
		for (int k = 0; k < 5; k++) bench_isr(&b, false, model_cnt_for(b.ccr3), true, ADC_TRIGGER_DIAG_F_ADC0_EOIC);

		uint32_t writes_before = b.writes;
		bench_isr(&b, true, 1234, true, 0);

		if (b.ccr3 != ADC_TRIGGER_DIAG_CCR3_MAX) restored_ok = 0;
		if (adc_trigger_diag_owns_ch3()) released_ok = 0;
		if (adc_trigger_diag_state() != ADC_TRIGGER_DIAG_ABORTED_MOE) released_ok = 0;
		if (b.writes != writes_before + 1) immediate_ok = 0;   /* restored in THAT ISR, not later */
		if (adc_trigger_diag_get_point(target)->complete) incomplete_ok = 0;
	}

	CHECK(restored_ok, "S5a: MOE mid-sweep restores the production CCR3, at every point");
	CHECK(released_ok, "S5b: ... releases CH3 and records ABORTED_MOE");
	CHECK(immediate_ok, "S5c: ... in the very ISR that saw MOE - exposure is one PWM period");
	CHECK(incomplete_ok, "S8: the interrupted point is left marked incomplete");
}

/* --------------------------------------------------------------------------------------- S7 */

static void s7_statistics(void)
{
	bench_t b;
	uint8_t i;
	int slope_ok = 1, dir_ok = 1;
	uint16_t prev_cnt = 0;

	/* every sample says ADC2 finished but ADC0 did not - the phase-C skew question */
	bench_init(&b);
	(void)run_full_sweep(&b, ADC_TRIGGER_DIAG_F_ADC2_EOIC);

	for (i = 0; i < ADC_TRIGGER_DIAG_POINTS; i++) {
		const adc_trigger_diag_point_t *p = adc_trigger_diag_get_point(i);
		if (p->cnt_min != model_cnt_for(p->ccr3)) slope_ok = 0;
		if (p->cnt_max != p->cnt_min) slope_ok = 0;           /* the model has no jitter */
		if (i > 0 && p->cnt_min <= prev_cnt) slope_ok = 0;    /* hypothesis A: CNT rises as CCR3 falls */
		prev_cnt = p->cnt_min;
		if (p->dir_up_count != 0) dir_ok = 0;                 /* every modelled sample counts down */
		if (p->adc0_late_count != p->isr_count) dir_ok = 0;   /* ADC0 never reported EOIC */
		if (p->adc2_late_count != 0) dir_ok = 0;              /* ADC2 always did */
	}

	CHECK(slope_ok,
		"S7b: CNT min/max per point reproduce the modelled trigger geometry - this is the slope the log is read from");
	CHECK(dir_ok,
		"S7c: direction and per-ADC late counters are recorded separately - the phase-C skew question is answerable");

	/* the 8-per-tick case the sweep exists to detect */
	bench_init(&b);
	b.isr_per_tick = 2U * ISR_PER_TICK;   /* two injected conversions per PWM period */
	bench_arm(&b, true, false);
	{
		uint32_t guard = 0;
		while (adc_trigger_diag_owns_ch3() && guard++ < 400000U)
			bench_isr(&b, false, model_cnt_for(b.ccr3), true, 0);
	}
	CHECK(adc_trigger_diag_get_point(0)->isr_count > ADC_TRIGGER_DIAG_POINT_TICKS * 6U,
		"S7d: a doubled ISR rate shows up as a doubled per-point count - the second-edge detector works");
}

/* ------------------------------------------------------------------------------ S9: transport */

/*
 * The exact index -> EFID -> payload contract the ride log is read by. This is the check that
 * would have caught the collision that shipped: the first version of this card claimed
 * 0x1022F..0x10235, which FW-112 A/B and FW-117 already own, and nothing failed until a decode
 * session on a real log.
 */
static void s9_transport_contract(void)
{
	static const uint32_t expect_efid[ADC_TRIGGER_DIAG_POINTS + 1U] = {
		0x00010240U, 0x00010241U, 0x00010242U, 0x00010243U,
		0x00010244U, 0x00010245U, 0x00010246U
	};
	/* CCR3 3740/3700/3660/3620/3580/3540 -> Data1 = CCR3-3500 */
	static const uint8_t expect_data1[ADC_TRIGGER_DIAG_POINTS] = { 0xF0, 0xC8, 0xA0, 0x78, 0x50, 0x28 };

	bench_t b;
	uint32_t efid;
	uint8_t data[8];
	uint8_t sub;
	int efid_ok = 1, data0_ok = 1, data1_ok = 1, idx_ok = 1;

	bench_init(&b);
	(void)run_full_sweep(&b, ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC);
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_DONE, "S9a: sweep completed before readout");

	for (sub = 0; sub < ADC_TRIGGER_DIAG_AGG_FRAMES; sub++) {
		if (!adc_trigger_diag_aggregate_frame(sub, &efid, data)) { efid_ok = 0; break; }
		if (efid != expect_efid[sub]) efid_ok = 0;
		if (sub == 0U) {
			/* FW-121.0B schema 6 status: state, index, abort chronology, snapshot flags. */
			if (data[0] != ADC_TRIGGER_DIAG_DONE) data0_ok = 0;
			if (data[1] != ADC_TRIGGER_DIAG_POINTS) data0_ok = 0;
			if (((uint16_t)data[2] << 8 | data[3]) != 0U) data0_ok = 0;      /* no abort */
			if (data[4] != ADC_TRIGGER_DIAG_ABORT_NONE) data0_ok = 0;
			if ((data[5] & ADC_TRIGGER_DIAG_SNAP_ARM) == 0U) data0_ok = 0;
			if ((data[5] & (ADC_TRIGGER_DIAG_SNAP_ABORT | ADC_TRIGGER_DIAG_SNAP_POEN)) != 0U) data0_ok = 0;
			if (((uint16_t)data[6] << 8 | data[7]) != 0U) data0_ok = 0;      /* no POEN enable seen */
		} else {
			uint8_t i = (uint8_t)(sub - 1U);
			if (data[0] != (uint8_t)(0x80U | i)) data0_ok = 0;
			if (data[1] != expect_data1[i]) data1_ok = 0;
		}
	}

	CHECK(efid_ok,
		"S9b: idx 14..20 map to EFID 0x10240..0x10246, in order");
	CHECK(idx_ok && ADC_TRIGGER_DIAG_AGG_FIRST_INDEX == 14U,
		"S9c: the module declares its first aggregate index as 14, so idx 14+sub is the absolute position");
	CHECK(data0_ok, "S9d: status payload and every point's Data0 = 0x80 | index");
	CHECK(data1_ok, "S9e: every point's Data1 = CCR3-3500 (F0 C8 A0 78 50 28)");

	CHECK(!adc_trigger_diag_aggregate_frame(ADC_TRIGGER_DIAG_AGG_FRAMES, &efid, data),
		"S9f: one past the last sub-frame returns false - the block ends where main.c expects");

	/* the ids must not land on any neighbour's block */
	CHECK(expect_efid[0] > 0x00010238U,
		"S9g: the whole block sits above FW-117's last id (0x10238) - no overlap with FW-112/FW-117");

	/* an incomplete point must be visibly incomplete, and still name its CCR3 */
	bench_init(&b);
	bench_arm(&b, true, false);
	bench_isr(&b, true, 0, true, 0);                 /* abort immediately */
	(void)adc_trigger_diag_aggregate_frame(3U, &efid, data);
	CHECK(data[0] == 2U && data[1] == expect_data1[2],
		"S9h: an unreached point reports index without 0x80 and still names its CCR3");
}


/* --------------------------------------------------------------- FW-121.0B: T1..T8 chronology */

static adc_trigger_diag_env_t mk_env(uint32_t tick, uint16_t cnt, uint8_t flags, uint8_t lifecycle,
                                     uint16_t iq)
{
	adc_trigger_diag_env_t e;
	e.tick = tick; e.cnt = cnt; e.flags = flags; e.lifecycle = lifecycle; e.iq_setpoint_abs = iq;
	return e;
}

static void bench_arm_env(bench_t *b, bool standstill, bool bridge_live,
                          const adc_trigger_diag_env_t *env)
{
	uint16_t req;
	if (adc_trigger_diag_arm(standstill, bridge_live, env, &req)) bench_note_ccr3(b, req);
}

/* one ISR with an explicit environment */
static void isr_env(bench_t *b, const adc_trigger_diag_env_t *env, uint8_t adc_flags)
{
	if (adc_trigger_diag_owns_ch3()) {
		adc_trigger_diag_sample_t s;
		uint16_t next;
		s.env = *env;
		s.adc_flags = adc_flags;
		if (adc_trigger_diag_isr(&s, &next)) bench_note_ccr3(b, next);
	}
	if (++b->isr_in_tick >= b->isr_per_tick) { b->isr_in_tick = 0; b->tick++; }
}

static void t1_t2_arm_snapshot(void)
{
	bench_t b;
	adc_trigger_diag_env_t e1 = mk_env(1000U, 3352U, ADC_TRIGGER_DIAG_ENV_STANDSTILL, 0U, 0U);
	adc_trigger_diag_env_t e2 = mk_env(9999U, 111U, 0xFFU, 5U, 4242U);
	const adc_trigger_diag_env_t *s;

	bench_init(&b);
	bench_arm_env(&b, true, false, &e1);
	s = adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_ARM);
	CHECK(s != NULL, "T1a: a successful ARM freezes the ARM snapshot");
	CHECK(s && s->tick == 1000U && s->cnt == 3352U && s->lifecycle == 0U,
		"T1b: ... with the values handed in at that instant");
	CHECK((adc_trigger_diag_snap_flags() & ADC_TRIGGER_DIAG_SNAP_ARM) != 0U,
		"T1c: ... and the status flags say so");

	for (int i = 0; i < 20; i++) bench_arm_env(&b, true, false, &e2);
	s = adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_ARM);
	CHECK(s && s->tick == 1000U && s->cnt == 3352U && s->iq_setpoint_abs == 0U,
		"T2: the ARM snapshot is immutable - later arm attempts never overwrite it");
}

static void t3_first_isr_is_one(void)
{
	bench_t b;
	adc_trigger_diag_env_t e = mk_env(0U, 3352U, 0U, 0U, 0U);

	bench_init(&b);
	bench_arm_env(&b, true, false, &e);
	CHECK(adc_trigger_diag_arm_isr_count() == 0U, "T3a: right after ARM the ISR counter is 0");

	isr_env(&b, &e, ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC);
	CHECK(adc_trigger_diag_arm_isr_count() == 1U,
		"T3b: the FIRST ISR after ARM is number 1 - no off-by-one left to interpret");

	isr_env(&b, &e, 0);
	isr_env(&b, &e, 0);
	CHECK(adc_trigger_diag_arm_isr_count() == 3U, "T3c: ... and it keeps counting every ISR");
}

static void t4_t6_abort_on_first_isr(void)
{
	bench_t b;
	adc_trigger_diag_env_t clean = mk_env(0U, 3352U, 0U, 0U, 0U);
	adc_trigger_diag_env_t moe = mk_env(77U, 1234U,
		ADC_TRIGGER_DIAG_ENV_POEN | ADC_TRIGGER_DIAG_ENV_IQ_NONZERO, 2U, 14U);
	const adc_trigger_diag_env_t *s;

	bench_init(&b);
	bench_arm_env(&b, true, false, &clean);
	isr_env(&b, &moe, 0);

	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_ABORTED_MOE, "T4a: MOE on ISR 1 aborts");
	CHECK(adc_trigger_diag_abort_isr_count() == 1U,
		"T4b: abort_isr_count == 1 - the case the old isr_count=0 could not distinguish");
	CHECK(adc_trigger_diag_abort_reason() == ADC_TRIGGER_DIAG_ABORT_POEN, "T4c: reason is POEN");
	CHECK(adc_trigger_diag_get_point(0)->isr_count == 0U,
		"T4d: the per-point counter still excludes the aborting sample - measurement unchanged");
	CHECK(b.ccr3 == ADC_TRIGGER_DIAG_CCR3_MAX, "T4e: production CCR3 restored in that same call");

	s = adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_ABORT);
	CHECK(s && s->tick == 77U && s->cnt == 1234U && s->lifecycle == 2U && s->iq_setpoint_abs == 14U,
		"T6a: the ABORT snapshot records the environment that caused it, before the state changed");

	for (int i = 0; i < 50; i++) isr_env(&b, &clean, 0);
	s = adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_ABORT);
	CHECK(s && s->tick == 77U && s->cnt == 1234U,
		"T6b: the ABORT snapshot is immutable - later riding cannot overwrite it");
	CHECK(adc_trigger_diag_abort_isr_count() == 1U, "T6c: ... and neither can abort_isr_count");
}

static void t5_abort_after_400(void)
{
	bench_t b;
	adc_trigger_diag_env_t clean = mk_env(0U, 3352U, 0U, 0U, 0U);
	adc_trigger_diag_env_t moe = mk_env(0U, 900U, ADC_TRIGGER_DIAG_ENV_POEN, 2U, 0U);
	int i;

	bench_init(&b);
	bench_arm_env(&b, true, false, &clean);
	for (i = 0; i < 399; i++) { clean.tick = (uint32_t)(i / 4); isr_env(&b, &clean, 0); }
	CHECK(adc_trigger_diag_arm_isr_count() == 399U, "T5a: 399 clean ISRs counted");
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_RUNNING, "T5b: still running");

	moe.tick = 100U;
	isr_env(&b, &moe, 0);
	CHECK(adc_trigger_diag_abort_isr_count() == 400U,
		"T5c: MOE on the 400th ISR reports 400 - a 25 ms bridge-start lead time is measurable");
	CHECK(adc_trigger_diag_get_point(0)->isr_count == 399U,
		"T5d: the per-point counter holds the 399 samples that were actually taken");
}

static void t7_poen_snapshot(void)
{
	bench_t b;
	adc_trigger_diag_env_t clean = mk_env(0U, 3352U, 0U, 0U, 0U);
	adc_trigger_diag_env_t poen = mk_env(55U, 2222U, ADC_TRIGGER_DIAG_ENV_POEN, 2U, 30U);
	adc_trigger_diag_env_t poen2 = mk_env(66U, 3333U, ADC_TRIGGER_DIAG_ENV_POEN, 5U, 40U);
	const adc_trigger_diag_env_t *s;

	bench_init(&b);
	bench_arm_env(&b, true, false, &clean);
	CHECK(adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_POEN) == NULL,
		"T7a: no POEN snapshot until an enable actually happens");
	CHECK((adc_trigger_diag_snap_flags() & ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING) == 0U,
		"T7b: ... and the discriminator bit is clear");
	CHECK(adc_trigger_diag_poen_enable_count() == 0U, "T7c: ... and nothing is counted");

	adc_trigger_diag_note_poen_enable(&poen);
	s = adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_POEN);
	CHECK(s && s->tick == 55U && s->cnt == 2222U, "T7d: a real enable is recorded");
	CHECK((adc_trigger_diag_snap_flags() & ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING) != 0U,
		"T7e: ... and flagged as having happened WHILE the sweep was running");
	CHECK(adc_trigger_diag_poen_enable_count() == 1U, "T7f: ... and counted");

	adc_trigger_diag_note_poen_enable(&poen2);
	s = adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_POEN);
	CHECK(s && s->tick == 55U, "T7g: only the FIRST enable while running is frozen");
	CHECK(adc_trigger_diag_poen_enable_count() == 2U, "T7h: ... but every enable is counted");

	bench_init(&b);
	adc_trigger_diag_note_poen_enable(&poen);
	adc_trigger_diag_note_poen_enable(&poen2);
	CHECK(adc_trigger_diag_poen_enable_count() == 2U, "T7i: enables before ARM are still counted");
	CHECK(adc_trigger_diag_snapshot(ADC_TRIGGER_DIAG_TAG_POEN) == NULL &&
	      (adc_trigger_diag_snap_flags() & ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING) == 0U,
		"T7j: THE DISCRIMINATOR - an enable outside RUNNING never sets the while-running bit");
}

static void t8_normal_sweep_unchanged(void)
{
	bench_t b;
	uint8_t i;
	int ok = 1;

	bench_init(&b);
	(void)run_full_sweep(&b, ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC);
	CHECK(adc_trigger_diag_state() == ADC_TRIGGER_DIAG_DONE, "T8a: an undisturbed sweep still completes");
	for (i = 0; i < ADC_TRIGGER_DIAG_POINTS; i++) {
		const adc_trigger_diag_point_t *p = adc_trigger_diag_get_point(i);
		if (!p->complete) ok = 0;
		if (p->tick_span != ADC_TRIGGER_DIAG_POINT_TICKS) ok = 0;
		if (p->isr_count < ADC_TRIGGER_DIAG_POINT_TICKS * ISR_PER_TICK) ok = 0;
	}
	CHECK(ok, "T8b: every point still completes with its full span and sample count");
	CHECK(adc_trigger_diag_abort_reason() == ADC_TRIGGER_DIAG_ABORT_NONE,
		"T8c: no abort reason on a clean sweep");
	CHECK(adc_trigger_diag_arm_isr_count() >= (uint32_t)ADC_TRIGGER_DIAG_POINTS * ADC_TRIGGER_DIAG_POINT_TICKS * ISR_PER_TICK,
		"T8d: arm_isr_count spans the whole sweep");
}

/* ------------------------------------------------------------------------ FW-121.0B transport */

static void t_transport_snapshots(void)
{
	bench_t b;
	adc_trigger_diag_env_t clean = mk_env(0U, 3352U, 0U, 0U, 0U);
	adc_trigger_diag_env_t poen = mk_env(0x1234U, 0x0ABCU,
		ADC_TRIGGER_DIAG_ENV_POEN | ADC_TRIGGER_DIAG_ENV_RUNNING, 2U, 300U);
	uint32_t efid; uint8_t d[8];

	bench_init(&b);
	bench_arm_env(&b, true, false, &clean);
	adc_trigger_diag_note_poen_enable(&poen);
	isr_env(&b, &poen, 0);

	CHECK(adc_trigger_diag_aggregate_frame(0U, &efid, d) && efid == ADC_TRIGGER_DIAG_EFID_STATUS,
		"T10a: status frame id unchanged");
	CHECK(d[0] == ADC_TRIGGER_DIAG_ABORTED_MOE && d[1] == 0U,
		"T10b: status carries state and point index");
	CHECK(((uint16_t)d[2] << 8 | d[3]) == 1U, "T10c: status carries abort_isr_count = 1");
	CHECK(d[4] == ADC_TRIGGER_DIAG_ABORT_POEN, "T10d: status carries the abort reason");
	CHECK((d[5] & ADC_TRIGGER_DIAG_SNAP_ARM) && (d[5] & ADC_TRIGGER_DIAG_SNAP_ABORT) &&
	      (d[5] & ADC_TRIGGER_DIAG_SNAP_POEN) && (d[5] & ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING),
		"T10e: status advertises all three snapshots and the while-running bit");
	CHECK(((uint16_t)d[6] << 8 | d[7]) == 1U, "T10f: status carries the POEN enable count");

	CHECK(adc_trigger_diag_aggregate_frame(4U, &efid, d) &&
	      efid == ADC_TRIGGER_DIAG_EFID_POINT_BASE + 3U && d[0] == ADC_TRIGGER_DIAG_TAG_ARM,
		"T10g: after an abort sub 4 (0x10244) is the ARM snapshot, tagged 0xA1");
	CHECK(adc_trigger_diag_aggregate_frame(5U, &efid, d) &&
	      efid == ADC_TRIGGER_DIAG_EFID_POINT_BASE + 4U && d[0] == ADC_TRIGGER_DIAG_TAG_ABORT,
		"T10h: sub 5 (0x10245) is the ABORT snapshot, tagged 0xA2");
	CHECK(adc_trigger_diag_aggregate_frame(6U, &efid, d) &&
	      efid == ADC_TRIGGER_DIAG_EFID_POINT_BASE + 5U && d[0] == ADC_TRIGGER_DIAG_TAG_POEN,
		"T10i: sub 6 (0x10246) is the POEN-ENABLE snapshot, tagged 0xA3");
	CHECK(((uint16_t)d[2] << 8 | d[3]) == 0x1234U && ((uint16_t)d[4] << 8 | d[5]) == 0x0ABCU &&
	      d[6] == 2U && d[7] == 255U,
		"T10j: snapshot body = tick16 / CNT / lifecycle / iq (saturating)");

	bench_init(&b);
	(void)run_full_sweep(&b, ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC);
	CHECK(adc_trigger_diag_aggregate_frame(4U, &efid, d) && d[0] == (0x80U | 3U),
		"T10k: with no abort, sub 4 is still point 3 - the tags never collide with 0x80|index");
}

/* -------------------------------------------------------------------------------------- main */
int main(void)
{
	printf("FW-121.0 ADC trigger timing diagnostic (real adc_trigger_diag.c)\n");
	printf("  sweep: %u points, %u..%u step %u, %u ticks each\n",
		(unsigned)ADC_TRIGGER_DIAG_POINTS, (unsigned)ADC_TRIGGER_DIAG_CCR3_MAX,
		(unsigned)ADC_TRIGGER_DIAG_CCR3_MIN, (unsigned)ADC_TRIGGER_DIAG_CCR3_STEP,
		(unsigned)ADC_TRIGGER_DIAG_POINT_TICKS);

	s1_arming_gate();
	s2_s3_s4_sweep_shape();
	s5_moe_interlock();
	s6_once_per_power_cycle();
	s7_statistics();
	s9_transport_contract();
	t1_t2_arm_snapshot();
	t3_first_isr_is_one();
	t4_t6_abort_on_first_isr();
	t5_abort_after_400();
	t7_poen_snapshot();
	t8_normal_sweep_unchanged();
	t_transport_snapshots();

	if (host_test_failures == 0) {
		printf("\nAll FW-121.0 checks passed.\n");
		return 0;
	}
	printf("\n%d FW-121.0 check(s) FAILED.\n", host_test_failures);
	return 1;
}
