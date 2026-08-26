/*
 * FW-121.0 isolation guard: source-text checks on src/main.c and src/adc_trigger_diag.c, same
 * methodology as the other wiring guards in this directory. Neither file can be linked here -
 * main.c is the ARM entry point and the module's executable half is covered by
 * fw121_0_trigger_diag_host.c.
 *
 * The point of this one is narrow and specific: FW-121.0 moves the instant at which the FOC's own
 * current samples are taken. That is exactly the kind of change that must be PROVABLY absent from
 * the production build, and provably interlocked in the diagnostic one.
 *
 * WHAT THIS PROVES (W1-W6):
 *   W1: every single mention of the module in src/main.c sits inside a
 *       `#if CAN_DIAGNOSTICS_ENABLE` region. Not "most of them" - the guard walks the
 *       preprocessor nesting and checks each occurrence's position, so one stray call outside a
 *       guard fails the test.
 *   W2: the module's own source compiles out at DIAG=0 - its whole body is inside one
 *       `#if CAN_DIAGNOSTICS_ENABLE`, with the documented no-state stub in the #else.
 *   W3: exactly one owner of TIMER0 CH3 per ISR: dyn_adc_trigger_update() is the ELSE of the
 *       diagnostic branch, so the two can never both write it and production never loses it.
 *   W4: the sweep can only be armed behind the confirmed-standstill gate AND a bridge-dark
 *       term, and arming lives in the control loop, not in the ISR.
 *   W5: the ISR-side MOE interlock is real: the handler reads TIMER0's own output-enable bit
 *       (CCHP/POEN) and hands it to the module every conversion.
 *   W6: the capture is taken at ISR ENTRY - before fwdgt_counter_reload() and before the JDR
 *       reads - or the measured latency would include this handler's own prologue.
 *
 * WHAT THIS DOES NOT PROVE: that DIAG=0 is byte-identical. That is the build step's job, and the
 * card reports the flash/RAM figures beside these checks.
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
#ifndef ADC_TRIGGER_DIAG_C_PATH
#error "ADC_TRIGGER_DIAG_C_PATH must be defined (by the build script)"
#endif
#ifndef ROLLING_NO_ASSIST_DIAG_C_PATH
#error "ROLLING_NO_ASSIST_DIAG_C_PATH must be defined (by the build script)"
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

/* strstr over a bounded region (the text is NUL-terminated, so plain strstr would run past). */
static int memmem_like(const char *hay, long hay_len, const char *needle);

/* Blank comments and string bodies so prose can never satisfy a check. */
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

/*
 * Mark every byte that lies inside a `#if CAN_DIAGNOSTICS_ENABLE` region (any nesting depth of
 * other conditionals inside it included). Returns a malloc'd byte-per-char map, 1 = guarded.
 * The directive lines themselves are marked guarded so an `#if` line's own text does not trip a
 * check that looks for the macro name.
 */
static unsigned char *map_diag_guarded(const char *text, long len)
{
	unsigned char *map = (unsigned char *)calloc((size_t)len + 1, 1);
	if (!map) return NULL;

	long i = 0;
	int depth = 0;          /* nesting depth of ALL preprocessor conditionals */
	int guard_depth = -1;   /* the depth at which the CAN_DIAGNOSTICS_ENABLE guard opened */

	while (i < len) {
		long line_start = i;
		long j = i;
		while (j < len && text[j] != '\n') j++;
		long line_end = j;

		/* first non-blank char of the line */
		long k = line_start;
		while (k < line_end && (text[k] == ' ' || text[k] == '\t')) k++;

		int is_directive = (k < line_end && text[k] == '#');
		int opens = 0, closes = 0, is_guard_open = 0, is_else = 0;
		if (is_directive) {
			long d = k + 1;
			while (d < line_end && (text[d] == ' ' || text[d] == '\t')) d++;
			long dlen = line_end - d;
			if (dlen >= 2 && strncmp(&text[d], "if", 2) == 0) {
				opens = 1;
				/* #if CAN_DIAGNOSTICS_ENABLE / #ifdef ... - only the plain positive form counts */
				const char *nl = memchr(&text[d], '\n', (size_t)(len - d));
				long scan_len = nl ? (nl - &text[d]) : (len - d);
				if (memmem_like(&text[d], scan_len, "CAN_DIAGNOSTICS_ENABLE") &&
				    !memmem_like(&text[d], scan_len, "!CAN_DIAGNOSTICS_ENABLE"))
					is_guard_open = 1;
			} else if (dlen >= 5 && strncmp(&text[d], "endif", 5) == 0) {
				closes = 1;
			} else if ((dlen >= 4 && strncmp(&text[d], "else", 4) == 0) ||
			           (dlen >= 4 && strncmp(&text[d], "elif", 4) == 0)) {
				is_else = 1;
			}
		}

		if (opens) {
			if (is_guard_open && guard_depth < 0) guard_depth = depth;
			depth++;
		}

		int guarded = (guard_depth >= 0);
		/* the #else of the guard itself is the DIAG=0 side - still "accounted for", so a symbol
		 * there would be caught by the checks below rather than silently accepted */
		if (is_else && guard_depth >= 0 && depth == guard_depth + 1) guarded = 1;

		for (long m = line_start; m <= line_end && m < len; m++) map[m] = (unsigned char)(guarded ? 1 : 0);

		if (closes) {
			depth--;
			if (guard_depth >= 0 && depth == guard_depth) guard_depth = -1;
			map[line_start] = 1;  /* the #endif line belongs to the region it closes */
		}

		i = line_end + 1;
	}
	return map;
}

/* strstr over a bounded region (the text is NUL-terminated, so plain strstr would run past). */
static int memmem_like(const char *hay, long hay_len, const char *needle)
{
	long nlen = (long)strlen(needle);
	for (long i = 0; i + nlen <= hay_len; i++)
		if (memcmp(hay + i, needle, (size_t)nlen) == 0) return 1;
	return 0;
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
	const char *main_path = STRINGIZE(MAIN_C_PATH);
	const char *mod_path = STRINGIZE(ADC_TRIGGER_DIAG_C_PATH);
	printf("FW-121.0 diagnostic isolation guard (source-text check)\n");
	printf("  MAIN_C_PATH = %s\n", main_path);

	long len = 0;
	char *raw = read_whole_file(main_path, &len);
	CHECK(raw != NULL, "setup: src/main.c was readable");
	if (!raw) { printf("\n1 FW-121.0 check(s) FAILED.\n"); return 1; }
	char *clean = strip_comments(raw, len);
	free(raw);
	CHECK(clean != NULL, "setup: sanitization succeeded");
	if (!clean) { printf("\n1 FW-121.0 check(s) FAILED.\n"); return 1; }
	long clean_len = (long)strlen(clean);

	unsigned char *guarded = map_diag_guarded(clean, clean_len);
	CHECK(guarded != NULL, "setup: preprocessor region map built");
	if (!guarded) { printf("\n1 FW-121.0 check(s) FAILED.\n"); return 1; }

	/* --- W1: EVERY mention of the module in main.c is inside a diagnostics guard --- */
	{
		int total = 0, unguarded = 0, includes = 0;
		const char *p = clean;
		while ((p = strstr(p, "adc_trigger_diag")) != NULL) {
			/* the #include is exempt and is the repo's convention for every other diagnostic
			 * module (fw112_diag.h, fw117_trace.h ...): the header declares functions and
			 * defines no storage, so at DIAG=0 it emits nothing. W2 covers the module itself. */
			long off = p - clean;
			long ls = off;
			while (ls > 0 && clean[ls - 1] != '\n') ls--;
			if (strncmp(&clean[ls], "#include", 8) == 0) { includes++; p += 16; continue; }
			total++;
			if (!guarded[off]) {
				unguarded++;
				if (unguarded == 1) {
					long line = 1;
					for (long q = 0; q < off; q++) if (clean[q] == '\n') line++;
					printf("  first unguarded reference at src/main.c line %ld\n", line);
				}
			}
			p += 16;
		}
		CHECK(total > 0, "W1a: main.c does reference the module at all");
		CHECK(includes == 1, "W1a2: exactly one #include of the module header");
		CHECK(unguarded == 0,
			"W1b: every adc_trigger_diag* CALL in main.c is inside #if CAN_DIAGNOSTICS_ENABLE");

		int fw121_total = 0, fw121_unguarded = 0;
		p = clean;
		while ((p = strstr(p, "fw121_")) != NULL) {
			fw121_total++;
			if (!guarded[p - clean]) fw121_unguarded++;
			p += 6;
		}
		CHECK(fw121_total > 0, "W1c: the ISR's capture locals exist");
		CHECK(fw121_unguarded == 0,
			"W1d: ... and every one of them is inside a diagnostics guard - DIAG=0 sees no new code");
		printf("  %d module references, %d capture-local references, all guarded\n", total, fw121_total);
	}

	/* --- W3: one owner of CH3 --- */
	CHECK(count_occurrences(clean, "dyn_adc_trigger_update();") == 1,
		"W3a: dyn_adc_trigger_update() is still called from exactly one place");
	{
		const char *owner = strstr(clean, "if(fw121_active){");
		const char *prod = strstr(clean, "dyn_adc_trigger_update();");
		CHECK(owner != NULL, "W3b: the ISR has the diagnostic-ownership branch");
		CHECK(owner != NULL && prod != NULL && owner < prod,
			"W3c: production's CH3 update is the ELSE of that branch - never both, never neither");
		CHECK(strstr(clean, "} else\n#endif\n\tdyn_adc_trigger_update();") != NULL ||
		      strstr(clean, "} else\r\n#endif\r\n\tdyn_adc_trigger_update();") != NULL,
			"W3d: the else/#endif/production-call sequence is intact");
	}

	/* --- W4: FW-126.0 arms only inside the existing neutral dwell --- */
	{
		const char *arm = strstr(clean, "adc_trigger_diag_arm_neutral_dwell(");
		CHECK(arm != NULL, "W4a: the arm call exists");
		if (arm) {
			const char *semi = strchr(arm, ';');
			long n = semi ? (semi - arm) : 0;
			CHECK(n > 0 && memmem_like(arm, n, "ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR"),
				"W4b: arming requires committed neutral phase CCRs");
			CHECK(n > 0 && memmem_like(arm, n, "ADC_TRIGGER_DIAG_SAFE_FOC_HELD"),
				"W4c: ... and the existing FOC-blocking dwell");
		}
		CHECK(strstr(clean, "neutral_dwell_active = 1") != NULL,
			"W4d: dwell is committed before the diagnostic can arm");
		CHECK(count_occurrences(clean, "adc_trigger_diag_arm_neutral_dwell(") == 1,
			"W4e: there is exactly one place the sweep can start");
	}

	/* --- W5: the ISR-side interlock --- */
	{
		const char *isr = strstr(clean, "void ADC0_1_IRQHandler(void)");
		CHECK(isr != NULL, "W5a: the injected-ADC handler was found");
		if (isr) {
			const char *isr_end = strstr(isr, "\n}");
			long n = isr_end ? (isr_end - isr) : (long)strlen(isr);
			CHECK(memmem_like(isr, n, "fw121_env_from(&fw121_s.env, fw121_cchp"),
				"W5b: every conversion hands the module the live MOE state (inside the env snapshot)");
			CHECK(memmem_like(isr, n, "TIMER_CCHP(TIMER0)"),
				"W5c: ... read from the timer register, not from a software flag");
		CHECK(memmem_like(isr, n, "adc_trigger_diag_neutral_dwell_isr(&fw121_s"),
				"W5d: ... on every injected conversion while the sweep owns CH3");
		}
	}

	/* --- W6: the capture is at ISR entry --- */
	{
		const char *isr = strstr(clean, "void ADC0_1_IRQHandler(void)");
		const char *capture = isr ? strstr(isr, "TIMER_CNT(TIMER0)") : NULL;
		const char *wdg = isr ? strstr(isr, "fwdgt_counter_reload()") : NULL;
		const char *jdr = isr ? strstr(isr, "adc_inserted_data_read") : NULL;
		CHECK(capture != NULL, "W6a: the ISR reads TIMER0 CNT");
		CHECK(capture && wdg && capture < wdg,
			"W6b: CNT is captured BEFORE fwdgt_counter_reload() - the measured latency is the "
			"hardware's, not this handler's prologue");
		CHECK(capture && jdr && capture < jdr, "W6c: ... and before the JDR reads");
	}

	/* --- W8 (FW-121.0B): the three instrumentation hooks exist, are guarded, and are ordered --- */
	{
		const char *arm_env = strstr(clean, "fw121_env_now(&fw121_env)");
		const char *arm_call = strstr(clean, "adc_trigger_diag_arm_neutral_dwell(");
		const char *poen_hook = strstr(clean, "adc_trigger_diag_note_poen_enable(");
		const char *poen_enable = strstr(clean, "timer_primary_output_config(TIMER0,ENABLE)");
		const char *isr_env = strstr(clean, "fw121_env_from(&fw121_s.env");

		CHECK(arm_env != NULL && arm_call != NULL && arm_env < arm_call,
			"W8a: the ARM environment is captured immediately BEFORE the arm call, not after it");
		CHECK(poen_hook != NULL, "W8b: the POEN-enable hook exists");
		CHECK(poen_enable != NULL && poen_hook > poen_enable,
			"W8c: ... and sits AFTER the enable, so POEN reads back as 1");
		CHECK(count_occurrences(clean, "adc_trigger_diag_note_poen_enable(") == 1,
			"W8d: exactly one POEN hook - it must not be spread over several sites");
		CHECK(isr_env != NULL,
			"W8e: the ISR builds its environment from the values captured at ISR entry");
		CHECK(strstr(clean, "fw121_env_from(&fw121_s.env, fw121_cchp, fw121_ctl0, fw121_cnt,") != NULL,
			"W8f: ... using exactly those entry-captured registers, never re-reading them");
		CHECK(count_occurrences(clean, "timer_primary_output_config(TIMER0,ENABLE)") == 2,
			"W8g: still only two POEN enables in the file (bridge start + the unreachable autodetect)");
	}

	/* --- W7: no magic EFIDs left in main.c --- */
	{
		CHECK(strstr(clean, "0x00010240") == NULL && strstr(clean, "0x00010241") == NULL &&
		      strstr(clean, "0x00010246") == NULL,
			"W7a: main.c does not spell the FW-121.0 CAN ids - they come from inc/adc_trigger_diag.h");
		CHECK(strstr(clean, "0x0001022F") == NULL && strstr(clean, "0x00010235") == NULL,
			"W7b: the old colliding ids (FW-112 A/B / FW-117 territory) are gone from main.c");
		CHECK(strstr(clean, "adc_trigger_diag_aggregate_frame(") != NULL,
			"W7c: the sweep frames are built by the module, not open-coded in the aggregate builder");
		CHECK(strstr(clean, "diag_efid_map.h") != NULL,
			"W7d: main.c pulls in the diagnostic id map, so its collision asserts are compiled");
	}

	/* --- W9 (FW-122): reuse this isolation owner for ROLLING_NO_ASSIST. --- */
	{
		int total = 0, unguarded = 0, includes = 0;
		const char *p = clean;
		while ((p = strstr(p, "rolling_no_assist")) != NULL) {
			long off = p - clean;
			long ls = off;
			while (ls > 0 && clean[ls - 1] != '\n') ls--;
			if (strncmp(&clean[ls], "#include", 8) == 0) {
				includes++;
				p += 17;
				continue;
			}
			total++;
			if (!guarded[off]) unguarded++;
			p += 17;
		}
		CHECK(total > 0, "W9a: main.c references ROLLING_NO_ASSIST recorder");
		CHECK(includes == 2, "W9b: recorder and explicit replay headers are the only rolling_no_assist includes");
		CHECK(unguarded == 0,
			"W9c: every ROLLING_NO_ASSIST call/state in main.c is CAN-diagnostic guarded");
	}

	free(guarded);
	free(clean);

	/* --- W2: the module itself compiles out at DIAG=0 --- */
	{
		long mlen = 0;
		char *mraw = read_whole_file(mod_path, &mlen);
		CHECK(mraw != NULL, "W2a: src/adc_trigger_diag.c was readable");
		if (mraw) {
			char *mclean = strip_comments(mraw, mlen);
			free(mraw);
			if (mclean) {
				CHECK(strstr(mclean, "#if CAN_DIAGNOSTICS_ENABLE") != NULL,
					"W2b: the module body is behind #if CAN_DIAGNOSTICS_ENABLE");
				CHECK(strstr(mclean, "#else") != NULL &&
				      strstr(mclean, "adc_trigger_diag_not_compiled_in") != NULL,
					"W2c: ... with the documented zero-state stub on the DIAG=0 side");
				CHECK(count_occurrences(mclean, "#if CAN_DIAGNOSTICS_ENABLE") == 1,
					"W2d: exactly one such region - the whole module, not a part of it");
				free(mclean);
			}
		}
	}

	/* FW-122 module adds no NORMAL fallback functions or storage. */
	{
		const char *rna_path = STRINGIZE(ROLLING_NO_ASSIST_DIAG_C_PATH);
		long rlen = 0;
		char *rraw = read_whole_file(rna_path, &rlen);
		CHECK(rraw != NULL, "W9d: src/rolling_no_assist_diag.c was readable");
		if (rraw) {
			char *rclean = strip_comments(rraw, rlen);
			free(rraw);
			if (rclean) {
				CHECK(strstr(rclean,
				      "#if CAN_DIAGNOSTICS_ENABLE && ROLLING_NO_ASSIST_DIAG_ENABLE") != NULL,
					"W9e: whole recorder is behind CAN_DIAG && RNA_ENABLE");
				CHECK(strstr(rclean, "rolling_no_assist_diag_not_compiled_in") != NULL,
					"W9f: disabled side is a zero-state typedef only");
				free(rclean);
			}
		}
	}

	if (host_test_failures == 0) {
		printf("\nAll FW-121.0 isolation checks passed.\n");
		return 0;
	}
	printf("\n%d FW-121.0 isolation check(s) FAILED.\n", host_test_failures);
	return 1;
}
