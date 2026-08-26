/*
 * FW-120.1: the reconstruction pair must belong to the PWM period the samples came from.
 *
 * This harness links the real src/dyn_adc_state.c and drives it through a model of the actual
 * acquisition pipeline:
 *
 *   ISR n-1: FOC -> switchtime[] -> CCR0/1/2   (shadow disabled: live immediately, and they
 *            then shape the whole next PWM period)
 *   top n:   CH3 fires the injected trigger; the three shunts are sampled under THOSE CCRs.
 *            The phase with the longest high-side on-time has the shortest low-side window,
 *            so its shunt reading is garbage - modelled here as POISON.
 *   ISR n:   JDR read -> reconstruction -> Clarke (which consumes A and B only).
 *
 * The model keeps two separate arrays for exactly the reason this card exists: `st` is the
 * firmware's switchtime[] and `ccr` is the hardware register set the sampler reads. They differ
 * only between this ISR's FOC_calculation() and its CCR write, which is precisely the window the
 * old ordering fell into.
 *
 * WHAT THIS PROVES (S1-S8):
 *   S1: dyn_adc_state_select() unit table - strict maximum per phase, and a tie returns the
 *       caller's previous state unchanged (the neutral / dwell / cutoff case).
 *   S2: dyn_adc_state_reconstruct() arithmetic - each state rebuilds exactly the true current,
 *       and phase C is never written back (it is not a Clarke input).
 *   S3: production ISR order, forward 360 deg duty sweep: POISON never reaches Clarke.
 *   S4: the same sweep with the rotation reversed - no direction assumption anywhere.
 *   S5: the three named ranking crossings C->A, A->B, B->C, each as a warmed-up scenario, under
 *       the production order.
 *   S6: NEGATIVE CONTROL - the pre-FW-120.1 order (verdict computed in the previous ISR) run
 *       through the identical model FAILS on every one of those crossings, and fails EXACTLY on
 *       the first period sampled under the new ranking. Without this the suite would pass just
 *       as happily on code that never had the fix.
 *   S7: the failures in S6 are confined to the crossings - away from a boundary both orders
 *       agree, so the fix is not "reconstruct more".
 *   S8: all-equal CCRs (neutral dwell / soft-cutoff end) still deliver the true currents.
 *
 * WHAT THIS DOES NOT PROVE: that a sample taken inside the "trusted" window is itself clean.
 * Whether the low-side conduction window is long enough for the ADC aperture at a given duty is
 * FW-121 (sampling validity), deliberately out of scope. This proves WHICH pair is used, never
 * that the pair was sampled well.
 */

#include "../common/check.h"

#include <stdio.h>
#include <stdint.h>

#include "dyn_adc_state.h"

/* Center-aligned _T from inc/config.h; only the RANKING matters here, not the exact scale. */
#define MODEL_T        3750
#define MODEL_NEUTRAL  (MODEL_T / 2)
#define POISON         ((int16_t)12345)

/* ------------------------------------------------------------------ hardware/pipeline model */

typedef struct {
	uint16_t st[3];       /* the firmware's switchtime[] array                              */
	uint16_t ccr[3];      /* TIMER0 CH0/1/2 compare registers as the sampler sees them      */
	uint8_t  state;       /* MS.char_dyn_adc_state                                          */
	uint8_t  pending;     /* legacy model only: verdict carried over from the previous ISR  */
	int16_t  clarke_a;    /* what FOC_calculation() received as Ia this ISR                 */
	int16_t  clarke_b;    /* ... and as Ib                                                  */
} pipeline_t;

static int argmax3(const uint16_t v[3])
{
	int m = 0;
	if (v[1] > v[m]) m = 1;
	if (v[2] > v[m]) m = 2;
	return m;
}

static int has_strict_max(const uint16_t v[3])
{
	int m = argmax3(v);
	int i;
	for (i = 0; i < 3; i++) {
		if (i != m && v[i] == v[m]) return 0;
	}
	return 1;
}

/*
 * The three true phase currents for this period. Any triple summing to zero will do - the
 * reconstruction identity is what is under test, not the waveform.
 */
static void true_currents(int step, int16_t out[3])
{
	int16_t a = (int16_t)(100 + 7 * step);
	int16_t b = (int16_t)(-40 - 3 * step);
	out[0] = a;
	out[1] = b;
	out[2] = (int16_t)(-a - b);
}

/*
 * The counter-top acquisition: every shunt reads true, except the one whose high side is on
 * longest - that is the one whose low-side window is too short to be believed. With all three
 * CCRs equal there is no such phase and all three read true.
 */
static void sample_shunts(const uint16_t ccr[3], const int16_t truth[3], int16_t jdr[3])
{
	int i;
	for (i = 0; i < 3; i++) jdr[i] = truth[i];
	if (has_strict_max(ccr)) jdr[argmax3(ccr)] = POISON;
}

/*
 * This ISR's FOC: publishes new switchtime[], then writes them to the CCRs - the same order
 * src/main.c uses (FOC_calculation() followed by three timer_channel_output_pulse_value_config()
 * calls, with output shadow DISABLED so the write is live at once).
 */
static void run_foc(pipeline_t *p, const uint16_t next_st[3])
{
	int i;
	for (i = 0; i < 3; i++) p->st[i] = next_st[i];
	for (i = 0; i < 3; i++) p->ccr[i] = next_st[i];
}

/* Production order (FW-120.1): select from switchtime[] at the TOP of the ISR, before FOC. */
static void isr_fixed(pipeline_t *p, int step, const uint16_t next_st[3])
{
	int16_t truth[3], jdr[3];
	true_currents(step, truth);
	sample_shunts(p->ccr, truth, jdr);

	p->state = dyn_adc_state_select(p->st, p->state);
	dyn_adc_state_reconstruct(p->state, &jdr[0], &jdr[1], jdr[2]);

	p->clarke_a = jdr[0];
	p->clarke_b = jdr[1];

	run_foc(p, next_st);
}

/*
 * Pre-FW-120.1 order, reproduced faithfully: the reconstruction consumed a verdict the PREVIOUS
 * ISR had computed, and that ISR computed it before its own FOC_calculation() - so the verdict
 * described the period before the one that was sampled. Kept only as S6's negative control.
 */
static void isr_legacy(pipeline_t *p, int step, const uint16_t next_st[3])
{
	int16_t truth[3], jdr[3];
	true_currents(step, truth);
	sample_shunts(p->ccr, truth, jdr);

	p->state = p->pending;                     /* verdict left behind by the previous ISR */
	dyn_adc_state_reconstruct(p->state, &jdr[0], &jdr[1], jdr[2]);

	p->clarke_a = jdr[0];
	p->clarke_b = jdr[1];

	/* the old dyn_adc_state() ran here, still before FOC_calculation() */
	p->pending = dyn_adc_state_select(p->st, p->pending);

	run_foc(p, next_st);
}

static void pipeline_init(pipeline_t *p, const uint16_t st0[3])
{
	int i;
	for (i = 0; i < 3; i++) { p->st[i] = st0[i]; p->ccr[i] = st0[i]; }
	p->state = DYN_ADC_STATE_UNDECIDED;
	p->pending = DYN_ADC_STATE_UNDECIDED;
	p->clarke_a = 0;
	p->clarke_b = 0;
}

/* Did this ISR hand FOC the true Ia/Ib for the period the samples came from? */
static int clarke_is_true(const pipeline_t *p, int step)
{
	int16_t truth[3];
	true_currents(step, truth);
	return (p->clarke_a == truth[0]) && (p->clarke_b == truth[1]);
}

/* ----------------------------------------------------------------------- duty trajectories */

/* Local cosine series - keeps the harness free of -lm on every host toolchain. */
static double cos_series(double x)
{
	static const double PI_D = 3.14159265358979323846;
	static const double TWO_PI = 6.28318530717958647692;
	double term, sum, t;
	int n;
	while (x >  PI_D) x -= TWO_PI;
	while (x < -PI_D) x += TWO_PI;
	sum = 1.0;
	term = 1.0;
	t = x * x;
	for (n = 1; n <= 12; n++) {
		term *= -t / (double)((2 * n - 1) * (2 * n));
		sum += term;
	}
	return sum;
}

/*
 * A rotating three-phase duty set. Only the RANKING of the three values reaches the module, so
 * an ordinary cosine triple reproduces every ranking a real SVPWM period can present, including
 * the exact instants where two of them cross.
 */
static void duty_at(int deg, int reverse, uint16_t st[3])
{
	static const double PI_D = 3.14159265358979323846;
	int i;
	int d = reverse ? -deg : deg;
	for (i = 0; i < 3; i++) {
		double ang = ((double)d - 120.0 * (double)i) * PI_D / 180.0;
		double v = (double)MODEL_NEUTRAL + 1500.0 * cos_series(ang);
		st[i] = (uint16_t)(v + 0.5);
	}
}

/* --------------------------------------------------------------------------------- S1 / S2 */

static void s1_select_table(void)
{
	uint16_t st[3];

	st[0] = 1000; st[1] = 2000; st[2] = 3000;
	CHECK(dyn_adc_state_select(st, DYN_ADC_STATE_UNDECIDED) == DYN_ADC_STATE_C_HIGH,
		"S1a: phase C strictly highest -> C_HIGH");

	st[0] = 3000; st[1] = 1000; st[2] = 2000;
	CHECK(dyn_adc_state_select(st, DYN_ADC_STATE_UNDECIDED) == DYN_ADC_STATE_A_HIGH,
		"S1b: phase A strictly highest -> A_HIGH");

	st[0] = 2000; st[1] = 3000; st[2] = 1000;
	CHECK(dyn_adc_state_select(st, DYN_ADC_STATE_UNDECIDED) == DYN_ADC_STATE_B_HIGH,
		"S1c: phase B strictly highest -> B_HIGH");

	st[0] = 3000; st[1] = 3000; st[2] = 1000;
	CHECK(dyn_adc_state_select(st, DYN_ADC_STATE_B_HIGH) == DYN_ADC_STATE_B_HIGH,
		"S1d: two-way tie for the maximum keeps the previous state");

	st[0] = MODEL_NEUTRAL; st[1] = MODEL_NEUTRAL; st[2] = MODEL_NEUTRAL;
	CHECK(dyn_adc_state_select(st, DYN_ADC_STATE_A_HIGH) == DYN_ADC_STATE_A_HIGH,
		"S1e: neutral (all equal) keeps the previous state - dwell/cutoff behaviour unchanged");
	CHECK(dyn_adc_state_select(st, DYN_ADC_STATE_UNDECIDED) == DYN_ADC_STATE_UNDECIDED,
		"S1f: all equal from a cold start stays UNDECIDED");
}

static void s2_reconstruct_arithmetic(void)
{
	int k;
	int failures_a = 0, failures_b = 0, c_written = 0;

	for (k = -60; k <= 60; k++) {
		int16_t truth[3];
		int16_t a, b, c;
		true_currents(k, truth);

		/* A untrusted */
		a = POISON; b = truth[1]; c = truth[2];
		dyn_adc_state_reconstruct(DYN_ADC_STATE_A_HIGH, &a, &b, c);
		if (a != truth[0] || b != truth[1]) failures_a++;
		if (c != truth[2]) c_written++;

		/* B untrusted */
		a = truth[0]; b = POISON; c = truth[2];
		dyn_adc_state_reconstruct(DYN_ADC_STATE_B_HIGH, &a, &b, c);
		if (b != truth[1] || a != truth[0]) failures_b++;
		if (c != truth[2]) c_written++;

		/* C untrusted - A and B are already right and must be left alone */
		a = truth[0]; b = truth[1]; c = POISON;
		dyn_adc_state_reconstruct(DYN_ADC_STATE_C_HIGH, &a, &b, c);
		if (a != truth[0] || b != truth[1]) failures_a++;
	}

	CHECK(failures_a == 0, "S2a: A_HIGH rebuilds Ia = -Ib - Ic exactly, B untouched");
	CHECK(failures_b == 0, "S2b: B_HIGH rebuilds Ib = -Ia - Ic exactly, A untouched");
	CHECK(c_written == 0, "S2c: phase C is never written back - it is not a Clarke input");
}

/* ----------------------------------------------------------------------------- S3 / S4 / S7 */

static int sweep(int reverse, int legacy, int *crossings_out)
{
	pipeline_t p;
	uint16_t st[3], next[3];
	int deg;
	int bad = 0;
	int crossings = 0;
	int prev_top;

	duty_at(0, reverse, st);
	pipeline_init(&p, st);
	prev_top = argmax3(st);

	for (deg = 1; deg <= 360; deg++) {
		duty_at(deg, reverse, next);
		if (legacy) isr_legacy(&p, deg, next);
		else        isr_fixed(&p, deg, next);
		if (!clarke_is_true(&p, deg)) bad++;
		if (argmax3(next) != prev_top) { crossings++; prev_top = argmax3(next); }
	}
	if (crossings_out) *crossings_out = crossings;
	return bad;
}

/* ---------------------------------------------------------------------------------- S5 / S6 */

/*
 * One named crossing, warmed up first so nothing here can be blamed on a cold start:
 *
 *   ISR 1..3  duty set `a` (`from_high` on top)   - both orders settle
 *   ISR 4     FOC switches the output to `b`      - this ISR's own sample is still under `a`
 *   ISR 5     FIRST period physically sampled under `b` - THE CROSSING
 *   ISR 6..7  settled on `b`
 *
 * Returns the number of periods from ISR 4 onwards whose Clarke input was not the truth, and
 * reports the first such period in *first_bad (0 = none). Warm-up periods 1..3 are excluded on
 * purpose: the legacy order also fails ISR 1 simply because it has no verdict yet, and that is
 * not the defect under test.
 */
#define CROSS_WARMUP 3
#define CROSS_STEPS  7

static int crossing(int from_high, int to_high, int legacy, int *first_bad)
{
	static const uint16_t high = 3400, mid = 2000, low = 900;
	uint16_t a[3], b[3];
	pipeline_t p;
	int i, step, bad = 0;

	for (i = 0; i < 3; i++) a[i] = (i == from_high) ? high : ((i == (from_high + 1) % 3) ? mid : low);
	for (i = 0; i < 3; i++) b[i] = (i == to_high)   ? high : ((i == (to_high   + 1) % 3) ? mid : low);

	if (first_bad) *first_bad = 0;
	pipeline_init(&p, a);

	for (step = 1; step <= CROSS_STEPS; step++) {
		const uint16_t *next = (step <= CROSS_WARMUP) ? a : b;
		if (legacy) isr_legacy(&p, step, next);
		else        isr_fixed(&p, step, next);
		if (step <= CROSS_WARMUP) continue;
		if (!clarke_is_true(&p, step)) {
			bad++;
			if (first_bad && *first_bad == 0) *first_bad = step;
		}
	}
	return bad;
}

/* The first period actually sampled under the new duty set: the FOC of ISR CROSS_WARMUP+1
 * publishes it, so the sample taken at the top of the following period is the first affected. */
#define CROSS_FIRST_AFFECTED (CROSS_WARMUP + 2)

/* --------------------------------------------------------------------------------------- S8 */

static void s8_neutral(void)
{
	pipeline_t p;
	uint16_t neutral[3];
	int bad = 0, step;

	neutral[0] = MODEL_NEUTRAL; neutral[1] = MODEL_NEUTRAL; neutral[2] = MODEL_NEUTRAL;

	pipeline_init(&p, neutral);
	for (step = 1; step <= 8; step++) {
		isr_fixed(&p, step, neutral);
		if (!clarke_is_true(&p, step)) bad++;
	}
	CHECK(bad == 0, "S8: all-equal CCRs (neutral dwell / cutoff end) deliver the true Ia/Ib");
}

/* -------------------------------------------------------------------------------------- main */

int main(void)
{
	int fwd_cross = 0, rev_cross = 0;
	int fwd_fixed, rev_fixed, fwd_legacy;
	int ca, ab, bc, ca_l, ab_l, bc_l;
	int ca_step = 0, ab_step = 0, bc_step = 0, dummy = 0;

	printf("FW-120.1 reconstruction/PWM-period timing (real dyn_adc_state.c)\n");

	s1_select_table();
	s2_reconstruct_arithmetic();

	fwd_fixed = sweep(0, 0, &fwd_cross);
	CHECK(fwd_cross >= 3, "S3a: the forward sweep really does cross all three ranking boundaries");
	CHECK(fwd_fixed == 0,
		"S3b: forward 360 deg sweep, production order - the untrusted shunt never reaches Clarke");

	rev_fixed = sweep(1, 0, &rev_cross);
	CHECK(rev_cross >= 3, "S4a: the reverse sweep crosses all three boundaries too");
	CHECK(rev_fixed == 0,
		"S4b: reversed rotation, production order - same result, no direction assumption anywhere");

	ca = crossing(2, 0, 0, &dummy);
	ab = crossing(0, 1, 0, &dummy);
	bc = crossing(1, 2, 0, &dummy);
	CHECK(ca == 0, "S5a: C highest -> A highest, production order: every period uses its own PWM's pair");
	CHECK(ab == 0, "S5b: A highest -> B highest, production order: every period uses its own PWM's pair");
	CHECK(bc == 0, "S5c: B highest -> C highest, production order: every period uses its own PWM's pair");

	ca_l = crossing(2, 0, 1, &ca_step);
	ab_l = crossing(0, 1, 1, &ab_step);
	bc_l = crossing(1, 2, 1, &bc_step);
	CHECK(ca_l > 0 && ca_step == CROSS_FIRST_AFFECTED,
		"S6a: NEGATIVE CONTROL - pre-card order corrupts Clarke on the first period sampled after C -> A");
	CHECK(ab_l > 0 && ab_step == CROSS_FIRST_AFFECTED,
		"S6b: NEGATIVE CONTROL - pre-card order corrupts Clarke on the first period sampled after A -> B");
	CHECK(bc_l > 0 && bc_step == CROSS_FIRST_AFFECTED,
		"S6c: NEGATIVE CONTROL - pre-card order corrupts Clarke on the first period sampled after B -> C");
	CHECK(ca_l == 1 && ab_l == 1 && bc_l == 1,
		"S6e: exactly one period per crossing is corrupted - the verdict lags by exactly one PWM period");

	fwd_legacy = sweep(0, 1, NULL);
	CHECK(fwd_legacy > 0,
		"S6d: NEGATIVE CONTROL - the pre-card order fails the forward sweep the fix passes");
	CHECK(fwd_legacy <= fwd_cross + 1,
		"S7: those failures are confined to the crossings (plus the cold first period) - away from a boundary both orders agree");

	s8_neutral();

	printf("  forward sweep: %d ranking crossings, production bad periods = %d, pre-card bad periods = %d\n",
		fwd_cross, fwd_fixed, fwd_legacy);
	printf("  reverse sweep: %d ranking crossings, production bad periods = %d\n", rev_cross, rev_fixed);
	printf("  pre-card bad periods per crossing: C->A %d (at ISR %d), A->B %d (at ISR %d), B->C %d (at ISR %d)\n",
		ca_l, ca_step, ab_l, ab_step, bc_l, bc_step);

	if (host_test_failures == 0) {
		printf("\nAll FW-120.1 checks passed.\n");
		return 0;
	}
	printf("\n%d FW-120.1 check(s) FAILED.\n", host_test_failures);
	return 1;
}
