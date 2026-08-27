/*
 * FW-127A: the guarded array was renamed switchtime[] -> pwm_applied[]. The property is
 * unchanged and strictly stronger: pwm_applied[] is what TIMER0 actually received, whereas
 * switchtime[] is now only the (possibly illegal) request SVPWM made.
 *
 * FW-120.1 wiring guard: source-text checks on src/main.c, same methodology as
 * fw119_current_cal_wiring_host.c, step2a_neutral_dwell_wiring_host.c and
 * main_startup_wiring_host.c. main.c is the ARM entry point, wired straight to GD32 registers,
 * so it cannot be linked here - the executable half of FW-120.1 is the module, covered by
 * fw120_1_reconstruction_timing_host.c.
 *
 * The module test can prove that a given ORDER is right. Only this guard can prove that
 * src/main.c actually uses that order, and it is the ordering that was the whole defect.
 *
 * WHAT THIS PROVES (W1-W7):
 *   W1: the old inline reconstruction switch is gone - main.c no longer open-codes
 *       "i16_ph1_current = -i16_ph2_current-i16_ph3_current" anywhere.
 *   W2: the pair is selected exactly once, from pwm_applied[], and reconstruction is applied
 *       exactly once, to the ISR's own phase-current variables.
 *   W3: THE CARD. Inside ADC0_1_IRQHandler the order is
 *          JDR read -> FW-118/119 offset subtraction -> select -> reconstruct -> FOC_calculation
 *       so the verdict is taken from the APPLIED geometry that shaped the sampled period, before
 *       this ISR's FOC overwrites that array.
 *   W4: no second selection happens after FOC_calculation - nothing can re-stamp
 *       MS.char_dyn_adc_state with the period that has not been sampled yet.
 *   W5: MS.char_dyn_adc_state is assigned in exactly one place in the whole file.
 *   W6: dyn_adc_trigger_update() - the forward-looking CH3 half of the old dyn_adc_state() -
 *       still exists, is still called before FOC_calculation, and does NOT touch
 *       MS.char_dyn_adc_state. The two jobs stay separated.
 *   W7: Clarke's inputs are unchanged - FOC_calculation is still handed i16_ph1_current and
 *       i16_ph2_current, so this card changed ordering only, never the reconstruction math.
 *
 * WHAT THIS DOES NOT PROVE: anything about hardware timing. That the CCRs written by ISR n-1
 * are the ones live at the counter top of period n follows from the timer configuration
 * (center-aligned, output shadow DISABLED), not from this text check.
 */

#include "../common/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)

#ifndef MAIN_C_PATH
#error "MAIN_C_PATH must be defined (by the build script) to the path of src/main.c"
#endif

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

/*
 * Blank out comments and string bodies so a check can never be satisfied by prose - this card
 * writes a lot of explanatory comment, and every one of them names the symbols being checked.
 * Same sanitizer the other wiring guards use; kept local because each harness is standalone.
 */
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
			char quote = text[i];
			out[i] = text[i]; i++;
			while (i < len && text[i] != quote) {
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
	size_t nlen = strlen(needle);
	while ((p = strstr(p, needle)) != NULL) { n++; p += nlen; }
	return n;
}

int main(void)
{
	const char *path = STRINGIZE(MAIN_C_PATH);
	printf("FW-120.1 ISR order wiring guard (source-text check)\n");
	printf("  MAIN_C_PATH = %s\n", path);

	long len = 0;
	char *raw = read_whole_file(path, &len);
	CHECK(raw != NULL, "setup: src/main.c was readable");
	if (!raw) {
		printf("\n1 FW-120.1 check(s) FAILED (could not read file).\n");
		return 1;
	}

	char *clean = strip_comments(raw, len);
	CHECK(clean != NULL, "setup: comment/string-aware sanitization succeeded");
	free(raw);
	if (!clean) {
		printf("\n1 FW-120.1 check(s) FAILED (sanitization failed).\n");
		return 1;
	}

	/* --- W1: the open-coded reconstruction is gone --- */
	CHECK(strstr(clean, "-i16_ph2_current-i16_ph3_current") == NULL &&
	      strstr(clean, "-i16_ph1_current-i16_ph3_current") == NULL,
		"W1a: main.c no longer open-codes the sum-rule reconstruction");
	CHECK(strstr(clean, "switch (MS.char_dyn_adc_state)") == NULL &&
	      strstr(clean, "switch(MS.char_dyn_adc_state)") == NULL,
		"W1b: the inline switch on MS.char_dyn_adc_state is gone");

	/* --- W2: one selection, one reconstruction, on the ISR's own variables --- */
	const char *select_call = strstr(clean, "dyn_adc_state_select(pwm_applied, MS.char_dyn_adc_state)");
	CHECK(select_call != NULL,
		"W2a: the pair is selected from pwm_applied[] with the previous state as fallback");
	CHECK(count_occurrences(clean, "dyn_adc_state_select(pwm_applied, MS.char_dyn_adc_state)") == 1,
		"W2b: that selection happens in exactly one place");

	const char *recon_call = strstr(clean, "dyn_adc_state_reconstruct(MS.char_dyn_adc_state");
	CHECK(recon_call != NULL, "W2c: reconstruction is driven by MS.char_dyn_adc_state");
	CHECK(count_occurrences(clean, "dyn_adc_state_reconstruct(") == 1,
		"W2d: reconstruction is applied exactly once per ISR");
	CHECK(recon_call != NULL &&
	      strstr(recon_call, "&i16_ph1_current, &i16_ph2_current, i16_ph3_current") != NULL,
		"W2e: it is applied to the ISR's own A/B by reference and C by value");

	/* --- W3: the order inside ADC0_1_IRQHandler --- THE CARD --- */
	const char *isr = strstr(clean, "void ADC0_1_IRQHandler(void)");
	CHECK(isr != NULL, "W3a: ADC0_1_IRQHandler found");

	const char *jdr_read = isr ? strstr(isr, "i16_ph1_current = adc_inserted_data_read") : NULL;
	const char *offsets  = isr ? strstr(isr, "if(current_cal.valid)") : NULL;
	const char *foc_call = isr ? strstr(isr, "FOC_calculation(i16_ph1_current, i16_ph2_current") : NULL;

	CHECK(jdr_read != NULL, "W3b: the JDR reads are inside the ISR");
	CHECK(offsets != NULL, "W3c: the FW-118/119 offset subtraction is inside the ISR");
	CHECK(foc_call != NULL, "W3d: FOC_calculation is inside the ISR");

	if (jdr_read && offsets && select_call && recon_call && foc_call) {
		CHECK(jdr_read < offsets,
			"W3e: JDR are read before the offsets are subtracted");
		CHECK(offsets < select_call,
			"W3f: offsets are subtracted before reconstruction - the sum rule needs a true zero");
		CHECK(select_call < recon_call,
			"W3g: the pair is selected before it is used");
		CHECK(recon_call < foc_call,
			"W3h: reconstruction happens before Clarke sees the currents");
		CHECK(select_call < foc_call,
			"W3i: THE FIX - the pair is selected BEFORE this ISR's FOC_calculation overwrites "
			"pwm_applied[], so the verdict describes the period the samples came from");
	}

	/*
	 * --- W4: nothing re-decides after FOC, anywhere in the REST OF THE ISR ---
	 * Bounded at the handler's closing brace (the first line-initial brace after it): the
	 * dyn_adc_trigger_update() DEFINITION further down the file legitimately calls the
	 * selector, and searching to end-of-file would trip over it.
	 */
	const char *isr_end = isr ? strstr(isr, "\n}") : NULL;
	CHECK(isr_end != NULL, "W4a: the handler's closing brace was located");
	if (foc_call && isr_end) {
		const char *late_select = strstr(foc_call, "dyn_adc_state_select");
		const char *late_recon  = strstr(foc_call, "dyn_adc_state_reconstruct");
		CHECK(late_select == NULL || late_select > isr_end,
			"W4b: no selection after FOC_calculation - the next period's ranking never stamps this sample");
		CHECK(late_recon == NULL || late_recon > isr_end,
			"W4c: no reconstruction after FOC_calculation");
	}

	/* --- W5: one owner of the state variable --- */
	CHECK(count_occurrences(clean, "MS.char_dyn_adc_state =") +
	      count_occurrences(clean, "MS.char_dyn_adc_state=") == 1,
		"W5: MS.char_dyn_adc_state is assigned in exactly one place in the whole file");

	/* --- W6: the CH3 trigger half stayed separate --- */
	const char *trig_call = isr ? strstr(isr, "dyn_adc_trigger_update();") : NULL;
	CHECK(trig_call != NULL, "W6a: dyn_adc_trigger_update() is still called from the ISR");
	if (trig_call && foc_call) {
		CHECK(trig_call < foc_call,
			"W6b: CH3 is still programmed before FOC_calculation, exactly where it always was");
	}
	if (recon_call && trig_call) {
		CHECK(recon_call < trig_call,
			"W6c: the sample in hand is reconstructed before the next acquisition is armed");
	}

	const char *trig_def = strstr(clean, "void dyn_adc_trigger_update(void){");
	CHECK(trig_def != NULL, "W6d: dyn_adc_trigger_update() is defined");
	if (trig_def) {
		const char *body_end = strstr(trig_def, "\n}");
		size_t body_len = body_end ? (size_t)(body_end - trig_def) : strlen(trig_def);
		char *body = (char *)malloc(body_len + 1);
		if (body) {
			memcpy(body, trig_def, body_len);
			body[body_len] = '\0';
			CHECK(strstr(body, "MS.char_dyn_adc_state") == NULL,
				"W6e: the trigger function never writes the reconstruction state - the two jobs stay apart");
			CHECK(strstr(body, "TIMER_CH_3") != NULL,
				"W6f: ... and it still does its own job, programming CH3");
			free(body);
		}
	}
	CHECK(strstr(clean, "dyn_adc_state(q31_rotorposition_absolute)") == NULL,
		"W6g: the old two-jobs-one-name dyn_adc_state() call is gone");

	/* --- W7: the reconstruction math and Clarke's inputs are untouched --- */
	CHECK(foc_call != NULL,
		"W7: FOC_calculation is still fed i16_ph1_current / i16_ph2_current - ordering changed, math did not");

	free(clean);

	if (host_test_failures == 0) {
		printf("\nAll FW-120.1 wiring checks passed.\n");
		return 0;
	}
	printf("\n%d FW-120.1 wiring check(s) FAILED.\n", host_test_failures);
	return 1;
}
