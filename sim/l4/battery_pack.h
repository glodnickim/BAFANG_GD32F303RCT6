#ifndef EVD_L4_BATTERY_PACK_H_
#define EVD_L4_BATTERY_PACK_H_

#include <stdint.h>

typedef enum {
    EVD_BATT_PROFILE_LG_M58T = 0,
    EVD_BATT_PROFILE_FEB21700G = 1
} evd_battery_profile_t;

typedef struct {
    evd_battery_profile_t profile;
    uint8_t series_cells;
    float capacity_ah;
    float true_soc_pct;
    float r0_ohm;
    float r_dyn_ohm;
    float tau_s;
    float dyn_sag_v;
    float ocv_v;
    float terminal_v;
    float current_a;
    float discharged_ah;
    float discharged_wh;
} evd_battery_pack_t;

void evd_battery_init(evd_battery_pack_t *b, evd_battery_profile_t profile,
                      uint8_t series_cells, float capacity_ah, float initial_soc_pct,
                      float r0_mohm, float r_dyn_mohm, float tau_s);
float evd_battery_cell_ocv_v(evd_battery_profile_t profile, float soc_pct);
float evd_battery_pack_ocv_v(const evd_battery_pack_t *b);
void evd_battery_step(evd_battery_pack_t *b, float current_a, float dt_s);

#endif
