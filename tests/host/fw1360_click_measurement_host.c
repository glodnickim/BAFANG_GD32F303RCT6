/*
 * FW-136.0: the four numbers that turn "I can hear a click" into something falsifiable.
 *
 * Three cards (FW-131, FW-131.1, QZERO-3) shipped on the strength of the ear alone and none of
 * them changed anything. The hypothesis under test is "click = current x commutation-angle jump",
 * and it is decided by whether ANY current flows in the speed zone where the angle steps.
 *
 * A measurement is only worth the ride it costs if it cannot quietly measure nothing. Two of this
 * project's diagnostics already failed that way - FW-121.0's frames were dropped between the
 * builder and the wire, and FW-106's aggregate turned out to be one snapshot per session when the
 * instruction said "capture it live". Both cost a ride. So the guards below are aimed less at the
 * arithmetic than at the ways this measurement could silently produce nothing useful.
 *
 * main.c and CAN_Display.c are the ARM entry point and the CAN receive path and cannot be linked
 * on a host (same reasoning as armed_zero_lifecycle_host.c), so the wiring is proven against the
 * production source text and the gating arithmetic against a replica pinned to it.
 *
 *   T1  BOTH GATES              the peak needs a zero reference AND a rotor below the switch.
 *                               Either gate alone measures the wrong thing entirely.
 *   T2  SAME IN A AND B         the measurement sits OUTSIDE QUIET_ZERO_ENABLE. If it did not,
 *                               the A/B comparison it exists for would be meaningless.
 *   T3  CUMULATIVE              nothing resets the counters, so the number of stops and their
 *                               order cannot change the answer.
 *   T4  PRESENT IN EVERY BUILD  READ 0x6032 is answered outside CAN_DIAGNOSTICS_ENABLE - the
 *                               image that clicks is the image that gets measured.
 *   T5  READ IS INERT           a status read must not disturb what it reports.
 *   T6  WIRE FORMAT             one 8-byte frame, little-endian, on the documented id.
 *   T7  REPLICA BEHAVIOUR       the gating actually does what T1 claims, including that a peak
 *                               never decreases.
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
/* Replica of the production gating (main.c, runPIcontrol()). Pinned to the real text by T1.     */
/* ------------------------------------------------------------------------------------------- */

#define REPLICA_COAST_RELEASE_ERPS 10

typedef struct {
	uint16_t release_count;
	uint16_t peak_iq;
	uint8_t  prev_ref_nonzero;
} replica_t;

static void replica_tick(replica_t *r, int32_t iq_ref, int32_t iq_measured,
	int32_t erps, bool policy_quiet)
{
	uint8_t ref_nonzero = (iq_ref != 0) ? 1U : 0U;
	if (r->prev_ref_nonzero && !ref_nonzero && policy_quiet) {
		if (r->release_count < 0xFFFFU) r->release_count++;
	}
	r->prev_ref_nonzero = ref_nonzero;

	if (!ref_nonzero && erps < REPLICA_COAST_RELEASE_ERPS) {
		int32_t meas = iq_measured;
		if (meas < 0) meas = -meas;
		if (meas > 0xFFFF) meas = 0xFFFF;
		if ((uint16_t)meas > r->peak_iq) r->peak_iq = (uint16_t)meas;
	}
}

int main(void)
{
	printf("FW-136.0 click-zone measurement - wiring guards + gating replica\n");

	char *mainc = load(STRINGIZE(MAIN_C_PATH));
	char *canc  = load(STRINGIZE(CAN_DISPLAY_C_PATH));
	CHECK(mainc && canc, "sources readable");
	if (!mainc || !canc) {
		printf("\n1 FW-136.0 check FAILED (sources unreadable).\n");
		return 1;
	}

	/* ==== T1: both gates, and the replica above matches the production text ==== */
	{
		const char *gate = strstr(mainc,
			"if(!ref_nonzero && (int32_t)ui16_erps < (int32_t)RIDE_COAST_RELEASE_ERPS){");
		CHECK(gate != NULL,
			"T1: the peak is gated on a ZERO reference AND a rotor below the commutation switch");
		CHECK(strstr(mainc, "if(meas < 0) meas = -meas;") != NULL,
			"T1: the peak is of the MAGNITUDE - a braking current is negative and must still count");
		CHECK(strstr(mainc, "if((uint16_t)meas > click_zone_peak_iq) click_zone_peak_iq = (uint16_t)meas;")
			!= NULL,
			"T1: and it is a running maximum, matching the replica in this file");
	}

	/* ==== T2: the measurement must be identical in both A/B images ==== */
	{
		const char *fn = strstr(mainc, "void runPIcontrol(void){");
		CHECK(fn != NULL, "T2: runPIcontrol() found");
		const char *meas = fn ? strstr(fn, "static uint8_t click_prev_ref_nonzero = 0;") : NULL;
		const char *qz_gate = fn ? strstr(fn, "#if QUIET_ZERO_ENABLE") : NULL;
		CHECK(meas != NULL, "T2: the measurement block is inside runPIcontrol()");
		CHECK(qz_gate != NULL, "T2: the QZERO A/B gate is still there");
		CHECK(meas != NULL && qz_gate != NULL && meas < qz_gate,
			"T2: the measurement sits BEFORE the QUIET_ZERO_ENABLE gate, so variant A and variant B "
			"measure the same thing the same way - otherwise the comparison is worthless");
	}

	/* ==== T3: cumulative - the only assignment to zero is the definition ==== */
	{
		CHECK(count_occurrences(mainc, "click_zone_peak_iq=0") == 1,
			"T3: click_zone_peak_iq is zeroed exactly once, at its definition - nothing resets it "
			"between stops, so neither the number of stops nor their order changes the answer");
		CHECK(count_occurrences(mainc, "click_release_count=0") == 1,
			"T3: same for the release counter");
		CHECK(count_occurrences(mainc, "click_handback_count=0") == 1,
			"T3: same for the handback counter");
		CHECK(strstr(mainc, "if(click_release_count < 0xFFFFU) click_release_count++;") != NULL &&
			strstr(mainc, "if(click_handback_count < 0xFFFFU) click_handback_count++;") != NULL,
			"T3: both counters saturate instead of wrapping - a wrapped counter reads as a small "
			"number and would be believed");
	}

	/* ==== T4: answered in every build, not only DIAG ==== */
	{
		const char *rd = strstr(canc, "case READ_CMD:");
		CHECK(rd != NULL, "T4: the READ dispatch exists");
		const char *ours = rd ? strstr(rd, "Ext_ID_Rx.command==0x6032") : NULL;
		const char *diag_gate = rd ? strstr(rd, "#if CAN_DIAGNOSTICS_ENABLE") : NULL;
		CHECK(ours != NULL, "T4: READ 0x6032 is dispatched");
		CHECK(ours != NULL && (diag_gate == NULL || ours < diag_gate),
			"T4: and it is dispatched BEFORE the diagnostics gate, so the NORMAL image - the one "
			"that actually clicks - answers it too");
		CHECK(strstr(canc, "Ext_ID_Rx.command==0x6032 && Ext_ID_Rx.source==5U && receive_message.rx_dlen==0U")
			!= NULL,
			"T4: tool only and zero-length, like every other side-effect-free status read here");
	}

	/* ==== T5: reading must not disturb what it reports ==== */
	{
		const char *fn = strstr(canc, "static void send_click_zone_status(void)");
		CHECK(fn != NULL, "T5: the responder exists");
		const char *end = fn ? strstr(fn, "can_tx_queue_enqueue(0x022A6032U, 8U, d);") : NULL;
		CHECK(end != NULL, "T5: and it enqueues exactly one frame");
		if (fn && end) {
			size_t span = (size_t)(end - fn);
			char *body = (char *)malloc(span + 1U);
			memcpy(body, fn, span);
			body[span] = '\0';
			CHECK(strstr(body, "++") == NULL && strstr(body, "= 0") == NULL,
				"T5: the responder only reads the counters - a status read that alters what it "
				"reports cannot be trusted twice in one session");
			free(body);
		}
	}

	/* ==== T6: the documented wire format ==== */
	{
		CHECK(strstr(canc, "d[0] = (uint8_t)(click_release_count & 0xFFU);") != NULL &&
			strstr(canc, "d[1] = (uint8_t)((click_release_count >> 8) & 0xFFU);") != NULL,
			"T6: little-endian, low byte first - the 0x60xx family convention, NOT the big-endian "
			"diag logger frames");
		CHECK(strstr(canc, "d[4] = (uint8_t)(click_zone_peak_iq & 0xFFU);") != NULL,
			"T6: the peak current is in bytes 4..5, where the card says it is");
		CHECK(strstr(canc, "d[6] = (uint8_t)(click_handback_erps & 0xFFU);") != NULL,
			"T6: and the handback speed in bytes 6..7");
	}

	/* ==== T7: the gating behaves as claimed ==== */
	{
		replica_t r;
		memset(&r, 0, sizeof r);

		/* Riding: current under power, well above the switch. Must not be recorded. */
		replica_tick(&r, 200, 180, 90, false);
		replica_tick(&r, 200, 195, 85, false);
		CHECK(r.peak_iq == 0,
			"T7: current drawn while RIDING is not click-zone current - the reference is not zero");

		/* Crawling under power below the switch: still a demand, still not the click zone. */
		replica_tick(&r, 40, 38, 4, false);
		CHECK(r.peak_iq == 0,
			"T7: crawling under power below the switch is not the click zone either - without the "
			"zero-reference gate this would record ordinary slow riding");

		/* The release: reference reaches zero on a quiet policy. */
		replica_tick(&r, 0, 30, 60, true);
		CHECK(r.release_count == 1, "T7: the release edge is counted once");
		CHECK(r.peak_iq == 0,
			"T7: and current still flowing ABOVE the switch is not click-zone current");

		/* Coasting down through the switch with current still flowing - the click zone. */
		replica_tick(&r, 0, 12, 9, true);
		CHECK(r.peak_iq == 12, "T7: current below the switch with a zero reference IS recorded");

		replica_tick(&r, 0, -25, 5, true);
		CHECK(r.peak_iq == 25, "T7: a negative (braking) current counts by magnitude");

		replica_tick(&r, 0, 3, 2, true);
		CHECK(r.peak_iq == 25,
			"T7: a later smaller sample never lowers the peak - one snapshot per session has to "
			"survive everything that happened before it");

		/* A second stop keeps accumulating rather than starting over. */
		replica_tick(&r, 150, 140, 70, false);
		replica_tick(&r, 0, 0, 60, true);
		CHECK(r.release_count == 2, "T7: the next release is counted too");
		CHECK(r.peak_iq == 25, "T7: and the peak survives into the next stop");
	}

	free(mainc); free(canc);

	if (host_test_failures == 0) {
		printf("All FW-136.0 click-zone measurement checks passed.\n");
		return 0;
	}
	printf("\n%d FW-136.0 click-zone measurement check(s) FAILED.\n", host_test_failures);
	return 1;
}
