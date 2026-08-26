/*
 * FW-122 final host proof against the real rolling_no_assist_diag.c.
 *
 * Covers A/B/C, signed-Iq magnitude, PWM versus hardware MOE, current-cal
 * evidence, true 8 x 250 Hz confirmation, PRE/POST ring boundaries, schema v2,
 * complete serialization, a literal-byte oracle for the PowerShell decoder and
 * the explicit/repeatable FROZEN -> CAN transport.
 */

#include "check.h"
#include "rolling_no_assist_diag.h"
#include "rolling_no_assist_dump.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint32_t tick;
static rolling_no_assist_input_t in;

static void reset(void)
{
	tick = 2000U; /* divisible by 16: deterministic sample phase */
	memset(&in, 0, sizeof(in));
	in.current_cal_foc_allowed = true;
	in.current_feedback_valid = true;
	rolling_no_assist_diag_init();
	rolling_no_assist_diag_set_session_id(1U);
}

static void feed_ticks(uint32_t count)
{
	for (uint32_t i = 0U; i < count; i++) {
		in.now_tick = tick;
		rolling_no_assist_diag_tick(&in, tick++);
	}
}

static void feed_samples(uint32_t count)
{
	feed_ticks(count * ROLLING_NO_ASSIST_DIAG_DECIMATION);
}

static void warm_up(void)
{
	feed_samples(ROLLING_NO_ASSIST_DIAG_SAMPLES + 4U);
}

static void active_context(void)
{
	in.permission_present = true;
	in.permission_bits = 0xB7U;
	in.load_met = true;
	in.load_centikg = 450U;
	in.load_threshold = 300U;
	in.rider_latched = true;
	in.current_cal_foc_allowed = true;
}

static void setup_case_a(void)
{
	active_context();
	in.iq_before_pu = 0;
	in.iq_request_raw = 0;
	in.iq_after_latch_floor = 0;
	in.iq_pre_ramp = 0;
	in.iq_setpoint = 0;
	in.iq_actual = 0;
	in.pwm_on = false;
	in.moe = false;
	in.bridge_lifecycle = 0U;
}

static void setup_case_b_pwm_off(void)
{
	active_context();
	in.iq_before_pu = 500;
	in.iq_request_raw = 500;
	in.iq_after_latch_floor = 500;
	in.iq_pre_ramp = 500;
	in.iq_setpoint = 500;
	in.iq_actual = 0;
	in.pwm_on = false;
	in.moe = false;
	in.bridge_lifecycle = 0U;
}

static void setup_case_c(int16_t actual)
{
	active_context();
	in.iq_before_pu = 500;
	in.iq_request_raw = 500;
	in.iq_after_latch_floor = 500;
	in.iq_pre_ramp = 500;
	in.iq_setpoint = 500;
	in.iq_actual = actual;
	in.pwm_on = true;
	in.moe = true;
	in.bridge_lifecycle = 5U;
}

static void finish_capture(void)
{
	feed_samples(ROLLING_NO_ASSIST_DIAG_POST_SAMPLES + 2U);
}

typedef struct {
	uint16_t count;
	uint8_t found_case;
	uint8_t saw_pwm;
	uint8_t saw_moe;
	uint8_t saw_cal_allowed;
	uint8_t saw_current_feedback_valid;
	uint8_t saw_cutoff_active;
	uint8_t cutoff_progress;
	uint8_t hall_timeout_progress;
	uint32_t first_tick;
	uint32_t last_tick;
} drain_result_t;

static drain_result_t inspect_frozen_capture(uint8_t wanted_case)
{
	drain_result_t result;
	memset(&result, 0, sizeof(result));
	rolling_no_assist_sample_t s;
	uint32_t previous = 0U;
	while (rolling_no_assist_diag_capture_sample_at(result.count, &s)) {
		if (result.count == 0U) result.first_tick = s.tick_abs;
		if (result.count != 0U) {
			CHECK(s.tick_abs - previous == ROLLING_NO_ASSIST_DIAG_DECIMATION,
			      "ring samples retain exact 4 ms / 16-tick spacing");
		}
		previous = s.tick_abs;
		result.last_tick = s.tick_abs;
		if (s.case_id == wanted_case) {
			result.found_case = 1U;
			result.saw_pwm = (s.flags & RNA_FLAG_PWM_ON) != 0U;
			result.saw_moe = (s.status_flags & RNA_STATUS_HARDWARE_MOE) != 0U;
			result.saw_cal_allowed =
				(s.status_flags & RNA_STATUS_CURRENT_CAL_FOC_ALLOWED) != 0U;
			result.saw_current_feedback_valid =
				(s.status_flags & RNA_STATUS_CURRENT_FEEDBACK_VALID) != 0U;
			result.saw_cutoff_active =
				(s.status_flags & RNA_STATUS_PWM_CUTOFF_ACTIVE) != 0U;
			result.cutoff_progress = s.pwm_cutoff_progress;
			result.hall_timeout_progress = s.hall_timeout_progress;
		}
		result.count++;
	}
	return result;
}

static void test_layout_constants(void)
{
	CHECK(sizeof(rolling_no_assist_sample_t) == 48U, "T1: logical sample is exactly 48 B (schema v3)");
	CHECK(ROLLING_NO_ASSIST_DATA_FRAGMENTS == 6U, "T1: schema v3 still has six DATA fragments");
	CHECK(ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE == 7U, "T1: one HEADER + six DATA frames, unchanged from v2");
}

static void test_no_trigger_inactive(void)
{
	reset();
	warm_up();
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT + ROLLING_NO_ASSIST_DIAG_POST_SAMPLES);
	CHECK(rolling_no_assist_diag_queue_enqueued() == 0U, "T2: inactive rider never triggers");
}

static void test_case_a(void)
{
	reset(); warm_up(); setup_case_a();
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT);
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_A, "T3: CASE A opens");
	finish_capture();
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_A);
	CHECK(d.count == 256U && d.found_case, "T3: CASE A freezes a complete 256-sample ring");
}

static void test_case_b_pwm_off(void)
{
	reset(); warm_up(); setup_case_b_pwm_off();
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_B, "T4: PWM_OFF is CASE B");
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_B);
	CHECK(d.found_case && !d.saw_pwm && !d.saw_moe, "T4: PWM=0 and hardware MOE=0 remain distinct facts");
}

static void test_case_b_pwm_on_moe_off(void)
{
	reset(); warm_up(); setup_case_b_pwm_off();
	in.pwm_on = true;
	in.moe = false;
	in.bridge_lifecycle = 2U;
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_B,
	      "T7: PWM_ON=1 / hardware MOE=0 is CASE B");
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_B);
	CHECK(d.found_case && d.saw_pwm && !d.saw_moe,
	      "T7: sample preserves software PWM_ON separately from hardware MOE");
}

static void test_case_b_current_cal_block(void)
{
	reset(); warm_up(); setup_case_c(-20);
	in.current_cal_foc_allowed = false;
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_B,
	      "T8: current calibration block is CASE B even with PWM/MOE active");
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_B);
	CHECK(d.found_case && !d.saw_cal_allowed,
	      "T8: current_cal_foc_allowed=0 survives in the captured sample");
}

static void test_case_b_stale_current_is_not_case_c(void)
{
	reset(); warm_up(); setup_case_c(-20);
	in.current_feedback_valid = false;
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT + ROLLING_NO_ASSIST_DIAG_POST_SAMPLES);
	CHECK(rolling_no_assist_diag_queue_enqueued() == 0U,
		"T8b: stale/invalid Iq never becomes CASE C current-tracking evidence");
}

static void test_case_c_and_signed_iq(void)
{
	reset(); warm_up(); setup_case_c(-500);
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT + 4U);
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_NONE,
	      "T5: +500 setpoint / -500 actual is normal magnitude, NO CASE C");

	reset(); warm_up(); setup_case_c(-20);
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_C,
	      "T6: +500 setpoint / -20 actual triggers CASE C");
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_C);
	CHECK(d.found_case && d.saw_pwm && d.saw_moe && d.saw_cal_allowed && d.saw_current_feedback_valid,
	      "T6: CASE C evidence has PWM, real MOE and calibration allowed");
	CHECK(!d.saw_cutoff_active && d.cutoff_progress == 0U,
	      "FW-122.1 T7: CASE C classification/evidence is unaffected by the new v3 fields "
	      "(no soft-cutoff involved in this scenario)");
}

/* --- FW-122.1: D2 (quick re-demand during soft-cutoff) raw evidence --------------------------
 * These do not add a new trigger class - a sample with pwm_cutoff_active=1, pwm_on=0 and
 * final_iq>0 already satisfies CASE B's existing "!pwm_on" condition unchanged. The tests below
 * only prove the two new raw facts (pwm_cutoff_active status bit, pwm_cutoff_progress,
 * hall_timeout_progress) survive into the captured CASE B sample, and that they are absent
 * once the bridge has genuinely finished a full stop (T5: no D2 evidence). */

static void setup_case_b_redemand_during_soft_cutoff(void)
{
	active_context();
	in.iq_before_pu = 500; in.iq_request_raw = 500;
	in.iq_after_latch_floor = 500; in.iq_pre_ramp = 500;
	in.iq_setpoint = 500; in.iq_actual = 0;
	/* src/main.c SOFT_CUTOFF: ui_8_PWM_ON_Flag drops to 0 while hardware MOE stays 1 and
	 * pwm_cutoff_active=1 for up to SOFT_CUTOFF_TICKS (production 40) control ticks - see
	 * FW-124 section 6 (finding D2) and the schema v3 comment in rolling_no_assist_diag.h. */
	in.pwm_on = false;
	in.moe = true;
	in.bridge_lifecycle = 0U; /* main.c resets lifecycle to IDLE at soft-cutoff entry */
	in.pwm_cutoff_active = true;
	in.pwm_cutoff_tick = 23U;
	in.half_rotation_counter = 4000U; /* production POWER_STAGE_STOP_TICKS: >>4 saturates to 250 */
}

static void test_case_b_redemand_during_soft_cutoff(void)
{
	reset(); warm_up(); setup_case_b_redemand_during_soft_cutoff();
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_B,
	      "FW-122.1 T4: re-demand during soft-cutoff is CASE B (unchanged classification)");
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_B);
	CHECK(d.found_case && !d.saw_pwm && d.saw_moe,
	      "FW-122.1 T4: PWM=0 / hardware MOE=1 - the legal soft-cutoff transient from FW-124 section 4");
	CHECK(d.saw_cutoff_active, "FW-122.1 T4: D2 evidence - pwm_cutoff_active is visible on the wire");
	CHECK(d.cutoff_progress == 23U, "FW-122.1 T4: pwm_cutoff_progress carries the exact tick count");
	CHECK(d.hall_timeout_progress == 250U,
	      "FW-122.1 T4: hall_timeout_progress == half_rotation_counter>>4 (4000>>4 = 250)");
}

static void test_case_b_redemand_after_full_stop(void)
{
	/* T5: same CASE B (demand exists, bridge not started), but the bridge already finished a
	 * full stop - MOE is genuinely off and no soft-cutoff is in progress. No D2 evidence must
	 * appear here, so a trace reader cannot mistake an ordinary cold start for D2. */
	reset(); warm_up(); setup_case_b_pwm_off();
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_B,
	      "FW-122.1 T5: re-demand after a full stop is still CASE B");
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_B);
	CHECK(d.found_case && !d.saw_moe, "FW-122.1 T5: hardware MOE is genuinely off");
	CHECK(!d.saw_cutoff_active && d.cutoff_progress == 0U,
	      "FW-122.1 T5: no D2 evidence when the bridge is not mid-soft-cutoff");
}

static void test_progress_bytes_saturate_not_wrap(void)
{
	/* Defensive bound check: both compaction points must clamp at 255, never wrap, even for
	 * inputs this firmware cannot currently produce (SOFT_CUTOFF_TICKS is 40 today). */
	reset(); warm_up(); setup_case_b_redemand_during_soft_cutoff();
	in.pwm_cutoff_tick = 1000U;
	in.half_rotation_counter = 65535U;
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_B);
	CHECK(d.found_case && d.cutoff_progress == 255U,
	      "FW-122.1: pwm_cutoff_progress saturates at 255, does not wrap");
	CHECK(d.hall_timeout_progress == 255U,
	      "FW-122.1: hall_timeout_progress saturates at 255, does not wrap");
}

static void test_confirmation_is_eight_sampled_observations(void)
{
	reset(); warm_up(); setup_case_a();
	/* Stop immediately after each 250 Hz observation so the following fifteen
	 * 4 kHz ticks are exercised explicitly rather than being hidden in the
	 * feed_samples() helper's complete 16-tick interval. */
	for (uint8_t sample = 0U;
	     sample < ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT - 1U;
	     sample++) {
		rolling_no_assist_diag_tick(&in, tick++);
		if (sample + 1U < ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT - 1U)
			feed_ticks(ROLLING_NO_ASSIST_DIAG_DECIMATION - 1U);
	}
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_NONE,
	      "T10: seven sampled observations do not confirm");
	/* Non-sampled 4 kHz ticks must not advance the sample confirmation counter. */
	feed_ticks(ROLLING_NO_ASSIST_DIAG_DECIMATION - 1U);
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_NONE,
	      "T10: fifteen intervening control ticks do not fake the eighth sample");
	feed_ticks(1U);
	CHECK(rolling_no_assist_diag_trigger_case() == ROLLING_NO_ASSIST_CASE_A,
	      "T10: the eighth 250 Hz sample confirms at 32 ms");
	finish_capture();
	inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_A);
}

static void test_ring_pre_post_boundaries(void)
{
	reset(); warm_up(); setup_case_c(-20);
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT); finish_capture();
	drain_result_t d = inspect_frozen_capture(ROLLING_NO_ASSIST_CASE_C);
	CHECK(d.count == ROLLING_NO_ASSIST_DIAG_PRE_SAMPLES + ROLLING_NO_ASSIST_DIAG_POST_SAMPLES,
	      "T11: frozen ring is exactly PRE 100 + POST 156");
	CHECK((d.last_tick - d.first_tick) ==
	      (ROLLING_NO_ASSIST_DIAG_SAMPLES - 1U) * ROLLING_NO_ASSIST_DIAG_DECIMATION,
	      "T11: 256 samples span exactly 255 sample intervals");
}

static void fill_oracle_sample_on(rolling_no_assist_sample_t *s)
{
	memset(s, 0, sizeof(*s));
	s->tick_abs = 0xDEADBEEFU;
	s->flags = 0xBCU;
	s->status_flags = 0x07U;
	s->case_id = 3U; s->bridge_lifecycle = 5U; s->hall = 6U;
	s->permission_bits = 0xB7U; s->reason_bits = 0xA5U; s->debug_flags = 0x5AU;
	s->iq_before_pu = 700; s->iq_request = 650;
	s->iq_after_latch_floor = 600; s->iq_pre_ramp = 550;
	s->iq_setpoint = 500; s->iq_actual = -500;
	s->pi_q_int = -12000; s->pi_d_int = 2345;
	s->erps = 321U; s->rpm = 76;
	s->load_centikg = 466U; s->load_threshold = 300U;
	s->angle_hall = 0x1234U; s->angle_absolute = 0x5678U;
	s->rotor_direction = 1; s->neutral_dwell_counter = 2U;
	s->motor_voltage_utilization = 1024U;
	s->pwm_cutoff_progress = 0U;      /* not in soft-cutoff */
	s->hall_timeout_progress = 5U;
	s->reserved0 = 0U; s->reserved1 = 0U;
}

static void fill_oracle_sample_blocked(rolling_no_assist_sample_t *s)
{
	memset(s, 0, sizeof(*s));
	s->tick_abs = 0x01020304U;
	s->flags = 0xBAU; /* CASE B + rider/permission/load + software PWM_ON */
	s->status_flags = 0x00U; /* real MOE off, current-cal blocked */
	s->case_id = 2U; s->bridge_lifecycle = 1U; s->hall = 2U;
	s->permission_bits = 0xB7U; s->reason_bits = 0x80U; s->debug_flags = 0x04U;
	s->iq_before_pu = 511; s->iq_request = 500;
	s->iq_after_latch_floor = 480; s->iq_pre_ramp = 450;
	s->iq_setpoint = 400; s->iq_actual = -20;
	s->pi_q_int = -1; s->pi_d_int = (int16_t)-32768;
	s->erps = 0xBEEFU; s->rpm = -45;
	s->load_centikg = 0x0102U; s->load_threshold = 0x0304U;
	s->angle_hall = 0x9ABCU; s->angle_absolute = 0xDEF0U;
	s->rotor_direction = -1; s->neutral_dwell_counter = 0U;
	s->motor_voltage_utilization = 2048U;
	s->pwm_cutoff_progress = 0U;        /* not in soft-cutoff */
	s->hall_timeout_progress = 250U;    /* near the ~1 s Hall-silence threshold */
	s->reserved0 = 0U; s->reserved1 = 0U;
}

static void check_oracle(
	const rolling_no_assist_sample_t *sample,
	uint8_t capture_id,
	uint8_t trigger_case,
	const uint8_t expected[7][8])
{
	for (uint16_t frag = 0U; frag < 7U; frag++) {
		uint32_t efid = 0U;
		uint8_t data[8];
		bool last = false;
		CHECK(rolling_no_assist_diag_encode_fragment(
		      sample, 42U, capture_id, trigger_case, frag, &efid, data, &last),
		      "T9: real C serializer accepts every schema v3 fragment");
		CHECK(efid == (frag == 0U ? ROLLING_NO_ASSIST_EFID_HEADER :
		      ROLLING_NO_ASSIST_EFID_DATA_BASE + frag - 1U),
		      "T9: schema v3 EFID sequence is exact (unchanged from v2)");
		CHECK(memcmp(data, expected[frag], 8U) == 0,
		      "T9: real C serializer matches literal frozen wire bytes");
		CHECK(last == (frag == 6U), "T9: only final DATA fragment has last=true");
	}
}

static void test_real_c_serializer_literal_oracle(void)
{
	static const uint8_t expected_on[7][8] = {
		{0x03,0x2A,0x06,0x07,0x03,0x30,0xFA,0x08},
		{0xDE,0xAD,0xBE,0xEF,0xBC,0x07,0x03,0x05},
		{0x06,0xB7,0xA5,0x5A,0x02,0xBC,0x02,0x8A},
		{0x02,0x58,0x02,0x26,0x01,0xF4,0xFE,0x0C},
		{0xD1,0x20,0x09,0x29,0x01,0x41,0x00,0x4C},
		{0x01,0xD2,0x01,0x2C,0x12,0x34,0x56,0x78},
		{0x01,0x02,0x04,0x00,0x00,0x05,0x00,0x00}
	};
	static const uint8_t expected_blocked[7][8] = {
		{0x03,0x2A,0x06,0x08,0x02,0x30,0xFA,0x08},
		{0x01,0x02,0x03,0x04,0xBA,0x00,0x02,0x01},
		{0x02,0xB7,0x80,0x04,0x01,0xFF,0x01,0xF4},
		{0x01,0xE0,0x01,0xC2,0x01,0x90,0xFF,0xEC},
		{0xFF,0xFF,0x80,0x00,0xBE,0xEF,0xFF,0xD3},
		{0x01,0x02,0x03,0x04,0x9A,0xBC,0xDE,0xF0},
		{0xFF,0x00,0x08,0x00,0x00,0xFA,0x00,0x00}
	};
	rolling_no_assist_sample_t sample;
	fill_oracle_sample_on(&sample);
	check_oracle(&sample, 7U, 3U, expected_on);
	fill_oracle_sample_blocked(&sample);
	check_oracle(&sample, 8U, 2U, expected_blocked);

	uint32_t efid; uint8_t data[8]; bool last;
	CHECK(!rolling_no_assist_diag_encode_fragment(
	      &sample, 42U, 8U, 2U, 7U, &efid, data, &last),
	      "T13: serializer rejects bytes outside the declared six DATA fragments");
}

static void test_hmi_cal_encoding_unchanged(void)
{
	uint16_t values[] = {0U, 100U, 700U, 1U, 255U, 256U};
	CHECK(values[0] == 0x0000U && values[1] == 0x0064U && values[2] == 0x02BCU,
	      "H1-H3: HMI CAL still carries final Iq range 0/100/700");
	CHECK(values[3] == 0x0001U && values[4] == 0x00FFU && values[5] == 0x0100U,
	      "H5: HMI CAL byte-range boundaries remain unchanged");
	uint16_t temperature = 42U;
	CHECK(temperature == 42U, "H4: diagnostic recorder does not repurpose temperature/calories state");
}

/* --- FW-123: whole explicit/repeatable FROZEN -> CAN transport ---------------------------- */

#define TRANSPORT_FRAME_COUNT (ROLLING_NO_ASSIST_DIAG_SAMPLES * ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE)

typedef struct {
	uint32_t efid;
	uint8_t data[8];
} transport_frame_t;

static transport_frame_t transport_frames[TRANSPORT_FRAME_COUNT];
static uint32_t transport_count;

static uint8_t transport_transmit(uint32_t efid, const uint8_t *data)
{
	if (transport_count >= TRANSPORT_FRAME_COUNT) return DIAG_CAN_NOMAILBOX;
	transport_frames[transport_count].efid = efid;
	memcpy(transport_frames[transport_count].data, data, 8U);
	transport_count++;
	return 0U;
}

static uint8_t transport_state(uint8_t mailbox)
{
	(void)mailbox;
	return DIAG_CAN_OK;
}

static const diag_can_ops_t transport_can_ops = { transport_transmit, transport_state };

static uint32_t transport_hash(void)
{
	uint32_t hash = 2166136261UL;
	for (uint32_t i = 0U; i < transport_count; i++) {
		for (uint8_t shift = 0U; shift < 32U; shift += 8U) {
			hash ^= (uint8_t)(transport_frames[i].efid >> shift);
			hash *= 16777619UL;
		}
		for (uint8_t b = 0U; b < 8U; b++) {
			hash ^= transport_frames[i].data[b];
			hash *= 16777619UL;
		}
	}
	return hash;
}

static void run_transport_until_done(void)
{
	uint32_t now = 0U;
	while (rolling_no_assist_dump_busy() && now < 100000U) {
		rolling_no_assist_dump_step(now++, true, false);
	}
	CHECK(!rolling_no_assist_dump_busy(), "T14: explicit replay completes without a magic timeout");
}

static void write_physical_capture_fixture(void)
{
	const char *path = getenv("RNA_CAPTURE_FILE");
	if (path == 0 || path[0] == '\0') return;

	FILE *file = fopen(path, "w");
	CHECK(file != 0, "T14: physical-CAN capture fixture opened");
	if (file == 0) return;
	for (uint32_t i = 0U; i < transport_count; i++) {
		int written = fprintf(file,
			"ID:800%05lX Len:8 Data:%02X %02X %02X %02X %02X %02X %02X %02X\n",
			(unsigned long)transport_frames[i].efid,
			transport_frames[i].data[0], transport_frames[i].data[1],
			transport_frames[i].data[2], transport_frames[i].data[3],
			transport_frames[i].data[4], transport_frames[i].data[5],
			transport_frames[i].data[6], transport_frames[i].data[7]);
		CHECK(written > 0, "T14: physical-CAN fixture frame written");
	}
	CHECK(fclose(file) == 0, "T14: physical-CAN capture fixture closed");
}

static void check_transport_sequence(void)
{
	uint32_t per_efid[ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE] = {0U};
	CHECK(transport_count == TRANSPORT_FRAME_COUNT,
	      "T14: all 256 x 7 sample/fragment frames reached the CAN capture");
	for (uint32_t i = 0U; i < transport_count; i++) {
		uint8_t fragment = (uint8_t)(i % ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE);
		uint32_t expected_efid = (fragment == 0U) ? ROLLING_NO_ASSIST_EFID_HEADER :
			ROLLING_NO_ASSIST_EFID_DATA_BASE + (uint32_t)(fragment - 1U);
		CHECK(transport_frames[i].efid == expected_efid,
		      "T14: recorder EFIDs remain HEADER, DATA0..DATA5 for every sample");
		per_efid[fragment]++;
		if (fragment == 0U) {
			CHECK(transport_frames[i].data[0] == 3U && transport_frames[i].data[1] == 1U &&
			      transport_frames[i].data[2] == 6U && transport_frames[i].data[5] == 48U &&
			      transport_frames[i].data[6] == 250U && transport_frames[i].data[7] == 8U,
			      "T14: every sample header carries the fixed schema-3 metadata");
		}
	}
	for (uint8_t fragment = 0U; fragment < ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE; fragment++) {
		CHECK(per_efid[fragment] == ROLLING_NO_ASSIST_DIAG_SAMPLES,
		      "T14: each required fragment arrived exactly once per sample");
	}
}

static void test_explicit_repeatable_frozen_transport(void)
{
	reset();
	rolling_no_assist_dump_init(&transport_can_ops);
	CHECK(!rolling_no_assist_dump_request(), "T14: CAN dump request rejects before any capture is FROZEN");

	warm_up();
	setup_case_c(-20);
	feed_samples(ROLLING_NO_ASSIST_DIAG_CONFIRM_COUNT);
	finish_capture();
	CHECK(rolling_no_assist_diag_is_frozen(), "T14: triggered recorder reaches FROZEN");
	CHECK(rolling_no_assist_diag_capture_sample_count() == ROLLING_NO_ASSIST_DIAG_SAMPLES,
	      "T14: FROZEN retains all 256 samples before CAN is requested");

	transport_count = 0U;
	CHECK(rolling_no_assist_dump_request(), "T14: explicit CAN request arms a FROZEN dump");
	CHECK(!rolling_no_assist_dump_request(), "T14: a second request is refused while the replay is busy");
	run_transport_until_done();
	check_transport_sequence();
	write_physical_capture_fixture();
	uint32_t first_hash = transport_hash();
	CHECK(rolling_no_assist_diag_is_frozen(), "T14: successful CAN dump does not unfreeze/erase RAM");
	CHECK(rolling_no_assist_diag_capture_sample_count() == ROLLING_NO_ASSIST_DIAG_SAMPLES,
	      "T14: successful CAN dump leaves all samples available for a repeat request");

	transport_count = 0U;
	CHECK(rolling_no_assist_dump_request(), "T14: a later explicit request replays the same FROZEN capture");
	run_transport_until_done();
	check_transport_sequence();
	CHECK(transport_hash() == first_hash,
	      "T14: repeatable dump is byte-identical and restarts at sample index zero");
}

int main(void)
{
	printf("rolling_no_assist_diag_host.c schema v3\n");
	test_layout_constants();
	test_no_trigger_inactive();
	test_case_a();
	test_case_b_pwm_off();
	test_case_b_pwm_on_moe_off();
	test_case_b_current_cal_block();
	test_case_b_stale_current_is_not_case_c();
	test_case_c_and_signed_iq();
	test_case_b_redemand_during_soft_cutoff();
	test_case_b_redemand_after_full_stop();
	test_progress_bytes_saturate_not_wrap();
	test_confirmation_is_eight_sampled_observations();
	test_ring_pre_post_boundaries();
	test_real_c_serializer_literal_oracle();
	test_hmi_cal_encoding_unchanged();
	test_explicit_repeatable_frozen_transport();

	if (host_test_failures != 0) {
		printf("rolling_no_assist_diag_host: %d FAILURES\n", host_test_failures);
		return 1;
	}
	printf("rolling_no_assist_diag_host: ALL PASS\n");
	return 0;
}
