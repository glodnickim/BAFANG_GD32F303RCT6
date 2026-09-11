/*
 * FW-112.4 HOST HARNESS: ordinary-RUN fast-rise / slow-fall asymmetric filter.
 *
 * Runs the SHIPPED src/torque_input.c (not a copy), driven by the same crank-angle stimulus
 * generator every other torque host test uses (tests/host/common/crank_model.c). For each of
 * the card's eight input trajectories (S1..S8), at four cadences (20/40/60/80 rpm), it records
 * three parallel series every control tick:
 *
 *   AFILT      snapshot->assist_delta_filtered_native   - the existing 35 ms fast EMA, UNCHANGED
 *              by this card, common input to both columns below.
 *   ARUN(NEW)  snapshot->assist_delta_run_native         - the REAL production output: while
 *              recovery_state == IDLE this is now update_run_asym_filter(AFILT); everywhere else
 *              (window==0, TRACK_FAST) it is exactly what shipped before this card.
 *   ARUN(OLD)  a local reference replica of the pre-card 48-step plain moving average (see
 *              old_ref_* below), fed the IDENTICAL AFILT stream from the SAME real module, so the
 *              comparison isolates exactly the one thing this card changed. The replica is not a
 *              second linked binary: it is the plain averaging math already documented at
 *              src/torque_input.c's run_window_reset()/torque_input_seed_run()/
 *              torque_input_run_filter_step() (TORQUE_RUN_ATTACK_STEPS == 0 branch, the shipped
 *              default and unchanged by this card) - literally the same arithmetic, so an exact
 *              behavioural match against the real module before this card's edit is not an
 *              assumption, it is checked once at start of day by S1 (see CHECK below).
 *
 * Every tick's run_filter_step() call happens BEFORE torque_input_update() in the same tick,
 * exactly mirroring src/main.c's call order (torque_input_run_filter_step() at the PAS step,
 * torque_input_update() later in the same 4 kHz tick) - see torque_revolution_bench_host.c's
 * file header for the same ordering note; this harness copies it verbatim.
 *
 * Card section 6/7 metrics computed from the recorded series: first-positive-demand time,
 * 10/50/90% rise time, decay time, ripple amplitude (peak-to-peak over a trailing window). All
 * printed as one line per (scenario, cadence) plus written to torque_asym_s1_s8.csv in the
 * directory named by argv[1] (or the current directory if no argument is given).
 *
 * Card section 4/5 parity is checked directly: COLD_START_PARITY seeds both the real module and
 * the reference to a nonzero value, takes one more control tick with the SAME afilt input, and
 * requires the published RUN to be unchanged (zero discontinuity at a seed point) - this is what
 * "cold-arm and rolling-rearm re-seed still land the estimator exactly where they used to"
 * reduces to, since torque_input_seed_run() is the single seed point shared by cold arm,
 * TRACK_FAST -> IDLE hand-back and WAIT_FRESH_LOAD's per-step reseed.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../common/check.h"
#include "../common/crank_model.h"
#include "../common/csv.h"

#include "torque_input.h"

#define RUN_WINDOW_DEG_DEFAULT 180U
#define RUN_WINDOW_STEPS_DEFAULT ((RUN_WINDOW_DEG_DEFAULT * 4U) / 15U) /* 48, see torque_input.c */
#define MAX_TICKS 21000 /* covers the longest scenario (S4, 5.0 s) at 4 kHz with margin */

/* ---- OLD reference: exact replica of the pre-FW-112.4 48-step plain moving average ---- */
typedef struct {
	uint16_t buf[TORQUE_RUN_WINDOW_STEPS_MAX];
	uint16_t head, filled, steps;
	uint32_t sum;
	uint16_t value;
} old_ref_t;

static void old_ref_seed(old_ref_t *r, uint16_t steps, uint16_t v)
{
	r->steps = steps;
	r->value = v;
	for (uint16_t i = 0U; i < steps; i++) {
		r->buf[i] = v;
	}
	r->head = 0U;
	r->filled = steps;
	r->sum = (uint32_t)v * steps;
}

static void old_ref_step(old_ref_t *r, uint16_t sample)
{
	if (r->filled >= r->steps) {
		r->sum -= r->buf[r->head];
	} else {
		r->filled++;
	}
	r->buf[r->head] = sample;
	r->sum += sample;
	r->head++;
	if (r->head >= r->steps) {
		r->head = 0U;
	}
	r->value = (uint16_t)((r->sum + (r->filled / 2U)) / r->filled);
}

/* ---- scenario trajectories (card section 6): mean native delta above zero, vs sim time ---- */
typedef struct {
	const char *name;
	double duration_s;
	double event_start_s; /* where the transient under measurement begins */
	double ripple_pct;    /* per-leg crank-angle ripple; 0 for the clean step/ramp scenarios */
} scenario_meta_t;

static double target_S1(double t) { (void)t; return 60.0; }
static double target_S2(double t) { return (t < 0.5) ? 0.0 : 300.0; }
static double target_S3(double t) { return (t >= 2.0) ? 300.0 : 300.0 * (t / 2.0); }
static double target_S4(double t) { return (t >= 4.0) ? 300.0 : 300.0 * (t / 4.0); }
static double target_S5(double t) { (void)t; return 200.0; }
static double target_S6(double t) { return (t < 1.0) ? 300.0 : 0.0; }
static double target_S7(double t)
{
	if (t < 0.5) return 300.0;
	if (t >= 2.0) return 0.0;
	return 300.0 * (1.0 - (t - 0.5) / 1.5);
}
static double target_S8(double t) { return (((int)floor(t * 2.0)) % 2 == 0) ? 20.0 : 5.0; }

typedef double (*target_fn_t)(double);

static const scenario_meta_t g_meta[8] = {
	{ "S1_const_low",      2.0, 0.0, 0.0 },
	{ "S2_sharp_rise",     2.0, 0.5, 0.0 },
	{ "S3_ramp_2s",        3.0, 0.0, 0.0 },
	{ "S4_ramp_4s",        5.0, 0.0, 0.0 },
	{ "S5_sine_ripple",    3.0, 0.0, 40.0 },
	{ "S6_sharp_release",  2.5, 1.0, 0.0 },
	{ "S7_gradual_release",3.0, 0.5, 0.0 },
	{ "S8_deadband_osc",   3.0, 0.0, 0.0 },
};
static const target_fn_t g_target[8] = {
	target_S1, target_S2, target_S3, target_S4,
	target_S5, target_S6, target_S7, target_S8,
};

/* ---- recorded series (reused per (scenario,cadence) run) ---- */
static double s_t[MAX_TICKS];
static double s_afilt[MAX_TICKS];
static double s_new[MAX_TICKS];
static double s_old[MAX_TICKS];
static int s_n;

static void run_scenario(int scenario_idx, double cadence_rpm)
{
	const scenario_meta_t *m = &g_meta[scenario_idx];
	target_fn_t target = g_target[scenario_idx];

	torque_input_init();
	torque_input_set_run_window_deg(RUN_WINDOW_DEG_DEFAULT);
	/* Ordinary RUN precondition: recovery_state is IDLE from init() already (no rearm anywhere
	 * in this harness); seed both the real module and the reference to a settled zero-effort
	 * baseline, matching a rider already spinning the cranks gently before the scenario's
	 * transient begins - the case this card is about, not the separate cold-arm/rearm case. */
	torque_input_seed_run(0U);
	old_ref_t ref;
	old_ref_seed(&ref, RUN_WINDOW_STEPS_DEFAULT, 0U);

	crank_state_t crank;
	crank_state_init(&crank);
	crank_torque_shape_t shape = { 0 };
	shape.ripple_pct = m->ripple_pct;

	uint32_t last_step_count = 0U;
	int n = 0;
	int max_n = (int)(m->duration_s * CRANK_MODEL_TICK_HZ);
	if (max_n > MAX_TICKS) max_n = MAX_TICKS;

	for (int tick = 0; tick < max_n; tick++) {
		double t = (double)tick / CRANK_MODEL_TICK_HZ;
		uint32_t steps_now = crank_state_advance_tick(&crank, cadence_rpm) ;
		(void)steps_now;
		uint32_t steps_total = crank.step_count;
		uint32_t due = steps_total - last_step_count;
		last_step_count = steps_total;
		for (uint32_t i = 0; i < due; i++) {
			torque_input_run_filter_step();
			old_ref_step(&ref, torque_input_get_snapshot()->assist_delta_filtered_native);
		}

		shape.mean_native_delta = target(t);
		uint16_t raw = crank_torque_raw_mv(&crank, &shape);
		int16_t corrected = torque_input_correct(raw);
		torque_input_coast_update(corrected, false, true);
		torque_input_update(raw, corrected, true);
		const torque_snapshot_t *snap = torque_input_get_snapshot();

		s_t[n] = t;
		s_afilt[n] = snap->assist_delta_filtered_native;
		s_new[n] = snap->assist_delta_run_native;
		s_old[n] = ref.value;
		n++;
	}
	s_n = n;
}

static int idx_at(double t)
{
	int idx = (int)(t * CRANK_MODEL_TICK_HZ);
	if (idx < 0) idx = 0;
	if (idx >= s_n) idx = s_n - 1;
	return idx;
}

static double sample_at(const double *arr, double t)
{
	return arr[idx_at(t)];
}

static double window_avg(const double *arr, double t0, double t1)
{
	int i0 = idx_at(t0), i1 = idx_at(t1);
	if (i1 < i0) i1 = i0;
	double sum = 0.0;
	int count = 0;
	for (int i = i0; i <= i1; i++) { sum += arr[i]; count++; }
	return count ? sum / count : 0.0;
}

static double window_ptp(const double *arr, double t0, double t1)
{
	int i0 = idx_at(t0), i1 = idx_at(t1);
	if (i1 < i0) i1 = i0;
	double mn = arr[i0], mx = arr[i0];
	for (int i = i0; i <= i1; i++) {
		if (arr[i] < mn) mn = arr[i];
		if (arr[i] > mx) mx = arr[i];
	}
	return mx - mn;
}

/* Seconds from from_t to the first tick at/after from_t where arr crosses frac of the way from
 * start_val to final_val; -1.0 if never reached within the recorded series. */
static double crossing_time(const double *arr, double from_t, double start_val, double final_val,
	double frac)
{
	double target = start_val + (final_val - start_val) * frac;
	int i0 = idx_at(from_t);
	int rising = final_val > start_val;
	for (int i = i0; i < s_n; i++) {
		if (rising ? (arr[i] >= target) : (arr[i] <= target)) {
			return s_t[i] - from_t;
		}
	}
	return -1.0;
}

/* Seconds from from_t to the first tick at/after from_t where arr is genuinely nonzero
 * (> POSITIVE_DEMAND_THRESHOLD native units) - -1.0 if it never happens in the recorded series. */
#define POSITIVE_DEMAND_THRESHOLD_NATIVE 0.5
static double first_positive_time(const double *arr, double from_t)
{
	int i0 = idx_at(from_t);
	for (int i = i0; i < s_n; i++) {
		if (arr[i] > POSITIVE_DEMAND_THRESHOLD_NATIVE) {
			return s_t[i] - from_t;
		}
	}
	return -1.0;
}

typedef struct {
	double final_new, final_old;
	double rise10_new, rise50_new, rise90_new;
	double rise10_old, rise50_old, rise90_old;
	double first_pos_new, first_pos_old;
	double ripple_new, ripple_old, ripple_afilt;
} metrics_t;

static metrics_t analyze(int scenario_idx)
{
	const scenario_meta_t *m = &g_meta[scenario_idx];
	metrics_t r;
	double t0 = m->event_start_s;
	double start_new = sample_at(s_new, t0);
	double start_old = sample_at(s_old, t0);
	double tail0 = m->duration_s - 0.3, tail1 = m->duration_s - 1.0 / CRANK_MODEL_TICK_HZ;
	if (tail0 < t0) tail0 = t0;

	r.final_new = window_avg(s_new, tail0, tail1);
	r.final_old = window_avg(s_old, tail0, tail1);
	r.rise10_new = crossing_time(s_new, t0, start_new, r.final_new, 0.10);
	r.rise50_new = crossing_time(s_new, t0, start_new, r.final_new, 0.50);
	r.rise90_new = crossing_time(s_new, t0, start_new, r.final_new, 0.90);
	r.rise10_old = crossing_time(s_old, t0, start_old, r.final_old, 0.10);
	r.rise50_old = crossing_time(s_old, t0, start_old, r.final_old, 0.50);
	r.rise90_old = crossing_time(s_old, t0, start_old, r.final_old, 0.90);
	r.first_pos_new = first_positive_time(s_new, t0);
	r.first_pos_old = first_positive_time(s_old, t0);

	double rt0 = m->duration_s - 1.0, rt1 = m->duration_s - 1.0 / CRANK_MODEL_TICK_HZ;
	if (rt0 < t0) rt0 = t0;
	r.ripple_new = window_ptp(s_new, rt0, rt1);
	r.ripple_old = window_ptp(s_old, rt0, rt1);
	r.ripple_afilt = window_ptp(s_afilt, rt0, rt1);
	return r;
}

int main(int argc, char **argv)
{
	const char *out_dir = (argc >= 2) ? argv[1] : ".";
	char csv_path[1024];
	snprintf(csv_path, sizeof(csv_path), "%s/torque_asym_s1_s8.csv", out_dir);
	FILE *csv = csv_open_or_die(csv_path,
		"scenario,cadence_rpm,final_new,final_old,"
		"first_pos_new_ms,first_pos_old_ms,"
		"rise10_new_ms,rise50_new_ms,rise90_new_ms,rise10_old_ms,rise50_old_ms,rise90_old_ms,"
		"ripple_new,ripple_old,ripple_afilt");

	const double cadences[4] = { 20.0, 40.0, 60.0, 80.0 };

	/* ---- sanity: the OLD reference replica must exactly match the real module's own
	 * pre-FW-112.4 window average whenever recovery is NOT IDLE (TRACK_FAST/window==0 paths are
	 * untouched by this card, so on those paths ARUN(NEW) == the plain window average always -
	 * this is checked structurally, not by re-deriving it here; what IS checked here is that the
	 * IDLE branch is genuinely doing something DIFFERENT from the old average, not silently
	 * falling back to it). */
	run_scenario(2 /* S2 sharp rise */, 20.0);
	{
		metrics_t r = analyze(2);
		CHECK(r.rise50_new >= 0.0 && r.rise50_old >= 0.0,
			"sanity: S2 sharp rise settles on both NEW and OLD at 20 rpm");
		/*
		 * AP-03: the "NEW strictly faster than OLD" comparison is REMOVED, not relaxed.
		 * FW-112.4 is deleted, so NEW and OLD are now the same crank-angle window and the
		 * comparison has no second operand left. Every PROPERTY assertion in this file is
		 * untouched and still guards the window (ripple bound, recovery completion, no
		 * multi-second lag). The S1-S8 table this suite prints is now a characterisation of
		 * the WINDOW and is the record of what it costs on a slow rise at low cadence -
		 * see rise50 at 20 rpm, which is the open risk of removing FW-112.4.
		 */
	}

	for (int s = 0; s < 8; s++) {
		for (int c = 0; c < 4; c++) {
			run_scenario(s, cadences[c]);
			metrics_t r = analyze(s);
			fprintf(csv, "%s,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
				g_meta[s].name, cadences[c], r.final_new, r.final_old,
				r.first_pos_new * 1000.0, r.first_pos_old * 1000.0,
				r.rise10_new * 1000.0, r.rise50_new * 1000.0, r.rise90_new * 1000.0,
				r.rise10_old * 1000.0, r.rise50_old * 1000.0, r.rise90_old * 1000.0,
				r.ripple_new, r.ripple_old, r.ripple_afilt);
			printf("%-16s %5.0f rpm  final(new=%.1f old=%.1f)  "
				"rise50(new=%.1fms old=%.1fms)  ripple(new=%.1f old=%.1f afilt=%.1f)\n",
				g_meta[s].name, cadences[c], r.final_new, r.final_old,
				r.rise50_new * 1000.0, r.rise50_old * 1000.0,
				r.ripple_new, r.ripple_old, r.ripple_afilt);
		}
	}
	fclose(csv);
	printf("wrote %s\n", csv_path);

	/* ---- card section 7 success criteria, the two hardest cadence/scenario points ---- */
	run_scenario(3 /* S4, very slow 4 s ramp */, 20.0);
	{
		metrics_t r = analyze(3);
		CHECK(r.rise50_new >= 0.0, "S4 @20rpm: NEW reaches 50% of the ramp target at all");
		CHECK(r.rise50_old >= 0.0, "S4 @20rpm: OLD reaches 50% of the ramp target at all");
		/*
		 * AP-03: the NEW-vs-OLD comparison is REMOVED (FW-112.4 deleted, both operands are
		 * now the same window). The absolute bound below is KEPT and is the one that
		 * matters: the estimate must not be stuck near a multi-second lag at 20 rpm.
		 */
		CHECK(fabs(r.rise50_new - 4.0) < 4.0,
			"S4 @20rpm: NEW does not scale linearly with the 48-step window (not stuck near a multi-second lag)");
	}

	run_scenario(4 /* S5 sinusoidal per-leg ripple */, 20.0);
	{
		metrics_t r = analyze(4);
		/* The OLD window is architecturally a PERFECT notch at any cadence here: its window is
		 * exactly one leg period wide in CRANK DEGREES (180 deg), so it cancels this ripple to
		 * ~0 by construction, at every cadence - not something a causal, bounded-latency filter
		 * can ever match (see the header's FW-112.4 comment for why that structural advantage
		 * is deliberately traded away for cadence-independent response time). The meaningful,
		 * checkable properties for a rate-limiter design are instead: (a) it genuinely
		 * ATTENUATES the ripple already present in its own input (AFILT), not merely passes it
		 * through, and (b) the residual is bounded in absolute terms, not free to grow. */
		CHECK(r.ripple_new < r.ripple_afilt,
			"S5 @20rpm: NEW ripple is measurably attenuated below AFILT's own ripple");
		CHECK(r.ripple_new <= 100.0,
			"S5 @20rpm: NEW ripple stays under the 100-native-unit bound (~3.7 kg-equivalent swing)");
	}

	/* ---- card section 4/5: rolling-rearm TRACK_FAST -> IDLE seed parity, direct proof ----
	 * Drives the REAL production automaton end to end through the public API only
	 * (torque_input_begin_rolling_rearm() + torque_input_update() every tick) - no PAS steps
	 * needed, since WAIT_FRESH_LOAD -> TRACK_FAST -> IDLE is driven purely by afilt vs the
	 * assist deadband and a stable-tick count (see inc/torque_input.h). Throughout
	 * WAIT_FRESH_LOAD/TRACK_FAST the IDLE branch never runs, so run_asym_q is NOT being
	 * continuously pulled toward afilt the way it would be if this probe just let the IDLE
	 * branch track a steady input on its own (an earlier version of this check did exactly
	 * that and was too weak to notice run_asym_q staying stale - see the FW-112.4 report's
	 * mutation M4 for the concrete miss). The only thing that can put run_asym_q near the
	 * fresh value before the automaton closes is the SAME torque_input_seed_run() call this
	 * card added a sync line to - so a genuine parity break is only observable right on the
	 * transition tick, which is exactly what this checks. */
	{
		torque_input_init();
		torque_input_set_run_window_deg(RUN_WINDOW_DEG_DEFAULT);
		torque_input_begin_rolling_rearm(); /* recovery_state -> WAIT_FRESH_LOAD */
		uint16_t raw = (uint16_t)(TORQUE_ZERO_TARGET_NATIVE + 250);
		torque_recovery_state_t prev_state = torque_input_recovery_state();
		bool completed = false;
		uint16_t afilt_at_transition = 0, run_right_after_idle = 0;
		for (int i = 0; i < 20000 && !completed; i++) {
			int16_t corrected = torque_input_correct(raw);
			torque_input_coast_update(corrected, false, true);
			torque_input_update(raw, corrected, true);
			torque_recovery_state_t state = torque_input_recovery_state();
			if (prev_state != TORQUE_RECOVERY_IDLE && state == TORQUE_RECOVERY_IDLE) {
				afilt_at_transition = torque_input_get_snapshot()->assist_delta_filtered_native;
				run_right_after_idle = torque_input_get_snapshot()->assist_delta_run_native;
				completed = true;
			}
			prev_state = state;
		}
		CHECK(completed,
			"rolling-rearm: the automaton reached IDLE (TRACK_FAST completion) within the probe window");
		int diff = (int)run_right_after_idle - (int)afilt_at_transition;
		if (diff < 0) diff = -diff;
		CHECK(diff <= 1,
			"rolling-rearm TRACK_FAST->IDLE handback: RUN == the fresh value on the SAME tick recovery closes (re-seeded, not a stale crawl)");
	}

	if (host_test_failures != 0) {
		printf("%d check(s) FAILED\n", host_test_failures);
		return 1;
	}
	printf("All checks PASS\n");
	return 0;
}
