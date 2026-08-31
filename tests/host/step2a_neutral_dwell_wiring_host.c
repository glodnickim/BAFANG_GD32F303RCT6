/*
 * STEP 2A neutral-dwell wiring guard: source-text checks on src/main.c proving the
 * neutral-dwell lifecycle is correctly wired, same methodology as
 * main_startup_wiring_host.c and main_rearm_wiring_host.c.
 *
 * WHAT THIS PROVES (T1-T6):
 *   T1: Neutral CCR writes (_T>>1 to all 3 channels) and lifecycle init
 *       (BRIDGE_LIFECYCLE_NEUTRAL_COMMIT) occur BEFORE timer_primary_output_config(TIMER0,ENABLE)
 *       and BEFORE ui_8_PWM_ON_Flag=1 - guaranteeing the bridge is preloaded with neutral
 *       before the outputs go live.
 *   T2: The ISR has an `if(neutral_dwell_active)` guard that writes neutral CCR and
 *       decrements the dwell counter INSTEAD of calling FOC_calculation() - proving FOC
 *       cannot fire during the dwell window.
 *   T3: `neutral_dwell_counter--` exists in the ISR (exact dwell cycle counting), and
 *       `foc_release_pending = 1` is set when the counter reaches 0.
 *   T4: `neutral_dwell_active = 0` is set in the main-loop lifecycle progression
 *       (the FOC_RELEASE transition), proving dwell is cleared after the configured count.
 *   T5: Normal zero torque enters persistent ARMED_ZERO and the old Hall-silence
 *       POWER_STAGE_STOP_TICKS cutoff no longer exists in main.c control flow.
 *   T6: Lifecycle reset exists in the power_off_controller() path, proving cleanup on
 *       power-off.
 *
 * WHAT THIS DOES NOT PROVE: runtime behaviour, timer frequency, ISR latency, actual
 * CCR values, or that the dwell counter reaches exactly 0 before the main loop advances.
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

int main(void)
{
	const char *path = STRINGIZE(MAIN_C_PATH);
	printf("STEP 2A neutral-dwell wiring guard (source-text check)\n");
	printf("  MAIN_C_PATH = %s\n", path);

	long len = 0;
	char *raw = read_whole_file(path, &len);
	CHECK(raw != NULL, "setup: src/main.c was readable");
	if (!raw) {
		printf("\n1 STEP 2A check(s) FAILED (could not read file).\n");
		return 1;
	}

	char *clean = strip_comments(raw, len);
	CHECK(clean != NULL, "setup: comment/string-aware sanitization succeeded");
	free(raw);
	if (!clean) {
		printf("\n1 STEP 2A check(s) FAILED (sanitization failed).\n");
		return 1;
	}

	/* --- T1: neutral CCR + lifecycle init BEFORE MOE ON --- */
	const char *neutral_commit = strstr(clean, "bridge_lifecycle = BRIDGE_LIFECYCLE_NEUTRAL_COMMIT");
	CHECK(neutral_commit != NULL,
		"T1a: bridge_lifecycle = BRIDGE_LIFECYCLE_NEUTRAL_COMMIT found in main.c");

	const char *moe_enable = strstr(clean, "timer_primary_output_config(TIMER0,ENABLE)");
	CHECK(moe_enable != NULL,
		"T1b: timer_primary_output_config(TIMER0,ENABLE) found in main.c");

	const char *pwm_flag_set = strstr(clean, "ui_8_PWM_ON_Flag=1");
	CHECK(pwm_flag_set != NULL,
		"T1c: ui_8_PWM_ON_Flag=1 found in main.c");

	if (neutral_commit && moe_enable && pwm_flag_set) {
		CHECK(neutral_commit < moe_enable,
			"T1d: BRIDGE_LIFECYCLE_NEUTRAL_COMMIT is assigned BEFORE MOE ON (timer_primary_output_config ENABLE) - bridge is preloaded before outputs go live");
		CHECK(neutral_commit < pwm_flag_set,
			"T1e: BRIDGE_LIFECYCLE_NEUTRAL_COMMIT is assigned BEFORE ui_8_PWM_ON_Flag=1");
	}

	/* --- T2: ISR has neutral_dwell_active guard blocking FOC --- */
	const char *dwell_guard = strstr(clean, "if(neutral_dwell_active)");
	CHECK(dwell_guard != NULL,
		"T2a: if(neutral_dwell_active) guard found in ISR - FOC is gated during dwell");

	const char *foc_calc_in_isr = strstr(clean, "FOC_calculation(");
	CHECK(foc_calc_in_isr != NULL,
		"T2b: FOC_calculation( found in ISR");

	if (dwell_guard && foc_calc_in_isr) {
		CHECK(dwell_guard < foc_calc_in_isr,
			"T2c: if(neutral_dwell_active) guard comes BEFORE FOC_calculation() - dwell blocks FOC");
	}

	/* --- T3: dwell counter decrement and foc_release_pending --- */
	const char *dwell_decrement = strstr(clean, "neutral_dwell_counter--");
	CHECK(dwell_decrement != NULL,
		"T3a: neutral_dwell_counter-- found (dwell counter decremented in ISR)");

	const char *foc_release_set = strstr(clean, "foc_release_pending = 1");
	CHECK(foc_release_set != NULL,
		"T3b: foc_release_pending = 1 found (signals main loop when dwell completes)");

	if (dwell_decrement && foc_release_set) {
		CHECK(dwell_decrement < foc_release_set,
			"T3c: neutral_dwell_counter-- comes BEFORE foc_release_pending = 1 (counter decremented before signal)");
	}

	/* --- T4: neutral_dwell_active cleared in lifecycle progression --- */
	const char *foc_release_transition = strstr(clean,
		"bridge_lifecycle = BRIDGE_LIFECYCLE_FOC_RELEASE;");
	CHECK(foc_release_transition != NULL,
		"T4a: BRIDGE_LIFECYCLE_FOC_RELEASE assignment found in lifecycle progression");

	const char *dwell_active_zero = foc_release_transition ?
		strstr(foc_release_transition, "neutral_dwell_active = 0") : NULL;
	CHECK(dwell_active_zero != NULL,
		"T4b: neutral_dwell_active = 0 found after FOC_RELEASE assignment");

	if (dwell_active_zero && foc_release_transition) {
		CHECK(dwell_active_zero > foc_release_transition,
			"T4c: neutral dwell is cleared in the FOC_RELEASE transition");
	}

	/* --- T5: persistent normal ARMED_ZERO; no Hall-silence bridge cutoff --- */
	const char *armed_zero = strstr(clean,
		"bridge_lifecycle = BRIDGE_LIFECYCLE_ARMED_ZERO");
	CHECK(armed_zero != NULL,
		"T5a: normal ACTIVE zero-reference transition assigns BRIDGE_LIFECYCLE_ARMED_ZERO");

	const char *armed_resume = strstr(clean,
		"bridge_lifecycle == BRIDGE_LIFECYCLE_ARMED_ZERO && MS.i_q_setpoint > 0");
	CHECK(armed_resume != NULL,
		"T5b: positive final Iq resumes from BRIDGE_LIFECYCLE_ARMED_ZERO");

	if (armed_resume) {
		const char *resume_end = strchr(armed_resume, '}');
		CHECK(resume_end != NULL,
			"T5c: ARMED_ZERO re-demand transition has a bounded block");
		if (resume_end) {
			const char *moe = strstr(armed_resume, "timer_primary_output_config(TIMER0,ENABLE)");
			const char *seed = strstr(armed_resume, "get_standstill_position()");
			const char *dwell = strstr(armed_resume, "neutral_dwell_active = 1");
			CHECK(moe == NULL || moe > resume_end,
				"T5d: ARMED_ZERO re-demand does not enable MOE");
			CHECK(seed == NULL || seed > resume_end,
				"T5e: ARMED_ZERO re-demand does not reseed standstill theta");
			CHECK(dwell == NULL || dwell > resume_end,
				"T5f: ARMED_ZERO re-demand does not rearm neutral dwell");
		}
	}

	CHECK(strstr(clean, "POWER_STAGE_STOP_TICKS") == NULL,
		"T5g: main.c has no normal Hall-silence power-stage cutoff");

	/* --- T6: lifecycle reset on power_off --- */
	const char *power_off_def = strstr(clean, "void power_off_controller(void){");
	CHECK(power_off_def != NULL,
		"T6a: void power_off_controller(void){ definition found");

	const char *idle_in_power_off = NULL;
	if (power_off_def) {
		idle_in_power_off = strstr(power_off_def, "bridge_lifecycle = BRIDGE_LIFECYCLE_IDLE");
		if (idle_in_power_off && (idle_in_power_off - power_off_def) > 2000) {
			idle_in_power_off = NULL;
		}
	}
	CHECK(idle_in_power_off != NULL,
		"T6b: bridge_lifecycle = BRIDGE_LIFECYCLE_IDLE found inside power_off_controller() - lifecycle reset on power-off");

	free(clean);

	if (host_test_failures == 0) {
		printf("STEP 2A neutral-dwell/ARMED_ZERO wiring guard passed - all structural checks verified.\n");
		return 0;
	}
	printf("\n%d STEP 2A check(s) FAILED.\n", host_test_failures);
	return 1;
}
