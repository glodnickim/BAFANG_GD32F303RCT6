/*
 * FW-137: the rotor speed must fall when the rotor stops, not freeze at its last value.
 *
 * ui16_erps is computed ONLY inside the Hall capture ISR, from the interval between edges. That
 * is a sound way to answer "how fast" and a structurally impossible way to answer "has it
 * stopped": a stop produces no edge, and no edge produces no update, so the last value survives.
 * On top of that the estimate is a 32-edge moving average, which during a slowdown always
 * reports the past - and the past is faster than the present.
 *
 * Measured on the bike (FW-136.0, 2026-09-05): across a whole session including a real pedal
 * release, the LOWEST speed ever reported during a coast was 24. The thresholds waiting below it:
 *
 *     10   FW-048 coast release      "stop feeding current before the angle jumps"
 *     10   QZERO handback            "give the axis back before the angle jumps"
 *      3   FW-041 gear preload       "take up backlash quietly instead of one slap"
 *      0   smooth start              "arm the launch envelope while stopped"
 *
 * None of them could ever be reached. They were not broken; they were never called. Two of the
 * four exist specifically to remove a click - one at the stop, one at the start - which is
 * exactly the symptom that survived three cards of attempted fixes.
 *
 * THE FIX is not a filter. Three Hall sensors 120 degrees apart give six states per electrical
 * revolution, so every edge is exactly 60 degrees, and the time SINCE the last edge is therefore
 * a measurement: if 60 degrees have not been completed in that time, the rotor cannot be turning
 * faster than one edge per that time. An upper bound that follows from the edge NOT arriving.
 *
 *   T1  NEVER RAISES              the bound can only ever lower the estimate.
 *   T2  SILENT AT STEADY SPEED    with the 2x guard band it cannot bind while edges arrive on
 *                                 time, at any speed in the range - this is what makes it safe
 *                                 to put underneath six consumers at once.
 *   T3  DECAYS TO ZERO            a stopped rotor reads zero instead of freezing, and passes
 *                                 through every one of the four thresholds above on the way.
 *   T4  THE MEASURED CASE         starting from the value the bike actually reported (24), the
 *                                 estimate crosses 10 and 3 within milliseconds of the stop.
 *   T5  RE-ACQUIRE IS EXACT       the first edge after a stop reports the true speed, not one
 *                                 thirty-second of it - the bug the clamp would otherwise create.
 *   T6  WALK ASSIST UNTOUCHED     walk_assist_motor takes the raw interval and the edge age and
 *                                 never ui16_erps, so no Walk Assist setting can be affected.
 *   T7  PRODUCTION WIRING         the clamp is in the periodic path, not the ISR - an ISR that
 *                                 does not fire cannot fix a problem caused by it not firing.
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
#ifndef WALK_ASSIST_MOTOR_C_PATH
#error "WALK_ASSIST_MOTOR_C_PATH must be supplied by run-host-tests.ps1"
#endif

#define CONTROL_TIMEBASE_HZ 4000U

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

/* ------------------------------------------------------------------------------------------- */
/* Replica of the production pair: the Hall-ISR update and the periodic edge-age ceiling.        */
/* ------------------------------------------------------------------------------------------- */

typedef struct {
	uint16_t erps;         /* ui16_erps */
	uint32_t cumulated;    /* ui32_erps_cumulated */
	uint16_t age_ticks;    /* ui16_erps_counter */
} erps_model_t;

static void model_reset(erps_model_t *m)
{
	memset(m, 0, sizeof *m);
}

/* One Hall edge, carrying the instantaneous speed that interval represents. */
static void model_edge(erps_model_t *m, uint32_t sample_erps)
{
	if (m->erps == 0U) {
		m->cumulated = sample_erps << 5;      /* re-acquire: history is worthless */
	} else {
		m->cumulated -= m->cumulated >> 5;
		m->cumulated += sample_erps;
	}
	m->erps = (uint16_t)(m->cumulated >> 5);
	m->age_ticks = 0U;
}

/* One 4 kHz control tick with no edge. */
static void model_tick(erps_model_t *m)
{
	if (m->age_ticks < 64000U) m->age_ticks++;
	if (m->erps > 0U) {
		uint32_t edges_per_s = (uint32_t)m->erps * 6U;
		uint32_t expected_ticks = CONTROL_TIMEBASE_HZ / edges_per_s;
		if ((uint32_t)m->age_ticks > (2U * expected_ticks) + 1U) {
			uint32_t ceiling = CONTROL_TIMEBASE_HZ / ((uint32_t)m->age_ticks * 6U);
			if (ceiling < (uint32_t)m->erps) {
				m->erps = (uint16_t)ceiling;
				m->cumulated = (uint32_t)m->erps << 5;
			}
		}
	}
}

/* Settle the model at a constant speed. */
static void model_settle(erps_model_t *m, uint32_t erps)
{
	for (int i = 0; i < 200; i++) model_edge(m, erps);
}

int main(void)
{
	printf("FW-137 edge-age speed ceiling - wiring guards + estimator replica\n");

	char *mainc = load(STRINGIZE(MAIN_C_PATH));
	char *wac   = load(STRINGIZE(WALK_ASSIST_MOTOR_C_PATH));
	CHECK(mainc && wac, "sources readable");
	if (!mainc || !wac) {
		printf("\n1 FW-137 check FAILED (sources unreadable).\n");
		return 1;
	}

	/* ==== T1: the bound can only ever lower the estimate ==== */
	{
		CHECK(strstr(mainc, "if(ceiling < (uint32_t)ui16_erps){") != NULL,
			"T1: the ceiling is applied only when it is BELOW the current estimate - a bound that "
			"could raise the reading would be a second source of speed, not a bound on the first");
	}

	/* ==== T2: silent at steady speed, across the whole usable range ==== */
	{
		CHECK(strstr(mainc, "(2U * expected_ticks) + 1U") != NULL,
			"T2: the 2x guard band is present - without it the bound sits exactly on the steady "
			"state boundary and nibbles at every reading through ordinary jitter");

		/* Walk Assist is limited to 20..60 chainring rpm; erps = rpm * 4/3. Ride speeds go far
		 * higher. Sweep well past both ends. */
		int bound_bit_at_steady = 0;
		for (uint32_t erps = 3; erps <= 400; erps++) {
			erps_model_t m;
			model_reset(&m);
			model_settle(&m, erps);
			uint32_t interval_ticks = CONTROL_TIMEBASE_HZ / (erps * 6U);
			if (interval_ticks == 0U) interval_ticks = 1U;
			/* Ten edges at exactly this speed; the estimate must not move. */
			for (int e = 0; e < 10; e++) {
				for (uint32_t t = 0; t < interval_ticks; t++) model_tick(&m);
				if (m.erps < erps) { bound_bit_at_steady = 1; break; }
				model_edge(&m, erps);
			}
			if (bound_bit_at_steady) {
				printf("  T2 FAIL at steady %u erps (interval %u ticks)\n", erps, interval_ticks);
				break;
			}
		}
		CHECK(!bound_bit_at_steady,
			"T2: at every steady speed from 3 to 400 erps the bound never binds - this is what "
			"makes it safe to place underneath six consumers in one change");
	}

	/* ==== T3: a stopped rotor decays to zero ==== */
	{
		erps_model_t m;
		model_reset(&m);
		model_settle(&m, 60);
		uint16_t prev = m.erps;
		for (int t = 0; t < 4000; t++) {          /* one second with no edge at all */
			model_tick(&m);
			CHECK(m.erps <= prev, "T3: the estimate is monotonically non-increasing while silent");
			prev = m.erps;
		}
		printf("  T3 after 1 s of silence from 60 erps: %u\n", m.erps);
		CHECK(m.erps == 0U,
			"T3: a rotor that has produced no edge for a second reads ZERO - which is what smooth "
			"start's 'motor_erps == 0' has been waiting for since it was written");
	}

	/* ==== T4: the case actually measured on the bike ==== */
	{
		erps_model_t m;
		model_reset(&m);
		model_settle(&m, 24);                     /* the value the bike froze at */

		int ticks_to_10 = -1, ticks_to_3 = -1;
		for (int t = 1; t <= 4000; t++) {
			model_tick(&m);
			if (ticks_to_10 < 0 && m.erps < 10) ticks_to_10 = t;
			if (ticks_to_3  < 0 && m.erps < 3)  ticks_to_3  = t;
		}
		printf("  T4 from the measured 24 erps: below 10 after %d ticks (%.0f ms), "
			"below 3 after %d ticks (%.0f ms)\n",
			ticks_to_10, ticks_to_10 * 0.25, ticks_to_3, ticks_to_3 * 0.25);
		CHECK(ticks_to_10 > 0 && ticks_to_10 < 200,
			"T4: FW-048 and the QZERO handback (threshold 10) become reachable within 50 ms of "
			"the last edge - they were unreachable at any time before this card");
		CHECK(ticks_to_3 > 0 && ticks_to_3 < 800,
			"T4: FW-041 gear preload (threshold 3) becomes reachable within 200 ms");
	}

	/* ==== T5: the first edge after a stop must be exact ==== */
	{
		erps_model_t m;
		model_reset(&m);
		model_settle(&m, 80);
		for (int t = 0; t < 4000; t++) model_tick(&m);
		CHECK(m.erps == 0U, "T5: precondition - the model has declared the rotor stopped");

		model_edge(&m, 50);
		CHECK(m.erps == 50U,
			"T5: the first edge after a stop reports the TRUE speed. Blending it into a collapsed "
			"32-edge average would report 1/32 of it and take 32 edges to recover - trading the "
			"frozen reading for an under-reading at every start");
	}

	/* ==== T6: Walk Assist cannot be affected by any of this ==== */
	{
		CHECK(strstr(wac, "ui16_erps") == NULL,
			"T6: walk_assist_motor.c never references ui16_erps. It builds its own estimate from "
			"the raw Hall interval and uses the edge-age clock for liveness (FW-130/130.1), so no "
			"Walk Assist target - 20 rpm or 60 - can be disturbed by this change");
		CHECK(strstr(mainc, ".motor_hall_ticks = wa_hall_ticks,") != NULL &&
			strstr(mainc, ".motor_erps_age_ticks = wa_erps_age,") != NULL,
			"T6: and the two things it IS given are the raw interval and the edge age");
	}

	/* ==== T7: the fix must live where it still runs when nothing happens ==== */
	{
		const char *ceiling = strstr(mainc, "uint32_t expected_ticks = (uint32_t)CONTROL_TIMEBASE_HZ");
		const char *counter = strstr(mainc, "if(ui16_erps_counter<64000)ui16_erps_counter++;");
		CHECK(ceiling != NULL, "T7: the ceiling exists");
		CHECK(counter != NULL, "T7: the periodic edge-age counter exists");
		CHECK(ceiling != NULL && counter != NULL && counter < ceiling,
			"T7: the ceiling sits in the periodic path right after the edge-age counter, NOT in "
			"the Hall ISR - an interrupt that is not firing cannot correct a value that is wrong "
			"BECAUSE it is not firing");
	}

	free(mainc); free(wac);

	if (host_test_failures == 0) {
		printf("All FW-137 edge-age ceiling checks passed.\n");
		return 0;
	}
	printf("\n%d FW-137 edge-age ceiling check(s) FAILED.\n", host_test_failures);
	return 1;
}
