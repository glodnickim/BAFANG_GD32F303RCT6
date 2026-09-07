#ifndef ROTOR_MOTION_H_
#define ROTOR_MOTION_H_
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t edge_erps;       /* never age-decayed; valid only while speed_fresh */
    uint16_t period_ticks;    /* ceil(real 500 kHz capture / 125), in 4 kHz ticks */
    bool anchored, valid;
} rotor_motion_t;

static inline bool rotor_motion_angle_stale(const volatile rotor_motion_t *m, uint16_t age)
{
    return !m->anchored || age > (m->period_ticks ? 4U * m->period_ticks + 1U : 2000U);
}
static inline bool rotor_motion_speed_fresh(const volatile rotor_motion_t *m, uint16_t age)
{
    return m->valid && age <= 2U * m->period_ticks + 1U;
}
static inline void rotor_motion_note_edge(volatile rotor_motion_t *m, uint16_t age, uint16_t capture)
{
    /* First edge after standstill is an anchor, not a measured sector interval.
     * A 16-bit 500 kHz timer wraps after 131.072 ms: refuse ambiguous captures. */
    bool interval_valid = m->anchored && !rotor_motion_angle_stale(m, age) && age < 524U && capture != 0U;
    m->anchored = true;
    m->valid = interval_valid;
    m->period_ticks = interval_valid ? (uint16_t)(((uint32_t)capture + 124U) / 125U) : 0U;
    m->edge_erps = interval_valid ? (uint16_t)(500000U / ((uint32_t)capture * 6U)) : 0U;
}

/* Called once per real 4 kHz TIMER1 update, never per foreground iteration.
 * TIMER1 and Hall TIMER2 have equal preemption priority: these updates serialize. */
static inline uint16_t rotor_motion_age_next(uint16_t age)
{
    return age < 64000U ? (uint16_t)(age + 1U) : 64000U;
}
static inline uint16_t rotor_motion_speed_ceiling(uint16_t speed, uint16_t age)
{
    if (speed != 0U && age > 4000U / ((uint32_t)speed * 6U) * 2U + 1U) {
        uint32_t ceiling = 4000U / ((uint32_t)age * 6U);
        if (ceiling < speed) return (uint16_t)ceiling;
    }
    return speed;
}
#endif
