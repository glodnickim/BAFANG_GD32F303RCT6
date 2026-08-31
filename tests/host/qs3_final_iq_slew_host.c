/*
 * QS-3 final-Iq slew proof.
 *
 * Tests 1--6 and the deterministic elapsed-tick repair run the real production
 * assist_dynamics.c.  Tests 7--10 combine that real ramp with a deliberately
 * small lifecycle model and source-wiring checks: main.c cannot be linked in a
 * host executable because it is the GD32 entry point and owns live peripherals.
 */

#include "../common/check.h"

#include "assist_dynamics.h"

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
#ifndef RIDE_CONTROL_C_PATH
#error "RIDE_CONTROL_C_PATH must be supplied by run-host-tests.ps1"
#endif
#ifndef ASSIST_DYNAMICS_C_PATH
#error "ASSIST_DYNAMICS_C_PATH must be supplied by run-host-tests.ps1"
#endif
#ifndef MOTOR_CORE_C_PATH
#error "MOTOR_CORE_C_PATH must be supplied by run-host-tests.ps1"
#endif

#define IQ_SCALE 700
#define RISE_SLOW_MS 600U
#define RISE_FAST_MS 300U
#define FALL_SLOW_MS 1000U
#define FALL_FAST_MS 180U
#define RELEASE_MS 650U
#define HARD_RELEASE_MS 200U
#define CONTROL_TICKS_PER_MS 4U

static assist_dynamics_input_t normal_input(void)
{
	assist_dynamics_input_t input;
	memset(&input, 0, sizeof(input));
	input.speed_x100 = 0U;             /* adaptive path selects the shipped slow values */
	input.cadence_rpm = 0U;
	input.iq_scale = IQ_SCALE;
	input.phase_current_max = IQ_SCALE;
	input.profile_pedaling_active = true;
	input.ramp_up_slow_ms = RISE_SLOW_MS;
	input.ramp_up_fast_ms = RISE_FAST_MS;
	input.ramp_down_slow_ms = FALL_SLOW_MS;
	input.ramp_down_fast_ms = FALL_FAST_MS;
	input.elapsed_ticks = 1U;
	return input;
}

static unsigned ramp_until(int32_t target, int32_t *reference,
	const assist_dynamics_input_t *input, unsigned maximum_ticks)
{
	unsigned ticks = 0U;
	while (*reference != target && ticks < maximum_ticks) {
		*reference = assist_dynamics_apply(target, *reference, input);
		ticks++;
	}
	return ticks;
}

static int32_t prime_reference(int32_t target, assist_dynamics_input_t *input)
{
	int32_t reference = 0;
	assist_dynamics_reset();
	unsigned ticks = ramp_until(target, &reference, input, 10000U);
	CHECK(reference == target && ticks < 10000U,
		"setup: real final-Iq ramp reaches its requested starting reference");
	return reference;
}

static void test_1_rise(void)
{
	assist_dynamics_input_t input = normal_input();
	int32_t reference = 0;
	int32_t previous = 0;
	int32_t first_visible_increment = -1;
	unsigned ticks;

	assist_dynamics_reset();
	for (ticks = 0U; ticks < 4000U && reference != IQ_SCALE; ticks++) {
		reference = assist_dynamics_apply(IQ_SCALE, reference, &input);
		CHECK(reference >= previous && reference <= IQ_SCALE,
			"T1: 0->100% is monotonic and bounded by its target");
		if (first_visible_increment < 0 && reference > previous) {
			first_visible_increment = reference - previous;
		}
		previous = reference;
	}
	CHECK(reference == IQ_SCALE, "T1: 0->100% clamps exactly at the target");
	CHECK(ticks >= 2380U && ticks <= 2400U,
		"T1: shipped 600 ms slow-rise completes at the expected Q8-rounded duration");
	CHECK(first_visible_increment >= 0 && first_visible_increment <= 1,
		"T1: first visible Iq_ref increment is at most one Iq count");
}

static unsigned ordinary_fall_ticks(int32_t start_target)
{
	assist_dynamics_input_t input = normal_input();
	int32_t reference = prime_reference(start_target, &input);
	int32_t previous = reference;
	unsigned ticks = 0U;
	while (reference != 0 && ticks < 6000U) {
		reference = assist_dynamics_apply(0, reference, &input);
		CHECK(reference <= previous && reference >= 0,
			"T2/T3: ordinary fall is monotonic, non-negative and has no overshoot");
		previous = reference;
		ticks++;
	}
	CHECK(reference == 0, "T2/T3: ordinary fall clamps at exact zero");
	return ticks;
}

static void test_2_and_3_fall(void)
{
	unsigned full_ticks = ordinary_fall_ticks(IQ_SCALE);
	unsigned partial_ticks = ordinary_fall_ticks(IQ_SCALE / 5);
	CHECK(full_ticks >= 3970U && full_ticks <= 4000U,
		"T2: 100%->0 uses the shipped 1000 ms ordinary-fall slope");
	CHECK(partial_ticks >= 780U && partial_ticks <= 810U,
		"T3: 20%->0 has the shorter amplitude-dependent duration of a constant slope");
	CHECK(partial_ticks * 5U >= full_ticks - 20U &&
		partial_ticks * 5U <= full_ticks + 20U,
		"T3: 20% ordinary fall is one fifth of the full-scale fall within Q8 rounding");
}

static void test_4_and_5_partial_transitions(void)
{
	assist_dynamics_input_t input = normal_input();
	int32_t reference = prime_reference(IQ_SCALE / 2, &input);
	unsigned rise_ticks = ramp_until(IQ_SCALE, &reference, &input, 3000U);
	CHECK(reference == IQ_SCALE && rise_ticks >= 1180U && rise_ticks <= 1200U,
		"T4: 50%->100% keeps the same normalized rise slope");

	reference = prime_reference(IQ_SCALE, &input);
	unsigned fall_ticks = ramp_until(IQ_SCALE / 2, &reference, &input, 5000U);
	CHECK(reference == IQ_SCALE / 2 && fall_ticks >= 1980U && fall_ticks <= 2000U,
		"T5: 100%->50% keeps the same normalized fall slope");
}

static void test_6_direction_change(void)
{
	assist_dynamics_input_t input = normal_input();
	int32_t reference = 0;
	int32_t before;

	assist_dynamics_reset();
	for (unsigned tick = 0U; tick < 200U; tick++) {
		reference = assist_dynamics_apply(IQ_SCALE, reference, &input);
	}
	before = reference;
	reference = assist_dynamics_apply(10, reference, &input);
	CHECK(reference <= before && reference >= 10,
		"T6: changing rise to fall cannot create a positive stale-accumulator jump");
	(void)ramp_until(10, &reference, &input, 3000U);
	CHECK(reference == 10, "T6: reversed target clamps exactly without a stale tail");
	before = reference;
	reference = assist_dynamics_apply(IQ_SCALE, reference, &input);
	CHECK(reference >= before && reference <= before + 1,
		"T6: changing fall to rise restarts through the same small bounded step");
}

static void test_elapsed_tick_parity(void)
{
	assist_dynamics_input_t per_tick = normal_input();
	assist_dynamics_input_t burst = normal_input();
	int32_t expected = 0;
	int32_t observed;

	assist_dynamics_reset();
	for (unsigned tick = 0U; tick < 64U; tick++) {
		expected = assist_dynamics_apply(IQ_SCALE, expected, &per_tick);
	}
	assist_dynamics_reset();
	burst.elapsed_ticks = 64U;
	observed = assist_dynamics_apply(IQ_SCALE, 0, &burst);
	CHECK(observed == expected,
		"QS-3: one 64-tick hardware interval advances the real ramp exactly like 64 normal ticks");
}

static void test_8_normal_release(void)
{
	assist_dynamics_input_t input = normal_input();
	int32_t reference = prime_reference(IQ_SCALE, &input);
	unsigned ticks;

	input.profile_pedaling_active = false;
	input.profile_release_ms = RELEASE_MS;
	ticks = ramp_until(0, &reference, &input, 4000U);
	CHECK(reference == 0 && ticks >= 2580U && ticks <= RELEASE_MS * CONTROL_TICKS_PER_MS,
		"T8: normal rider release reaches exact zero in its configured 650 ms envelope");
}

static void test_9_hard_release_and_bypasses(void)
{
	assist_dynamics_input_t input = normal_input();
	int32_t reference = prime_reference(IQ_SCALE, &input);
	unsigned ticks;

	input.profile_pedaling_active = false;
	input.profile_release_ms = HARD_RELEASE_MS;
	ticks = ramp_until(0, &reference, &input, 1600U);
	CHECK(reference == 0 && ticks >= 790U && ticks <= HARD_RELEASE_MS * CONTROL_TICKS_PER_MS,
		"T9: hard safety release keeps its independent 200 ms exact-zero envelope");

	reference = prime_reference(IQ_SCALE, &input);
	input.coast_release = true;
	CHECK(assist_dynamics_apply(0, reference, &input) == 0,
		"T9: coast release remains the existing exact-zero bypass");

	assist_dynamics_reset();
	input = normal_input();
	input.walk_active = true;
	CHECK(assist_dynamics_apply(37, 0, &input) == 37,
		"T9: Walk Assist keeps ownership of its existing trajectory bypass");
}

typedef enum {
	MODEL_DISABLED,
	MODEL_ACTIVE,
	MODEL_ARMED_ZERO
} model_lifecycle_t;

typedef struct {
	model_lifecycle_t lifecycle;
	bool moe;
	bool foc_alive;
	unsigned prepare_count;
	unsigned fw117_count;
	unsigned moe_edges;
} model_bridge_t;

static void model_apply_iq(model_bridge_t *bridge, int32_t iq_ref)
{
	if (bridge->lifecycle == MODEL_DISABLED && iq_ref > 0) {
		bridge->lifecycle = MODEL_ACTIVE;
		bridge->moe = true;
		bridge->foc_alive = true;
		bridge->prepare_count++;
		bridge->fw117_count++;
		bridge->moe_edges++;
	} else if (bridge->lifecycle == MODEL_ACTIVE && iq_ref == 0) {
		bridge->lifecycle = MODEL_ARMED_ZERO;
	} else if (bridge->lifecycle == MODEL_ARMED_ZERO && iq_ref > 0) {
		bridge->lifecycle = MODEL_ACTIVE;
	}
}

static char *read_whole_file(const char *path, long *out_len)
{
	FILE *file = fopen(path, "rb");
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
	long length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
	char *text = (char *)malloc((size_t)length + 1U);
	if (!text) { fclose(file); return NULL; }
	size_t read = fread(text, 1U, (size_t)length, file);
	fclose(file);
	text[read] = '\0';
	if (out_len) *out_len = (long)read;
	return text;
}

static char *strip_comments(const char *text, long length)
{
	char *out = (char *)malloc((size_t)length + 1U);
	if (!out) return NULL;
	long i = 0;
	while (i < length) {
		if (text[i] == '/' && i + 1 < length && text[i + 1] == '/') {
			while (i < length && text[i] != '\n') { out[i] = ' '; i++; }
			continue;
		}
		if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
			out[i++] = ' '; out[i++] = ' ';
			while (i < length && !(text[i] == '*' && i + 1 < length && text[i + 1] == '/')) {
				out[i] = (text[i] == '\n') ? '\n' : ' ';
				i++;
			}
			if (i < length) out[i++] = ' ';
			if (i < length) out[i++] = ' ';
			continue;
		}
		out[i] = text[i];
		i++;
	}
	out[length] = '\0';
	return out;
}

static void test_7_and_10_lifecycle_and_wiring(void)
{
	model_bridge_t bridge = {0};
	assist_dynamics_input_t input = normal_input();
	int32_t first_reference;
	long main_length = 0, ride_length = 0, dynamics_length = 0, motor_length = 0;
	char *main_raw = read_whole_file(STRINGIZE(MAIN_C_PATH), &main_length);
	char *ride_raw = read_whole_file(STRINGIZE(RIDE_CONTROL_C_PATH), &ride_length);
	char *dynamics_raw = read_whole_file(STRINGIZE(ASSIST_DYNAMICS_C_PATH), &dynamics_length);
	char *motor_raw = read_whole_file(STRINGIZE(MOTOR_CORE_C_PATH), &motor_length);

	assist_dynamics_reset();
	first_reference = assist_dynamics_apply(IQ_SCALE, 0, &input);
	model_apply_iq(&bridge, 1);
	model_apply_iq(&bridge, 0);
	unsigned edges_before = bridge.moe_edges;
	unsigned prepare_before = bridge.prepare_count;
	unsigned fw117_before = bridge.fw117_count;
	model_apply_iq(&bridge, first_reference > 0 ? first_reference : 1);
	CHECK(first_reference >= 0 && first_reference <= 1,
		"T7: ARMED_ZERO restart uses the existing at-most-one-count first Iq_ref step");
	CHECK(bridge.lifecycle == MODEL_ACTIVE && bridge.moe && bridge.foc_alive &&
		bridge.moe_edges == edges_before && bridge.prepare_count == prepare_before &&
		bridge.fw117_count == fw117_before,
		"T7: ARMED_ZERO->positive model has no MOE toggle, cold PREPARE or FW117 rerun");
	CHECK(bridge.prepare_count == 1U && bridge.fw117_count == 1U,
		"T10: cold start remains the sole PREPARE/FW117 path in the lifecycle model");

	CHECK(main_raw != NULL && ride_raw != NULL && dynamics_raw != NULL && motor_raw != NULL,
		"setup: QS-3 production sources are readable");
	if (main_raw && ride_raw && dynamics_raw && motor_raw) {
		char *main_c = strip_comments(main_raw, main_length);
		char *ride_c = strip_comments(ride_raw, ride_length);
		char *dynamics_c = strip_comments(dynamics_raw, dynamics_length);
		char *motor_c = strip_comments(motor_raw, motor_length);
		CHECK(main_c != NULL && ride_c != NULL && dynamics_c != NULL && motor_c != NULL,
			"setup: QS-3 production sources sanitize successfully");
		if (main_c && ride_c && dynamics_c && motor_c) {
		CHECK(strstr(main_c, "uint32_t control_delta = 1U") != NULL &&
			strstr(main_c, "control_delta = control_now - control_prev_processed_tick") != NULL &&
			strstr(main_c, ".elapsed_ticks = control_delta") != NULL,
			"QS-3 wiring: TIMER1 hardware delta is handed to ride_control");
		CHECK(strstr(ride_c, ".elapsed_ticks = input->elapsed_ticks") != NULL &&
			strstr(dynamics_c, "uint32_t elapsed_ticks = (input->elapsed_ticks == 0U) ? 1U") != NULL &&
			strstr(dynamics_c, "elapsed_step_q") != NULL,
			"QS-3 wiring: the existing final ramp consumes elapsed hardware periods");
		CHECK(strstr(ride_c, "profile_release_ms = RIDE_HARD_CUT_RAMP_MS") != NULL &&
			strstr(ride_c, "if (iq_target == 0 && rider->motor_erps < RIDE_COAST_RELEASE_ERPS)") != NULL,
			"T9 wiring: hard safety release and coast exact-zero policy remain distinct");
		CHECK(strstr(main_c, "bridge_lifecycle == BRIDGE_LIFECYCLE_RUN && MS.i_q_setpoint == 0") != NULL &&
			strstr(main_c, "bridge_lifecycle == BRIDGE_LIFECYCLE_ARMED_ZERO && MS.i_q_setpoint > 0") != NULL,
			"T8 wiring: normal exact zero still enters persistent ARMED_ZERO");
		CHECK(strstr(main_c, "get_standstill_position()") != NULL &&
			strstr(motor_c, "state->i_q_setpoint = command->iq_target") != NULL &&
			strstr(main_c, "PI_iq.setpoint = MP.reverse * i8_reverse_flag * MS.i_q_setpoint;") != NULL &&
			strstr(main_c, "PI_iq.recent_value = MS.i_q") != NULL &&
			strstr(ride_c, "battery_iq_cap_update(") != NULL &&
			strstr(ride_c, "iq_battery_cap") != NULL &&
			strstr(ride_c, "assist_dynamics_apply(") != NULL,
			"T10 wiring: PI_iq stays in Iq domain; battery cap is upstream of the final slew");
		}
		free(main_c); free(ride_c); free(dynamics_c); free(motor_c);
	}
	free(main_raw); free(ride_raw); free(dynamics_raw); free(motor_raw);
}

int main(void)
{
	puts("QS-3 final Iq slew parity host proof");
	test_1_rise();
	test_2_and_3_fall();
	test_4_and_5_partial_transitions();
	test_6_direction_change();
	test_elapsed_tick_parity();
	test_8_normal_release();
	test_9_hard_release_and_bypasses();
	test_7_and_10_lifecycle_and_wiring();
	if (host_test_failures == 0) {
		puts("QS-3 final Iq slew parity: ALL CHECKS PASSED");
		return 0;
	}
	printf("QS-3 final Iq slew parity: %d CHECK(S) FAILED\n", host_test_failures);
	return 1;
}
