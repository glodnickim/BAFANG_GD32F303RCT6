/*
 * FW-127C wiring guard: a STRUCTURAL check on src/main.c's SOURCE TEXT.
 *
 * WHY STRUCTURAL. The invariant this card exists for is an ORDERING property inside an ISR that
 * cannot be linked on a host - main.c is the ARM entry point, wired to GD32 registers and
 * interrupt vectors throughout. The modules' own behaviour is covered head-on by
 * fw127c_sample_window_host.c, fw127b_sample_ctx_host.c and fw127a_pwm_geometry_host.c, which
 * link the real code. What is left, and what only source text can prove, is WHERE things happen.
 *
 * This guard replaces the FW-120.1 ISR-order guard. That one protected the same class of
 * property under the old architecture; the property survives, the mechanism changed.
 *
 * WHAT THIS PROVES:
 *   C1  the sampling decision happens AFTER SVPWM, not before it - the defect this card removes
 *   C2  the decision is fed the APPLIED geometry, never the request
 *   W1  the context is consumed once, before anything interprets the sample
 *   W2  the reconstruction uses the consumed context's sector, not a freshly derived one
 *   W3  the publish and the compare writes happen together
 *   X1  the old architecture is gone from this file
 */

#include "common/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAIN_C_PATH
#error "MAIN_C_PATH must be defined (see tests/host/run-host-tests.ps1)"
#endif
#define STRINGIZE_(x) #x
#define STRINGIZE(x) STRINGIZE_(x)

static char *read_whole_file(const char *path, long *out_len)
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
	if (out_len) *out_len = (long)got;
	return buf;
}

/* Comments become blanks, same length; string/char literals kept, so a commented-out call simply
 * does not exist any more and every offset still maps onto the original for ordering checks. */
static char *strip_comments(const char *text, long len)
{
	char *out = (char *)malloc((size_t)len + 1);
	if (!out) return NULL;
	long i = 0;
	while (i < len) {
		if (text[i] == '/' && i + 1 < len && text[i + 1] == '/') {
			while (i < len && text[i] != '\n') { out[i] = ' '; i++; }
			continue;
		}
		if (text[i] == '/' && i + 1 < len && text[i + 1] == '*') {
			out[i] = ' '; out[i + 1] = ' '; i += 2;
			while (i < len && !(text[i] == '*' && i + 1 < len && text[i + 1] == '/')) {
				out[i] = (text[i] == '\n') ? '\n' : ' ';
				i++;
			}
			if (i < len) { out[i] = ' '; i++; }
			if (i < len) { out[i] = ' '; i++; }
			continue;
		}
		if (text[i] == '"' || text[i] == '\'') {
			char q = text[i];
			out[i] = text[i]; i++;
			while (i < len && text[i] != q) {
				out[i] = text[i];
				if (text[i] == '\\' && i + 1 < len) { i++; out[i] = text[i]; }
				i++;
			}
			if (i < len) { out[i] = text[i]; i++; }
			continue;
		}
		out[i] = text[i];
		i++;
	}
	out[len] = '\0';
	return out;
}

static int count_occurrences(const char *hay, const char *needle)
{
	int n = 0;
	const char *p = hay;
	size_t l = strlen(needle);
	while ((p = strstr(p, needle)) != NULL) { n++; p += l; }
	return n;
}

int main(void)
{
	long len = 0;
	char *raw = read_whole_file(STRINGIZE(MAIN_C_PATH), &len);
	if (!raw) { printf("  FAIL  cannot read %s\n", STRINGIZE(MAIN_C_PATH)); return 1; }
	char *m = strip_comments(raw, len);
	if (!m) { printf("  FAIL  out of memory\n"); return 1; }

	const char *consume  = strstr(m, "current_sample_ctx_consume()");
	const char *recon    = strstr(m, "sample_window_reconstruct(sample_ctx->sector");
	const char *foc      = strstr(m, "FOC_calculation(");
	const char *clamp    = strstr(m, "pwm_geometry_apply(switchtime, pwm_applied");
	const char *decide   = strstr(m, "sample_window_decide(pwm_applied");
	const char *ccr0     = strstr(m, "TIMER_CH_0,pwm_applied[0]");
	const char *ch3      = strstr(m, "TIMER_CH_3,win.trigger_ccr");
	const char *publish  = strstr(m, "current_sample_ctx_publish(win.sector");

	CHECK(consume != NULL, "W1a. the transaction context is consumed in the ISR");
	CHECK(recon   != NULL, "W2a. reconstruction is driven by the consumed context");
	CHECK(clamp   != NULL, "A1.  the applied geometry is produced by the clamp");
	CHECK(decide  != NULL, "C2a. the sampling decision exists");
	CHECK(ch3     != NULL, "C6a. CH3 is programmed from the decision's own trigger");
	CHECK(publish != NULL, "W3a. the transaction is published");
	if (!consume || !recon || !foc || !clamp || !decide || !ccr0 || !ch3 || !publish) {
		printf("FW-127C wiring guard: %d CHECK(S) FAILED\n", host_test_failures + 1);
		return 1;
	}

	/* --- C1: THE defect this card removes. The decision must follow SVPWM. --------------- */
	CHECK(foc < clamp, "C1a. FOC_calculation() runs before the clamp - it produces the request");
	CHECK(clamp < decide, "C1b. the clamp runs before the sampling decision");
	CHECK(foc < decide,
	      "C1c. the sampling decision happens AFTER SVPWM - it describes geometry that exists");

	/* --- W1/W2: the sample is interpreted with its own context, before FOC overwrites ------ */
	CHECK(consume < recon, "W1b. the context is consumed before the sample is interpreted");
	CHECK(recon < foc, "W2b. reconstruction happens before FOC_calculation() replaces the geometry");
	CHECK(count_occurrences(m, "current_sample_ctx_consume()") == 1,
	      "W1c. the context is consumed in exactly one place - one conversion, one owner");

	/* --- W3: publish and compare writes belong together ------------------------------------ */
	CHECK(decide < ccr0, "W3b. the decision precedes the compare writes it describes");
	CHECK(ccr0 < ch3, "W3c. the phase compares and CH3 are written in the same block");
	CHECK(ch3 < publish, "W3d. the context is published with the geometry it describes");
	CHECK(count_occurrences(m, "current_sample_ctx_publish(") == 1,
	      "W3e. exactly one publish site - so one publish really is one transaction");
	CHECK(count_occurrences(m, "sample_window_decide(") == 1,
	      "W3f. exactly one sampling decision per ISR, not two disagreeing ones");

	/* --- C2: the decision uses the APPLIED geometry, never the request ---------------------- */
	CHECK(strstr(m, "sample_window_decide(switchtime") == NULL,
	      "C2b. the decision is never fed the requested geometry");
	CHECK(count_occurrences(m, "TIMER_CH_0,switchtime[0]") == 0,
	      "C2c. no compare is ever written from the request");

	/* --- X1: the old architecture is gone from this file ------------------------------------ */
	{
		static const char *banned[] = {
			"dyn_adc_trigger_update",   /* derived the N+1 trigger from geometry N        */
			"dyn_adc_state_select",     /* the second, historyless decision               */
			"dyn_adc_state_reconstruct",/* ownership re-derived instead of carried         */
			"dyn_adc_state.h",
			"char_dyn_adc_state_old",
			"DYNAMIC_ADC_THRESHOLD",    /* the threshold equal to ARR - see the pre-audit  */
		};
		for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
			char label[160];
			snprintf(label, sizeof(label), "X1. '%s' no longer appears in main.c", banned[i]);
			CHECK(strstr(m, banned[i]) == NULL, label);
		}
	}

	/* --- the context chain is reset at the lifecycle boundary -------------------------------- */
	{
		const char *reset = strstr(m, "current_sample_ctx_reset()");
		const char *moe = strstr(m, "timer_primary_output_config(TIMER0,ENABLE)");
		CHECK(reset != NULL, "R1. the transaction chain is reset at a start");
		CHECK(reset && moe && reset < moe,
		      "R2. ...before the bridge is enabled, so no stale context can be consumed");
	}

	if (host_test_failures == 0) {
		printf("FW-127C wiring guard: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-127C wiring guard: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
