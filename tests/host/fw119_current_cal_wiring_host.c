/*
 * FW-119 wiring guard: source-text checks on src/main.c, same methodology as
 * step2a_neutral_dwell_wiring_host.c and main_startup_wiring_host.c. main.c is the ARM entry
 * point, wired straight to GD32 registers, so it cannot be linked here - the executable half of
 * FW-119 is the policy module, covered by fw119_current_cal_host.c.
 *
 * FW-125 changed the SAMPLING MECHANISM the retry loop drives (regular-ADC poll on reg_ADC_flag
 * -> ISR accumulator poll on phase_cal_acc.done, see inc/current_cal.h and main.c's
 * ADC0_1_IRQHandler). W2 below was updated to match; W1/W3/W4/W5/W6 check the retry/finalize/
 * bridge-gate shape, which FW-125 does not touch, so they are unchanged.
 *
 * WHAT THIS PROVES (W1-W6, covering the card's T8 and T9):
 *   W1: the calibration sequence is retry-shaped and bounded - it loops on
 *       current_cal_attempt_allowed() and is closed by exactly one current_cal_finalize().
 *   W2: the sampling wait stays bounded (CURRENT_CAL_ISR_TIMEOUT is still armed inside the loop),
 *       so a dead TIMER0/ADC chain cannot hang the start no matter how many attempts are
 *       configured.
 *   W3: the whole calibration completes BEFORE the main loop and BEFORE anything can enable the
 *       bridge - offsets can never change under a live FOC.
 *   W4: the bridge-start condition is gated by current_cal_foc_allowed(), and it is the SAME
 *       condition FW-117's lifecycle hangs off - there is no second start machine (T8).
 *   W5: FW-117's neutral-dwell sequence is still intact and still ordered correctly around that
 *       gated start: NEUTRAL_COMMIT -> MOE ON -> ui_8_PWM_ON_Flag=1 (T8).
 *   W6: the PA0 battery-current calibration is untouched and independent - it still computes
 *       bat_current_offset against CAL_BAT_I_OFFSET, and no FW-119 symbol appears inside it (T9).
 *
 * WHAT THIS DOES NOT PROVE: runtime behaviour, ADC timing, or that the provisional FW-118
 * MIN/MAX/P2P limits are correct - those remain HW_PENDING.
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
 * Blank out comments and string bodies so a check can never be satisfied by prose. Identical to
 * the sanitizer the other wiring guards use; kept local because each harness is standalone.
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
	printf("FW-119 calibration wiring guard (source-text check)\n");
	printf("  MAIN_C_PATH = %s\n", path);

	long len = 0;
	char *raw = read_whole_file(path, &len);
	CHECK(raw != NULL, "setup: src/main.c was readable");
	if (!raw) {
		printf("\n1 FW-119 check(s) FAILED (could not read file).\n");
		return 1;
	}

	char *clean = strip_comments(raw, len);
	CHECK(clean != NULL, "setup: comment/string-aware sanitization succeeded");
	free(raw);
	if (!clean) {
		printf("\n1 FW-119 check(s) FAILED (sanitization failed).\n");
		return 1;
	}

	/* --- W1: the sequence is retry-shaped and closed exactly once --- */
	const char *cal_init = strstr(clean, "current_cal_init(&current_cal");
	CHECK(cal_init != NULL, "W1a: current_cal_init(&current_cal ...) found");

	const char *cal_begin = strstr(clean, "current_cal_begin(&current_cal)");
	CHECK(cal_begin != NULL, "W1b: current_cal_begin(&current_cal) found");

	const char *retry_loop = strstr(clean, "while(current_cal_attempt_allowed(&current_cal))");
	CHECK(retry_loop != NULL,
		"W1c: the sampling runs inside while(current_cal_attempt_allowed(...)) - retries are budget-bounded");

	const char *submit = strstr(clean, "current_cal_submit(&current_cal");
	CHECK(submit != NULL, "W1d: current_cal_submit(&current_cal ...) found inside the sequence");

	const char *finalize = strstr(clean, "current_cal_finalize(&current_cal)");
	CHECK(finalize != NULL, "W1e: current_cal_finalize(&current_cal) found");
	CHECK(count_occurrences(clean, "current_cal_finalize(&current_cal)") == 1,
		"W1f: finalize is called exactly once - the policy is resolved in one place");

	if (cal_init && cal_begin && retry_loop && submit && finalize) {
		CHECK(cal_init < cal_begin && cal_begin < retry_loop,
			"W1g: init -> begin -> retry loop, in that order");
		CHECK(submit > retry_loop && submit < finalize,
			"W1h: attempts are submitted inside the loop, before the policy is resolved");
	}

	/* --- W2: the sampling wait is still bounded (FW-125: ISR-accumulator poll) --- */
	/* Leading whitespace anchors this to the bare `guard` inside the retry loop, not the
	 * `uint32_t verify_guard = CURRENT_CAL_ISR_TIMEOUT;` in the unrelated DIAG self-test below -
	 * "guard = CURRENT_CAL_ISR_TIMEOUT" alone is a substring of that line too. */
	const char *timeout_arm = NULL;
	if (retry_loop) timeout_arm = strstr(retry_loop, "        guard = CURRENT_CAL_ISR_TIMEOUT");
	CHECK(timeout_arm != NULL && (finalize == NULL || timeout_arm < finalize),
		"W2a: CURRENT_CAL_ISR_TIMEOUT is re-armed inside the retry loop - every attempt is bounded");

	const char *bounded_wait = strstr(clean, "while(!phase_cal_acc.done && --guard)");
	CHECK(bounded_wait != NULL,
		"W2b: the ADC wait is the bounded form while(!phase_cal_acc.done && --guard)");
	if (bounded_wait && retry_loop && finalize) {
		CHECK(bounded_wait > retry_loop && bounded_wait < finalize,
			"W2c: the bounded wait is the one inside the calibration sequence");
	}

	const char *acc_start = strstr(clean, "phase_cal_acc_start(CURRENT_CAL_SAMPLES)");
	CHECK(acc_start != NULL, "W2d: the calibration attempt arms phase_cal_acc_start(CURRENT_CAL_SAMPLES)");
	if (acc_start && retry_loop && bounded_wait) {
		CHECK(acc_start > retry_loop && acc_start < bounded_wait,
			"W2e: the accumulator is armed before the bounded wait, inside the retry loop");
	}

	/* --- W3: calibration is finished before the bridge can ever be enabled --- */
	const char *foc_gate = strstr(clean, "current_cal_foc_allowed(&current_cal)");
	CHECK(foc_gate != NULL, "W3a: current_cal_foc_allowed(&current_cal) found");
	if (finalize && foc_gate) {
		CHECK(finalize < foc_gate,
			"W3b: the policy is resolved BEFORE the bridge-start gate that reads it - offsets never move under a live FOC");
	}

	const char *isr_guard = strstr(clean, "if(current_cal.valid)");
	CHECK(isr_guard != NULL,
		"W3c: the ISR applies offsets behind if(current_cal.valid) - one guard for RUNTIME and LKG alike");
	if (finalize && isr_guard) {
		CHECK(finalize < isr_guard,
			"W3d: the ISR guard lives after the startup sequence (in the interrupt handler, not the boot path)");
	}
	CHECK(strstr(clean, "current_cal.valid =") == NULL &&
	      strstr(clean, "current_cal.valid=") == NULL,
		"W3e: main.c never writes current_cal.valid directly - only the policy module owns it");

	/* --- W4: the FOC gate extends the EXISTING bridge-start condition (T8) --- */
	const char *gated_start =
		strstr(clean, "if(MS.i_q_setpoint > 0 && current_cal_foc_allowed(&current_cal))");
	CHECK(gated_start != NULL,
		"W4a: the bridge-start condition is Iq>0 AND current_cal_foc_allowed - no deadzone");
	CHECK(count_occurrences(clean, "BRIDGE_START_IQ_DEADZONE") == 0,
		"W4b: BRIDGE_START_IQ_DEADZONE is fully removed - no deadzone gate remains");

	/* --- W5: FW-117's neutral dwell still hangs off that same start, in order (T8) --- */
	const char *neutral_commit = NULL, *moe_enable = NULL, *pwm_flag_set = NULL;
	if (gated_start) {
		neutral_commit = strstr(gated_start, "bridge_lifecycle = BRIDGE_LIFECYCLE_NEUTRAL_COMMIT");
		moe_enable = strstr(gated_start, "timer_primary_output_config(TIMER0,ENABLE)");
		pwm_flag_set = strstr(gated_start, "ui_8_PWM_ON_Flag=1");
	}
	CHECK(neutral_commit != NULL,
		"W5a: BRIDGE_LIFECYCLE_NEUTRAL_COMMIT is still assigned inside the gated bridge start");
	CHECK(moe_enable != NULL, "W5b: MOE ON is still inside the gated bridge start");
	CHECK(pwm_flag_set != NULL, "W5c: ui_8_PWM_ON_Flag=1 is still inside the gated bridge start");
	if (neutral_commit && moe_enable && pwm_flag_set) {
		CHECK(neutral_commit < moe_enable,
			"W5d: neutral commit still precedes MOE ON - FW-117 dwell order intact under the new gate");
		CHECK(moe_enable < pwm_flag_set,
			"W5e: MOE ON still precedes ui_8_PWM_ON_Flag=1 - FW-117 sequence unchanged");
	}
	CHECK(strstr(clean, "neutral_dwell_active") != NULL &&
	      strstr(clean, "BRIDGE_LIFECYCLE_FOC_RELEASE") != NULL,
		"W5f: the FW-117 dwell flag and FOC_RELEASE state are still present (step2a suite checks them in full)");

	/* --- W6: PA0 battery-current calibration untouched and independent (T9) --- */
	const char *bat_cal = strstr(clean, "CAL_BAT_I_OFFSET-200");
	CHECK(bat_cal != NULL,
		"W6a: the PA0 battery-current calibration window (CAL_BAT_I_OFFSET +/- 200) is still there");

	const char *bat_assign = strstr(clean, "bat_current_offset=acc");
	CHECK(bat_assign != NULL, "W6b: bat_current_offset is still computed from its own accumulator");

	if (bat_cal && bat_assign && cal_init) {
		CHECK(bat_assign < cal_init,
			"W6c: the battery calibration completes before the phase calibration begins - separate, sequential paths");
		/*
		 * The battery block is short; anything from FW-119 appearing between its window check and
		 * its assignment would mean the two calibrations had been entangled.
		 */
		const char *leak = strstr(bat_cal, "current_cal");
		CHECK(leak == NULL || leak >= cal_init,
			"W6d: no FW-119 symbol appears inside the battery calibration block - PA0 stays independent");
	}
	CHECK(strstr(clean, "adc_value[0]") != NULL,
		"W6e: the battery path still reads adc_value[0] - a different channel from the phase ones");

	free(clean);

	if (host_test_failures == 0) {
		printf("FW-119 calibration wiring guard passed - all structural checks verified.\n");
		return 0;
	}
	printf("\n%d FW-119 check(s) FAILED.\n", host_test_failures);
	return 1;
}
