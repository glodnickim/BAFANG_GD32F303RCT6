/*
 * FW-136.0: the numbers that turn "I can hear a click" into something falsifiable.
 *
 * Three cards (FW-131, FW-131.1, QZERO-3) shipped on the strength of the ear alone and none of
 * them changed anything. The hypothesis under test is "click = current x commutation-angle jump",
 * and it turns on whether ANY current flows while the rotor is slow enough for the angle to step.
 *
 * A measurement is only worth the ride it costs if it cannot quietly measure nothing. This suite
 * is therefore aimed less at arithmetic than at the ways this measurement could produce a
 * confident zero that means nothing - because that has now happened three times in this project:
 *
 *   - FW-121.0: frames were built and then silently dropped, because the snapshot cap was never
 *     raised. Cost a ride.
 *   - FW-106: the instruction said to capture the aggregate live, but it is one snapshot per
 *     session. Cost a second ride.
 *   - FW-136.0 revision 1, caught by a bench log before it cost anything: the peak was captured
 *     only while ui16_erps was below RIDE_COAST_RELEASE_ERPS - hanging the whole measurement on
 *     the one signal this card exists to doubt. If that reading freezes above the threshold as
 *     the rotor stops, the window never opens and the peak reads 0 whether or not current flowed.
 *     T2 and T8 exist so that specific mistake cannot come back.
 *
 * main.c and CAN_Display.c are the ARM entry point and the CAN receive path and cannot be linked
 * on a host (same reasoning as armed_zero_lifecycle_host.c), so the wiring is proven against the
 * production source text and the gating arithmetic against a replica pinned to it.
 *
 *   T1  ZERO-REFERENCE GATE     the coast is where the reference is zero, nothing else.
 *   T2  NO SPEED GATE ON CAPTURE  the doubted signal must not be able to close the window.
 *   T3  SAME IN A AND B         the measurement sits OUTSIDE QUIET_ZERO_ENABLE, or the A/B
 *                               comparison it exists for would be meaningless.
 *   T4  CUMULATIVE              nothing resets the counters between stops.
 *   T5  PRESENT IN EVERY BUILD  READ 0x6032 is answered outside CAN_DIAGNOSTICS_ENABLE.
 *   T6  READ IS INERT           a status read must not disturb what it reports.
 *   T7  WIRE FORMAT             one 8-byte frame, little-endian, on the documented id.
 *   T8  REPLICA BEHAVIOUR       including the case that broke revision 1: a coast whose reported
 *                               speed never falls below the threshold must still be measured.
 */

#include "../common/check.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)

#ifndef MAIN_C_PATH
#error "MAIN_C_PATH must be supplied by run-host-tests.ps1"
#endif
#ifndef CAN_DISPLAY_C_PATH
#error "CAN_DISPLAY_C_PATH must be supplied by run-host-tests.ps1"
#endif

static char *read_whole_file(const char *path, long *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long len = ftell(f);
	if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
	char *text = (char *)malloc((size_t)len + 1U);
	if (!text) { fclose(f); return NULL; }
	size_t got = fread(text, 1, (size_t)len, f);
	fclose(f);
	text[got] = '\0';
	if (out_len) *out_len = (long)got;
	return text;
}

/* Comments explain intent; they must never be able to satisfy a guard. */
static char *strip_comments(const char *text, long len)
{
	char *out = (char *)malloc((size_t)len + 1U);
	if (!out) return NULL;
	long i = 0, o = 0;
	while (i < len) {
		if (text[i] == '/' && i + 1 < len && text[i + 1] == '/') {
			while (i < len && text[i] != '\n') i++;
			continue;
		}
		if (text[i] == '/' && i + 1 < len && text[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
			i += 2;
			out[o++] = ' ';
			continue;
		}
		out[o++] = text[i++];
	}
	out[o] = '\0';
	return out;
}

/* Collapse whitespace runs so a guard survives reindentation. */
static char *squeeze(const char *text)
{
	size_t len = strlen(text);
	char *out = (char *)malloc(len + 1U);
	if (!out) return NULL;
	size_t o = 0;
	bool in_space = false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (!in_space && o > 0) out[o++] = ' ';
			in_space = true;
			continue;
		}
		out[o++] = (char)c;
		in_space = false;
	}
	while (o > 0 && out[o - 1] == ' ') o--;
	out[o] = '\0';
	return out;
}

static char *load(const char *path)
{
	long len = 0;
	char *raw = read_whole_file(path, &len);
	if (!raw) return NULL;
	char *stripped = strip_comments(raw, len);
	free(raw);
	if (!stripped) return NULL;
	char *flat = squeeze(stripped);
	free(stripped);
	return flat;
}

static int count_occurrences(const char *hay, const char *needle)
{
	int n = 0;
	size_t nlen = strlen(needle);
	if (nlen == 0) return 0;
	for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nlen) n++;
	return n;
}

/* ------------------------------------------------------------------------------------------- */
/* Replica of the production capture (main.c, runPIcontrol()). Pinned to the real text by T1/T2.  */
/* ------------------------------------------------------------------------------------------- */

typedef struct {
	uint16_t release_count;
	uint16_t peak_iq;
	uint16_t peak_erps;
	uint16_t min_erps;
	uint8_t  prev_ref_nonzero;
} replica_t;

static void replica_reset(replica_t *r)
{
	memset(r, 0, sizeof *r);
	r->min_erps = 0xFFFFU;
}

static void replica_tick(replica_t *r, int32_t iq_ref, int32_t iq_measured,
	uint16_t erps, bool policy_quiet)
{
	uint8_t ref_nonzero = (iq_ref != 0) ? 1U : 0U;
	if (r->prev_ref_nonzero && !ref_nonzero && policy_quiet) {
		if (r->release_count < 0xFFFFU) r->release_count++;
	}
	r->prev_ref_nonzero = ref_nonzero;

	if (!ref_nonzero && erps > 0U) {
		int32_t meas = iq_measured;
		if (meas < 0) meas = -meas;
		if (meas > 0xFFFF) meas = 0xFFFF;
		if ((uint16_t)meas > r->peak_iq) {
			r->peak_iq = (uint16_t)meas;
			r->peak_erps = erps;
		}
		if (erps < r->min_erps) r->min_erps = erps;
	}
}

int main(void)
{
	printf("FW-136.0 coast current measurement - wiring guards + capture replica\n");

	char *mainc = load(STRINGIZE(MAIN_C_PATH));
	char *canc  = load(STRINGIZE(CAN_DISPLAY_C_PATH));
	CHECK(mainc && canc, "sources readable");
	if (!mainc || !canc) {
		printf("\n1 FW-136.0 check FAILED (sources unreadable).\n");
		return 1;
	}

	/* ==== T1: the coast is defined by the reference, and the peak is a magnitude ==== */
	{
		CHECK(strstr(mainc, "if(!ref_nonzero && ui16_erps > 0U){") != NULL,
			"T1: capture runs while the Iq reference is ZERO and the rotor still reports motion");
		CHECK(strstr(mainc, "if(meas < 0) meas = -meas;") != NULL,
			"T1: the peak is of the MAGNITUDE - a braking current is negative and must still count");
		CHECK(strstr(mainc, "if((uint16_t)meas > coast_peak_iq){") != NULL &&
			strstr(mainc, "coast_peak_erps = ui16_erps;") != NULL,
			"T1: it is a running maximum and it records WHERE the peak happened");
	}

	/* ==== T2: the doubted signal must not be able to close the capture window ==== */
	{
		const char *fn = strstr(mainc, "void runPIcontrol(void){");
		CHECK(fn != NULL, "T2: runPIcontrol() found");
		const char *cap = fn ? strstr(fn, "if(!ref_nonzero && ui16_erps > 0U){") : NULL;
		const char *end = cap ? strstr(cap, "if(ui16_erps < coast_min_erps)") : NULL;
		CHECK(cap != NULL && end != NULL, "T2: the capture block is present");
		if (cap && end) {
			size_t span = (size_t)(end - cap);
			char *body = (char *)malloc(span + 1U);
			memcpy(body, cap, span);
			body[span] = '\0';
			CHECK(strstr(body, "RIDE_COAST_RELEASE_ERPS") == NULL,
				"T2: the CAPTURE must not be gated on RIDE_COAST_RELEASE_ERPS. Revision 1 was, and "
				"that hangs the measurement on the very reading this card doubts - if erps freezes "
				"above the threshold the window never opens and a zero peak proves nothing");
			free(body);
		}
		CHECK(strstr(mainc, "if(ui16_erps < coast_min_erps) coast_min_erps = ui16_erps;") != NULL,
			"T2: and the doubted reading is MEASURED instead - the lowest erps ever seen on a coast "
			"says directly whether it ever gets below the threshold");
	}

	/* ==== T3: the measurement must be identical in both A/B images ==== */
	{
		const char *fn = strstr(mainc, "void runPIcontrol(void){");
		const char *meas = fn ? strstr(fn, "static uint8_t click_prev_ref_nonzero = 0;") : NULL;
		const char *qz_gate = fn ? strstr(fn, "#if QUIET_ZERO_ENABLE") : NULL;
		CHECK(meas != NULL, "T3: the measurement block is inside runPIcontrol()");
		CHECK(qz_gate != NULL, "T3: the QZERO A/B gate is still there");
		CHECK(meas != NULL && qz_gate != NULL && meas < qz_gate,
			"T3: the measurement sits BEFORE the QUIET_ZERO_ENABLE gate, so variant A and variant B "
			"measure the same thing the same way - otherwise the comparison is worthless");
	}

	/* ==== T4: cumulative - the only assignment to a start value is the definition ==== */
	{
		CHECK(count_occurrences(mainc, "coast_peak_iq=0") == 1,
			"T4: coast_peak_iq is initialised exactly once - nothing resets it between stops");
		CHECK(count_occurrences(mainc, "click_release_count=0") == 1,
			"T4: same for the release counter");
		CHECK(count_occurrences(mainc, "coast_min_erps=0xFFFF") == 1,
			"T4: the minimum starts at 0xFFFF, which is also how 'no coast sampled yet' reads on "
			"the wire - a 0 there would be indistinguishable from a genuine stop");
		CHECK(strstr(mainc, "if(click_release_count < 0xFFFFU) click_release_count++;") != NULL,
			"T4: the release counter saturates instead of wrapping - a wrapped counter reads as a "
			"small number and would be believed");
	}

	/* ==== T5: answered in every build, not only DIAG ==== */
	{
		const char *rd = strstr(canc, "case READ_CMD:");
		CHECK(rd != NULL, "T5: the READ dispatch exists");
		const char *ours = rd ? strstr(rd, "Ext_ID_Rx.command==0x6032") : NULL;
		const char *diag_gate = rd ? strstr(rd, "#if CAN_DIAGNOSTICS_ENABLE") : NULL;
		CHECK(ours != NULL, "T5: READ 0x6032 is dispatched");
		CHECK(ours != NULL && (diag_gate == NULL || ours < diag_gate),
			"T5: and dispatched BEFORE the diagnostics gate, so the NORMAL image - the one that "
			"actually clicks - answers it too");
		CHECK(strstr(canc, "Ext_ID_Rx.command==0x6032 && Ext_ID_Rx.source==5U && receive_message.rx_dlen==0U")
			!= NULL,
			"T5: tool only and zero-length, like every other side-effect-free status read here");
	}

	/* ==== T6: reading must not disturb what it reports ==== */
	{
		const char *fn = strstr(canc, "static void send_click_zone_status(void)");
		CHECK(fn != NULL, "T6: the responder exists");
		const char *end = fn ? strstr(fn, "can_tx_queue_enqueue(0x022A6032U, 8U, d);") : NULL;
		CHECK(end != NULL, "T6: and it enqueues exactly one frame");
		if (fn && end) {
			size_t span = (size_t)(end - fn);
			char *body = (char *)malloc(span + 1U);
			memcpy(body, fn, span);
			body[span] = '\0';
			CHECK(strstr(body, "++") == NULL && strstr(body, "= 0") == NULL,
				"T6: the responder only reads - a status read that alters what it reports cannot be "
				"trusted twice in one session");
			free(body);
		}
	}

	/* ==== T7: the documented wire format ==== */
	{
		CHECK(strstr(canc, "d[0] = (uint8_t)(click_release_count & 0xFFU);") != NULL &&
			strstr(canc, "d[1] = (uint8_t)((click_release_count >> 8) & 0xFFU);") != NULL,
			"T7: little-endian, low byte first - the 0x60xx family convention, NOT the big-endian "
			"diag logger frames");
		CHECK(strstr(canc, "d[2] = (uint8_t)(coast_peak_iq & 0xFFU);") != NULL,
			"T7: the peak current is in bytes 2..3");
		CHECK(strstr(canc, "d[4] = (uint8_t)(coast_peak_erps & 0xFFU);") != NULL,
			"T7: the speed at the peak in bytes 4..5");
		CHECK(strstr(canc, "d[6] = (uint8_t)(coast_min_erps & 0xFFU);") != NULL,
			"T7: and the lowest coast speed in bytes 6..7");
	}

	/* ==== T8: the capture behaves as claimed, including revision 1's blind spot ==== */
	{
		replica_t r;
		replica_reset(&r);

		/* Riding under power: not a coast. */
		replica_tick(&r, 200, 180, 90, false);
		replica_tick(&r, 40, 38, 20, false);
		CHECK(r.peak_iq == 0 && r.min_erps == 0xFFFFU,
			"T8: current drawn while RIDING is not coast current - the reference is not zero");

		/* Release, then a coast whose reported speed NEVER falls below the 10 erps threshold -
		 * exactly what a frozen ui16_erps looks like, and exactly what revision 1 could not see. */
		replica_tick(&r, 0, 30, 60, true);
		CHECK(r.release_count == 1, "T8: the release edge is counted once");
		replica_tick(&r, 0, 44, 40, true);
		replica_tick(&r, 0, 51, 22, true);
		replica_tick(&r, 0, 33, 12, true);
		CHECK(r.peak_iq == 51,
			"T8: a coast that never reports below the threshold is STILL measured - this is the "
			"case revision 1 would have reported as a confident zero");
		CHECK(r.peak_erps == 22, "T8: and the peak is stamped with where it happened");
		CHECK(r.min_erps == 12,
			"T8: the lowest reported speed is 12 - above RIDE_COAST_RELEASE_ERPS, which is the "
			"reading that would confirm the freeze");

		/* A second, deeper coast: the peak must not fall, the minimum must. */
		replica_tick(&r, 180, 170, 80, false);
		replica_tick(&r, 0, 20, 30, true);
		replica_tick(&r, 0, 9, 4, true);
		CHECK(r.release_count == 2, "T8: the next release is counted too");
		CHECK(r.peak_iq == 51 && r.peak_erps == 22,
			"T8: a later smaller sample never lowers the peak - one reading has to survive "
			"everything that happened before it");
		CHECK(r.min_erps == 4,
			"T8: but the minimum does fall, which is how a WORKING speed reading looks");

		/* Standing still before the first Hall edge: a genuine zero, not a slow rotor. */
		replica_t cold;
		replica_reset(&cold);
		replica_tick(&cold, 0, 7, 0, false);
		CHECK(cold.peak_iq == 0 && cold.min_erps == 0xFFFFU,
			"T8: erps 0 is 'no Hall edge yet', not 'stopped' - sampling it would put a false floor "
			"under the minimum on every power-on");
	}

	free(mainc); free(canc);

	if (host_test_failures == 0) {
		printf("All FW-136.0 coast measurement checks passed.\n");
		return 0;
	}
	printf("\n%d FW-136.0 coast measurement check(s) FAILED.\n", host_test_failures);
	return 1;
}
