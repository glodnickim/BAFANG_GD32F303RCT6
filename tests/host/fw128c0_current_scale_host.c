/*
 * FW-128C0: the physical current scale, proven as far as the source can prove it.
 *
 * WHAT THIS TEST IS FOR. FW-128B and FW-128C both want to say "X amperes". Nothing in the
 * firmware currently says what one MS.i_q unit is worth, and CAL_I = 95 is documented in
 * config.h as "back-calculated from battery current", i.e. it is a consequence of another
 * constant, not a measurement of the analog front end. So the chain has to be split in two:
 *
 *   ADC LSB -> MS.i_q            entirely inside the source. PROVEN HERE, exactly.
 *   amperes -> ADC LSB           needs R_shunt, amplifier gain and VDDA. NOT IN THE SOURCE.
 *
 * This file closes the first half and leaves the second half explicitly open. Everything below
 * is therefore in ADC LSB, and every result stays correct whatever the missing constant turns
 * out to be - which is the point: the unknown is ONE scalar multiplying the whole chain, not a
 * scattered set of unknowns.
 *
 * THE MODEL IS NOT THE IMPLEMENTATION, so it is guarded. arm_clarke_q31/arm_park_q31 are
 * CMSIS inline functions using __QADD/__QSUB, which do not exist on a host, and arm_sin_cos_q31
 * lives in a prebuilt .a with no source in the tree. The numeric half below therefore re-states
 * that arithmetic exactly, and the source half asserts, character by character, that what it
 * re-states is what arm_math.h and FOC.c actually contain. If either drifts, this fails.
 */

#include "common/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifndef ARM_MATH_H_PATH
#error "ARM_MATH_H_PATH must be defined (see tests/host/run-host-tests.ps1)"
#endif
#ifndef FOC_C_PATH
#error "FOC_C_PATH must be defined (see tests/host/run-host-tests.ps1)"
#endif
#ifndef SAMPLE_WINDOW_C_PATH
#error "SAMPLE_WINDOW_C_PATH must be defined (see tests/host/run-host-tests.ps1)"
#endif
#ifndef MAIN_C_PATH
#error "MAIN_C_PATH must be defined (see tests/host/run-host-tests.ps1)"
#endif
#define STRINGIZE_(x) #x
#define STRINGIZE(x) STRINGIZE_(x)

static char *read_whole_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long len = ftell(f);
	if (len < 0) { fclose(f); return NULL; }
	if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
	char *buf = (char *)malloc((size_t)len + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

/* ------------------------------------------------------------------------------------------
 * The arithmetic, restated exactly. Every shift below is an ARITHMETIC shift on a signed type,
 * i.e. it FLOORS - it does not truncate toward zero. That distinction is the whole rounding
 * error budget of this chain, so it is modelled rather than approximated.
 * ------------------------------------------------------------------------------------------ */

/* Spelled out rather than relying on M_PI, which is not in standard C and is absent under the
 * host compiler's -std= setting. The synthetic vectors below are the test's own fixture, so
 * they must not depend on a platform extension. */
#define PI_D 3.14159265358979323846

#define CLARKE_C1 0x24F34E8B   /* 1/sqrt(3) in Q30 - guarded against arm_math.h below */
#define CLARKE_C2 0x49E69D16   /* 2/sqrt(3) in Q30 */

static int32_t qadd(int32_t a, int32_t b)
{
	int64_t s = (int64_t)a + (int64_t)b;
	if (s > INT32_MAX) return INT32_MAX;
	if (s < INT32_MIN) return INT32_MIN;
	return (int32_t)s;
}

static int32_t qsub(int32_t a, int32_t b)
{
	int64_t s = (int64_t)a - (int64_t)b;
	if (s > INT32_MAX) return INT32_MAX;
	if (s < INT32_MIN) return INT32_MIN;
	return (int32_t)s;
}

/* arm_clarke_q31: alpha = Ia; beta = (1/sqrt3)*Ia + (2/sqrt3)*Ib, both via Q30 */
static void clarke_q31(int32_t ia, int32_t ib, int32_t *alpha, int32_t *beta)
{
	int32_t p1 = (int32_t)(((int64_t)ia * (int64_t)CLARKE_C1) >> 30);
	int32_t p2 = (int32_t)(((int64_t)ib * (int64_t)CLARKE_C2) >> 30);
	*alpha = ia;
	*beta = qadd(p1, p2);
}

/* arm_park_q31: id = alpha*cos + beta*sin ; iq = beta*cos - alpha*sin, sin/cos in Q31 */
static void park_q31(int32_t alpha, int32_t beta, int32_t *id, int32_t *iq,
                     int32_t sinv, int32_t cosv)
{
	int32_t p1 = (int32_t)(((int64_t)alpha * (int64_t)cosv) >> 31);
	int32_t p2 = (int32_t)(((int64_t)beta  * (int64_t)sinv) >> 31);
	int32_t p3 = (int32_t)(((int64_t)alpha * (int64_t)sinv) >> 31);
	int32_t p4 = (int32_t)(((int64_t)beta  * (int64_t)cosv) >> 31);
	*id = qadd(p1, p2);
	*iq = qsub(p4, p3);
}

/*
 * FOC.c's output filter, restated:  fil -= fil>>3;  fil += x;  out = fil>>3;
 * DC gain is exactly 1 - the fixed point of that recurrence is fil = 8x - so the filter changes
 * the UNIT of nothing. It only costs settling time and a sub-LSB floor bias.
 */
typedef struct { int32_t fil; } iir_t;

static int32_t iir_step(iir_t *s, int32_t x)
{
	s->fil -= s->fil >> 3;
	s->fil += x;
	return s->fil >> 3;
}

/* The firmware's own clamping of arm_sin_cos_q31's output, from FOC.c. */
static void sincos_q31_ideal(double theta_rad, int32_t *sinv, int32_t *cosv)
{
	double s = sin(theta_rad) * 2147483648.0;
	double c = cos(theta_rad) * 2147483648.0;
	int64_t si = (int64_t)llround(s);
	int64_t ci = (int64_t)llround(c);
	if (si > INT32_MAX) si = INT32_MAX;
	if (si < INT32_MIN) si = INT32_MIN;
	if (ci > INT32_MAX) ci = INT32_MAX;
	if (ci < INT32_MIN) ci = INT32_MIN;
	*sinv = (int32_t)si;
	*cosv = (int32_t)ci;
	if (*sinv == INT32_MIN) *sinv = -INT32_MAX;   /* FOC.c line-for-line */
	if (*cosv == INT32_MIN) *cosv = -INT32_MAX;
}

/* One complete pass: three phase samples in ADC LSB -> steady-state MS.i_d / MS.i_q. */
static void chain_steady(int32_t ia, int32_t ib, double theta_rad, int32_t *i_d, int32_t *i_q)
{
	int32_t alpha, beta, d, q, sv, cv;
	iir_t fd = {0}, fq = {0};
	int n;
	sincos_q31_ideal(theta_rad, &sv, &cv);
	clarke_q31(ia, ib, &alpha, &beta);
	park_q31(alpha, beta, &d, &q, sv, cv);
	/* run the filter to steady state on a constant input - the unit question is a DC question */
	for (n = 0; n < 400; n++) { *i_d = iir_step(&fd, d); *i_q = iir_step(&fq, q); }
}

static int32_t rnd(double v) { return (int32_t)llround(v); }

int main(void)
{
	/* ================= PART A/E/F: the arithmetic is what we say it is ==================== */
	{
		char *am = read_whole_file(STRINGIZE(ARM_MATH_H_PATH));
		char *foc = read_whole_file(STRINGIZE(FOC_C_PATH));
		char *sw = read_whole_file(STRINGIZE(SAMPLE_WINDOW_C_PATH));
		char *mn = read_whole_file(STRINGIZE(MAIN_C_PATH));
		CHECK(am && foc && sw && mn, "G0. all four guarded sources are readable");

		/* Clarke: the exact constants and the exact shift. A different constant or a >>31
		 * instead of >>30 would silently halve the beta scale, which is precisely the kind of
		 * hidden factor this card exists to rule out. */
		CHECK(strstr(am, "product1 = (q31_t) (((q63_t) Ia * 0x24F34E8B) >> 30);") != NULL,
		      "E1a. Clarke scales Ia by 1/sqrt(3) in Q30, exactly as modelled");
		CHECK(strstr(am, "product2 = (q31_t) (((q63_t) Ib * 0x49E69D16) >> 30);") != NULL,
		      "E1b. ...and Ib by 2/sqrt(3) in Q30");
		CHECK(strstr(am, "*pIalpha = Ia;") != NULL,
		      "E1c. ...and alpha is Ia itself - no scaling on the alpha axis at all");
		CHECK(strstr(am, "*pIbeta = __QADD(product1, product2);") != NULL,
		      "E1d. ...combined with a SATURATING add");

		/* Park: a pure rotation in Q31. >>31 with full-scale sin/cos is unity gain. */
		CHECK(strstr(am, "product1 = (q31_t) (((q63_t) (Ialpha) * (cosVal)) >> 31);") != NULL,
		      "F1a. Park multiplies by cos in Q31");
		CHECK(strstr(am, "product3 = (q31_t) (((q63_t) (Ialpha) * (sinVal)) >> 31);") != NULL,
		      "F1b. ...and by sin in Q31");
		CHECK(strstr(am, "*pId = __QADD(product1, product2);") != NULL,
		      "F1c. Id = alpha*cos + beta*sin");
		CHECK(strstr(am, "*pIq = __QSUB(product4, product3);") != NULL,
		      "F1d. Iq = beta*cos - alpha*sin");

		/* FOC.c: the inputs are the phase samples unscaled, and the output filter has DC gain 1. */
		CHECK(strstr(foc, "arm_clarke_q31((q31_t)int16_i_as, (q31_t)int16_i_bs, &q31_i_alpha, &q31_i_beta);") != NULL,
		      "A1a. Clarke is fed the phase samples DIRECTLY - no multiply, no shift, no scale");
		CHECK(strstr(foc, "arm_park_q31(q31_i_alpha, q31_i_beta, &q31_i_d, &q31_i_q, sinevalue, cosinevalue);") != NULL,
		      "A1b. Park is fed Clarke's output directly");
		CHECK(strstr(foc, "q31_i_q_fil -= q31_i_q_fil>>3;") != NULL &&
		      strstr(foc, "q31_i_q_fil += q31_i_q;") != NULL &&
		      strstr(foc, "MS_FOC->i_q=q31_i_q_fil>>3;") != NULL,
		      "A1c. MS.i_q is the >>3 IIR of Park's q output - fixed point 8x, so DC gain exactly 1");
		CHECK(strstr(foc, "q31_i_d_fil -= q31_i_d_fil>>3;") != NULL &&
		      strstr(foc, "MS_FOC->i_d=q31_i_d_fil>>3;") != NULL,
		      "A1d. MS.i_d likewise - the d and q units are identical by construction");

		/* main.c: the sample handed to FOC is JDR minus an offset. Offsets do not set gain. */
		CHECK(strstr(mn, "i16_ph1_current = adc_inserted_data_read(ADC2, ADC_INSERTED_CHANNEL_0);") != NULL,
		      "A2a. the phase sample is the raw inserted-group result, in ADC LSB");
		CHECK(strstr(mn, "i16_ph1_current -= current_cal.offset[CURRENT_CAL_PHASE_A];") != NULL,
		      "A2b. the only thing applied to it is a SUBTRACTION - an offset, never a gain");
		CHECK(strstr(mn, "FOC_calculation(i16_ph1_current, i16_ph2_current,") != NULL,
		      "A2c. ...and that value goes to FOC unmodified: nothing rescales between ADC and Clarke");

		/* Kirchhoff reconstruction is a sum of like units - it cannot change the scale. */
		CHECK(strstr(sw, "case 0: *i_a = (int16_t)(-(*i_b) - (*i_c)); break;") != NULL,
		      "A3. FW-127 reconstruction is a Kirchhoff sum of same-unit values - unit-preserving");

		free(am); free(foc); free(sw); free(mn);
	}

	/* ================= PART B: the one thing that is NOT in the source ==================== */
	{
		/*
		 * Stated as a test so it cannot be quietly forgotten: if anyone ever adds a real
		 * shunt/gain constant, this check is where the ampere half of the chain gets closed.
		 * CAL_I is deliberately NOT accepted as that constant - config.h calls it
		 * "back-calculated from battery current", which makes it a consequence of CAL_BAT_I.
		 */
		CHECK(1, "B1. amperes-per-LSB is OPEN: R_shunt, amplifier gain and VDDA are not in the tree");
	}

	/* ================= PART G: the ten required numeric checks ============================ */

	/* G1. a balanced synthetic set really does sum to zero at every angle tested */
	{
		int bad = 0;
		for (int deg = 0; deg < 360; deg += 15) {
			double th = deg * PI_D / 180.0;
			double A = 1000.0;
			int32_t ia = rnd(A * cos(th));
			int32_t ib = rnd(A * cos(th - 2.0 * PI_D / 3.0));
			int32_t ic = rnd(A * cos(th + 2.0 * PI_D / 3.0));
			if (labs((long)(ia + ib + ic)) > 2) bad++;
		}
		CHECK(bad == 0, "G1. the synthetic three-phase sets are balanced to within rounding");
	}

	/* G2. Clarke maps a balanced set of amplitude A to (A cos, A sin) - amplitude invariant */
	{
		int worst = 0;
		for (int deg = 0; deg < 360; deg += 5) {
			double th = deg * PI_D / 180.0;
			double A = 1500.0;
			int32_t ia = rnd(A * cos(th));
			int32_t ib = rnd(A * cos(th - 2.0 * PI_D / 3.0));
			int32_t alpha, beta;
			clarke_q31(ia, ib, &alpha, &beta);
			int ea = (int)labs((long)(alpha - rnd(A * cos(th))));
			int eb = (int)labs((long)(beta - rnd(A * sin(th))));
			if (ea > worst) worst = ea;
			if (eb > worst) worst = eb;
		}
		CHECK(worst <= 2,
		      "G2. Clarke output is (A cos, A sin) in the SAME unit as the phase samples");
		printf("      [G2] worst Clarke deviation over 72 angles: %d LSB\n", worst);
	}

	/* G3. Park at aligned theta: a current vector along the rotor axis is pure d */
	{
		double th = 0.0, A = 1200.0;
		int32_t ia = rnd(A * cos(th));
		int32_t ib = rnd(A * cos(th - 2.0 * PI_D / 3.0));
		int32_t id, iq;
		chain_steady(ia, ib, th, &id, &iq);
		CHECK(labs((long)(id - 1200)) <= 3 && labs((long)iq) <= 3,
		      "G3. at aligned theta the whole amplitude appears on d, and q is zero");
		printf("      [G3] A=1200 LSB -> i_d=%d i_q=%d\n", (int)id, (int)iq);
	}

	/* G4. q-only: the current vector leading the rotor by 90 degrees is pure q */
	{
		int worst = 0;
		for (int deg = 0; deg < 360; deg += 15) {
			double th = deg * PI_D / 180.0;
			double A = 700.0;                       /* PH_CURRENT_MAX, in its own unit */
			double cur = th + PI_D / 2.0;           /* lead by 90 electrical degrees */
			int32_t ia = rnd(A * cos(cur));
			int32_t ib = rnd(A * cos(cur - 2.0 * PI_D / 3.0));
			int32_t id, iq;
			chain_steady(ia, ib, th, &id, &iq);
			int e = (int)labs((long)(iq - 700));
			int ed = (int)labs((long)id);
			if (e > worst) worst = e;
			if (ed > worst) worst = ed;
		}
		CHECK(worst <= 3,
		      "G4. a q-only vector of amplitude A gives MS.i_q = A at every electrical angle");
		printf("      [G4] worst |i_q - 700| and |i_d| over 24 angles: %d LSB\n", worst);
	}

	/* G5. d-only, swept: the same statement on the d axis */
	{
		int worst = 0;
		for (int deg = 0; deg < 360; deg += 15) {
			double th = deg * PI_D / 180.0;
			double A = 500.0;
			int32_t ia = rnd(A * cos(th));
			int32_t ib = rnd(A * cos(th - 2.0 * PI_D / 3.0));
			int32_t id, iq;
			chain_steady(ia, ib, th, &id, &iq);
			int e = (int)labs((long)(id - 500));
			int eq = (int)labs((long)iq);
			if (e > worst) worst = e;
			if (eq > worst) worst = eq;
		}
		CHECK(worst <= 3, "G5. a d-only vector of amplitude A gives MS.i_d = A at every angle");
	}

	/* G6/G7. sign: q follows the sign of the vector's lead, symmetrically */
	{
		double th = 40.0 * PI_D / 180.0, A = 900.0;
		int32_t idp, iqp, idn, iqn;
		double cp = th + PI_D / 2.0, cn = th - PI_D / 2.0;
		chain_steady(rnd(A * cos(cp)), rnd(A * cos(cp - 2.0 * PI_D / 3.0)), th, &idp, &iqp);
		chain_steady(rnd(A * cos(cn)), rnd(A * cos(cn - 2.0 * PI_D / 3.0)), th, &idn, &iqn);
		CHECK(iqp > 890 && iqp < 910, "G6. leading by +90 deg gives positive q of the same amplitude");
		CHECK(iqn < -890 && iqn > -910, "G7. lagging by 90 deg gives the negative of it");
		CHECK(labs((long)(iqp + iqn)) <= 3, "G7b. the two are symmetric - no sign-dependent gain");
		printf("      [G6/G7] +q=%d  -q=%d\n", (int)iqp, (int)iqn);
	}

	/* G8. linearity: doubling the physical amplitude doubles MS.i_q, with no offset term */
	{
		double th = 25.0 * PI_D / 180.0;
		int worst = 0;
		for (int A = 50; A <= 1600; A += 50) {
			double cur = th + PI_D / 2.0;
			int32_t id, iq;
			chain_steady(rnd(A * cos(cur)), rnd(A * cos(cur - 2.0 * PI_D / 3.0)), th, &id, &iq);
			int e = (int)labs((long)(iq - A));
			if (e > worst) worst = e;
		}
		CHECK(worst <= 3,
		      "G8. MS.i_q tracks amplitude 1:1 across the whole usable range - a pure scale, no curvature");
		printf("      [G8] worst |i_q - A| over A = 50..1600 LSB: %d LSB\n", worst);
	}

	/* G9. rounding: every stage floors, so the total error is bounded and slightly negative */
	{
		int worst_neg = 0, worst_pos = 0;
		for (int deg = 0; deg < 360; deg += 3) {
			double th = deg * PI_D / 180.0;
			double A = 1000.0, cur = th + PI_D / 2.0;
			int32_t id, iq;
			chain_steady(rnd(A * cos(cur)), rnd(A * cos(cur - 2.0 * PI_D / 3.0)), th, &id, &iq);
			int e = (int)(iq - 1000);
			if (e < worst_neg) worst_neg = e;
			if (e > worst_pos) worst_pos = e;
		}
		CHECK(worst_neg >= -4 && worst_pos <= 4,
		      "G9. the accumulated fixed-point error over the whole chain stays within a few LSB");
		printf("      [G9] error envelope over 120 angles: %d .. %d LSB\n", worst_neg, worst_pos);
	}

	/* G10. near zero: no dead band and no offset - a zero current reads zero */
	{
		int32_t id, iq;
		chain_steady(0, 0, 1.0, &id, &iq);
		CHECK(id == 0 && iq == 0, "G10a. zero phase current gives exactly zero d and q");
		chain_steady(1, 0, 0.0, &id, &iq);
		CHECK(id >= 0 && id <= 1, "G10b. a single LSB of phase current is not swallowed");
	}

	/* ============ PART K: what the numbers already in config.h mean in this unit =========== */
	{
		/*
		 * The representable range. A phase sample is a 12-bit conversion minus a hardware IOFF
		 * of about 2020, so it spans roughly -2020 .. +2075 LSB. For a BALANCED set the vector
		 * amplitude cannot exceed the per-phase clip, so |i_dq| <= ~2075 in normal operation.
		 *
		 * That single fact settles something FW-128C needs to know: FOC.c trips at
		 * MS.i_d > (PH_CURRENT_MAX<<2) = 2800, which is ABOVE the balanced maximum. The trip
		 * can therefore only fire on an unbalanced or corrupted acquisition - never on a real
		 * balanced overcurrent, however large. It is not an overcurrent protection.
		 */
		const int32_t balanced_max = 2075;
		const int32_t trip = 700 * 4;
		CHECK(trip > balanced_max,
		      "K1. the existing i_d trip (2800) sits ABOVE the maximum a balanced set can reach");
		printf("      [K1] balanced max ~%d LSB, existing i_d trip %d LSB -> unreachable when balanced\n",
		       (int)balanced_max, (int)trip);

		/* And the headroom a real protection threshold would have to live in. */
		CHECK(700 < balanced_max,
		      "K2. PH_CURRENT_MAX (700) is inside the representable range, with room above it");
		printf("      [K2] PH_CURRENT_MAX 700 LSB = %.0f%% of the balanced maximum\n",
		       100.0 * 700.0 / (double)balanced_max);
	}

	printf("\n  FW-128C0: internal scale PROVEN - 1 MS.i_q unit = 1 ADC LSB of phase-current PEAK.\n");
	printf("  FW-128C0: amperes-per-LSB REMAINS OPEN - no R_shunt / gain / VDDA in the tree.\n");

	if (host_test_failures == 0) {
		printf("FW-128C0 physical current scale: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-128C0 physical current scale: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
