/*
 * EVistDrive closed-loop supervisory SIL.
 * Runs shipped production C for PAS -> torque -> rider_input -> ride_control -> final 16 kHz Iq.
 * The plant is deliberately small but CLOSED LOOP: final Iq accelerates a rotor, rotor motion
 * generates Hall edges/age/speed, and those facts return to ride_control (notably START preload).
 * The rider plant generates physical crank angle, quadrature PAS and a two-leg torque waveform.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assist_modes.h"
#include "config.h"
#include "motor_core.h"
#include "pas_cadence.h"
#include "pas_direction.h"
#include "pas_sampler.h"
#include "rider_input.h"
#include "ride_control.h"
#include "ride_session.h"
#include "torque_input.h"
#include "tuning_config.h"

#define CTRL_HZ 4000U
#define INNER_PER_CTRL 4U
#define PI 3.14159265358979323846
#define TEST_BATTERY_MV 42000U
#define TEST_VOLTAGE_RAW 2000
#define TEST_TEMP_C 25

typedef struct {
    double iq_actual;
    double erps;
    double theta_e_rev;
    double breakaway_iq;
    double accel_erps_s_per_iq;
    double drag_per_s;
    uint16_t hall_age_ticks;
    uint32_t hall_edges;
    uint32_t last_hall_tick;
    uint32_t prev_hall_tick;
    bool hall_edge_this_ctrl;
} plant_t;

typedef struct {
    double rpm;
    double phase_steps;
    uint8_t state_index;
    uint8_t ab;
    uint8_t bounce_ticks;
    bool inject_bounce;
    uint32_t forward_edges;
    double torque_mean_ckg;
    double torque_ripple_ckg;
} rider_plant_t;

typedef struct {
    MotorState_t MS;
    plant_t plant;
    rider_plant_t rider;
    uint32_t tick;
    uint16_t cadence_filtered_x8;
    uint16_t last_forward_gap;
    uint16_t stop_timeout;
    uint8_t start_phase;
    uint32_t direction_inhibit_ticks;
    uint32_t false_reverse_events;
    uint32_t first_permission_tick;
    uint32_t first_iq_tick;
    uint32_t first_hall_tick;
    int32_t iq_min_run;
    int32_t iq_max_run;
    double iq_sum_run;
    double iq_sq_sum_run;
    uint32_t iq_samples_run;
} sim_t;

/* Raw forward ring for PAS_DIR_SIGN=-1: 00 -> 10 -> 11 -> 01 -> 00. */
static const uint8_t FWD_AB[4] = {0U, 2U, 3U, 1U};

static uint16_t sensor_native_from_ckg(double ckg)
{
    double d;
    if (ckg < 0.0) ckg = 0.0;
    if (ckg <= TORQUE_DEFAULT_LOW_CENTIKG) {
        d = ckg * (double)TORQUE_DEFAULT_LOW_NATIVE / TORQUE_DEFAULT_LOW_CENTIKG;
    } else {
        d = TORQUE_DEFAULT_LOW_NATIVE +
            (ckg - TORQUE_DEFAULT_LOW_CENTIKG) *
            (double)(TORQUE_DEFAULT_HIGH_NATIVE - TORQUE_DEFAULT_LOW_NATIVE) /
            (double)(TORQUE_DEFAULT_HIGH_CENTIKG - TORQUE_DEFAULT_LOW_CENTIKG);
    }
    if (d > TORQUE_SPAN_MAX_NATIVE) d = TORQUE_SPAN_MAX_NATIVE;
    return (uint16_t)llround((double)TORQUE_ZERO_TARGET_NATIVE + d);
}

static void plant_init(plant_t *p, double breakaway_iq)
{
    memset(p, 0, sizeof(*p));
    p->breakaway_iq = breakaway_iq;
    p->accel_erps_s_per_iq = 20.0; /* plant parameter, not firmware truth */
    p->drag_per_s = 5.0;
    p->hall_age_ticks = 0xFFFFU;
}

static void plant_inner_tick(plant_t *p, int32_t iq_cmd, uint32_t ctrl_tick)
{
    const double dt = 1.0 / 16000.0;
    /* Current loop stand-in: ~2 ms current tracking. The production PI is independently host-
     * tested; here we need its actuator effect so supervisory states see real/no Hall motion. */
    const double alpha = dt / (0.002 + dt);
    p->iq_actual += ((double)iq_cmd - p->iq_actual) * alpha;

    double drive = p->iq_actual - p->breakaway_iq;
    if (drive < 0.0) drive = 0.0;
    double accel = drive * p->accel_erps_s_per_iq - p->drag_per_s * p->erps;
    p->erps += accel * dt;
    if (p->erps < 0.0) p->erps = 0.0;

    double old = p->theta_e_rev;
    p->theta_e_rev += p->erps * dt;
    /* Hall edge each 1/6 electrical revolution. */
    uint64_t old_sector = (uint64_t)floor(old * 6.0);
    uint64_t new_sector = (uint64_t)floor(p->theta_e_rev * 6.0);
    if (new_sector != old_sector) {
        p->hall_edge_this_ctrl = true;
        p->hall_edges += (uint32_t)(new_sector - old_sector);
        p->prev_hall_tick = p->last_hall_tick;
        p->last_hall_tick = ctrl_tick;
    }
}

static void rider_init(rider_plant_t *r, double rpm, double mean_ckg, double ripple_ckg, bool bounce)
{
    memset(r, 0, sizeof(*r));
    r->rpm = rpm;
    r->torque_mean_ckg = mean_ckg;
    r->torque_ripple_ckg = ripple_ckg;
    r->state_index = 0U;
    r->ab = FWD_AB[0];
    r->inject_bounce = bounce;
}

static uint8_t rider_pas_tick(rider_plant_t *r)
{
    if (r->bounce_ticks == 1U) {
        r->bounce_ticks = 2U;
        /* return briefly to the previous raw state: one-tick reverse bounce */
        return FWD_AB[(r->state_index + 3U) & 3U];
    }
    if (r->bounce_ticks == 2U) {
        r->bounce_ticks = 0U;
        return r->ab;
    }
    r->phase_steps += r->rpm * (double)PAS_TRANSITIONS_PER_REV / (60.0 * CTRL_HZ);
    if (r->phase_steps >= 1.0) {
        r->phase_steps -= 1.0;
        r->state_index = (uint8_t)((r->state_index + 1U) & 3U);
        r->ab = FWD_AB[r->state_index];
        r->forward_edges++;
        /* deterministic bounce on every 7th physical edge */
        if (r->inject_bounce && (r->forward_edges % 7U) == 0U) r->bounce_ticks = 1U;
    }
    return r->ab;
}

static double rider_torque_ckg(const rider_plant_t *r, uint32_t tick)
{
    double rev_s = r->rpm / 60.0;
    double t = (double)tick / CTRL_HZ;
    /* two leg pushes per crank revolution; mean plus bounded sinusoidal ripple */
    double v = r->torque_mean_ckg + r->torque_ripple_ckg * sin(4.0 * PI * rev_s * t);
    return v < 0.0 ? 0.0 : v;
}

static void sim_init(sim_t *s, double rpm, double mean_ckg, double ripple_ckg,
                     bool bounce, double breakaway_iq)
{
    memset(s, 0, sizeof(*s));
    torque_input_init();
    torque_input_startup_zero(TORQUE_ZERO_TARGET_NATIVE);
    torque_input_set_run_window_deg(tuning_config_assist_torque_run_window_deg());
    assist_modes_init();
    assist_modes_set_active_bank(0U);
    motor_core_init(&s->MS);
    ride_control_init();
    pas_direction_init();
    pas_cadence_reset();
    pas_sampler_init(0U);
    rider_init(&s->rider, rpm, mean_ckg, ripple_ckg, bounce);
    plant_init(&s->plant, breakaway_iq);
    s->last_forward_gap = PAS_STOP_TICKS;
    s->stop_timeout = PAS_STOP_TICKS;
    s->iq_min_run = 0x7fffffff;
    s->iq_max_run = -0x7fffffff;
    /* seed physical PAS state */
    pas_sampler_isr_tick(s->rider.ab, 0U);
}

static void process_pas(sim_t *s, uint8_t ab)
{
    pas_sampler_isr_tick(ab, s->tick);
    pas_step_event_t ev;
    while (pas_sampler_pop(&ev)) {
        int8_t st = ev.step;
        if (st > 0) {
            s->last_forward_gap = ev.gap ? ev.gap : s->last_forward_gap;
            uint32_t x = (uint32_t)s->last_forward_gap * 2U;
            if (x < PAS_STOP_TICKS) x = PAS_STOP_TICKS;
            if (x > PAS_STOP_TICKS_MAX) x = PAS_STOP_TICKS_MAX;
            s->stop_timeout = (uint16_t)x;
            pas_direction_on_step(st);
            torque_input_run_filter_step();
            pas_cadence_step_t cad = pas_cadence_forward_step(ev.tick,
                pas_direction_fwd_run() == 1U ? 1U : 0U);
            if (cad.pulse && cad.measured) {
                s->MS.cadence = cad.rpm;
                s->start_phase = 0U;
                s->cadence_filtered_x8 -= s->cadence_filtered_x8 >> 3;
                s->cadence_filtered_x8 += s->MS.cadence;
            }
        } else if (st < 0) {
            s->false_reverse_events++;
            pas_cadence_break_epoch(0U);
            pas_direction_on_step(st);
        } else {
            pas_cadence_break_epoch(1U);
            pas_direction_on_step(st);
        }
    }
    if (pas_sampler_take_overflow()) pas_cadence_break_epoch(2U);
    if (s->MS.cadence == 0U && !s->start_phase &&
        pas_direction_fwd_run() >= START_PHASE_STEPS) s->start_phase = 1U;
}

static void sim_ctrl_tick(sim_t *s, FILE *csv)
{
    s->tick++;
    s->plant.hall_edge_this_ctrl = false;

    uint8_t ab = rider_pas_tick(&s->rider);
    process_pas(s, ab);

    uint32_t idle = s->tick - pas_sampler_last_transition_tick();
    bool real_stop = idle > s->stop_timeout;
    bool crank_direction_ok = (s->MS.cadence > 0U || s->start_phase) && !real_stop;
    bool pedaling = crank_direction_ok &&
        pas_direction_fwd_run() >= tuning_config_start_steps();

    double load_ckg = rider_torque_ckg(&s->rider, s->tick);
    uint16_t raw = sensor_native_from_ckg(load_ckg);
    torque_input_update(raw, torque_input_correct(raw), true);
    const torque_snapshot_t *ts = torque_input_get_snapshot();

    rider_input_t r;
    memset(&r, 0, sizeof(r));
    r.torque_raw_mv = raw;
    r.torque_corrected_mv = torque_input_correct(raw);
    r.torque_filtered = ts->delta_native;
    r.torque_assist_now_native = ts->assist_delta_native;
    r.torque_assist_filtered = ts->assist_delta_filtered_native;
    r.torque_run_filtered = ts->assist_delta_run_native;
    r.torque_load_centikg = ts->load_centikg;
    r.cadence_rpm = s->MS.cadence;
    r.wheel_speed_x100 = 0U;
    r.motor_erps = (uint16_t)(s->plant.erps > 65535.0 ? 65535.0 : llround(s->plant.erps));
    r.motor_erps_age_ticks = s->plant.hall_age_ticks;
    r.pas_forward = pedaling;
    r.pedaling_active = pedaling;
    r.crank_forward_steps = pas_direction_fwd_run();
    r.crank_direction_ok = crank_direction_ok;
    r.real_stop = real_stop;
    r.wheel_valid = false;
    r.direction_inhibit_active = pas_direction_direction_inhibit_active();
    r.forward_confirmed_this_tick = pas_direction_forward_confirmed_last_call();
    r.sample_tick = s->tick;
    r.start_phase = s->start_phase != 0U;
    r.torque_sensor_valid = true;
    r.pas_sensor_valid = true;
    rider_input_update(&r);

    ride_control_input_t in;
    memset(&in, 0, sizeof(in));
    in.speed_x100 = 0U;
    in.cadence_rpm = s->MS.cadence;
    in.assist_level_index = 3U; /* default Power level 3 */
    in.battery_voltage_mv = TEST_BATTERY_MV;
    in.iq_scale = PH_CURRENT_MAX;
    in.ride_core_iq_limit = PH_CURRENT_MAX;
    in.phase_current_max = PH_CURRENT_MAX;
    in.battery_current_mA = 0;
    in.battery_current_max = 15000;
    in.u_abs = 600;
    in.cal_i = 95;
    in.current_iq = (int32_t)llround(s->plant.iq_actual);
    in.current_id = 0;
    in.voltage_raw = TEST_VOLTAGE_RAW;
    in.voltage_min_raw = VOLTAGE_MIN;
    in.controller_temperature_c = TEST_TEMP_C;
    in.cadence_filtered_x8 = s->cadence_filtered_x8;
    in.speed_limit_x100 = SPEEDLIMIT;
    in.legal_enabled = true;
    in.elapsed_ticks = 1U;
    ride_control_update(&in);

    if (ride_control_get_session_state() == RIDE_SESSION_ACTIVE && s->first_permission_tick == 0U)
        s->first_permission_tick = s->tick;

    for (unsigned k = 0; k < INNER_PER_CTRL; k++) {
        fast_iq_slew_tick(ride_control_final_iq_slew_mailbox(), &s->MS.i_q_setpoint);
        plant_inner_tick(&s->plant, s->MS.i_q_setpoint, s->tick);
    }
    if (s->plant.hall_edge_this_ctrl) {
        s->plant.hall_age_ticks = 0U;
        if (s->first_hall_tick == 0U) s->first_hall_tick = s->tick;
    } else if (s->plant.hall_age_ticks < 0xFFFFU) {
        s->plant.hall_age_ticks++;
    }
    if (s->MS.i_q_setpoint > 0 && s->first_iq_tick == 0U) s->first_iq_tick = s->tick;
    if (r.direction_inhibit_active) s->direction_inhibit_ticks++;

    if (s->tick > 2U * CTRL_HZ) {
        int32_t iq = s->MS.i_q_setpoint;
        if (iq < s->iq_min_run) s->iq_min_run = iq;
        if (iq > s->iq_max_run) s->iq_max_run = iq;
        s->iq_sum_run += iq;
        s->iq_sq_sum_run += (double)iq * iq;
        s->iq_samples_run++;
    }

    if (csv && (s->tick % 4U) == 0U) {
        const assist_mode_output_t *mo = assist_modes_get_last_output();
        fprintf(csv, "%u,%u,%u,%u,%u,%d,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%d\n",
            s->tick, ab, pas_direction_fwd_run(), s->MS.cadence,
            s->cadence_filtered_x8 >> 3, s->MS.i_q_setpoint,
            s->plant.iq_actual, s->plant.erps, s->plant.hall_age_ticks,
            ride_control_get_session_state(), ride_control_get_debug_flags(),
            ts->load_centikg, ts->assist_delta_filtered_native,
            ts->assist_delta_run_native, mo->iq_request);
    }
}

static void run_scenario(const char *name, double rpm, double mean_ckg, double ripple_ckg,
                         bool bounce, double breakaway_iq, double seconds)
{
    sim_t s;
    sim_init(&s, rpm, mean_ckg, ripple_ckg, bounce, breakaway_iq);
    char path[256];
    snprintf(path, sizeof(path), ".build/sil/%s.csv", name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "tick,ab,fwd_run,cadence_raw,cadence_filtered,iq_ref,iq_actual,erps,hall_age,session,debug,load_ckg,torque_fast,torque_run,iq_request\n");
    uint32_t n = (uint32_t)(seconds * CTRL_HZ);
    for (uint32_t i = 0; i < n; i++) sim_ctrl_tick(&s, f);
    fclose(f);

    double mean = s.iq_samples_run ? s.iq_sum_run / s.iq_samples_run : 0.0;
    double var = s.iq_samples_run ? s.iq_sq_sum_run / s.iq_samples_run - mean * mean : 0.0;
    if (var < 0.0) var = 0.0;
    double std = sqrt(var);
    double permission_ms = s.first_permission_tick ? 1000.0 * s.first_permission_tick / CTRL_HZ : -1.0;
    double iq_ms = s.first_iq_tick ? 1000.0 * s.first_iq_tick / CTRL_HZ : -1.0;
    double hall_ms = s.first_hall_tick ? 1000.0 * s.first_hall_tick / CTRL_HZ : -1.0;
    double perm_to_hall = (s.first_permission_tick && s.first_hall_tick) ?
        1000.0 * (s.first_hall_tick - s.first_permission_tick) / CTRL_HZ : -1.0;
    printf("SCENARIO %-18s permission=%7.2fms firstIq=%7.2fms firstHall=%7.2fms perm->Hall=%7.2fms falseR=%u inhibitTicks=%u steadyIqMean=%.1f std=%.1f pp=%d\n",
        name, permission_ms, iq_ms, hall_ms, perm_to_hall,
        s.false_reverse_events, s.direction_inhibit_ticks, mean, std,
        s.iq_samples_run ? (s.iq_max_run - s.iq_min_run) : 0);
}

int main(void)
{
    system("mkdir -p .build/sil");
    run_scenario("clean_start", 40.0, 1800.0, 0.0, false, 6.0, 4.0);
    run_scenario("loaded_start", 40.0, 1800.0, 0.0, false, 15.0, 4.0);
    run_scenario("pas_bounce", 60.0, 1800.0, 300.0, true, 6.0, 4.0);
    run_scenario("steady_ripple", 60.0, 1800.0, 500.0, false, 6.0, 6.0);
    return 0;
}
