/*
 * Persistent ARMED_ZERO lifecycle host simulation and production wiring guard.
 *
 * The small model covers the observable contract for cases A-G.  The source
 * checks bind those modelled transitions to main.c/main.h/FOC.c, which cannot
 * be linked on a host because they directly own the GD32 entry point and ISR.
 */

#include "../common/check.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)

#ifndef MAIN_C_PATH
#error "MAIN_C_PATH must be supplied by run-host-tests.ps1"
#endif
#ifndef MAIN_H_PATH
#error "MAIN_H_PATH must be supplied by run-host-tests.ps1"
#endif
#ifndef FOC_C_PATH
#error "FOC_C_PATH must be supplied by run-host-tests.ps1"
#endif

#define MODEL_POWER_STAGE_STOP_TICKS 4000U

typedef enum {
	MODEL_DISABLED,
	MODEL_PREPARE,
	MODEL_ACTIVE,
	MODEL_ARMED_ZERO,
	MODEL_FAULT
} model_lifecycle_t;

typedef struct {
	model_lifecycle_t lifecycle;
	bool moe;
	bool foc_alive;
	bool neutral_dwell;
	unsigned moe_enable_count;
	unsigned moe_disable_count;
	unsigned standstill_seed_count;
	unsigned neutral_dwell_rearm_count;
} model_bridge_t;

static void model_set_moe(model_bridge_t *bridge, bool enabled)
{
	if (bridge->moe == enabled) return;
	bridge->moe = enabled;
	if (enabled) bridge->moe_enable_count++;
	else bridge->moe_disable_count++;
}

static void model_update(model_bridge_t *bridge, int iq_ref, bool hard_fault)
{
	if (hard_fault) {
		model_set_moe(bridge, false);
		bridge->foc_alive = false;
		bridge->neutral_dwell = false;
		bridge->lifecycle = MODEL_FAULT;
		return;
	}

	if (bridge->lifecycle == MODEL_DISABLED && iq_ref > 0) {
		bridge->lifecycle = MODEL_PREPARE;
		bridge->standstill_seed_count++;
		bridge->neutral_dwell_rearm_count++;
		bridge->neutral_dwell = true;
		model_set_moe(bridge, true);
		bridge->foc_alive = true;
		bridge->neutral_dwell = false;
		bridge->lifecycle = MODEL_ACTIVE;
		return;
	}
	if (bridge->lifecycle == MODEL_ACTIVE && iq_ref == 0) {
		bridge->lifecycle = MODEL_ARMED_ZERO;
		return;
	}
	if (bridge->lifecycle == MODEL_ARMED_ZERO && iq_ref > 0) {
		bridge->lifecycle = MODEL_ACTIVE;
	}
}

static void model_shutdown(model_bridge_t *bridge)
{
	model_set_moe(bridge, false);
	bridge->foc_alive = false;
	bridge->neutral_dwell = false;
	bridge->lifecycle = MODEL_DISABLED;
}

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
	long i = 0;
	while (i < len) {
		if (text[i] == '/' && i + 1 < len && text[i + 1] == '/') {
			while (i < len && text[i] != '\n') { out[i] = ' '; i++; }
			continue;
		}
		if (text[i] == '/' && i + 1 < len && text[i + 1] == '*') {
			out[i++] = ' '; out[i++] = ' ';
			while (i < len && !(text[i] == '*' && i + 1 < len && text[i + 1] == '/')) {
				out[i] = (text[i] == '\n') ? '\n' : ' ';
				i++;
			}
			if (i < len) out[i++] = ' ';
			if (i < len) out[i++] = ' ';
			continue;
		}
		out[i] = text[i];
		i++;
	}
	out[len] = '\0';
	return out;
}

static bool span_contains(const char *first, const char *last, const char *needle)
{
	const char *found = strstr(first, needle);
	return found != NULL && found < last;
}

static void model_cases(void)
{
	model_bridge_t bridge = {0};

	/* CASE D: cold start remains the only PREPARE/MOE-on sequence. */
	model_update(&bridge, 1, false);
	CHECK(bridge.lifecycle == MODEL_ACTIVE && bridge.moe && bridge.foc_alive,
		"D: cold start reaches ACTIVE with MOE and FOC on");
	CHECK(bridge.moe_enable_count == 1U && bridge.standstill_seed_count == 1U &&
		bridge.neutral_dwell_rearm_count == 1U,
		"D: cold start retains one PREPARE/theta-seed/neutral-dwell sequence");

	/* CASE A: the former Hall timeout cannot disarm normal zero torque. */
	model_update(&bridge, 0, false);
	for (unsigned tick = 0; tick <= MODEL_POWER_STAGE_STOP_TICKS; ++tick) {
		model_update(&bridge, 0, false);
	}
	CHECK(bridge.lifecycle == MODEL_ARMED_ZERO && bridge.moe && bridge.foc_alive &&
		!bridge.neutral_dwell,
		"A: > POWER_STAGE_STOP_TICKS at zero remains ARMED_ZERO, MOE on and FOC alive");
	CHECK(bridge.moe_enable_count == 1U && bridge.moe_disable_count == 0U,
		"A: zero torque has no MOE edge");

	/* CASE B: persistence is independent of elapsed Hall-silence time. */
	for (unsigned tick = 0; tick < 10U * MODEL_POWER_STAGE_STOP_TICKS; ++tick) {
		model_update(&bridge, 0, false);
	}
	CHECK(bridge.lifecycle == MODEL_ARMED_ZERO && bridge.moe && bridge.foc_alive &&
		bridge.moe_disable_count == 0U,
		"B: ARMED_ZERO remains armed far beyond the old timeout");

	/* CASE C: positive re-demand is a direct ACTIVE transition, not cold start. */
	const unsigned moe_edges_before = bridge.moe_enable_count + bridge.moe_disable_count;
	const unsigned seeds_before = bridge.standstill_seed_count;
	const unsigned dwells_before = bridge.neutral_dwell_rearm_count;
	model_update(&bridge, 1, false);
	CHECK(bridge.lifecycle == MODEL_ACTIVE && bridge.moe && bridge.foc_alive,
		"C: positive demand returns ARMED_ZERO directly to ACTIVE");
	CHECK(bridge.moe_enable_count + bridge.moe_disable_count == moe_edges_before &&
		bridge.standstill_seed_count == seeds_before &&
		bridge.neutral_dwell_rearm_count == dwells_before,
		"C: ARMED_ZERO re-demand has no MOE edge, theta seed or dwell rearm");

	/* CASE E: existing hard-off remains immediate from ACTIVE. */
	model_update(&bridge, 1, true);
	CHECK(bridge.lifecycle == MODEL_FAULT && !bridge.moe && !bridge.foc_alive &&
		bridge.moe_disable_count == 1U,
		"E: hard fault from ACTIVE still forces MOE off");

	/* CASE F: the same independent hard-off applies from ARMED_ZERO. */
	memset(&bridge, 0, sizeof(bridge));
	model_update(&bridge, 1, false);
	model_update(&bridge, 0, false);
	model_update(&bridge, 0, true);
	CHECK(bridge.lifecycle == MODEL_FAULT && !bridge.moe && !bridge.foc_alive &&
		bridge.moe_disable_count == 1U,
		"F: hard fault from ARMED_ZERO still forces MOE off");

	/* CASE G: the pre-existing explicit system shutdown stays coherent. */
	memset(&bridge, 0, sizeof(bridge));
	model_update(&bridge, 1, false);
	model_shutdown(&bridge);
	CHECK(bridge.lifecycle == MODEL_DISABLED && !bridge.moe && !bridge.foc_alive &&
		bridge.moe_disable_count == 1U,
		"G: explicit system shutdown remains a coherent MOE-off path");
}

static void production_wiring_checks(void)
{
	long main_len = 0, main_h_len = 0, foc_len = 0;
	char *main_raw = read_whole_file(STRINGIZE(MAIN_C_PATH), &main_len);
	char *main_h_raw = read_whole_file(STRINGIZE(MAIN_H_PATH), &main_h_len);
	char *foc_raw = read_whole_file(STRINGIZE(FOC_C_PATH), &foc_len);
	CHECK(main_raw != NULL && main_h_raw != NULL && foc_raw != NULL,
		"setup: production lifecycle sources are readable");
	if (!main_raw || !main_h_raw || !foc_raw) goto done;

	char *main_c = strip_comments(main_raw, main_len);
	char *main_h = strip_comments(main_h_raw, main_h_len);
	char *foc_c = strip_comments(foc_raw, foc_len);
	CHECK(main_c != NULL && main_h != NULL && foc_c != NULL,
		"setup: production lifecycle sources sanitize successfully");
	if (!main_c || !main_h || !foc_c) { free(main_c); free(main_h); free(foc_c); goto done; }

	CHECK(strstr(main_h, "#define BRIDGE_LIFECYCLE_ARMED_ZERO      6U") != NULL &&
		strstr(main_h, "#define BRIDGE_LIFECYCLE_FAULT           7U") != NULL,
		"wiring: shared lifecycle declares ARMED_ZERO and FAULT");
	CHECK(strstr(main_h, "extern uint8_t ui_8_PWM_ON_Flag") != NULL &&
		strstr(main_h, "extern uint8_t bridge_lifecycle") != NULL,
		"wiring: bridge/MOE mirror and lifecycle are shared with hard-fault code");
	CHECK(strstr(main_c, "POWER_STAGE_STOP_TICKS") == NULL,
		"wiring: main.c has no Hall-silence normal-disarm condition");

	const char *zero_transition = strstr(main_c,
		"bridge_lifecycle == BRIDGE_LIFECYCLE_RUN && MS.i_q_setpoint == 0");
	const char *armed_transition = strstr(main_c,
		"bridge_lifecycle == BRIDGE_LIFECYCLE_ARMED_ZERO && MS.i_q_setpoint > 0");
	const char *dwell_failsafe = strstr(main_c, "static uint16_t dwell_timeout_counter");
	CHECK(zero_transition != NULL && armed_transition != NULL && dwell_failsafe != NULL,
		"wiring: ACTIVE->ARMED_ZERO and ARMED_ZERO->ACTIVE transitions exist before dwell failsafe");
	if (zero_transition && dwell_failsafe) {
		CHECK(!span_contains(zero_transition, dwell_failsafe,
			"timer_primary_output_config(TIMER0,DISABLE)"),
			"wiring: normal zero transition has no MOE disable");
		CHECK(!span_contains(zero_transition, dwell_failsafe, "ui_8_PWM_ON_Flag=0"),
			"wiring: normal zero transition keeps the bridge/MOE mirror asserted");
	}
	if (armed_transition && dwell_failsafe) {
		CHECK(!span_contains(armed_transition, dwell_failsafe,
			"timer_primary_output_config(TIMER0,ENABLE)") &&
			!span_contains(armed_transition, dwell_failsafe, "get_standstill_position()") &&
			!span_contains(armed_transition, dwell_failsafe, "neutral_dwell_active = 1"),
			"wiring: ARMED_ZERO re-demand has no MOE enable, theta seed or neutral dwell");
	}

	CHECK(strstr(main_c, "if(ui_8_PWM_ON_Flag){") != NULL &&
		strstr(main_c, "FOC_calculation(") != NULL,
		"wiring: active bridge flag continues to gate FOC/current feedback");

	const char *power_off = strstr(main_c, "void power_off_controller(void){");
	CHECK(power_off != NULL && span_contains(power_off, power_off + 1200,
		"timer_primary_output_config(TIMER0,DISABLE)") &&
		span_contains(power_off, power_off + 1200, "ui_8_PWM_ON_Flag=0"),
		"wiring: explicit system shutdown retains coherent MOE-off/mirror-off path");

	const char *hard_off = strstr(foc_c, "if(MS_FOC->i_d>(PH_CURRENT_MAX<<2))");
	CHECK(hard_off != NULL && span_contains(hard_off, hard_off + 900,
		"timer_primary_output_config(TIMER0,DISABLE)") &&
		span_contains(hard_off, hard_off + 900, "bridge_lifecycle=BRIDGE_LIFECYCLE_FAULT") &&
		span_contains(hard_off, hard_off + 900, "ui_8_PWM_ON_Flag=0"),
		"wiring: hard overcurrent still disables MOE and records coherent FAULT state");

	free(main_c);
	free(main_h);
	free(foc_c);
done:
	free(main_raw);
	free(main_h_raw);
	free(foc_raw);
}

int main(void)
{
	puts("Persistent ARMED_ZERO lifecycle host simulation");
	model_cases();
	production_wiring_checks();
	if (host_test_failures == 0) {
		puts("Persistent ARMED_ZERO lifecycle: ALL CHECKS PASSED");
		return 0;
	}
	printf("Persistent ARMED_ZERO lifecycle: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
