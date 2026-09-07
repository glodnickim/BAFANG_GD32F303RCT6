#ifndef RIDE_TELEMETRY_H_
#define RIDE_TELEMETRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "diag_session.h"

/*
 * FW-145: continuous, best-effort live-ride telemetry for CANable / Level-4 replay.
 *
 * This module is OBSERVATION ONLY. It never owns a control value and it never runs from the
 * 16 kHz FOC ISR. main.c copies already-produced state into ride_telemetry_snapshot_t from the
 * ordinary 4 kHz foreground domain; ride_telemetry_step() serializes that snapshot later, only
 * when critical HMI/multiframe traffic has no pending claim on a CAN mailbox.
 *
 * Wire block. 0x10300..0x10307 is STOP_TRACE, so live telemetry deliberately lives in a new
 * range. diag_efid_map.h asserts this block stays disjoint from every existing diagnostic owner.
 */
#define RIDE_TELEMETRY_EFID_BASE   0x00010400U
#define RIDE_TELEMETRY_EFID_CORE   (RIDE_TELEMETRY_EFID_BASE + 0U)
#define RIDE_TELEMETRY_EFID_DEMAND (RIDE_TELEMETRY_EFID_BASE + 1U)
#define RIDE_TELEMETRY_EFID_MOTOR  (RIDE_TELEMETRY_EFID_BASE + 2U)
#define RIDE_TELEMETRY_EFID_BATT   (RIDE_TELEMETRY_EFID_BASE + 3U)
#define RIDE_TELEMETRY_EFID_LIMITS (RIDE_TELEMETRY_EFID_BASE + 4U)
#define RIDE_TELEMETRY_EFID_STATE  (RIDE_TELEMETRY_EFID_BASE + 5U)
#define RIDE_TELEMETRY_EFID_ROTOR  (RIDE_TELEMETRY_EFID_BASE + 6U)
#define RIDE_TELEMETRY_EFID_META   (RIDE_TELEMETRY_EFID_BASE + 7U)
#define RIDE_TELEMETRY_EFID_LAST   RIDE_TELEMETRY_EFID_META

#define RIDE_TELEMETRY_SCHEMA_VERSION 1U
#define RIDE_TELEMETRY_DATA_FRAMES   7U

/* One CAN frame every 3 ms. Seven data frames therefore describe one coherent snapshot every
 * ~21 ms (~47.6 Hz at 4 kHz). META is inserted at most once per second between snapshots.
 * A 3 ms start interval is intentionally modest: this is a developer-only stream and remains
 * lower priority than the critical CAN queue/multiframe producer. */
#define RIDE_TELEMETRY_FRAME_INTERVAL_TICKS ((CONTROL_TIMEBASE_HZ * 3U) / 1000U)
#define RIDE_TELEMETRY_META_INTERVAL_TICKS  (CONTROL_TIMEBASE_HZ)
#define RIDE_TELEMETRY_CAPTURE_INTERVAL_TICKS \
    (RIDE_TELEMETRY_FRAME_INTERVAL_TICKS * RIDE_TELEMETRY_DATA_FRAMES)
_Static_assert(RIDE_TELEMETRY_FRAME_INTERVAL_TICKS > 0U,
               "ride telemetry frame interval must be at least one control tick");

/* flags16 in 0x10404. Bits 13..15 carry bridge_lifecycle (0..7). */
#define RIDE_TELEM_F_BATTERY_LIMIT   (1U << 0)
#define RIDE_TELEM_F_BRAKE           (1U << 1)
#define RIDE_TELEM_F_WALK            (1U << 2)
#define RIDE_TELEM_F_TORQUE_FAULT    (1U << 3)
#define RIDE_TELEM_F_OVERTEMP_CUT    (1U << 4)
#define RIDE_TELEM_F_FOC_SATURATED   (1U << 5)
#define RIDE_TELEM_F_TORQUE_CAL      (1U << 6)
#define RIDE_TELEM_F_OFFROAD          (1U << 7)
#define RIDE_TELEM_F_PWM_ON           (1U << 8)
#define RIDE_TELEM_F_START_PHASE      (1U << 9)
#define RIDE_TELEM_F_WALK_CAN_REQ     (1U << 10)
#define RIDE_TELEM_F_DIRECTION_INHIBIT (1U << 11)
#define RIDE_TELEM_F_BACKPEDAL        (1U << 12)
#define RIDE_TELEM_F_BRIDGE_SHIFT     13U
#define RIDE_TELEM_F_BRIDGE_MASK      (7U << RIDE_TELEM_F_BRIDGE_SHIFT)

typedef struct {
    uint32_t control_tick;

    uint16_t load_centikg;
    uint16_t torque_fast_native;
    uint16_t torque_run_native;
    uint8_t cadence_raw_rpm;
    uint8_t cadence_control_rpm;

    int16_t iq_requested;
    int16_t iq_allowed;
    int16_t iq_ref;
    int16_t iq_actual;
    int16_t id_actual;
    uint16_t motor_erps;

    uint16_t battery_voltage_10mv;
    int16_t battery_current_10ma;
    uint16_t soc_display_x10;

    uint16_t wheel_speed_x100;
    uint16_t u_abs;
    uint16_t flags;

    uint8_t permission_bits;
    uint8_t debug_flags;
    uint8_t session_state;
    uint8_t qzero_state;
    uint8_t assist_level;

    int16_t theta_q15;
    uint16_t hall_age_ticks;
    uint8_t hall_state;
    uint8_t rotor_trusted;
    uint8_t bridge_lifecycle;
    uint8_t pwm_on;
    uint8_t pas_ab;
    uint8_t pas_direction_state;
    uint8_t pas_backpedal;
    uint8_t direction_inhibit;
    uint8_t start_phase;

    uint8_t active_profile_bank;
} ride_telemetry_snapshot_t;

void ride_telemetry_init(const diag_can_ops_t *can_ops);
bool ride_telemetry_capture_due(uint32_t now_tick);
void ride_telemetry_capture(const ride_telemetry_snapshot_t *snapshot);

/* Non-blocking. `allow_new_tx` is main.c's proof that the critical queue and multiframe producer
 * are idle. `ride_active` keeps this continuous stream on the ride itself and out of the way of
 * the post-ride diagnostic dump. Polling a mailbox already owned by this module always remains
 * legal even when either argument turns false. */
void ride_telemetry_step(uint32_t now_tick, bool allow_new_tx, bool ride_active);

uint32_t ride_telemetry_sent_frames(void);
uint32_t ride_telemetry_failed_frames(void);
uint32_t ride_telemetry_completed_snapshots(void);

#endif /* RIDE_TELEMETRY_H_ */
