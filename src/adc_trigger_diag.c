#include "adc_trigger_diag.h"
#include "diag_budget.h"

#if CAN_DIAGNOSTICS_ENABLE

#include <string.h>

#define ADC_TRIGGER_DIAG_MODE_NONE     0U
#define ADC_TRIGGER_DIAG_MODE_DARK     1U /* legacy FW-121 host-test path; not armed by main.c */
#define ADC_TRIGGER_DIAG_MODE_NEUTRAL  2U /* FW-126.0 production diagnostic path                 */

/*
 * FW-121.0: see the header for what this measures and why. Implementation notes only.
 *
 * All mutable state is in the single struct S so the RAM budget check measures the whole module.
 * Nothing here touches a register: main.c reads TIMER0/ADC and writes CH3, this file only decides.
 * That is what makes the safety interlock and the sweep sequence executable in a host test.
 */

static struct {
	adc_trigger_diag_point_t point[ADC_TRIGGER_DIAG_POINTS];
	/* FW-126.2: the neutral-dwell sweep's own storage. Separate from point[] above so the
	 * legacy dark-bridge slots keep working untouched - see the header. */
	adc_trigger_diag_npoint_t npoint[ADC_TRIGGER_DIAG_NEUTRAL_POINTS];
	bool     first_isr_discarded;  /* FW-126.2 rule 1: the MOE-ON interrupt never counts */
	/* FW-121.0B: the three frozen chronology snapshots. Written once each, never again. */
	adc_trigger_diag_env_t arm_env;
	adc_trigger_diag_env_t abort_env;
	adc_trigger_diag_env_t poen_env;
	uint32_t arm_isr_count;    /* ISRs since the successful ARM - incremented BEFORE the guard */
	uint32_t abort_isr_count;  /* frozen copy of arm_isr_count at the abort                    */
	uint32_t isr_per_control_max;
	uint32_t adc0_late_total;
	uint32_t adc1_late_total;
	uint32_t adc2_late_total;
	uint32_t last_control_tick;
	uint32_t tick_start;       /* control tick at the first ISR of the current point */
	uint16_t poen_enable_count;/* every POEN enable seen since boot (saturating)     */
	uint16_t production_ccr3;  /* restored on finish and on abort                     */
	uint8_t  state;
	uint8_t  index;
	uint8_t  snap_flags;       /* ADC_TRIGGER_DIAG_SNAP_*                             */
	uint8_t  abort_reason;     /* ADC_TRIGGER_DIAG_ABORT_*                            */
	uint8_t  mode;
	uint8_t  isr_this_control_tick;
	bool     have_control_tick;
	bool     point_started;    /* tick_start is valid for the current point           */
	bool     armed_once;       /* one sweep per power cycle, never re-arms            */
} S;

static void add_sat32(uint32_t *v)
{
	if (*v < 0xFFFFFFFFU) (*v)++;
}

static uint16_t ccr3_for_index(uint8_t i)
{
	/* Descending from the production value, clamped by construction: the lowest index this
	 * module can reach is ADC_TRIGGER_DIAG_POINTS-1, and the table is built so that point maps
	 * exactly onto ADC_TRIGGER_DIAG_CCR3_MIN. */
	uint32_t v = (uint32_t)ADC_TRIGGER_DIAG_CCR3_MAX - (uint32_t)i * ADC_TRIGGER_DIAG_CCR3_STEP;
	if (v > (uint32_t)ADC_TRIGGER_DIAG_CCR3_MAX) v = ADC_TRIGGER_DIAG_CCR3_MAX;
	if (v < (uint32_t)ADC_TRIGGER_DIAG_CCR3_MIN) v = ADC_TRIGGER_DIAG_CCR3_MIN;
	return (uint16_t)v;
}

/* The table has to land exactly on the card's floor, or the sweep would silently stop short of
 * the one point where the two hypotheses are expected to visibly diverge. */
_Static_assert(ADC_TRIGGER_DIAG_CCR3_MAX
	- (ADC_TRIGGER_DIAG_POINTS - 1U) * ADC_TRIGGER_DIAG_CCR3_STEP == ADC_TRIGGER_DIAG_CCR3_MIN,
	"FW-121.0: the CCR3 sweep table does not end on ADC_TRIGGER_DIAG_CCR3_MIN");

static void add_sat16(uint16_t *v)
{
	if (*v < 0xFFFFU) (*v)++;
}

void adc_trigger_diag_init(uint16_t production_ccr3)
{
	memset(&S, 0, sizeof(S));
	S.production_ccr3 = production_ccr3;
	S.state = ADC_TRIGGER_DIAG_IDLE;
	/* Pre-fill the CCR3 each point WILL be measured at, so a sweep that never reached a point
	 * still reports which value that point stands for. Reporting only - it does not decide, and
	 * cannot disagree with, what the sweep actually programs (both come from ccr3_for_index()). */
	for (uint8_t i = 0; i < ADC_TRIGGER_DIAG_POINTS; i++) S.point[i].ccr3 = ccr3_for_index(i);
	/* FW-126.2: same for the neutral-dwell points - a sweep that never armed still says which
	 * CH3 value each of its three points stands for. */
	for (uint8_t i = 0; i < ADC_TRIGGER_DIAG_NEUTRAL_POINTS; i++)
		S.npoint[i].ccr3_requested = ccr3_for_index(i);
}

bool adc_trigger_diag_owns_ch3(void)
{
	return S.state == ADC_TRIGGER_DIAG_RUNNING;
}

uint8_t adc_trigger_diag_state(void) { return S.state; }
uint8_t adc_trigger_diag_index(void) { return S.index; }

const adc_trigger_diag_point_t *adc_trigger_diag_get_point(uint8_t i)
{
	if (i >= ADC_TRIGGER_DIAG_POINTS) return 0;
	return &S.point[i];
}

bool adc_trigger_diag_arm(bool standstill_ok, bool moe_on,
                          const adc_trigger_diag_env_t *env, uint16_t *ccr3_out)
{
	if (S.armed_once) return false;                 /* once per power cycle, full stop */
	if (S.state != ADC_TRIGGER_DIAG_IDLE) return false;
	if (!standstill_ok || moe_on) return false;     /* the bike must be still AND the bridge dark */

	S.armed_once = true;
	S.mode = ADC_TRIGGER_DIAG_MODE_DARK;
	S.state = ADC_TRIGGER_DIAG_RUNNING;
	S.index = 0;
	S.point_started = false;
	S.point[0].ccr3 = ccr3_for_index(0);

	/*
	 * FW-121.0B: freeze the ARM snapshot HERE, on the transition itself - not one call later and
	 * not from values re-read afterwards. `armed_once` above guarantees this runs at most once,
	 * so the snapshot is immutable by construction rather than by a separate guard.
	 */
	S.arm_isr_count = 0U;
	if (env != 0) {
		S.arm_env = *env;
		S.snap_flags |= ADC_TRIGGER_DIAG_SNAP_ARM;
	}

	if (ccr3_out) *ccr3_out = S.point[0].ccr3;
	return true;
}

bool adc_trigger_diag_arm_neutral_dwell(const adc_trigger_diag_env_t *env,
									uint8_t safety_flags, uint16_t *ccr3_out)
{
	/* This is deliberately a separate entry point from adc_trigger_diag_arm(): FW-126.0 must
	 * never quietly re-enable the unusable MOE-off sweep on a future refactor. */
	if (S.armed_once || S.state != ADC_TRIGGER_DIAG_IDLE) return false;
	if (env == 0) return false;
	if (!(env->flags & ADC_TRIGGER_DIAG_ENV_DWELL)) return false;
	if ((safety_flags & (ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR | ADC_TRIGGER_DIAG_SAFE_FOC_HELD))
	    != (ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR | ADC_TRIGGER_DIAG_SAFE_FOC_HELD)) return false;

	S.armed_once = true;
	S.mode = ADC_TRIGGER_DIAG_MODE_NEUTRAL;
	S.state = ADC_TRIGGER_DIAG_RUNNING;
	S.index = 0U;
	S.point_started = false;
	S.arm_isr_count = 0U;
	S.have_control_tick = false;
	S.isr_this_control_tick = 0U;
	S.isr_per_control_max = 0U;
	S.adc0_late_total = 0U;
	S.adc1_late_total = 0U;
	S.adc2_late_total = 0U;
	S.first_isr_discarded = false;   /* FW-126.2: armed here, spent on the next interrupt */
	for (uint8_t i = 0U; i < ADC_TRIGGER_DIAG_NEUTRAL_POINTS; i++) {
		adc_trigger_diag_npoint_t *np = &S.npoint[i];
		for (uint8_t k = 0U; k < ADC_TRIGGER_DIAG_SAMPLES_TARGET; k++) np->cnt[k] = 0U;
		np->ccr3_requested = ccr3_for_index(i);
		np->ccr3_readback = 0U;
		np->cnt_median = 0U;
		np->cnt_min = 0U;
		np->cnt_max = 0U;
		np->isr_seq_first = 0U;
		np->accepted = 0U;
		np->rejected_late = 0U;
		np->dir_up_count = 0U;
		np->eoic_all_count = 0U;
		np->env_flags = 0U;
		np->complete = 0U;
	}

	if (ccr3_out) *ccr3_out = S.npoint[0].ccr3_requested;
	return true;
}

void adc_trigger_diag_note_poen_enable(const adc_trigger_diag_env_t *env)
{
	/* FW-126.0 deliberately enables MOE as part of its neutral-dwell measurement. The old FW-121
	 * chronology hook is meaningful only for the retained dark-bridge experiment, whose safety
	 * condition is precisely that MOE must abort it. */
	if (S.mode != ADC_TRIGGER_DIAG_MODE_DARK) return;

	/* Measurement only: this never gates, cancels or delays the enable it is observing. */
	if (S.poen_enable_count < 0xFFFFU) S.poen_enable_count++;

	/*
	 * The discriminator FW-121.0A needs. Freeze the FIRST enable that happens while the sweep is
	 * RUNNING - that is the one that can abort it. If the sweep aborts and this snapshot is
	 * ABSENT, POEN was already high when the arm gate read it as low, and the contradiction is on
	 * the arm side rather than in a later bridge start.
	 */
	if (S.state == ADC_TRIGGER_DIAG_RUNNING &&
	    !(S.snap_flags & ADC_TRIGGER_DIAG_SNAP_POEN)) {
		if (env != 0) {
			S.poen_env = *env;
			S.snap_flags |= (ADC_TRIGGER_DIAG_SNAP_POEN | ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING);
		} else {
			S.snap_flags |= ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING;
		}
	}
}

/* Close the current point and move on; returns the CCR3 the caller must program. */
static uint16_t finish_point(uint32_t tick)
{
	adc_trigger_diag_point_t *p = &S.point[S.index];

	p->tick_span = (uint16_t)(tick - S.tick_start);
	p->complete = 1U;

	S.index++;
	if (S.index >= ADC_TRIGGER_DIAG_POINTS) {
		S.index = ADC_TRIGGER_DIAG_POINTS;   /* one past the end: "all points done" */
		S.state = ADC_TRIGGER_DIAG_DONE;
		return S.production_ccr3;
	}

	S.point_started = false;
	S.point[S.index].ccr3 = ccr3_for_index(S.index);
	return S.point[S.index].ccr3;
}

bool adc_trigger_diag_isr(const adc_trigger_diag_sample_t *s, uint16_t *ccr3_out)
{
	if (S.mode != ADC_TRIGGER_DIAG_MODE_DARK) return false;
	if (S.state != ADC_TRIGGER_DIAG_RUNNING) return false;

	/*
	 * FW-121.0B: count the ISR FIRST, before any guard can return. This is the whole point of the
	 * card - the per-point isr_count below is incremented only for samples that were actually
	 * taken, so "aborted on the first interrupt" and "no interrupt ever ran" both read as 0 there.
	 * arm_isr_count has no such ambiguity: the first ISR after ARM is 1, the four-hundredth is
	 * 400. The per-point counters keep their old meaning exactly - nothing this card does changes
	 * what the sweep measures.
	 */
	add_sat32(&S.arm_isr_count);

	/*
	 * THE INTERLOCK. The bridge coming up outranks everything: hand CH3 straight back to the
	 * production value and stop, before this sample is even counted. The main loop's own gate
	 * runs at 4 kHz and the bridge can be enabled between two of its iterations, so this - at the
	 * PWM rate, reading the timer's own output-enable bit - is what actually bounds the exposure
	 * to a single PWM period. UNCHANGED by FW-121.0B: same condition, same instant, same result.
	 */
	if (s->env.flags & ADC_TRIGGER_DIAG_ENV_POEN) {
		/* Freeze the ABORT snapshot BEFORE the state changes, so it records the environment that
		 * caused the abort rather than the aftermath. Reached at most once: the state leaves
		 * RUNNING on this same line. */
		S.abort_env = s->env;
		S.abort_isr_count = S.arm_isr_count;
		S.abort_reason = ADC_TRIGGER_DIAG_ABORT_POEN;
		S.snap_flags |= ADC_TRIGGER_DIAG_SNAP_ABORT;

		S.state = ADC_TRIGGER_DIAG_ABORTED_MOE;
		if (ccr3_out) *ccr3_out = S.production_ccr3;
		return true;
	}

	adc_trigger_diag_point_t *p = &S.point[S.index];

	if (!S.point_started) {
		S.point_started = true;
		S.tick_start = s->env.tick;
		p->cnt_first = s->env.cnt;
		p->cnt_min = s->env.cnt;
		p->cnt_max = s->env.cnt;
	} else {
		if (s->env.cnt < p->cnt_min) p->cnt_min = s->env.cnt;
		if (s->env.cnt > p->cnt_max) p->cnt_max = s->env.cnt;
	}

	add_sat16(&p->isr_count);
	if (!(s->env.flags & ADC_TRIGGER_DIAG_ENV_DIR_DOWN)) add_sat16(&p->dir_up_count);
	if (!(s->adc_flags & ADC_TRIGGER_DIAG_F_ADC0_EOIC)) add_sat16(&p->adc0_late_count);
	if (!(s->adc_flags & ADC_TRIGGER_DIAG_F_ADC2_EOIC)) add_sat16(&p->adc2_late_count);

	if ((uint32_t)(s->env.tick - S.tick_start) >= ADC_TRIGGER_DIAG_POINT_TICKS) {
		uint16_t next = finish_point(s->env.tick);
		if (ccr3_out) *ccr3_out = next;
		return true;
	}
	return false;
}

static void neutral_count_isr_rate(uint32_t tick)
{
	if (!S.have_control_tick || S.last_control_tick != tick) {
		S.have_control_tick = true;
		S.last_control_tick = tick;
		S.isr_this_control_tick = 1U;
	} else if (S.isr_this_control_tick < 0xFFU) {
		S.isr_this_control_tick++;
	}
	if (S.isr_this_control_tick > S.isr_per_control_max)
		S.isr_per_control_max = S.isr_this_control_tick;
}

/*
 * FW-126.2: close a point once it has its samples. The MEDIAN is the point's answer - with a
 * single sample per point (FW-126.0) one disturbed reading moved the whole slope, which is
 * exactly what the 17:44 log showed. min/max travel with it so "stable" is visible rather than
 * assumed. Insertion sort on a local copy: at most SAMPLES_TARGET elements, and the stored
 * samples must stay in arrival order for the frame builder.
 */
static void finalize_npoint(adc_trigger_diag_npoint_t *np)
{
	uint16_t sorted[ADC_TRIGGER_DIAG_SAMPLES_TARGET];
	uint8_t n = np->accepted;
	uint8_t i, j;

	if (n == 0U) { np->complete = 0U; return; }
	for (i = 0U; i < n; i++) sorted[i] = np->cnt[i];
	for (i = 1U; i < n; i++) {
		uint16_t key = sorted[i];
		j = i;
		while (j > 0U && sorted[j - 1U] > key) { sorted[j] = sorted[j - 1U]; j--; }
		sorted[j] = key;
	}
	np->cnt_min = sorted[0];
	np->cnt_max = sorted[n - 1U];
	np->cnt_median = sorted[n / 2U];
	/* A point is evidence only once it holds at least SAMPLES_MIN accepted conversions. Below
	 * that it is reported, with its counts, but never marked complete - so a decoder cannot
	 * build a slope out of it by accident. */
	np->complete = (n >= ADC_TRIGGER_DIAG_SAMPLES_MIN) ? 1U : 0U;
}

bool adc_trigger_diag_neutral_dwell_isr(const adc_trigger_diag_sample_t *s, uint16_t *ccr3_out)
{
	adc_trigger_diag_npoint_t *np;
	uint8_t need = ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR | ADC_TRIGGER_DIAG_SAFE_FOC_HELD;

	if (S.mode != ADC_TRIGGER_DIAG_MODE_NEUTRAL || S.state != ADC_TRIGGER_DIAG_RUNNING)
		return false;
	if (s == 0) return false;

	/* ADC1 EOIC caused this IRQ. Count its sequence even when the safety interlock aborts: that
	 * distinguishes a real conversion from an unobserved/no-IRQ point. */
	add_sat32(&S.arm_isr_count);
	neutral_count_isr_rate(s->env.tick);

	/* Hard safety interlock: any loss of the existing neutral dwell, a non-neutral phase CCR, or
	 * FOC no longer being held ends ownership before the changed CH3 can reach torque FOC. */
	if (!(s->env.flags & ADC_TRIGGER_DIAG_ENV_DWELL) || (s->safety_flags & need) != need) {
		S.abort_env = s->env;
		S.abort_isr_count = S.arm_isr_count;
		S.abort_reason = ADC_TRIGGER_DIAG_ABORT_SAFETY;
		S.snap_flags |= ADC_TRIGGER_DIAG_SNAP_ABORT;
		S.state = ADC_TRIGGER_DIAG_ABORTED_MOE;
		if (ccr3_out) *ccr3_out = S.production_ccr3;
		return true;
	}

	/*
	 * FW-126.2 rule 1. The very first interrupt after MOE ON is discarded whatever it contains.
	 * In the 17:44 log it was the only sample entered while the counter was still counting UP -
	 * both hypotheses predict entry on the down slope, so that reading was measuring the bridge
	 * coming up rather than the trigger, and it alone produced the -4 slope. Discarded BEFORE
	 * the EOIC test so it can never be mistaken for a late conversion either.
	 */
	if (!S.first_isr_discarded) {
		S.first_isr_discarded = true;
		return false;
	}

	np = &S.npoint[S.index];

	/* FW-126.2 rule 3: ADC1's group must have completed. Such an interrupt is NOT a timing
	 * sample - it is counted against this point and the point waits for another one. */
	if (!(s->adc_flags & ADC_TRIGGER_DIAG_F_ADC1_EOIC)) {
		add_sat32(&S.adc1_late_total);
		if (np->rejected_late < 0xFFU) np->rejected_late++;
		return false;
	}

	if (np->accepted == 0U) np->isr_seq_first = S.arm_isr_count;
	if (np->accepted < ADC_TRIGGER_DIAG_SAMPLES_TARGET) {
		np->cnt[np->accepted] = s->env.cnt;
		np->accepted++;
	}
	np->ccr3_readback = s->ccr3_readback;
	np->env_flags = s->env.flags;
	if (!(s->env.flags & ADC_TRIGGER_DIAG_ENV_DIR_DOWN) && np->dir_up_count < 0xFFU)
		np->dir_up_count++;
	if ((s->adc_flags & (ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC1_EOIC |
	                     ADC_TRIGGER_DIAG_F_ADC2_EOIC)) ==
	    (ADC_TRIGGER_DIAG_F_ADC0_EOIC | ADC_TRIGGER_DIAG_F_ADC1_EOIC | ADC_TRIGGER_DIAG_F_ADC2_EOIC)) {
		if (np->eoic_all_count < 0xFFU) np->eoic_all_count++;
	}
	/* The other two ADCs are recorded, never gating: only ADC1 raised this interrupt. */
	if (!(s->adc_flags & ADC_TRIGGER_DIAG_F_ADC0_EOIC)) add_sat32(&S.adc0_late_total);
	if (!(s->adc_flags & ADC_TRIGGER_DIAG_F_ADC2_EOIC)) add_sat32(&S.adc2_late_total);

	/* Still filling this CH3 value: keep the compare where it is and take another conversion. */
	if (np->accepted < ADC_TRIGGER_DIAG_SAMPLES_TARGET) return false;

	finalize_npoint(np);

	S.index++;
	if (S.index >= ADC_TRIGGER_DIAG_NEUTRAL_POINTS) {
		S.index = ADC_TRIGGER_DIAG_NEUTRAL_POINTS;
		S.state = ADC_TRIGGER_DIAG_DONE;
		if (ccr3_out) *ccr3_out = S.production_ccr3;
		return true;
	}

	S.npoint[S.index].ccr3_requested = ccr3_for_index(S.index);
	if (ccr3_out) *ccr3_out = S.npoint[S.index].ccr3_requested;
	return true;
}

const adc_trigger_diag_npoint_t *adc_trigger_diag_get_npoint(uint8_t i)
{
	return (i < ADC_TRIGGER_DIAG_NEUTRAL_POINTS) ? &S.npoint[i] : 0;
}

bool adc_trigger_diag_first_isr_discarded(void) { return S.first_isr_discarded; }

bool adc_trigger_diag_needs_dwell(void)
{
	return (S.mode == ADC_TRIGGER_DIAG_MODE_NEUTRAL) && (S.state == ADC_TRIGGER_DIAG_RUNNING);
}

uint32_t adc_trigger_diag_arm_isr_count(void)   { return S.arm_isr_count; }
uint32_t adc_trigger_diag_abort_isr_count(void) { return S.abort_isr_count; }
uint8_t  adc_trigger_diag_abort_reason(void)    { return S.abort_reason; }
uint8_t  adc_trigger_diag_snap_flags(void)      { return S.snap_flags; }
uint16_t adc_trigger_diag_poen_enable_count(void) { return S.poen_enable_count; }
uint32_t adc_trigger_diag_isr_per_control_max(void) { return S.isr_per_control_max; }
uint32_t adc_trigger_diag_adc0_late_total(void) { return S.adc0_late_total; }
uint32_t adc_trigger_diag_adc1_late_total(void) { return S.adc1_late_total; }
uint32_t adc_trigger_diag_adc2_late_total(void) { return S.adc2_late_total; }

const adc_trigger_diag_env_t *adc_trigger_diag_snapshot(uint8_t tag)
{
	switch (tag) {
	case ADC_TRIGGER_DIAG_TAG_ARM:
		return (S.snap_flags & ADC_TRIGGER_DIAG_SNAP_ARM) ? &S.arm_env : 0;
	case ADC_TRIGGER_DIAG_TAG_ABORT:
		return (S.snap_flags & ADC_TRIGGER_DIAG_SNAP_ABORT) ? &S.abort_env : 0;
	case ADC_TRIGGER_DIAG_TAG_POEN:
		return (S.snap_flags & ADC_TRIGGER_DIAG_SNAP_POEN) ? &S.poen_env : 0;
	default:
		return 0;
	}
}

/* One env snapshot serialized into 8 bytes. See the header for the field meanings. */
static void put_env(uint8_t tag, const adc_trigger_diag_env_t *e, uint8_t data[8])
{
	data[0] = tag;
	data[1] = e->flags;
	data[2] = (uint8_t)((e->tick >> 8) & 0xFFU);   /* control tick, low 16 bits, 250 us/LSB */
	data[3] = (uint8_t)(e->tick & 0xFFU);
	data[4] = (uint8_t)((e->cnt >> 8) & 0xFFU);    /* TIMER0 CNT, 8.333 ns/LSB              */
	data[5] = (uint8_t)(e->cnt & 0xFFU);
	data[6] = e->lifecycle;
	data[7] = (uint8_t)((e->iq_setpoint_abs > 255U) ? 255U : e->iq_setpoint_abs);
}

bool adc_trigger_diag_aggregate_frame(uint8_t sub, uint32_t *efid, uint8_t data[8])
{
	uint8_t i;

	if (sub >= ADC_TRIGGER_DIAG_AGG_FRAMES) return false;
	if (efid == 0 || data == 0) return false;

	for (i = 0; i < 8U; i++) data[i] = 0U;

	/* FW-126.0 neutral-dwell result, schema 7. Reuse the existing seven FW-121 ids rather than
	 * allocate another diagnostic namespace:
	 *   0x10240 status: state, completed points, total ISR sequence, max ISR/control tick,
	 *                   ADC0/ADC1/ADC2 late totals (u8 saturated)
	 *   0x10241..43 point: point, CH3-3500, CNT_ISR, ISR sequence low16, lifecycle and flags
	 *   0x10244..46 raw: ADC2/A, ADC1/B, ADC0/C raw JDR at the three points, respectively.
	 * The raw frames intentionally contain all three points per phase; no 16-bit JDR precision is
	 * lost merely to fit the metadata frame. */
	if (S.mode == ADC_TRIGGER_DIAG_MODE_NEUTRAL) {
		/* FW-126.2 schema 8. Seven frames still: status, then TWO per point. See the header. */
		if (sub == 0U) {
			*efid = ADC_TRIGGER_DIAG_EFID_STATUS;
			data[0] = S.state;
			data[1] = S.index;
			data[2] = (uint8_t)((S.arm_isr_count >> 8) & 0xFFU);
			data[3] = (uint8_t)(S.arm_isr_count & 0xFFU);
			data[4] = (uint8_t)((S.isr_per_control_max > 255U) ? 255U : S.isr_per_control_max);
			data[5] = (uint8_t)((S.adc0_late_total > 255U) ? 255U : S.adc0_late_total);
			data[6] = (uint8_t)((S.adc1_late_total > 255U) ? 255U : S.adc1_late_total);
			data[7] = (uint8_t)((S.adc2_late_total > 255U) ? 255U : S.adc2_late_total);
			return true;
		}
		{
			uint8_t idx = (uint8_t)((sub - 1U) / 2U);          /* which point   */
			bool frame_b = (((sub - 1U) % 2U) != 0U);          /* A first, then B */
			const adc_trigger_diag_npoint_t *np = &S.npoint[idx];
			uint16_t req_off = (np->ccr3_requested > 3500U) ? (uint16_t)(np->ccr3_requested - 3500U) : 0U;
			uint16_t rb_off  = (np->ccr3_readback  > 3500U) ? (uint16_t)(np->ccr3_readback  - 3500U) : 0U;
			uint32_t spread  = (uint32_t)np->cnt_max - (uint32_t)np->cnt_min;

			*efid = ADC_TRIGGER_DIAG_EFID_POINT_BASE + (sub - 1U);
			if (!frame_b) {
				data[0] = (uint8_t)(0xC0U | idx);
				data[1] = (uint8_t)((req_off > 255U) ? 255U : req_off);
				data[2] = (uint8_t)((rb_off  > 255U) ? 255U : rb_off);
				data[3] = np->accepted;
				data[4] = (uint8_t)((np->cnt_median >> 8) & 0xFFU);
				data[5] = (uint8_t)(np->cnt_median & 0xFFU);
				data[6] = np->rejected_late;
				data[7] = (uint8_t)(((np->accepted > 0U && np->dir_up_count == 0U) ? 0x01U : 0U)
					| ((np->dir_up_count > 0U) ? 0x02U : 0U)
					| ((np->accepted > 0U && np->eoic_all_count == np->accepted) ? 0x04U : 0U)
					| (np->complete ? 0x08U : 0U)
					| ((np->env_flags & ADC_TRIGGER_DIAG_ENV_POEN) ? 0x10U : 0U)
					| ((np->env_flags & ADC_TRIGGER_DIAG_ENV_DWELL) ? 0x20U : 0U));
			} else {
				data[0] = (uint8_t)(0xB0U | idx);
				data[1] = (uint8_t)((np->cnt_min >> 8) & 0xFFU);
				data[2] = (uint8_t)(np->cnt_min & 0xFFU);
				data[3] = (uint8_t)((np->cnt_max >> 8) & 0xFFU);
				data[4] = (uint8_t)(np->cnt_max & 0xFFU);
				data[5] = (uint8_t)((spread > 255U) ? 255U : spread);
				data[6] = (uint8_t)((np->isr_seq_first >> 8) & 0xFFU);
				data[7] = (uint8_t)(np->isr_seq_first & 0xFFU);
			}
			return true;
		}
	}

	if (sub == 0U) {
		/*
		 * FW-121.0B status. The four build constants this frame used to carry (production CCR3,
		 * point count, ticks per point, CCR3 step) are gone: every one of them is fixed at
		 * compile time and the step is anyway the difference between two consecutive point
		 * frames' Data1. Their bytes now carry the chronology, which is not recoverable any
		 * other way. Schema 6.
		 */
		uint32_t ai = S.abort_isr_count;
		*efid = ADC_TRIGGER_DIAG_EFID_STATUS;
		data[0] = S.state;
		data[1] = S.index;
		data[2] = (uint8_t)((ai > 0xFFFFU) ? 0xFFU : ((ai >> 8) & 0xFFU));
		data[3] = (uint8_t)((ai > 0xFFFFU) ? 0xFFU : (ai & 0xFFU));
		data[4] = S.abort_reason;
		data[5] = S.snap_flags;
		data[6] = (uint8_t)((S.poen_enable_count >> 8) & 0xFFU);
		data[7] = (uint8_t)(S.poen_enable_count & 0xFFU);
		return true;
	}

	/*
	 * FW-121.0B: after an abort the last three point slots carry the chronology snapshots
	 * instead. An aborted sweep cannot have data in them unless it had already completed four
	 * points, and the tag byte (0xA1..0xA3, never a value a point frame can produce) means a
	 * decoder never has to know the state to tell which is which.
	 */
	if (S.state == ADC_TRIGGER_DIAG_ABORTED_MOE && sub >= 4U) {
		static const uint8_t tag_of[3] = {
			ADC_TRIGGER_DIAG_TAG_ARM, ADC_TRIGGER_DIAG_TAG_ABORT, ADC_TRIGGER_DIAG_TAG_POEN
		};
		uint8_t tag = tag_of[sub - 4U];
		const adc_trigger_diag_env_t *e = adc_trigger_diag_snapshot(tag);
		*efid = ADC_TRIGGER_DIAG_EFID_POINT_BASE + (sub - 1U);
		if (e == 0) { data[0] = tag; return true; }   /* tag with an all-zero body = not taken */
		put_env(tag, e, data);
		return true;
	}

	{
		uint8_t idx = (uint8_t)(sub - 1U);
		const adc_trigger_diag_point_t *p = &S.point[idx];
		uint16_t spread = (uint16_t)(p->cnt_max - p->cnt_min);
		uint16_t ccr3_off = (p->ccr3 > 3500U) ? (uint16_t)(p->ccr3 - 3500U) : 0U;

		*efid = ADC_TRIGGER_DIAG_EFID_POINT_BASE + idx;
		data[0] = (uint8_t)(idx | (p->complete ? 0x80U : 0x00U));
		data[1] = (uint8_t)((ccr3_off > 255U) ? 255U : ccr3_off);
		data[2] = (uint8_t)((p->isr_count >> 8) & 0xFFU);
		data[3] = (uint8_t)(p->isr_count & 0xFFU);
		data[4] = (uint8_t)((p->cnt_min >> 8) & 0xFFU);
		data[5] = (uint8_t)(p->cnt_min & 0xFFU);
		data[6] = (uint8_t)((spread > 255U) ? 255U : spread);
		data[7] = (uint8_t)((p->dir_up_count ? 0x01U : 0x00U)
		                  | ((p->isr_count && p->dir_up_count == p->isr_count) ? 0x02U : 0x00U)
		                  | (p->adc0_late_count ? 0x04U : 0x00U)
		                  | (p->adc2_late_count ? 0x08U : 0x00U));
		return true;
	}
}

/* The RAM this module is allowed to spend, checked by the compiler like every other recorder. */
_Static_assert(sizeof(S) <= DIAG_BUDGET_ADC_TRIGGER_BYTES,
	"FW-121.0: adc_trigger_diag state exceeds its RAM budget line item");

#else  /* !CAN_DIAGNOSTICS_ENABLE */

/* Like every other diagnostic module, this costs ZERO RAM and zero flash in the production
 * build - a #if rather than a reliance on --gc-sections, see pas_raw.c for why. */
typedef int adc_trigger_diag_not_compiled_in;

#endif /* CAN_DIAGNOSTICS_ENABLE */
