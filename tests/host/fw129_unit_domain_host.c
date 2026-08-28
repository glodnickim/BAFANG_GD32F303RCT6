/*
 * FW-129 unit-domain harness — runs the SHIPPED src/torque_input.c, src/assist_modes.c,
 * src/assist_start.c and src/tuning_config.c, not copies of them.
 *
 * The card this covers has one central claim: after it, the shape of assist comes from the
 * PHYSICAL pedal load and the mode's own settings, and from nothing else. Not from the
 * sensor's calibration span, not from an ADC range, not from PH_CURRENT_MAX, and not from
 * where a launch fallback happens to hand over. Each block below pins one half of that.
 *
 * T1  (card §29) normalized torque axis: kg -> 0..160, at two different full-scale settings.
 * T2  (card §30) CALIBRATION INVARIANCE - the card's main PASS/FAIL. Two sensors with
 *                different gains, given native signals that mean the SAME physical force,
 *                must produce the same kilograms and the same demand in every mode.
 * T3  (card §31) Power Linear from real rider power, including the corrected cadence claim.
 * T4  (card §32) Progressive / Curve: monotonic, inside the support window, span-independent.
 * T5  (card §33) eMTB matrix, and that ASSIST TORQUE FULL SCALE moves the knee predictably.
 * T6  (card §34) Torque matrix; cadence must not change the torque gain.
 * T7  (card §35) low-duty handover: continuous, no jump, no collapse, and the launch anchor
 *                is retired once the measured duty is usable.
 * T8            crank length is in the rider-power equation, proportionally.
 * T9            the two settings are ride-feel only: neither touches the reported kilograms.
 * T10 (card §36) the modifier matrix against EVERY mode, not just Power Linear: startup boost,
 *                assist without rotation, the power filter, both per-level ceilings, level 0,
 *                cadence compensation.
 * T11 (card §23/§24) tuning blob v8 from the controller's side: 32 B geometry, round trip,
 *                clamping, and that v2..v7 are still accepted and migrate to the defaults.
 *
 * Finally it prints the BEFORE/AFTER table the card asks for in §8 of the owner's brief, at
 * 5/10/20/30/40/60 kg x 40/60/80/100 rpm for all five active modes. BEFORE is computed here
 * from the OLD equations, kept verbatim in old_reference_iq() below, so the comparison is
 * against what the firmware really did rather than against a memory of it.
 *
 * Usage: fw129_unit_domain_host            (checks + table)
 *        fw129_unit_domain_host --quiet    (checks only)
 */

#include <stdio.h>
#include <string.h>

#include "assist_modes.h"
#include "check.h"
#include "config.h"
#include "rider_input.h"
#include "torque_input.h"
#include "tuning_config.h"

#define BATTERY_MV 42000U
#define IQ_LIMIT ((int32_t)PH_CURRENT_MAX)
/* The reference duty the firmware's launch anchor is anchored to. Kept as a literal on
 * purpose: if assist_modes.c changes it, this harness must be re-read, not silently follow. */
#define LAUNCH_REFERENCE_U_ABS 1024U
#define LAUNCH_REFERENCE_RPM 60U

/* ------------------------------------------------------------------ helpers */

/*
 * Level 3's defaults with the STARTUP BOOST OFF.
 *
 * The boost is a deliberate low-cadence amplifier with its own curve, and leaving it on would
 * put its fade on top of every cadence sweep here - a 2x load multiplier at 1 rpm decaying to
 * nothing by 60 rpm, which reads exactly like a handover discontinuity and is not one. It has
 * its own regression (card 36); these blocks are about the conversion underneath it.
 */
static assist_level_config_t level_for(assist_mode_type_t mode)
{
	assist_level_config_t cfg = *assist_modes_get_default_level(3);
	cfg.mode_type = mode;
	cfg.startup_boost.enabled = false;
	return cfg;
}

/*
 * Drive one calculation at a chosen operating point. torque_run_filtered is the signal the
 * modes actually consume, so it is set directly: this harness is about the assist arithmetic,
 * not about the RUN estimator's filling behaviour, which has its own suites.
 */
/*
 * The duty a mid-drive actually has at a given crank cadence: the motor is geared to the
 * cranks, so duty is proportional to cadence, and 60 rpm is where the launch reference sits.
 * Sweeping cadence without sweeping duty with it describes a bike that does not exist, and
 * makes the conversion look cadence-dependent when it is not.
 */
static uint16_t duty_for_cadence(uint8_t cadence_rpm)
{
	return (uint16_t)((uint32_t)LAUNCH_REFERENCE_U_ABS * cadence_rpm / LAUNCH_REFERENCE_RPM);
}

static assist_mode_output_t run_once(
	const assist_level_config_t *cfg,
	uint16_t run_native,
	uint8_t cadence_rpm,
	uint16_t u_abs)
{
	rider_input_t r;
	memset(&r, 0, sizeof(r));
	r.cadence_rpm = cadence_rpm;
	/*
	 * A launch: the cranks are turning but the first interval has not been measured yet, so
	 * cadence still reads 0. Without this the cadence gate in prepare_assist_input() blocks
	 * the calculation outright and the harness would be measuring the gate, not the handover.
	 */
	r.start_phase = (cadence_rpm == 0U);
	r.torque_run_filtered = run_native;
	r.torque_assist_filtered = run_native;
	r.torque_load_centikg = torque_input_native_delta_to_centikg(run_native);
	r.motor_voltage_utilization = u_abs;
	r.torque_sensor_valid = true;
	r.pas_sensor_valid = true;

	assist_mode_output_t out;
	memset(&out, 0, sizeof(out));
	/* The power filter is stateful and seeds on the first request; reset before every point
	 * so each row of the tables below is an independent measurement, not a filter transient. */
	assist_modes_reset();
	assist_modes_calculate(&r, cfg, BATTERY_MV, IQ_LIMIT, &out);
	return out;
}

/* Native delta that means `centikg` on the sensor CURRENTLY configured. */
static uint16_t native_for_centikg(uint16_t centikg)
{
	return torque_input_centikg_to_native_delta(centikg);
}

/*
 * The pre-FW-129 phase-Iq equations, kept verbatim so the BEFORE column is the real old
 * behaviour rather than a recollection of it.
 *
 *   Power modes:  Iq = iq_limit * min(1000, (load_ckg*ratio + 1500)/3000) / 1000
 *                 ("60 kg at 500 % support = full Iq", an anchor with no physical source)
 *   eMTB/Torque:  Iq = target_x160 / 160 * iq_limit
 *                 (the x160 result read as a percentage of the phase limit, when the value
 *                  it actually carries is a battery current in 160 mA steps)
 *
 * Both then met a P/U ceiling; that ceiling is applied here too when u_abs > 0, exactly as
 * finish_power_request() used to, so the comparison is like for like.
 */
static int32_t old_reference_iq(
	const assist_level_config_t *cfg,
	uint16_t load_centikg,
	uint8_t cadence_rpm,
	uint16_t u_abs,
	uint16_t run_native)
{
	uint32_t iq;
	uint32_t motor_power_mw;

	if (cfg->mode_type == ASSIST_MODE_EMTB || cfg->mode_type == ASSIST_MODE_TORQUE) {
		/* The old input domain: native delta over the sensor's calibration span. */
		uint32_t span = torque_input_span_native();
		uint32_t delta_x160_q = ((uint32_t)run_native * 160U * 256U + span / 2U) / span;
		uint32_t target_q;
		if (cfg->mode_type == ASSIST_MODE_EMTB) {
			uint32_t den = 510U - 2U * cfg->emtb_parameter;
			if (cfg->emtb_based_on_power) {
				den = (den > cadence_rpm) ? den - cadence_rpm : 0U;
			}
			den += 10U;
			target_q = (delta_x160_q * delta_x160_q) / (den * 256U);
		} else {
			target_q = (delta_x160_q * cfg->torque_assist_factor) / 120U;
		}
		uint32_t full_q = 160U * 256U;
		if (target_q > full_q) target_q = full_q;
		iq = (target_q * (uint32_t)IQ_LIMIT + full_q - 1U) / full_q;
		uint32_t current_ma = (target_q * 160U + 128U) / 256U;
		motor_power_mw = (current_ma * cfg->emtb_reference_voltage_mv) / 1000U;
	} else {
		uint32_t ratio = cfg->support_ratio_pct;
		uint32_t load = (load_centikg > 6000U) ? 6000U : load_centikg;
		uint32_t permille = (load * ratio + 1500U) / 3000U;
		if (permille > 1000U) permille = 1000U;
		iq = ((uint32_t)IQ_LIMIT * permille + 999U) / 1000U;
		uint32_t human_mw = load_centikg * cadence_rpm +
			(load_centikg * cadence_rpm * 694U + 500U) / 1000U;
		motor_power_mw = (human_mw * ratio) / 100U;
	}

	if (u_abs > 0U) {
		uint32_t current_ma = (motor_power_mw * 1000U) / BATTERY_MV;
		uint32_t ceiling = (current_ma * 2048U) / ((uint32_t)u_abs * (uint32_t)CAL_I);
		if (iq > ceiling) iq = ceiling;
	}
	return (int32_t)iq;
}

/* ------------------------------------------------------------------ T1 */

static void t1_torque_normalization(void)
{
	printf("T1 (card 29) normalized torque axis from calibrated kg\n");
	assist_level_config_t cfg = level_for(ASSIST_MODE_TORQUE);

	struct { uint16_t centikg; uint16_t expect; } at60[] = {
		{ 0, 0 }, { 600, 16 }, { 1500, 40 }, { 3000, 80 },
		{ 4500, 120 }, { 6000, 160 }, { 8400, 160 },
	};
	for (unsigned i = 0; i < sizeof(at60) / sizeof(at60[0]); i++) {
		assist_mode_output_t out = run_once(&cfg,
			native_for_centikg(at60[i].centikg), 60U, LAUNCH_REFERENCE_U_ABS);
		char label[96];
		snprintf(label, sizeof(label),
			"T1: full scale 60.0 kg, %u.%02u kg -> %u / 160 (got %u)",
			at60[i].centikg / 100U, at60[i].centikg % 100U,
			at60[i].expect, out.assist_torque_x160);
		/* +-1 for the deadband the assist path subtracts and integer rounding. */
		int delta = (int)out.assist_torque_x160 - (int)at60[i].expect;
		CHECK(delta <= 1 && delta >= -1, label);
	}

	/* Halving the full scale must double the position on the axis, up to saturation. */
	uint8_t blob[TUNING_BLOB_LEN];
	tuning_config_serialize(blob);
	blob[24] = 4000U & 0xFFU; blob[25] = (4000U >> 8) & 0xFFU; /* 40.0 kg */
	{
		uint16_t crc = 0xFFFFU;
		for (uint8_t i = 0; i < TUNING_BLOB_LEN - 2U; i++) {
			crc ^= (uint16_t)blob[i] << 8;
			for (uint8_t b = 0; b < 8; b++) {
				crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
			}
		}
		blob[30] = (uint8_t)(crc & 0xFFU); blob[31] = (uint8_t)(crc >> 8);
	}
	CHECK(tuning_config_apply_blob(blob, TUNING_BLOB_LEN), "T1: v8 blob with 40.0 kg accepted");
	CHECK(tuning_config_assist_torque_full_scale_centikg() == 4000U,
		"T1: full scale setting took effect");

	struct { uint16_t centikg; uint16_t expect; } at40[] = {
		{ 1000, 40 }, { 2000, 80 }, { 3000, 120 }, { 4000, 160 }, { 6000, 160 },
	};
	for (unsigned i = 0; i < sizeof(at40) / sizeof(at40[0]); i++) {
		assist_mode_output_t out = run_once(&cfg,
			native_for_centikg(at40[i].centikg), 60U, LAUNCH_REFERENCE_U_ABS);
		char label[96];
		snprintf(label, sizeof(label),
			"T1: full scale 40.0 kg, %u.%02u kg -> %u / 160 (got %u)",
			at40[i].centikg / 100U, at40[i].centikg % 100U,
			at40[i].expect, out.assist_torque_x160);
		int delta = (int)out.assist_torque_x160 - (int)at40[i].expect;
		CHECK(delta <= 2 && delta >= -2, label);
	}

	/* Back to the default for everything that follows. */
	tuning_config_serialize(blob);
	blob[24] = 6000U & 0xFFU; blob[25] = (6000U >> 8) & 0xFFU;
	{
		uint16_t crc = 0xFFFFU;
		for (uint8_t i = 0; i < TUNING_BLOB_LEN - 2U; i++) {
			crc ^= (uint16_t)blob[i] << 8;
			for (uint8_t b = 0; b < 8; b++) {
				crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
			}
		}
		blob[30] = (uint8_t)(crc & 0xFFU); blob[31] = (uint8_t)(crc >> 8);
	}
	tuning_config_apply_blob(blob, TUNING_BLOB_LEN);
}

/* ------------------------------------------------------------------ T2 */

/*
 * THE CARD'S MAIN PASS/FAIL (§30).
 *
 * Sensor A is the factory characteristic. Sensor B is the same physical load cell with a
 * different gain - it produces GAIN_NUM/GAIN_DEN times as many native units for the same
 * force - and has been calibrated with a reference weight, exactly as a rider would.
 *
 * Feed each sensor the native signal that its own hardware would produce for a given real
 * force. The kilograms must agree, and so must the assist demand in every mode. Before this
 * card they did not: the user calibration replaced the measured piecewise characteristic with
 * a straight line through the calibration point (+21 % at light load on a PERFECT sensor),
 * and eMTB/Torque normalized their input by the calibration span on top of that.
 */
#define GAIN_NUM 13
#define GAIN_DEN 10
#define CAL_REFERENCE_CENTIKG 2000U

static void calibrate_sensor_b(void)
{
	torque_input_restore_default_span();
	/* What sensor B reads at the reference weight: the default delta, times its gain. */
	uint32_t default_delta = torque_input_centikg_to_native_delta(CAL_REFERENCE_CENTIKG);
	uint32_t measured = (default_delta * GAIN_NUM) / GAIN_DEN;
	uint32_t span = (measured * TORQUE_DEFAULT_SPAN_NATIVE + default_delta / 2U) / default_delta;
	CHECK(torque_input_set_user_span((uint16_t)span),
		"T2: the calibration this sensor would produce is in range");
}

static void t2_calibration_invariance(void)
{
	printf("T2 (card 30) CALIBRATION INVARIANCE - same force, same kg, same demand\n");
	const uint16_t forces[] = { 1000U, 2000U, 4000U, 6000U };
	const assist_mode_type_t modes[] = {
		ASSIST_MODE_POWER_LINEAR, ASSIST_MODE_POWER_PROGRESSIVE, ASSIST_MODE_POWER_CURVE,
		ASSIST_MODE_EMTB, ASSIST_MODE_TORQUE,
	};
	const char *names[] = { "Power Linear", "Power Progressive", "Power Curve", "eMTB", "Torque" };

	for (unsigned f = 0; f < sizeof(forces) / sizeof(forces[0]); f++) {
		uint16_t force = forces[f];

		torque_input_restore_default_span();
		uint16_t native_a = torque_input_centikg_to_native_delta(force);
		uint16_t kg_a = torque_input_native_delta_to_centikg(native_a);

		calibrate_sensor_b();
		/* Sensor B's own hardware reading for the same physical force. */
		uint16_t native_b = (uint16_t)(((uint32_t)native_a * GAIN_NUM) / GAIN_DEN);
		uint16_t kg_b = torque_input_native_delta_to_centikg(native_b);

		char label[128];
		int kg_delta = (int)kg_b - (int)kg_a;
		snprintf(label, sizeof(label),
			"T2: %u.%02u kg reads the same on both sensors (A=%u B=%u centikg)",
			force / 100U, force % 100U, kg_a, kg_b);
		/* 1 % of the force, for integer rounding through two conversions. */
		CHECK(kg_delta <= (int)(force / 100U) + 2 && kg_delta >= -(int)(force / 100U) - 2, label);

		for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
			assist_level_config_t cfg = level_for(modes[m]);

			torque_input_restore_default_span();
			assist_mode_output_t a = run_once(&cfg, native_a, 60U, LAUNCH_REFERENCE_U_ABS);

			calibrate_sensor_b();
			assist_mode_output_t b = run_once(&cfg, native_b, 60U, LAUNCH_REFERENCE_U_ABS);

			int32_t spread = a.iq_request - b.iq_request;
			if (spread < 0) spread = -spread;
			int32_t tolerance = (a.iq_request / 20) + 2; /* 5 % + rounding */
			snprintf(label, sizeof(label),
				"T2: %s at %u.%02u kg - same demand on both sensors (A=%ld B=%ld)",
				names[m], force / 100U, force % 100U,
				(long)a.iq_request, (long)b.iq_request);
			CHECK(spread <= tolerance, label);
		}
	}
	torque_input_restore_default_span();
}

/* ------------------------------------------------------------------ T3 */

static void t3_power_linear(void)
{
	printf("T3 (card 31, as corrected by the audit) Power Linear from real rider power\n");
	assist_level_config_t cfg = level_for(ASSIST_MODE_POWER_LINEAR);
	uint16_t native10kg = native_for_centikg(1000U);

	/* 10.00 kg at 80 rpm on a 165 mm crank is ~135.5 W of rider power. */
	cfg.support_ratio_pct = 100U;
	assist_mode_output_t p100 = run_once(&cfg, native10kg, 80U, LAUNCH_REFERENCE_U_ABS);
	CHECK(p100.assist_basis_power_w >= 132 && p100.assist_basis_power_w <= 139,
		"T3: 10 kg @ 80 rpm, 165 mm crank -> about 135.5 W of rider power");
	CHECK(p100.raw_motor_power_w >= 132 && p100.raw_motor_power_w <= 139,
		"T3: 100 % support asks the motor for about the rider's own 135.5 W");

	cfg.support_ratio_pct = 200U;
	assist_mode_output_t p200 = run_once(&cfg, native10kg, 80U, LAUNCH_REFERENCE_U_ABS);
	CHECK(p200.raw_motor_power_w >= 265 && p200.raw_motor_power_w <= 277,
		"T3: 200 % support -> about 271 W");

	cfg.support_ratio_pct = 320U;
	assist_mode_output_t p320 = run_once(&cfg, native10kg, 80U, LAUNCH_REFERENCE_U_ABS);
	CHECK(p320.raw_motor_power_w >= 425 && p320.raw_motor_power_w <= 443,
		"T3: 320 % support -> about 434 W");

	/*
	 * The cadence half, as the audit corrected it. At half the cadence the rider produces
	 * half the power and the motor is asked for half the power - but the CURRENT must come
	 * out the same, because the motor is turning half as fast and its duty has halved with
	 * it. Expecting Iq to halve would be expecting the motor to deliver a quarter of the
	 * torque ratio it was configured for.
	 */
	assist_mode_output_t at80 = run_once(&cfg, native10kg, 80U, duty_for_cadence(80U));
	assist_mode_output_t at40 = run_once(&cfg, native10kg, 40U, duty_for_cadence(40U));
	CHECK(at40.assist_basis_power_w * 2 >= at80.assist_basis_power_w - 4 &&
		at40.assist_basis_power_w * 2 <= at80.assist_basis_power_w + 4,
		"T3: rider power at 40 rpm is half of the power at 80 rpm");
	CHECK(at40.raw_motor_power_w * 2 >= at80.raw_motor_power_w - 8 &&
		at40.raw_motor_power_w * 2 <= at80.raw_motor_power_w + 8,
		"T3: requested motor power at 40 rpm is half of the power at 80 rpm");
	{
		int32_t spread = at40.iq_request - at80.iq_request;
		if (spread < 0) spread = -spread;
		char label[128];
		snprintf(label, sizeof(label),
			"T3: final Iq is cadence-independent for the same load (40 rpm=%ld, 80 rpm=%ld)",
			(long)at40.iq_request, (long)at80.iq_request);
		CHECK(spread <= (at80.iq_request / 10) + 2, label);
	}

	/* Doubling the load doubles the demand: no dead zone, no arbitrary saturation. */
	assist_mode_output_t l10 = run_once(&cfg, native_for_centikg(1000U), 60U, LAUNCH_REFERENCE_U_ABS);
	assist_mode_output_t l20 = run_once(&cfg, native_for_centikg(2000U), 60U, LAUNCH_REFERENCE_U_ABS);
	CHECK(l20.iq_request > l10.iq_request * 18 / 10 &&
		l20.iq_request < l10.iq_request * 22 / 10,
		"T3: twice the pedal load asks for about twice the current");
}

/* ------------------------------------------------------------------ T4 */

static void t4_progressive_and_curve(void)
{
	printf("T4 (card 32) Progressive / Curve: monotonic, inside the window, span-independent\n");
	const uint16_t human_w[] = { 25U, 50U, 100U, 150U, 200U, 300U, 400U };
	const assist_mode_type_t modes[] = { ASSIST_MODE_POWER_PROGRESSIVE, ASSIST_MODE_POWER_CURVE };
	const char *names[] = { "Progressive", "Curve" };

	for (unsigned m = 0; m < 2U; m++) {
		assist_level_config_t cfg = level_for(modes[m]);
		cfg.support_min_pct = 50U;
		cfg.support_max_pct = 400U;
		cfg.reference_power_w = 200U;
		cfg.progression_pct = 50U;

		int32_t previous_iq = -1;
		uint16_t previous_ratio = 0U;
		for (unsigned i = 0; i < sizeof(human_w) / sizeof(human_w[0]); i++) {
			/* Pick the load that produces this rider power at 60 rpm. */
			uint32_t centikg = ((uint32_t)human_w[i] * 1000U * 1000U) / (60U * 1694U);
			assist_mode_output_t out = run_once(&cfg,
				native_for_centikg((uint16_t)centikg), 60U, LAUNCH_REFERENCE_U_ABS);
			char label[128];

			snprintf(label, sizeof(label), "T4: %s support ratio inside [min,max] at %u W (%u %%)",
				names[m], human_w[i], out.applied_support_ratio_pct);
			CHECK(out.applied_support_ratio_pct >= cfg.support_min_pct &&
				out.applied_support_ratio_pct <= cfg.support_max_pct, label);

			snprintf(label, sizeof(label), "T4: %s support ratio is monotonic at %u W",
				names[m], human_w[i]);
			CHECK(out.applied_support_ratio_pct >= previous_ratio, label);
			previous_ratio = out.applied_support_ratio_pct;

			snprintf(label, sizeof(label), "T4: %s demand is monotonic at %u W (%ld)",
				names[m], human_w[i], (long)out.iq_request);
			CHECK(out.iq_request >= previous_iq, label);
			previous_iq = out.iq_request;
		}

		/* Span-independence: the same physical load on a calibrated sensor. */
		uint32_t centikg = 2000U;
		torque_input_restore_default_span();
		assist_mode_output_t a = run_once(&cfg,
			native_for_centikg((uint16_t)centikg), 60U, LAUNCH_REFERENCE_U_ABS);
		uint16_t native_a = native_for_centikg((uint16_t)centikg);
		calibrate_sensor_b();
		assist_mode_output_t b = run_once(&cfg,
			(uint16_t)(((uint32_t)native_a * GAIN_NUM) / GAIN_DEN), 60U, LAUNCH_REFERENCE_U_ABS);
		torque_input_restore_default_span();

		int32_t spread = a.iq_request - b.iq_request;
		if (spread < 0) spread = -spread;
		char label[128];
		snprintf(label, sizeof(label),
			"T4: %s is independent of the sensor span (A=%ld B=%ld)",
			names[m], (long)a.iq_request, (long)b.iq_request);
		CHECK(spread <= (a.iq_request / 20) + 2, label);
	}
}

/* ------------------------------------------------------------------ T5/T6 */

static void set_full_scale_centikg(uint16_t centikg)
{
	uint8_t blob[TUNING_BLOB_LEN];
	tuning_config_serialize(blob);
	blob[24] = (uint8_t)(centikg & 0xFFU);
	blob[25] = (uint8_t)(centikg >> 8);
	uint16_t crc = 0xFFFFU;
	for (uint8_t i = 0; i < TUNING_BLOB_LEN - 2U; i++) {
		crc ^= (uint16_t)blob[i] << 8;
		for (uint8_t b = 0; b < 8; b++) {
			crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
		}
	}
	blob[30] = (uint8_t)(crc & 0xFFU);
	blob[31] = (uint8_t)(crc >> 8);
	tuning_config_apply_blob(blob, TUNING_BLOB_LEN);
}

static void t5_emtb(void)
{
	printf("T5 (card 33) eMTB matrix and the knee position vs ASSIST TORQUE FULL SCALE\n");
	const uint16_t loads[] = { 500U, 1000U, 1500U, 2000U, 3000U, 4000U, 6000U };
	const uint8_t cadences[] = { 0U, 40U, 60U, 80U, 100U };
	const uint8_t parameters[] = { 60U, 100U, 140U, 180U };
	const uint16_t scales[] = { 4000U, 5000U, 6000U, 7000U };

	for (unsigned s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
		set_full_scale_centikg(scales[s]);
		for (unsigned p = 0; p < sizeof(parameters) / sizeof(parameters[0]); p++) {
			assist_level_config_t cfg = level_for(ASSIST_MODE_EMTB);
			cfg.emtb_parameter = parameters[p];
			for (unsigned c = 0; c < sizeof(cadences) / sizeof(cadences[0]); c++) {
				int32_t previous = -1;
				for (unsigned l = 0; l < sizeof(loads) / sizeof(loads[0]); l++) {
					uint16_t u_abs = (uint16_t)((uint32_t)LAUNCH_REFERENCE_U_ABS *
						cadences[c] / LAUNCH_REFERENCE_RPM);
					assist_mode_output_t out = run_once(&cfg,
						native_for_centikg(loads[l]), cadences[c], u_abs);
					char label[160];
					snprintf(label, sizeof(label),
						"T5: eMTB monotonic in load (scale %u, param %u, %u rpm, %u ckg -> %ld)",
						scales[s], parameters[p], cadences[c], loads[l], (long)out.iq_request);
					CHECK(out.iq_request >= previous, label);
					previous = out.iq_request;
				}
			}
		}
	}

	/*
	 * The knee: a smaller full scale must put the SAME physical push further up the curve.
	 * eMTB squares its input, so the effect is strong and unambiguous.
	 */
	assist_level_config_t cfg = level_for(ASSIST_MODE_EMTB);
	set_full_scale_centikg(6000U);
	assist_mode_output_t wide = run_once(&cfg, native_for_centikg(2000U), 60U, LAUNCH_REFERENCE_U_ABS);
	set_full_scale_centikg(3000U);
	assist_mode_output_t narrow = run_once(&cfg, native_for_centikg(2000U), 60U, LAUNCH_REFERENCE_U_ABS);
	set_full_scale_centikg(6000U);
	CHECK(narrow.assist_torque_x160 > wide.assist_torque_x160 * 18 / 10,
		"T5: halving the full scale roughly doubles the position on the torque axis");
	CHECK(narrow.iq_request > wide.iq_request,
		"T5: and the same push therefore asks for more current");
}

static void t6_torque_mode(void)
{
	printf("T6 (card 34) Torque matrix; cadence gates the mode, it must not scale it\n");
	const uint16_t loads[] = { 500U, 1000U, 2000U, 3000U, 4000U, 6000U };
	const uint8_t factors[] = { 50U, 80U, 120U, 200U };

	for (unsigned f = 0; f < sizeof(factors) / sizeof(factors[0]); f++) {
		assist_level_config_t cfg = level_for(ASSIST_MODE_TORQUE);
		cfg.torque_assist_factor = factors[f];
		int32_t previous = -1;
		for (unsigned l = 0; l < sizeof(loads) / sizeof(loads[0]); l++) {
			assist_mode_output_t out = run_once(&cfg,
				native_for_centikg(loads[l]), 60U, LAUNCH_REFERENCE_U_ABS);
			char label[128];
			snprintf(label, sizeof(label),
				"T6: Torque monotonic in load (factor %u, %u ckg -> %ld)",
				factors[f], loads[l], (long)out.iq_request);
			CHECK(out.iq_request >= previous, label);
			previous = out.iq_request;
		}
	}

	/*
	 * CADENCE. Torque mode has no cadence term of its own - the factor multiplies the pedal
	 * load and nothing else, and that is checked below. But its RESULT is a battery current
	 * in the TSDZ units the algorithm is ported from, which reference voltage turns into a
	 * POWER request - so at a constant push the delivered CURRENT falls as the motor speeds
	 * up, because the same power at a higher duty needs less phase current.
	 *
	 * That is a genuine property of the source algorithm, not a units bug, and it is the
	 * headline difference between the two families on this bike: Power Linear / Progressive /
	 * Curve hold a constant TORQUE ratio as you spin up, eMTB and Torque hold a constant
	 * POWER. Pinned here so nobody "fixes" it later without deciding to.
	 */
	assist_level_config_t cfg = level_for(ASSIST_MODE_TORQUE);
	uint16_t native = native_for_centikg(2000U);
	int32_t previous_across_cadence = -1;
	for (unsigned c = 0; c < 4U; c++) {
		static const uint8_t rpm[] = { 40U, 60U, 80U, 100U };
		assist_mode_output_t out = run_once(&cfg, native, rpm[c], duty_for_cadence(rpm[c]));
		char label[160];
		snprintf(label, sizeof(label),
			"T6: Torque asks for a constant POWER, so current eases off as cadence rises "
			"(%u rpm=%ld)", rpm[c], (long)out.iq_request);
		if (previous_across_cadence >= 0) {
			CHECK(out.iq_request <= previous_across_cadence, label);
		}
		previous_across_cadence = out.iq_request;

		/* What must NOT move with cadence: the torque axis the factor multiplies. */
		snprintf(label, sizeof(label),
			"T6: the normalized torque axis is cadence-free at %u rpm (%u / 160)",
			rpm[c], out.assist_torque_x160);
		CHECK(out.assist_torque_x160 >= 51 && out.assist_torque_x160 <= 55, label);
	}

	/* And the factor itself is a clean gain on that axis. */
	assist_level_config_t half = cfg;
	half.torque_assist_factor = 60U;
	assist_level_config_t full = cfg;
	full.torque_assist_factor = 120U;
	assist_mode_output_t h = run_once(&half, native, 60U, duty_for_cadence(60U));
	assist_mode_output_t f = run_once(&full, native, 60U, duty_for_cadence(60U));
	CHECK(f.iq_request > h.iq_request * 18 / 10 && f.iq_request < h.iq_request * 22 / 10,
		"T6: doubling the torque factor doubles the demand");
}

/* ------------------------------------------------------------------ T7 */

static void t7_low_duty_handover(void)
{
	printf("T7 (card 35) low-duty handover: continuous, no jump, no collapse\n");
	const uint16_t sweep[] = { 0U, 32U, 64U, 128U, 256U, 512U, 1024U, 1536U, 2048U };
	const assist_mode_type_t modes[] = {
		ASSIST_MODE_POWER_LINEAR, ASSIST_MODE_EMTB, ASSIST_MODE_TORQUE,
	};
	const char *names[] = { "Power Linear", "eMTB", "Torque" };

	for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		assist_level_config_t cfg = level_for(modes[m]);
		uint16_t native = native_for_centikg(2000U);
		int32_t previous = -1;
		uint16_t previous_u = 0U;

		for (unsigned i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
			/* Cadence tracks duty, the way it does on a mid-drive: this is the sweep that
			 * corresponds to a real acceleration, and the one the handover must be smooth
			 * across. */
			uint8_t cadence = (uint8_t)((uint32_t)sweep[i] * LAUNCH_REFERENCE_RPM /
				LAUNCH_REFERENCE_U_ABS);
			/* cadence 0 with the cranks turning is a launch - run_once sets start_phase. */
			assist_mode_output_t out = run_once(&cfg, native, cadence, sweep[i]);
			char label[160];

			if (previous >= 0) {
				/* No step change bigger than half the value across one sweep point. */
				int32_t jump = out.iq_request - previous;
				if (jump < 0) jump = -jump;
				int32_t allowed = (previous > out.iq_request ? previous : out.iq_request) / 2 + 8;
				snprintf(label, sizeof(label),
					"T7: %s continuous from u_abs %u to %u (%ld -> %ld)",
					names[m], previous_u, sweep[i], (long)previous, (long)out.iq_request);
				CHECK(jump <= allowed, label);
			}
			snprintf(label, sizeof(label),
				"T7: %s never collapses to zero at u_abs %u under a real 20 kg push",
				names[m], sweep[i]);
			CHECK(out.iq_request > 0, label);
			previous = out.iq_request;
			previous_u = sweep[i];
		}

		/* Above the blend window the launch anchor must carry no weight at all. */
		assist_mode_output_t high = run_once(&cfg, native, 90U, 1536U);
		char label[128];
		snprintf(label, sizeof(label),
			"T7: %s uses the measured duty alone once above the blend window (%u permille)",
			names[m], high.launch_blend_permille);
		CHECK(high.launch_blend_permille == 1000U, label);

		assist_mode_output_t stopped = run_once(&cfg, native, 0U, 0U);
		snprintf(label, sizeof(label),
			"T7: %s falls back to the launch anchor alone at a standstill (%u permille)",
			names[m], stopped.launch_blend_permille);
		CHECK(stopped.launch_blend_permille == 0U, label);
		snprintf(label, sizeof(label),
			"T7: %s still asks for current at a standstill under a 20 kg push (%ld)",
			names[m], (long)stopped.iq_request);
		CHECK(stopped.iq_request > 0, label);
	}

	/* No pedal load, no current - whatever the duty is doing. */
	assist_level_config_t cfg = level_for(ASSIST_MODE_POWER_LINEAR);
	for (unsigned i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
		assist_mode_output_t out = run_once(&cfg, 0U, 60U, sweep[i]);
		char label[96];
		snprintf(label, sizeof(label), "T7: zero pedal load -> zero current at u_abs %u", sweep[i]);
		CHECK(out.iq_request == 0, label);
	}
}

/* ------------------------------------------------------------------ T8/T9 */

static void set_crank_length_mm(uint16_t mm)
{
	uint8_t blob[TUNING_BLOB_LEN];
	tuning_config_serialize(blob);
	blob[26] = (uint8_t)(mm & 0xFFU);
	blob[27] = (uint8_t)(mm >> 8);
	uint16_t crc = 0xFFFFU;
	for (uint8_t i = 0; i < TUNING_BLOB_LEN - 2U; i++) {
		crc ^= (uint16_t)blob[i] << 8;
		for (uint8_t b = 0; b < 8; b++) {
			crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
		}
	}
	blob[30] = (uint8_t)(crc & 0xFFU);
	blob[31] = (uint8_t)(crc >> 8);
	tuning_config_apply_blob(blob, TUNING_BLOB_LEN);
}

static void t8_crank_length(void)
{
	printf("T8 crank length is part of the rider-power equation\n");
	assist_level_config_t cfg = level_for(ASSIST_MODE_POWER_LINEAR);
	uint16_t native = native_for_centikg(2000U);

	set_crank_length_mm(165U);
	assist_mode_output_t ref = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_crank_length_mm(190U);
	assist_mode_output_t longer = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_crank_length_mm(150U);
	assist_mode_output_t shorter = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_crank_length_mm(165U);

	CHECK(longer.assist_basis_power_w > ref.assist_basis_power_w,
		"T8: a longer crank turns the same push into more rider power");
	CHECK(shorter.assist_basis_power_w < ref.assist_basis_power_w,
		"T8: a shorter crank into less");
	/* 190/165 = 1.1515 */
	CHECK(longer.assist_basis_power_w * 1000 >= ref.assist_basis_power_w * 1130 &&
		longer.assist_basis_power_w * 1000 <= ref.assist_basis_power_w * 1175,
		"T8: rider power scales with crank length in proportion");
	CHECK(longer.iq_request > ref.iq_request && shorter.iq_request < ref.iq_request,
		"T8: and the request follows it");
}

static void t9_ride_feel_only(void)
{
	printf("T9 the two new settings are ride feel, never a sensor reading\n");
	assist_level_config_t cfg = level_for(ASSIST_MODE_EMTB);
	uint16_t native = native_for_centikg(2000U);

	set_full_scale_centikg(6000U);
	assist_mode_output_t a = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_full_scale_centikg(3000U);
	assist_mode_output_t b = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_full_scale_centikg(6000U);
	CHECK(a.assist_load_centikg == b.assist_load_centikg,
		"T9: ASSIST TORQUE FULL SCALE does not move the measured pedal load");

	set_crank_length_mm(165U);
	assist_mode_output_t c = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_crank_length_mm(190U);
	assist_mode_output_t d = run_once(&cfg, native, 60U, LAUNCH_REFERENCE_U_ABS);
	set_crank_length_mm(165U);
	CHECK(c.assist_load_centikg == d.assist_load_centikg,
		"T9: CRANK LENGTH does not move the measured pedal load either");
	CHECK(c.iq_request == d.iq_request,
		"T9: and crank length has no effect on eMTB, which works from load, not power");

	/* Neither setting may reach the kilogram scale the tool displays. */
	uint16_t kg_before = torque_input_native_delta_to_centikg(native);
	set_full_scale_centikg(2000U);
	set_crank_length_mm(190U);
	uint16_t kg_after = torque_input_native_delta_to_centikg(native);
	set_full_scale_centikg(6000U);
	set_crank_length_mm(165U);
	CHECK(kg_before == kg_after,
		"T9: neither setting changes the public kilogram scale (0x6025 telemetry)");
}

/* ------------------------------------------------------------------ T10 */

/*
 * Card §36: the modifier matrix. Every modifier, against EVERY active mode - not just against
 * Power Linear, which is what makes this block worth its length. Each modifier sits at a
 * different point of the chain FW-129 rebuilt, and a modifier that silently stopped working
 * on one mode would be invisible in a single-mode test.
 */
static void t10_modifier_matrix(void)
{
	printf("T10 (card 36) modifier matrix across all five active modes\n");
	const assist_mode_type_t modes[] = {
		ASSIST_MODE_POWER_LINEAR, ASSIST_MODE_POWER_PROGRESSIVE, ASSIST_MODE_POWER_CURVE,
		ASSIST_MODE_EMTB, ASSIST_MODE_TORQUE,
	};
	const char *names[] = { "Power Linear", "Power Progressive", "Power Curve", "eMTB", "Torque" };
	const uint16_t native = native_for_centikg(2000U); /* a solid 20 kg push */

	for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		assist_level_config_t base = level_for(modes[m]);
		char label[160];

		/* --- startup boost OFF/ON. It multiplies the physical load now (FW-129 §16), so it
		 * must lift the demand in every mode, and must fade out as cadence rises. --- */
		assist_mode_output_t off = run_once(&base, native, 10U, duty_for_cadence(10U));
		assist_level_config_t boosted = base;
		boosted.startup_boost.enabled = true;
		boosted.startup_boost.strength_pct = 100U;
		assist_mode_output_t on = run_once(&boosted, native, 10U, duty_for_cadence(10U));
		snprintf(label, sizeof(label),
			"T10: %s - startup boost lifts the demand at 10 rpm (%ld -> %ld)",
			names[m], (long)off.iq_request, (long)on.iq_request);
		CHECK(on.iq_request > off.iq_request, label);
		snprintf(label, sizeof(label),
			"T10: %s - startup boost works on the PHYSICAL load, not the raw signal",
			names[m]);
		CHECK(on.assist_load_centikg > off.assist_load_centikg, label);

		assist_mode_output_t faded = run_once(&boosted, native, 100U, duty_for_cadence(100U));
		snprintf(label, sizeof(label),
			"T10: %s - startup boost has faded away by 100 rpm (%u ckg)",
			names[m], faded.assist_load_centikg);
		CHECK(faded.assist_load_centikg <= off.assist_load_centikg + 100U, label);

		/* --- assist without rotation. A deliberate push from a dead stop, cranks not
		 * turning: the demand must come through the SAME mode engine, in the same units. --- */
		assist_level_config_t no_rot = base;
		no_rot.assist_without_rotation = true;
		no_rot.minimum_pedal_load_centikg = 700U;
		rider_input_t r;
		memset(&r, 0, sizeof(r));
		r.cadence_rpm = 0U;
		r.torque_run_filtered = native;
		r.torque_assist_filtered = native;
		r.torque_load_centikg = torque_input_native_delta_to_centikg(native);
		r.torque_sensor_valid = true;
		r.pas_sensor_valid = true;
		assist_mode_output_t rot;
		memset(&rot, 0, sizeof(rot));
		assist_modes_reset();
		assist_modes_calculate(&r, &no_rot, BATTERY_MV, IQ_LIMIT, &rot);
		snprintf(label, sizeof(label),
			"T10: %s - assist without rotation engages above the kg threshold", names[m]);
		CHECK(rot.assist_without_rotation_active, label);
		snprintf(label, sizeof(label),
			"T10: %s - and produces a real demand from a standstill (%ld)",
			names[m], (long)rot.iq_request);
		CHECK(rot.iq_request > 0, label);

		/* Below the threshold it must stay shut - the guard against a foot resting on the
		 * pedal, which is the whole reason that setting is in kilograms. */
		r.torque_run_filtered = native_for_centikg(300U);
		r.torque_assist_filtered = r.torque_run_filtered;
		r.torque_load_centikg = 300U;
		memset(&rot, 0, sizeof(rot));
		assist_modes_reset();
		assist_modes_calculate(&r, &no_rot, BATTERY_MV, IQ_LIMIT, &rot);
		snprintf(label, sizeof(label),
			"T10: %s - assist without rotation stays shut below the threshold", names[m]);
		CHECK(!rot.assist_without_rotation_active && rot.iq_request == 0, label);

		/* --- power rise / fall filter. Turned right off it must be transparent; the
		 * seeding rule means the FIRST request is delivered in full either way. --- */
		assist_level_config_t nofilter = base;
		nofilter.power_rise_filter_ms = 0U;
		nofilter.power_fall_filter_ms = 0U;
		assist_mode_output_t unfiltered = run_once(&nofilter, native, 60U, duty_for_cadence(60U));
		assist_mode_output_t filtered = run_once(&base, native, 60U, duty_for_cadence(60U));
		snprintf(label, sizeof(label),
			"T10: %s - the first request is delivered in full, filtered or not (%ld vs %ld)",
			names[m], (long)filtered.iq_request, (long)unfiltered.iq_request);
		CHECK(filtered.iq_request == unfiltered.iq_request, label);

		/* --- the level's own ceilings still bite, in every mode. --- */
		assist_level_config_t capped = base;
		capped.max_iq_pct = 25U;
		assist_mode_output_t limited = run_once(&capped, native, 60U, duty_for_cadence(60U));
		snprintf(label, sizeof(label),
			"T10: %s - max_iq_pct still caps the request (%ld <= %ld)",
			names[m], (long)limited.iq_request, (long)(IQ_LIMIT / 4));
		CHECK(limited.iq_request <= IQ_LIMIT / 4, label);
		snprintf(label, sizeof(label),
			"T10: %s - and the pre-limit value records what was asked for before it",
			names[m]);
		CHECK(limited.iq_before_pu >= limited.iq_request, label);

		assist_level_config_t power_capped = base;
		power_capped.max_motor_power_w = 100U;
		assist_mode_output_t pl = run_once(&power_capped, native, 60U, duty_for_cadence(60U));
		snprintf(label, sizeof(label),
			"T10: %s - max_motor_power_w still caps the power (%u W)",
			names[m], pl.motor_power_w);
		CHECK(pl.motor_power_w <= 100U, label);

		/* --- level 0 / support disabled: no assist at all, whatever the rider does. --- */
		assist_level_config_t dead = base;
		dead.max_iq_pct = 0U;
		assist_mode_output_t nothing = run_once(&dead, native, 60U, duty_for_cadence(60U));
		snprintf(label, sizeof(label), "T10: %s - max_iq_pct 0 means no assist", names[m]);
		CHECK(nothing.iq_request == 0, label);
	}

	/*
	 * Cadence compensation is a per-BANK switch, so it is checked once rather than per mode -
	 * it multiplies the power request after the mode has produced it, which is the same point
	 * for all five. The default banks ship with it off; the check is that the default path
	 * really is unity, because a compensation silently applied twice was one of the audit's
	 * findings (D10) and the fix must stay visible.
	 */
	assist_level_config_t cfg = level_for(ASSIST_MODE_POWER_LINEAR);
	assist_mode_output_t out = run_once(&cfg, native, 90U, duty_for_cadence(90U));
	CHECK(out.cadence_comp_permille == 1000U,
		"T10: cadence compensation is off on the default bank, and reports unity");
	CHECK(out.precomp_motor_power_w == out.raw_motor_power_w,
		"T10: with compensation off, pre- and post-compensation power are the same number");
}

/* ------------------------------------------------------------------ T11 */

static void put_u16le(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)(v >> 8);
}

static void reseal(uint8_t *blob, uint16_t body)
{
	uint16_t crc = 0xFFFFU;
	for (uint16_t i = 0; i < body; i++) {
		crc ^= (uint16_t)blob[i] << 8;
		for (uint8_t b = 0; b < 8; b++) {
			crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
		}
	}
	put_u16le(&blob[body], crc);
}

/*
 * Card §23/§24: the tuning blob, from the firmware's side. The Canable side has its own
 * round-trip test; this one is about what the CONTROLLER accepts and what it does with an
 * older blob, which is the half a tool test cannot see.
 */
static void t11_tuning_blob_migration(void)
{
	printf("T11 (card 23/24) tuning blob v8: geometry, migration, sanity\n");

	uint8_t blob[TUNING_BLOB_LEN];
	uint16_t len = tuning_config_serialize(blob);
	CHECK(len == TUNING_BLOB_LEN && len == 32U, "T11: v8 serializes to exactly 32 B");
	CHECK(blob[2] == 8U, "T11: version byte is 8");
	CHECK(blob[0] == 0x54U && blob[1] == 0x55U, "T11: magic unchanged");
	CHECK(((uint16_t)blob[28] | ((uint16_t)blob[29] << 8)) == 0U,
		"T11: the third u16 stays reserved");

	/* The bank blob must not have moved: it is at its hard 255 B ceiling. */
	CHECK(ASSIST_BANK_BLOB_LEN == 255U, "T11: bank blob is still exactly 255 B");

	/* Round trip through the real apply path. */
	put_u16le(&blob[24], 9000U);
	put_u16le(&blob[26], 175U);
	reseal(blob, TUNING_BLOB_LEN - 2U);
	CHECK(tuning_config_apply_blob(blob, TUNING_BLOB_LEN), "T11: v8 blob accepted");
	CHECK(tuning_config_assist_torque_full_scale_centikg() == 9000U,
		"T11: full scale round-trips");
	CHECK(tuning_config_crank_length_mm() == 175U, "T11: crank length round-trips");

	/* Out of range is clamped, never wrapped. */
	put_u16le(&blob[24], 60000U);
	put_u16le(&blob[26], 900U);
	reseal(blob, TUNING_BLOB_LEN - 2U);
	tuning_config_apply_blob(blob, TUNING_BLOB_LEN);
	CHECK(tuning_config_assist_torque_full_scale_centikg() ==
		TUNING_ASSIST_TORQUE_FULL_SCALE_CENTIKG_MAX, "T11: over-range full scale clamps");
	CHECK(tuning_config_crank_length_mm() == TUNING_CRANK_LENGTH_MM_MAX,
		"T11: over-range crank length clamps");

	/*
	 * Zeros in a v8 blob are NOT a setting. "0 kg full scale" is a division by zero in the
	 * torque axis and "0 mm crank" makes every rider-power figure zero - so a blob built by
	 * tooling that left the reserved bytes alone must fall back to the defaults, not be
	 * taken literally.
	 */
	put_u16le(&blob[24], 0U);
	put_u16le(&blob[26], 0U);
	reseal(blob, TUNING_BLOB_LEN - 2U);
	tuning_config_apply_blob(blob, TUNING_BLOB_LEN);
	CHECK(tuning_config_assist_torque_full_scale_centikg() ==
		TUNING_ASSIST_TORQUE_FULL_SCALE_CENTIKG_DEFAULT,
		"T11: a zeroed v8 full scale falls back to 60.0 kg, never 0");
	CHECK(tuning_config_crank_length_mm() == TUNING_CRANK_LENGTH_MM_DEFAULT,
		"T11: a zeroed v8 crank length falls back to 165 mm, never 0");

	/*
	 * Migration. Every older version must still be ACCEPTED - a controller that rejected the
	 * blob an older Canable sends would lose the whole Dynamics write, not just two fields -
	 * and must migrate to the defaults, which are exactly what that firmware behaved as.
	 */
	for (uint8_t version = 2U; version <= 7U; version++) {
		/* Set a non-default value first, so a failed migration is visible rather than
		 * accidentally correct. */
		uint8_t v8blob[TUNING_BLOB_LEN];
		tuning_config_serialize(v8blob);
		put_u16le(&v8blob[24], 3000U);
		put_u16le(&v8blob[26], 190U);
		reseal(v8blob, TUNING_BLOB_LEN - 2U);
		tuning_config_apply_blob(v8blob, TUNING_BLOB_LEN);

		uint8_t old[TUNING_BLOB_LEN];
		tuning_config_serialize(old);
		old[2] = version;
		uint16_t body = (version == 2U) ? 20U : ((version <= 5U) ? 22U : TUNING_BLOB_LEN - 2U);
		uint16_t length = (version == 2U) ? TUNING_BLOB_LEN_V2
			: ((version <= 5U) ? TUNING_BLOB_LEN_V3 : TUNING_BLOB_LEN);
		reseal(old, body);

		char label[128];
		snprintf(label, sizeof(label), "T11: a v%u blob is still accepted", version);
		CHECK(tuning_config_apply_blob(old, length), label);
		snprintf(label, sizeof(label),
			"T11: v%u migrates the full scale to the 60.0 kg default (%u)",
			version, tuning_config_assist_torque_full_scale_centikg());
		CHECK(tuning_config_assist_torque_full_scale_centikg() ==
			TUNING_ASSIST_TORQUE_FULL_SCALE_CENTIKG_DEFAULT, label);
		snprintf(label, sizeof(label),
			"T11: v%u migrates the crank length to the 165 mm default (%u)",
			version, tuning_config_crank_length_mm());
		CHECK(tuning_config_crank_length_mm() == TUNING_CRANK_LENGTH_MM_DEFAULT, label);
	}

	/* A v7 blob must NOT lose its RUN smoothing window to the version bump - the exact trap
	 * FW-085 documented for start_steps, one version later. */
	{
		uint8_t v7[TUNING_BLOB_LEN];
		tuning_config_serialize(v7);
		v7[2] = 7U;
		put_u16le(&v7[20], 270U);
		reseal(v7, TUNING_BLOB_LEN - 2U);
		CHECK(tuning_config_apply_blob(v7, TUNING_BLOB_LEN), "T11: v7 blob accepted");
		CHECK(tuning_config_assist_torque_run_window_deg() == 270U,
			"T11: a v7 blob keeps its RUN window in DEGREES across the v8 bump");
	}

	/* An unknown future version is rejected outright rather than half-read. */
	{
		uint8_t bad[TUNING_BLOB_LEN];
		tuning_config_serialize(bad);
		bad[2] = 99U;
		reseal(bad, TUNING_BLOB_LEN - 2U);
		CHECK(!tuning_config_apply_blob(bad, TUNING_BLOB_LEN),
			"T11: an unknown version is rejected, not partially applied");
	}

	/* Leave the module on its defaults for anything that runs after this. */
	uint8_t restore[TUNING_BLOB_LEN];
	tuning_config_serialize(restore);
	put_u16le(&restore[24], TUNING_ASSIST_TORQUE_FULL_SCALE_CENTIKG_DEFAULT);
	put_u16le(&restore[26], TUNING_CRANK_LENGTH_MM_DEFAULT);
	reseal(restore, TUNING_BLOB_LEN - 2U);
	tuning_config_apply_blob(restore, TUNING_BLOB_LEN);
}

/* ------------------------------------------------------------------ table */

static void before_after_table(void)
{
	const uint16_t loads[] = { 500U, 1000U, 2000U, 3000U, 4000U, 6000U };
	const uint8_t cadences[] = { 40U, 60U, 80U, 100U };
	const assist_mode_type_t modes[] = {
		ASSIST_MODE_POWER_LINEAR, ASSIST_MODE_POWER_PROGRESSIVE, ASSIST_MODE_POWER_CURVE,
		ASSIST_MODE_EMTB, ASSIST_MODE_TORQUE,
	};
	const char *names[] = { "Power Linear", "Power Progressive", "Power Curve", "eMTB", "Torque" };

	printf("\n=== BEFORE / AFTER, level 3 defaults, %u mV pack, duty tracking cadence ===\n",
		BATTERY_MV);
	printf("BEFORE = the pre-FW-129 equations (60 kg x 500%%%% = full Iq for Power modes,\n");
	printf("         x160 read as a percent of the phase limit for eMTB/Torque), with the\n");
	printf("         P/U ceiling applied exactly as finish_power_request() used to.\n\n");

	for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		assist_level_config_t cfg = level_for(modes[m]);
		printf("--- %s ---\n", names[m]);
		printf("%6s %5s %7s %8s %9s %8s %8s %8s %8s %7s\n",
			"kg", "rpm", "u_abs", "humanW", "motorW", "launchIq", "normIq", "blend",
			"AFTER", "BEFORE");
		for (unsigned l = 0; l < sizeof(loads) / sizeof(loads[0]); l++) {
			for (unsigned c = 0; c < sizeof(cadences) / sizeof(cadences[0]); c++) {
				uint16_t native = native_for_centikg(loads[l]);
				uint16_t u_abs = (uint16_t)((uint32_t)LAUNCH_REFERENCE_U_ABS *
					cadences[c] / LAUNCH_REFERENCE_RPM);
				assist_mode_output_t out = run_once(&cfg, native, cadences[c], u_abs);
				int32_t before = old_reference_iq(&cfg,
					torque_input_native_delta_to_centikg(native),
					cadences[c], u_abs, native);
				printf("%3u.%02u %5u %7u %8u %9u %8ld %8ld %7u %8ld %7ld\n",
					loads[l] / 100U, loads[l] % 100U, cadences[c], u_abs,
					out.assist_basis_power_w, out.raw_motor_power_w,
					(long)out.iq_launch_request, (long)out.iq_normal_request,
					out.launch_blend_permille, (long)out.iq_request, (long)before);
			}
		}
		printf("\n");
	}
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	bool quiet = (argc > 1 && strcmp(argv[1], "--quiet") == 0);

	printf("FW-129 unit-domain harness: real torque_input.c + assist_modes.c + "
		"assist_start.c + tuning_config.c\n");
	torque_input_init();
	assist_modes_init();
	assist_modes_set_active_bank(0);

	t1_torque_normalization();
	t2_calibration_invariance();
	t3_power_linear();
	t4_progressive_and_curve();
	t5_emtb();
	t6_torque_mode();
	t7_low_duty_handover();
	t8_crank_length();
	t9_ride_feel_only();
	t10_modifier_matrix();
	t11_tuning_blob_migration();

	if (!quiet) {
		before_after_table();
	}

	if (host_test_failures == 0) {
		printf("FW-129 unit domain: ALL CHECKS PASSED\n");
		return 0;
	}
	printf("FW-129 unit domain: %d check(s) FAILED\n", host_test_failures);
	return 1;
}
