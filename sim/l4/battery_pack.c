#include "battery_pack.h"

#include <stddef.h>

static float clampf_local(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float interp_soc_curve(const float *soc, const float *v, size_t n, float q)
{
    q = clampf_local(q, 0.0f, 100.0f);
    if (q <= soc[0]) return v[0];
    if (q >= soc[n - 1U]) return v[n - 1U];
    for (size_t i = 0; i + 1U < n; i++) {
        if (q <= soc[i + 1U]) {
            float f = (q - soc[i]) / (soc[i + 1U] - soc[i]);
            return v[i] + f * (v[i + 1U] - v[i]);
        }
    }
    return v[n - 1U];
}

float evd_battery_cell_ocv_v(evd_battery_profile_t profile, float soc_pct)
{
    if (profile == EVD_BATT_PROFILE_FEB21700G) {
        /* EVistDrive reference FEB21700G discharge profile used by the project. It is a
         * PHYSICAL test-pack curve, not the firmware OCV estimator table. That difference is
         * intentional: Level 4 must expose estimator bias when chemistry/calibration differ. */
        static const float soc[] = {0,5,10,20,30,40,50,60,70,80,90,100};
        static const float v[] = {
            2.9000f,2.9782f,3.0618f,3.1936f,3.3200f,3.4482f,
            3.5600f,3.6591f,3.7709f,3.8636f,3.9155f,4.1800f
        };
        return interp_soc_curve(soc, v, sizeof(soc)/sizeof(soc[0]), soc_pct);
    }
    /* Same LG M58T points used by production calculate_SOC(), inverted here as the physical
     * pack law for the matched-chemistry scenario. */
    static const float soc[] = {0,5,10,20,30,40,50,60,70,80,90,100};
    static const float v[] = {
        2.799f,2.968f,3.086f,3.247f,3.450f,3.569f,
        3.681f,3.774f,3.853f,3.946f,3.989f,4.070f
    };
    return interp_soc_curve(soc, v, sizeof(soc)/sizeof(soc[0]), soc_pct);
}

float evd_battery_pack_ocv_v(const evd_battery_pack_t *b)
{
    if (!b || b->series_cells == 0U) return 0.0f;
    return evd_battery_cell_ocv_v(b->profile, b->true_soc_pct) * (float)b->series_cells;
}

void evd_battery_init(evd_battery_pack_t *b, evd_battery_profile_t profile,
                      uint8_t series_cells, float capacity_ah, float initial_soc_pct,
                      float r0_mohm, float r_dyn_mohm, float tau_s)
{
    if (!b) return;
    b->profile = profile;
    b->series_cells = series_cells;
    b->capacity_ah = capacity_ah > 0.01f ? capacity_ah : 0.01f;
    b->true_soc_pct = clampf_local(initial_soc_pct, 0.0f, 100.0f);
    b->r0_ohm = r0_mohm * 0.001f;
    b->r_dyn_ohm = r_dyn_mohm * 0.001f;
    b->tau_s = tau_s > 0.01f ? tau_s : 0.01f;
    b->dyn_sag_v = 0.0f;
    b->current_a = 0.0f;
    b->discharged_ah = 0.0f;
    b->discharged_wh = 0.0f;
    b->ocv_v = evd_battery_pack_ocv_v(b);
    b->terminal_v = b->ocv_v;
}

void evd_battery_step(evd_battery_pack_t *b, float current_a, float dt_s)
{
    if (!b || dt_s <= 0.0f) return;
    b->current_a = current_a;
    float target_dyn = current_a * b->r_dyn_ohm;
    float alpha = dt_s / (b->tau_s + dt_s);
    b->dyn_sag_v += alpha * (target_dyn - b->dyn_sag_v);

    float d_ah = current_a * dt_s / 3600.0f;
    b->discharged_ah += d_ah;
    if (b->discharged_ah < 0.0f) b->discharged_ah = 0.0f;
    b->true_soc_pct -= d_ah / b->capacity_ah * 100.0f;
    b->true_soc_pct = clampf_local(b->true_soc_pct, 0.0f, 100.0f);
    b->ocv_v = evd_battery_pack_ocv_v(b);
    b->terminal_v = b->ocv_v - current_a * b->r0_ohm - b->dyn_sag_v;
    if (b->terminal_v < 0.0f) b->terminal_v = 0.0f;
    b->discharged_wh += current_a * b->terminal_v * dt_s / 3600.0f;
}
