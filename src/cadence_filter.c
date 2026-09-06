#include "cadence_filter.h"

#include <stdbool.h>

static uint16_t cadence_x8;
static bool seeded;

void cadence_filter_reset(void)
{
    cadence_x8 = 0U;
    seeded = false;
}

uint8_t cadence_filter_update(uint8_t raw_rpm)
{
    if (!seeded) {
        cadence_x8 = (uint16_t)raw_rpm << 3;
        seeded = true;
    } else {
        cadence_x8 -= cadence_x8 >> 3;
        cadence_x8 += raw_rpm;
    }
    return cadence_filter_get();
}

uint8_t cadence_filter_get(void)
{
    uint16_t rpm = cadence_x8 >> 3;
    return (uint8_t)((rpm > 255U) ? 255U : rpm);
}

uint16_t cadence_filter_get_x8(void)
{
    return cadence_x8;
}
