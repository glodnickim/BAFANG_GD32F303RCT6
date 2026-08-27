/*
 * FW-112 RIDER TORQUE PATH REDESIGN - exploratory host model (NOT a regression suite, NOT
 * wired into run-host-tests.ps1: nothing here is shipped code, it exists only to put real
 * numbers behind the architecture report). Not registered as a suite - it is model output,
 * like a spreadsheet, not a proof of shipped behaviour.
 *
 * CURRENT: drives the REAL production chain (torque_input.c with the shipped FW-112.4
 * asymmetric ARUN filter, assist_modes.c's real LINEAR-mode support-ratio math, and the real
 * assist_dynamics.c ramp with assist level 3's shipped constants) - exactly the path a rider
 * on 0.0392 actually gets.
 *
 * PROPOSED: reuses the SAME AFILT stream (same sensor, same 35 ms fast filter - nothing
 * upstream of AFILT changes) but replaces "torque_for_assist = ARUN" with a hand-coded
 * BASE + capped-PEAK effort estimate, feeds it through an illustrative progressive mode
 * curve (NOT the shipped power-curve code - a separate formula, see mode_curve_pct() - this
 * card's whole point is that the CURVE and the EFFORT ESTIMATE are separable, so both are
 * modelled directly here rather than smuggled back through assist_modes.c's own curve path),
 * and then through the SAME real assist_dynamics_apply() call with the SAME level-3 ramp
 * constants - so any metric difference between CURRENT and PROPOSED in this harness is
 * attributable to the effort-estimation + curve change alone, not to a different ramp.
 *
 * This is a MODEL for the design report, not a claim that these exact constants are final -
 * see the report's section 7/8/12 for the parameter sweep this file's output feeds.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../common/crank_model.h"
#include "../common/csv.h"

#include "assist_dynamics.h"
#include "assist_modes.h"
#include "config.h"
#include "rider_input.h"
#include "torque_input.h"
#include "tuning_config.h"

#define TEST_ASSIST_LEVEL 3U
#define STEP_INTERVAL_TICKS_60RPM 42U /* 60 rpm, 96 steps/rev, 4 kHz - crank_model handles others */
#define TEST_IQ_LIMIT ((int32_t)PH_CURRENT_MAX)
#define TEST_BATTERY_MV 42000U
#define TICK_HZ 4000.0

/* ---- PROPOSED: BASE + capped PEAK effort model ---- */
#ifndef BASE_TAU_MS
#define BASE_TAU_MS 500.0   /* slower than FW-112.4's own 120/350 ARUN - a step further: the
                                "rider average", not the per-tick assist signal */
#endif
#ifndef PEAK_CAP_DEFAULT
#define PEAK_CAP_DEFAULT 60.0 /* ~2.6 kg on the default scale */
#endif
static double g_base_native;
static double g_peak_gain = 0.5;      /* swept in main() */
static double g_peak_cap_native = PEAK_CAP_DEFAULT;

static void proposed_reset(void) { g_base_native = 0.0; }

static double proposed_step(double afilt_native)
{
	double tau_ticks = BASE_TAU_MS * (TICK_HZ / 1000.0);
	g_base_native += (afilt_native - g_base_native) / tau_ticks;
	double peak = afilt_native - g_base_native;
	if (peak < 0.0) peak = 0.0;
	if (peak > g_peak_cap_native) peak = g_peak_cap_native;
	return g_base_native + g_peak_gain * peak;
}

/* Illustrative progressive curve: motor_target_pct saturates near REF_CENTIKG so a LIGHT
 * rider reaches full assist without extreme torque, while still modulating below it.
 * pct = 100 * (1 - exp(-effort_centikg / REF_CENTIKG)) - a single design candidate, swept in
 * main() over REF_CENTIKG, not a final choice (report section 8). */
static double mode_curve_pct(double effort_centikg, double ref_centikg)
{
	double pct = 100.0 * (1.0 - exp(-effort_centikg / ref_centikg));
	if (pct > 100.0) pct = 100.0;
	if (pct < 0.0) pct = 0.0;
	return pct;
}

/* ---- driving harness: real torque_input.c + real assist_modes.c for CURRENT ---- */
typedef struct {
	double t, afilt, arun_current, base_proposed, effort_proposed;
	double iq_target_current, iq_ramped_current;
	double iq_target_proposed, iq_ramped_proposed;
} sample_t;

#define MAX_SAMPLES 40000
static sample_t g_samples[MAX_SAMPLES];
static int g_n;

typedef struct {
	const char *name;
	double duration_s;
	double mean_native, ripple_pct; /* steady/sine scenarios */
	int is_release;                 /* strong steady, then drop to 0 at t=1.0s */
} scenario_t;

static const scenario_t SCEN_STEADY   = { "steady",   2.0, 200.0,  0.0, 0 };
static const scenario_t SCEN_SINE     = { "sine",     3.0, 200.0, 40.0, 0 };
static const scenario_t SCEN_RELEASE  = { "release",  2.5, 300.0,  0.0, 1 };

static double target_fn(const scenario_t *sc, double t)
{
	if (sc->is_release) {
		return (t < 1.0) ? sc->mean_native : 0.0;
	}
	return sc->mean_native;
}

static void run_scenario(const scenario_t *sc, double cadence_rpm, double ref_centikg,
	double iq_rise_slow_ms, double iq_rise_fast_ms, double iq_fall_slow_ms, double iq_fall_fast_ms)
{
	torque_input_init();
	torque_input_set_run_window_deg(180U);
	torque_input_seed_run(0U);
	proposed_reset();

	crank_state_t crank;
	crank_state_init(&crank);
	crank_torque_shape_t shape = { 0 };
	shape.ripple_pct = sc->ripple_pct;

	/* CURRENT: real assist_modes_calculate() needs a rider_input_t + level config each tick. */
	rider_input_t rider;
	memset(&rider, 0, sizeof(rider));

	int32_t iq_ref_current = 0, iq_ref_proposed = 0;
	uint32_t last_step_count = 0;
	int n = (int)(sc->duration_s * TICK_HZ);
	if (n > MAX_SAMPLES) n = MAX_SAMPLES;

	for (int tick = 0; tick < n; tick++) {
		double t = (double)tick / TICK_HZ;
		crank_state_advance_tick(&crank, cadence_rpm);
		uint32_t steps_now = crank.step_count - last_step_count;
		last_step_count = crank.step_count;
		for (uint32_t i = 0; i < steps_now; i++) {
			torque_input_run_filter_step();
		}

		shape.mean_native_delta = target_fn(sc, t);
		uint16_t raw = crank_torque_raw_mv(&crank, &shape);
		int16_t corrected = torque_input_correct(raw);
		torque_input_coast_update(corrected, false, true);
		torque_input_update(raw, corrected, true);
		const torque_snapshot_t *snap = torque_input_get_snapshot();

		/* ---- CURRENT: real assist_modes_calculate(), LEVEL 3 LINEAR ---- */
		rider.torque_raw_mv = raw;
		rider.torque_corrected_mv = corrected;
		rider.torque_assist_filtered = snap->assist_delta_filtered_native;
		rider.torque_run_filtered = snap->assist_delta_run_native;
		rider.torque_load_centikg = torque_input_load_centikg();
		rider.torque_assist_now_native = snap->assist_delta_native;
		rider.cadence_rpm = (uint8_t)((cadence_rpm > 255.0) ? 255 : cadence_rpm);
		rider.wheel_speed_x100 = 1500U;
		rider.motor_erps = 200U;
		rider.pas_forward = true;
		rider.pedaling_active = true;
		rider.crank_forward_steps = 255U;
		rider.crank_direction_ok = true;
		rider.torque_sensor_valid = true;
		rider.pas_sensor_valid = true;
		rider.sample_tick = (uint32_t)tick;

		const assist_level_config_t *level_cfg =
			assist_modes_get_default_level(TEST_ASSIST_LEVEL);
		assist_mode_output_t mode_out;
		bool supported = assist_modes_calculate(&rider, level_cfg, TEST_BATTERY_MV,
			TEST_IQ_LIMIT, &mode_out);
		int32_t iq_target_current = supported ? mode_out.iq_request : 0;

		assist_dynamics_input_t dyn_in;
		memset(&dyn_in, 0, sizeof(dyn_in));
		dyn_in.speed_x100 = 1500U;
		dyn_in.cadence_rpm = rider.cadence_rpm;
		dyn_in.iq_scale = TEST_IQ_LIMIT;
		dyn_in.phase_current_max = TEST_IQ_LIMIT;
		dyn_in.profile_pedaling_active = true;
		dyn_in.profile_release_ms = 650U;
		dyn_in.ramp_up_slow_ms = (uint16_t)iq_rise_slow_ms;
		dyn_in.ramp_up_fast_ms = (uint16_t)iq_rise_fast_ms;
		dyn_in.ramp_down_slow_ms = (uint16_t)iq_fall_slow_ms;
		dyn_in.ramp_down_fast_ms = (uint16_t)iq_fall_fast_ms;
		iq_ref_current = assist_dynamics_apply(iq_target_current, iq_ref_current, &dyn_in);

		/* ---- PROPOSED: BASE + capped PEAK -> illustrative curve -> SAME ramp ---- */
		double effort_native = proposed_step(snap->assist_delta_filtered_native);
		double effort_centikg = torque_input_native_delta_to_centikg(
			(uint16_t)(effort_native < 0.0 ? 0.0 : effort_native));
		double pct = mode_curve_pct(effort_centikg, ref_centikg);
		int32_t iq_target_proposed = (int32_t)((pct / 100.0) * TEST_IQ_LIMIT);
		iq_ref_proposed = assist_dynamics_apply(iq_target_proposed, iq_ref_proposed, &dyn_in);

		if (g_n < MAX_SAMPLES) {
			sample_t *s = &g_samples[g_n++];
			s->t = t;
			s->afilt = snap->assist_delta_filtered_native;
			s->arun_current = snap->assist_delta_run_native;
			s->base_proposed = g_base_native;
			s->effort_proposed = effort_native;
			s->iq_target_current = iq_target_current;
			s->iq_ramped_current = iq_ref_current;
			s->iq_target_proposed = iq_target_proposed;
			s->iq_ramped_proposed = iq_ref_proposed;
		}
	}
}

typedef struct {
	double mean_ramped, peak_ramped, trough_ramped;
	double rise50_ms, decay_to_base_ms;
} metrics_t;

/* mode: 0=current ramped, 1=proposed ramped, 2=current RAW target, 3=proposed RAW target */
static metrics_t analyze(int mode, double window_t0, double window_t1, double base_ref)
{
	metrics_t m = { 0 };
	int i0 = (int)(window_t0 * TICK_HZ), i1 = (int)(window_t1 * TICK_HZ);
	if (i1 >= g_n) i1 = g_n - 1;
	double sum = 0.0, mx = -1e18, mn = 1e18;
	int count = 0;
	for (int i = i0; i <= i1; i++) {
		double v;
		switch (mode) {
		case 0: v = g_samples[i].iq_ramped_current; break;
		case 1: v = g_samples[i].iq_ramped_proposed; break;
		case 2: v = g_samples[i].iq_target_current; break;
		default: v = g_samples[i].iq_target_proposed; break;
		}
		sum += v; count++;
		if (v > mx) mx = v;
		if (v < mn) mn = v;
	}
	m.mean_ramped = count ? sum / count : 0.0;
	m.peak_ramped = mx;
	m.trough_ramped = mn;

	/* rise to 50% of window-end value, from t=0 (ramped modes only) */
	m.rise50_ms = -1.0;
	if (mode == 0 || mode == 1) {
		double final_v = (mode == 1) ? g_samples[i1].iq_ramped_proposed : g_samples[i1].iq_ramped_current;
		double target50 = final_v * 0.5;
		for (int i = 0; i < g_n; i++) {
			double v = (mode == 1) ? g_samples[i].iq_ramped_proposed : g_samples[i].iq_ramped_current;
			if (v >= target50) { m.rise50_ms = g_samples[i].t * 1000.0; break; }
		}
	}
	(void)base_ref;
	return m;
}

int main(void)
{
	assist_modes_init();
	assist_modes_set_active_bank(0);

	FILE *csv = csv_open_or_die("rider_effort_ab.csv",
		"scenario,cadence_rpm,rider,peak_gain,ref_centikg,"
		"current_mean,current_peak,current_trough,current_peakmean,current_troughmean,"
		"proposed_mean,proposed_peak,proposed_trough,proposed_peakmean,proposed_troughmean");

	/* level 3 shipped ramp constants (src/assist_modes.c DEFAULT_POWER_LEVEL for level 3) */
	const double RISE_SLOW = 650.0, RISE_FAST = 380.0, FALL_SLOW = 1050.0, FALL_FAST = 250.0;

	typedef struct { const char *name; double mean_native; } rider_t;
	const rider_t riders[3] = {
		{ "light",  120.0 },
		{ "medium", 220.0 },
		{ "strong", 380.0 },
	};
	const double cadences[3] = { 40.0, 60.0, 90.0 };
	const scenario_t *scenarios[2] = { &SCEN_STEADY, &SCEN_SINE };
	const double peak_gains[4] = { 0.25, 0.5, 0.75, 1.0 };
	const double ref_centikgs[3] = { 800.0, 1500.0, 2500.0 }; /* 8/15/25 kg reference */

	printf("=== CURRENT vs PROPOSED: mean/peak/trough motor assist (level 3, LINEAR, ratio=320%%) ===\n");
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			for (int s = 0; s < 2; s++) {
				scenario_t sc = *scenarios[s];
				sc.mean_native = riders[r].mean_native;
				g_peak_gain = 0.5;
				g_peak_cap_native = PEAK_CAP_DEFAULT;
				g_n = 0;
				run_scenario(&sc, cadences[c], 1500.0, RISE_SLOW, RISE_FAST, FALL_SLOW, FALL_FAST);
				double tail0 = sc.duration_s - 1.0, tail1 = sc.duration_s - (1.0 / TICK_HZ);
				if (tail0 < 0.0) tail0 = 0.0;
				metrics_t cur = analyze(0, tail0, tail1, 0.0);
				metrics_t prop = analyze(1, tail0, tail1, 0.0);
				double cur_pm = (cur.mean_ramped > 0.1) ? cur.peak_ramped / cur.mean_ramped : 0.0;
				double cur_tm = (cur.mean_ramped > 0.1) ? cur.trough_ramped / cur.mean_ramped : 0.0;
				double prop_pm = (prop.mean_ramped > 0.1) ? prop.peak_ramped / prop.mean_ramped : 0.0;
				double prop_tm = (prop.mean_ramped > 0.1) ? prop.trough_ramped / prop.mean_ramped : 0.0;
				printf("%-7s %-7s %5.0frpm  CURRENT(mean=%5.1f peak=%5.1f trough=%5.1f P/M=%.2f T/M=%.2f)  "
					"PROPOSED(mean=%5.1f peak=%5.1f trough=%5.1f P/M=%.2f T/M=%.2f)\n",
					sc.name, riders[r].name, cadences[c],
					cur.mean_ramped, cur.peak_ramped, cur.trough_ramped, cur_pm, cur_tm,
					prop.mean_ramped, prop.peak_ramped, prop.trough_ramped, prop_pm, prop_tm);
				fprintf(csv, "%s,%.0f,%s,%.2f,%.0f,%.2f,%.2f,%.2f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f,%.3f\n",
					sc.name, cadences[c], riders[r].name, g_peak_gain, ref_centikgs[1],
					cur.mean_ramped, cur.peak_ramped, cur.trough_ramped, cur_pm, cur_tm,
					prop.mean_ramped, prop.peak_ramped, prop.trough_ramped, prop_pm, prop_tm);
			}
		}
	}

	printf("\n=== RAW TARGET (before assist_dynamics ramp) vs RAMPED: does the ramp dominate? (sine, medium rider) ===\n");
	for (int c = 0; c < 3; c++) {
		scenario_t sc = SCEN_SINE;
		sc.mean_native = riders[1].mean_native;
		g_peak_gain = 0.5;
		g_peak_cap_native = PEAK_CAP_DEFAULT;
		g_n = 0;
		run_scenario(&sc, cadences[c], 1500.0, RISE_SLOW, RISE_FAST, FALL_SLOW, FALL_FAST);
		double tail0 = sc.duration_s - 1.0, tail1 = sc.duration_s - (1.0 / TICK_HZ);
		metrics_t cur_raw = analyze(2, tail0, tail1, 0.0);
		metrics_t prop_raw = analyze(3, tail0, tail1, 0.0);
		metrics_t cur_ramp = analyze(0, tail0, tail1, 0.0);
		metrics_t prop_ramp = analyze(1, tail0, tail1, 0.0);
		double cur_raw_pm = cur_raw.mean_ramped > 0.1 ? cur_raw.peak_ramped / cur_raw.mean_ramped : 0;
		double prop_raw_pm = prop_raw.mean_ramped > 0.1 ? prop_raw.peak_ramped / prop_raw.mean_ramped : 0;
		double cur_ramp_pm = cur_ramp.mean_ramped > 0.1 ? cur_ramp.peak_ramped / cur_ramp.mean_ramped : 0;
		double prop_ramp_pm = prop_ramp.mean_ramped > 0.1 ? prop_ramp.peak_ramped / prop_ramp.mean_ramped : 0;
		printf("  %.0f rpm  RAW TARGET   P/M: current=%.2f proposed=%.2f  (delta=%.2f)\n",
			cadences[c], cur_raw_pm, prop_raw_pm, cur_raw_pm - prop_raw_pm);
		printf("  %.0f rpm  AFTER RAMP   P/M: current=%.2f proposed=%.2f  (delta=%.2f)\n",
			cadences[c], cur_ramp_pm, prop_ramp_pm, cur_ramp_pm - prop_ramp_pm);
	}

	printf("\n=== PEAK_GAIN sweep (sine, medium rider, 60 rpm) ===\n");
	for (int g = 0; g < 4; g++) {
		scenario_t sc = SCEN_SINE;
		sc.mean_native = riders[1].mean_native;
		g_peak_gain = peak_gains[g];
		g_peak_cap_native = PEAK_CAP_DEFAULT;
		g_n = 0;
		run_scenario(&sc, 60.0, 1500.0, RISE_SLOW, RISE_FAST, FALL_SLOW, FALL_FAST);
		metrics_t prop = analyze(1, 2.0, 3.0 - 1.0 / TICK_HZ, 0.0);
		double pm = (prop.mean_ramped > 0.1) ? prop.peak_ramped / prop.mean_ramped : 0.0;
		printf("  peak_gain=%.2f  mean=%.1f peak=%.1f trough=%.1f P/M=%.2f\n",
			g_peak_gain, prop.mean_ramped, prop.peak_ramped, prop.trough_ramped, pm);
	}

	printf("\n=== REF_CENTIKG sweep: physical torque needed for ~100%% assist (steady, medium rider, 60 rpm) ===\n");
	for (int rc = 0; rc < 3; rc++) {
		for (int r = 0; r < 3; r++) {
			scenario_t sc = SCEN_STEADY;
			sc.mean_native = riders[r].mean_native;
			sc.duration_s = 3.0;
			g_peak_gain = 0.5;
			g_peak_cap_native = PEAK_CAP_DEFAULT;
			g_n = 0;
			run_scenario(&sc, 60.0, ref_centikgs[rc], RISE_SLOW, RISE_FAST, FALL_SLOW, FALL_FAST);
			metrics_t prop = analyze(1, 2.0, 3.0 - 1.0 / TICK_HZ, 0.0);
			double pct_of_full = (prop.mean_ramped / TEST_IQ_LIMIT) * 100.0;
			printf("  ref=%.0fcentikg  rider=%-7s mean_native=%.0f -> proposed mean assist = %.1f%% of iq_limit\n",
				ref_centikgs[rc], riders[r].name, sc.mean_native, pct_of_full);
		}
	}

	printf("\n=== RELEASE decay: peak-decay-to-base behaviour (release, medium rider, 60 rpm) ===\n");
	{
		scenario_t sc = SCEN_RELEASE;
		sc.mean_native = riders[1].mean_native;
		g_peak_gain = 0.5;
		g_peak_cap_native = PEAK_CAP_DEFAULT;
		g_n = 0;
		run_scenario(&sc, 60.0, 1500.0, RISE_SLOW, RISE_FAST, FALL_SLOW, FALL_FAST);
		/* find peak just before release (t=1.0) and time to decay to 10% of it after release */
		int i_release = (int)(1.0 * TICK_HZ);
		double peak_cur = g_samples[i_release].iq_ramped_current;
		double peak_prop = g_samples[i_release].iq_ramped_proposed;
		double thresh_cur = peak_cur * 0.1, thresh_prop = peak_prop * 0.1;
		double t_cur = -1.0, t_prop = -1.0;
		for (int i = i_release; i < g_n; i++) {
			if (t_cur < 0.0 && g_samples[i].iq_ramped_current <= thresh_cur) t_cur = g_samples[i].t - 1.0;
			if (t_prop < 0.0 && g_samples[i].iq_ramped_proposed <= thresh_prop) t_prop = g_samples[i].t - 1.0;
		}
		printf("  CURRENT:  at release iq=%.1f, decay to 10%% takes %.0f ms\n", peak_cur, t_cur * 1000.0);
		printf("  PROPOSED: at release iq=%.1f, decay to 10%% takes %.0f ms\n", peak_prop, t_prop * 1000.0);
	}

	fclose(csv);
	printf("\nwrote rider_effort_ab.csv\n");
	return 0;
}
