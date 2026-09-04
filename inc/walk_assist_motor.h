#ifndef WALK_ASSIST_MOTOR_H_
#define WALK_ASSIST_MOTOR_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Walk Assist uses motor Hall ERPS to regulate the configured chainring-speed target. Wheel speed
 * is a safety fuse and never the controlled value; changing gear intentionally changes walking
 * speed. A Hall timeout uses a bounded recovery up to 24 Iq which never re-arms the start, and
 * still times out to STALL.
 *
 * This facade owns Hall estimation, the wheel-speed fuse and the safety states. The current
 * trajectory itself lives in walk_speed_controller.c, in one of two laws (WALK_GOVERNOR_ENABLE):
 *
 *   A - FW-060/079/080/081/082: one-shot 40 Iq START, then a bounded speed PI and output slew.
 *   B - FW-130: no integrator. A demand ramped at a constant rate in the G532 character (550 ms
 *       rise / 110 ms fall of full scale) whose ceiling is lowered continuously by the gear-RPM
 *       band and by the wheel-speed fuse. The configured "Walk current" percentage, resolved by
 *       the caller into walk_iq_max, is the one number that says how hard WA may push.
 *
 * Neither law keeps a positive RUN floor: FW-079 removed a 5 Iq floor because it made different
 * targets converge, and FW-113.1 took the remaining 2 Iq to 0. Both decisions stand.
 */
typedef enum {
	WA_STATE_OFF = 0,
	WA_STATE_REGULATE,
	WA_STATE_LIMIT,
	WA_STATE_STALL
} walk_motor_state_t;

typedef struct {
	bool active;                    /* MS.pushassist_flag */
	bool brake;
	bool fault;
	uint16_t wheel_speed_x100;      /* safety gate only, never the controlled value */
	uint16_t max_wheel_speed_x100;  /* per-bank cut-off; 0 = built-in fallback */
	uint16_t motor_hall_ticks;      /* 500 kHz ticks between Hall transitions */
	uint16_t motor_erps_age_ticks;  /* 4 kHz ticks since the last Hall event */
	int32_t motor_iq_actual;        /* MS.i_q, used by the jam watchdog */
	int32_t motor_iq_reference;     /* final Iq from the previous control tick */
	uint16_t target_chainring_rpm;  /* per-bank target configured in Canable */
	/*
	 * FW-130: the per-bank "Walk current" percentage already resolved into Iq by the
	 * caller (it owns PH_CURRENT_MAX). Clamped here to WA_MOTOR_IQ_ABS_MAX. 0 means
	 * "not configured" and keeps the pre-FW-130 fixed WA_MOTOR_START_MAX_IQ ceiling.
	 */
	int32_t walk_iq_max;
} walk_motor_input_t;

typedef struct {
	int32_t iq_target;
	uint8_t state;
	uint8_t flags;
	uint16_t target_erps;
	uint16_t measured_erps;
	int16_t error_erps;
	int32_t iq_cap;
	int16_t integral_iq;
	int16_t startup_iq;
	int16_t gear_factor_q8;         /* FW-130: gear-RPM governor, 0..256 (256 = no reduction) */
	int16_t wheel_factor_q8;        /* FW-130: wheel-speed fuse, 0..256 */
	uint16_t reason;
} walk_motor_output_t;

/* Diagnostic flag bits in CAN frame 0x00010205. */
#define WA_FLAG_HALL_VALID   0x01
#define WA_FLAG_JAM          0x02
#define WA_FLAG_BLOCKED      0x04
#define WA_FLAG_SATURATED    0x08
#define WA_FLAG_START_ACTIVE 0x10
#define WA_FLAG_ABOVE_TARGET 0x20
#define WA_FLAG_LIMIT        0x40
#define WA_FLAG_REACQUIRE    0x80

/*
 * FW-113.2: one unambiguous reason for WA being inactive or blocked, serialized in
 * CAN frame 0x00010228 (Data1 = activation reason, Data2 = module reason). There is
 * NO wall-clock hold-timeout bit by design: hold time alone can never disable WA, only
 * a real safety gate can. The module sets the module-level bits (BRAKE, ERROR,
 * SPEED_GATE from its inputs; HALL, JAM, STALL, LIMIT from its state machine); the
 * activation-level bits NO_CAN_REQUEST and BUTTON_RELEASED are set by main.c, which
 * alone knows the request origin.
 */
#define WA_REASON_NO_CAN_REQUEST  0x0001U
#define WA_REASON_BUTTON_RELEASED 0x0002U
#define WA_REASON_SPEED_GATE      0x0004U
#define WA_REASON_BRAKE           0x0008U
#define WA_REASON_ERROR           0x0010U
#define WA_REASON_HALL            0x0020U
#define WA_REASON_JAM             0x0040U
#define WA_REASON_STALL           0x0080U
#define WA_REASON_LIMIT           0x0100U

/* Clears the active run state. The stall latch intentionally survives. */
void walk_motor_reset(void);

/* Clears the run state and stall latch after the rider releases WA. */
void walk_motor_release(void);

/* Runs at 4 kHz and returns the complete WA Iq trajectory. */
int32_t walk_motor_update(const walk_motor_input_t *input, walk_motor_output_t *output);

#endif /* WALK_ASSIST_MOTOR_H_ */
