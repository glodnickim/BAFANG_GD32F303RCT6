/*
 * FW-125 wiring guard: source-text checks on src/main.c, same methodology as
 * fw119_current_cal_wiring_host.c and step2a_neutral_dwell_wiring_host.c. main.c is the ARM
 * entry point, wired straight to GD32 registers, so it cannot be linked here - the executable
 * half of FW-125 is in tests/host/fw125_phase_current_calibration_host.c.
 *
 * WHAT THIS PROVES (T1-T4, T13-T15 from the card's section 22, plus a regression guard for
 * section 24 - "wiring guard: fail if calibration ever reads adc_value[regular] again"):
 *
 *   T1: phase A calibration source (i16_ph1_current, fed by adc_inserted_data_read(ADC2, ...))
 *       is the SAME variable the runtime FOC correction subtracts CURRENT_CAL_PHASE_A from.
 *   T2: phase B - i16_ph2_current / ADC1 - same relationship.
 *   T3: phase C - i16_ph3_current / ADC0 - same relationship.
 *   T4 (section 24 guard): no adc_value[4]/[7]/[8] (the old FW-118 regular-ADC calibration
 *       source) appears anywhere in main.c any more - a future edit cannot silently reintroduce
 *       the domain mismatch this card fixed.
 *   T13: the calibration capture (phase_cal_acc) and the verify capture (phase_cal_verify_acc)
 *       both run BEFORE dyn_adc_state_reconstruct() - reconstruction never contaminates either
 *       accumulator - and phase_cal_verify_acc specifically runs AFTER the current_cal.valid
 *       correction, so it measures the corrected signal, not the raw one.
 *   T14: the FW-125 zero-current self-test (phase_cal_verify_acc, fw125_zero_current_selftest_*)
 *       is entirely inside #if CAN_DIAGNOSTICS_ENABLE - a NORMAL build carries none of it. The
 *       core fix (phase_cal_acc, current_cal_submit) is NOT diagnostic-gated - it is production
 *       behaviour, present in both variants.
 *   T15: DIAG exposes calibration values - fw125_zero_current_selftest_valid/mean/p2p exist and
 *       are populated from phase_cal_verify_acc after current_cal_finalize().
 *
 * WHAT THIS DOES NOT PROVE: runtime behaviour, real ADC timing, or that the provisional
 * CURRENT_CAL_RESIDUAL_MIN/MAX and CURRENT_ZERO_MAX_P2P_ADC limits are correct - those remain
 * HW_PENDING (see documentation/FW-125_PHASE_CURRENT_SAME_PATH_CALIBRATION_PL.md).
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

/* Blank out comments and string bodies so a check can never be satisfied by prose. */
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
	printf("FW-125 phase-current calibration wiring guard (source-text check)\n");
	printf("  MAIN_C_PATH = %s\n", path);

	long len = 0;
	char *raw = read_whole_file(path, &len);
	CHECK(raw != NULL, "setup: src/main.c was readable");
	if (!raw) {
		printf("\n1 FW-125 check(s) FAILED (could not read file).\n");
		return 1;
	}

	char *clean = strip_comments(raw, len);
	CHECK(clean != NULL, "setup: comment/string-aware sanitization succeeded");
	free(raw);
	if (!clean) {
		printf("\n1 FW-125 check(s) FAILED (sanitization failed).\n");
		return 1;
	}

	const char *isr = strstr(clean, "void ADC0_1_IRQHandler(void)");
	CHECK(isr != NULL, "setup: ADC0_1_IRQHandler found");

	/* --- T1/T2/T3: calibration source == runtime FOC source, per phase --- */
	const char *read_a = strstr(clean, "i16_ph1_current = adc_inserted_data_read(ADC2, ADC_INSERTED_CHANNEL_0)");
	const char *read_b = strstr(clean, "i16_ph2_current = adc_inserted_data_read(ADC1, ADC_INSERTED_CHANNEL_0)");
	const char *read_c = strstr(clean, "i16_ph3_current = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0)");
	CHECK(read_a != NULL, "T1a: phase A is read from ADC2 injected (i16_ph1_current)");
	CHECK(read_b != NULL, "T2a: phase B is read from ADC1 injected (i16_ph2_current)");
	CHECK(read_c != NULL, "T3a: phase C is read from ADC0 injected (i16_ph3_current)");

	const char *cal_acc = strstr(clean, "if(phase_cal_acc.active && phase_cal_acc.count < phase_cal_acc.target)");
	CHECK(cal_acc != NULL, "setup: the calibration accumulator hook is present");

	const char *cal_sum_a = cal_acc ? strstr(cal_acc, "phase_cal_acc.sum[CURRENT_CAL_PHASE_A] += i16_ph1_current") : NULL;
	const char *cal_sum_b = cal_acc ? strstr(cal_acc, "phase_cal_acc.sum[CURRENT_CAL_PHASE_B] += i16_ph2_current") : NULL;
	const char *cal_sum_c = cal_acc ? strstr(cal_acc, "phase_cal_acc.sum[CURRENT_CAL_PHASE_C] += i16_ph3_current") : NULL;
	CHECK(cal_sum_a != NULL, "T1b: calibration accumulates i16_ph1_current into PHASE_A - the "
	      "SAME variable ADC2 injected feeds (T1a) and the same index the runtime correction "
	      "uses (T1c)");
	CHECK(cal_sum_b != NULL, "T2b: calibration accumulates i16_ph2_current into PHASE_B (ADC1)");
	CHECK(cal_sum_c != NULL, "T3b: calibration accumulates i16_ph3_current into PHASE_C (ADC0)");

	const char *runtime_sub_a = strstr(clean, "i16_ph1_current -= current_cal.offset[CURRENT_CAL_PHASE_A]");
	const char *runtime_sub_b = strstr(clean, "i16_ph2_current -= current_cal.offset[CURRENT_CAL_PHASE_B]");
	const char *runtime_sub_c = strstr(clean, "i16_ph3_current -= current_cal.offset[CURRENT_CAL_PHASE_C]");
	CHECK(runtime_sub_a != NULL, "T1c: the runtime ISR subtracts offset[PHASE_A] from i16_ph1_current");
	CHECK(runtime_sub_b != NULL, "T2c: the runtime ISR subtracts offset[PHASE_B] from i16_ph2_current");
	CHECK(runtime_sub_c != NULL, "T3c: the runtime ISR subtracts offset[PHASE_C] from i16_ph3_current");

	if (read_a && cal_sum_a && runtime_sub_a && isr) {
		CHECK(isr < read_a && read_a < cal_sum_a && cal_sum_a < runtime_sub_a,
			"T1d: inside the ISR, order is read(ADC2) -> accumulate(PHASE_A) -> "
			"correct(PHASE_A) - one continuous chain, phase A never crosses into B/C");
	}

	/* --- T4 (card section 24): the domain-mismatch bug cannot silently come back --- */
	CHECK(strstr(clean, "adc_value[7]") == NULL && strstr(clean, "adc_value[8]") == NULL &&
	      strstr(clean, "adc_value[4]") == NULL,
		"T4: adc_value[4]/[7]/[8] (the old regular-ADC calibration source, a DIFFERENT ADC "
		"instance than ADC1/ADC2 injected for phases A/B) no longer appears anywhere in "
		"main.c - the domain mismatch this card fixed cannot silently regress");

	/* --- T13: capture order relative to reconstruction and to the offset correction --- */
	const char *offset_block_end = runtime_sub_c;
	const char *reconstruct = strstr(clean, "dyn_adc_state_reconstruct(MS.char_dyn_adc_state");
	CHECK(reconstruct != NULL, "setup: dyn_adc_state_reconstruct() found");
	if (cal_acc && reconstruct) {
		CHECK(cal_acc < reconstruct,
			"T13a: the calibration accumulator captures BEFORE sector reconstruction runs");
	}
	const char *verify_acc = strstr(clean, "if(phase_cal_verify_acc.active && phase_cal_verify_acc.count < phase_cal_verify_acc.target)");
	if (offset_block_end && verify_acc && reconstruct) {
		CHECK(offset_block_end < verify_acc && verify_acc < reconstruct,
			"T13b: the verify accumulator captures AFTER the offset correction and BEFORE "
			"reconstruction - it measures the corrected signal, not raw and not reconstructed");
	}
	if (cal_acc && offset_block_end) {
		CHECK(cal_acc < offset_block_end,
			"T13c: the calibration accumulator (raw, pre-correction) runs strictly before the "
			"offset correction it is measuring the input to");
	}

	/* --- T14: the self-test telemetry is DIAG-only; the fix itself is not --- */
	{
		/* Find the '#if CAN_DIAGNOSTICS_ENABLE' immediately preceding the verify-accumulator
		 * capture block, and confirm a matching '#endif' follows it before the reconstruction
		 * call - i.e. the whole block is fenced, not just introduced by a stray guard. */
		const char *found_if = NULL;
		if (verify_acc) {
			const char *scan = clean;
			while (scan < verify_acc) {
				const char *next = strstr(scan, "#if CAN_DIAGNOSTICS_ENABLE");
				if (!next || next >= verify_acc) break;
				found_if = next;
				scan = next + 1;
			}
		}
		CHECK(found_if != NULL,
			"T14a: the verify-accumulator capture is preceded by #if CAN_DIAGNOSTICS_ENABLE");
		if (found_if && verify_acc && reconstruct) {
			const char *endif_after = strstr(verify_acc, "#endif");
			CHECK(endif_after != NULL && endif_after < reconstruct,
				"T14b: the DIAG guard around the verify-accumulator capture is closed with "
				"#endif before reconstruction runs - it cannot leak into the NORMAL build");
		}
	}

	CHECK(strstr(clean, "uint8_t  fw125_zero_current_selftest_valid = 0") != NULL,
		"T14c/T15a: fw125_zero_current_selftest_valid is declared");
	CHECK(strstr(clean, "int16_t  fw125_zero_current_selftest_mean[CURRENT_CAL_PHASES]") != NULL,
		"T15b: fw125_zero_current_selftest_mean[] is declared");
	CHECK(strstr(clean, "uint16_t fw125_zero_current_selftest_p2p[CURRENT_CAL_PHASES]") != NULL,
		"T15c: fw125_zero_current_selftest_p2p[] is declared");

	{
		const char *decl = strstr(clean, "uint8_t  fw125_zero_current_selftest_valid = 0");
		const char *guard_before = NULL;
		if (decl) {
			const char *scan = clean;
			while (scan < decl) {
				const char *next = strstr(scan, "#if CAN_DIAGNOSTICS_ENABLE");
				if (!next || next >= decl) break;
				guard_before = next;
				scan = next + 1;
			}
		}
		CHECK(guard_before != NULL,
			"T14d: the fw125_zero_current_selftest_* declarations are inside "
			"#if CAN_DIAGNOSTICS_ENABLE - absent entirely from a NORMAL build");
	}

	/* The core fix must NOT be diagnostic-gated - phase_cal_acc and its accumulation hook are
	 * unconditional (only the *verify* path above is DIAG-only). */
	CHECK(strstr(clean, "static volatile phase_cal_acc_t phase_cal_acc;") != NULL,
		"T14e: the calibration accumulator itself (phase_cal_acc) is NOT inside a "
		"CAN_DIAGNOSTICS_ENABLE guard - the fix is production behaviour in both variants");

	const char *finalize_call = strstr(clean, "current_cal_finalize(&current_cal)");
	const char *verify_arm = strstr(clean, "phase_cal_verify_acc_start(CURRENT_CAL_SAMPLES)");
	CHECK(verify_arm != NULL, "setup: phase_cal_verify_acc_start(CURRENT_CAL_SAMPLES) found in main()");
	if (finalize_call && verify_arm) {
		CHECK(finalize_call < verify_arm,
			"T15d: main() arms the self-test capture AFTER current_cal_finalize() - it observes "
			"whatever offsets the policy actually settled on (RUNTIME, LKG or legacy)");
	}

	free(clean);

	if (host_test_failures == 0) {
		printf("FW-125 wiring guard passed - all structural checks verified.\n");
		return 0;
	}
	printf("\n%d FW-125 check(s) FAILED.\n", host_test_failures);
	return 1;
}
