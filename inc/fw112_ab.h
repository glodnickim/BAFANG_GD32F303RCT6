#ifndef FW112_AB_H_
#define FW112_AB_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * FW-112 A/B: a per-session REARM EPISODE LOGGER - the assist-level dependency of the restart
 * (a reverse, the permission REVOKED, and how long the chain takes to reach a positive setpoint
 * again) on the REAL bike. This is the second diagnostic half of the FW-112 card, alongside
 * fw112_diag.c. See src/fw112_ab.c for the implementation notes.
 *
 * WHY. The bench harness proved the level dependency: LINEAR assists from a lower load threshold
 * and reaches a positive setpoint faster at every higher level; eMTB sits behind a hard
 * power/current gate that zeroes demand until the rider pushes well above its level's threshold.
 * The harness also exposed a measurement trap - eMTB's hold/latch keeps the setpoint positive
 * across a short reverse, so a restart measured by "time to setpoint>0" is contaminated unless
 * the EPISODE opens at the REVOKED edge and every chain stage is captured. This module records
 * each restart episode on the bike so the same quantities can be read back from a real ride.
 *
 * EPISODE. One episode = one REARM SAGA, opened at the tick the session LEAVES ACTIVE (the
 * REVOKED edge - the reverse that took permission away) and closed at the first SETPOINT_POSITIVE
 * edge or at the FW112_AB_WINDOW_TICKS window. Opening at REVOKED is deliberate: it is the only
 * edge that is unambiguous on the bike, and it is what makes the D1..D5 deltas comparable across
 * levels. The episode carries:
 *   - a CONFIG record (record_type 0, one per episode, stamped at the REVOKED tick): the active
 *     assist level, mode and bank configuration (the level-dependency axis), controller
 *     temperature and battery voltage as context, the load threshold / required_steps / start_steps
 *     of the start gate as it stood, cadence at the arm, and the REVOKED tick itself (arm_tick);
 *   - a GRANTED milestone sample (record_type 1, milestone 2, offset 0) when the session re-enters
 *     ACTIVE - the schedule and all offsets are relative to THIS tick;
 *   - scheduled samples at exponentially spaced offsets since GRANTED (1,2,4,8,...,4096,8000 ticks,
 *     with GRANTED itself as sample_idx 0 at offset 0) while the episode runs, so a rearm that
 *     lingers in any stage is sampled at its own pace;
 *   - milestone samples captured at the FIRST tick each stage of the chain turns positive:
 *     TORQUE_POSITIVE  (torque_for_assist_mv > 0)            - the rider is pushing again,
 *     MODE_DEMAND      (assist_modes iq_request > 0)         - the mode calculation asks,
 *     IQ_REQUEST       (ride-latch iq_after_latch_floor > 0) - the latch/min-Iq floor releases,
 *     PRE_RAMP         (iq_pre_ramp > 0)                     - the pre-ramp target turned positive,
 *     SETPOINT         (MS.i_q_setpoint > 0)                 - something reaches the motor command.
 *   A milestone that fires while the session is still WAIT_GRANTED gets a NEGATIVE tick_offset;
 *   the reader computes D1..D5 (permission-to-recovery, recovery-to-latch, latch-to-preramp,
 *   preramp-to-setpoint, and permission-to-setpoint) as delta of the milestone offsets.
 *
 * REVOKED edges are edges, not levels: a session that is already SUSPENDED does not re-open the
 * episode every tick. A second REVOKED edge while an episode is still open (the rider reversed
 * again mid-rearm) closes the current episode and opens a new one - the new CONFIG marks the
 * boundary and episode_id makes episodes unambiguous to the reader.
 *
 * QUEUE. Ring of FW112_AB_RECORDS per session, keyed by session id like every other dump source
 * (queue_count_session/peek_session/release_session). When full, a new record is refused
 * (rejected_total++, surfaced as DIAG_ERR_CAPTURES_FULL + DIAG_TRAILER_F_FW112_AB_REJECTED by the
 * dump layer) rather than overwriting an older record. 144 x 32 B = 4608 B holds 6 complete
 * worst-case episodes (21 records each); the batch plan is to dump after 5-6 restarts so a
 * refused CONFIG means the batch plan was exceeded, not history overwritten.
 *
 * The module is driven entirely through fw112_ab_input_t - session/recovery/direction states as
 * plain bytes, the chain and configuration as plain scalars; nothing is linked from
 * ride_session.c, ride_control.c, assist_modes.c or main.c (same discipline as fw112_diag.c).
 */

/*
 * FW-112 TWO-MECHANISM DIAGNOSTIC: schema 2 adds direct numeric afilt/arun to the SAMPLE record,
 * in the two bytes schema 1 always left at 0 (fw112_ab_sample_record_t.reserved/.reserved2 -
 * see their comments below) - no new wire frame, no growth of the 32 B record, no RAM/budget
 * change. A schema 1 log's reserved/reserved2 bytes are genuinely 0 (never written), so a
 * decoder that only understands schema 1 is not misled by them; a schema-aware decoder reads
 * them as real afilt/arun starting at schema 2. This is diagnostic-only: nothing here changes
 * what any assist decision reads or does - see src/fw112_ab.c and inc/torque_input.h for the
 * production afilt (assist_delta_filtered_native) / arun (torque_run_filtered) this mirrors.
 */
#define FW112_AB_SCHEMA_VERSION 2U

/* Ring depth - see the RAM budget note in inc/diag_budget.h. 144 x 32 B = 4608 B: six complete
 * worst-case episodes (1 CONFIG + 20 SAMPLE = 21 records each) per batch, reject-on-full. */
#define FW112_AB_RECORDS 144U

/* Record wire frames (all free ids: 0x1022A..0x1022E are fw112_diag's, 0x1022F onwards free). */
#define FW112_AB_EFID_HEADER    0x0001022FU  /* one per record: schema/session/episode/type/milestone */
#define FW112_AB_EFID_SNAP_BASE 0x00010230U  /* +0..+3: 4 x 8 B record fragments */

typedef enum {
	FW112_AB_REC_CONFIG = 0,   /* the episode's CONFIG record (the REVOKED marker) */
	FW112_AB_REC_SAMPLE = 1    /* GRANTED / scheduled / milestone snapshot samples */
} fw112_ab_record_type_t;

/* Milestone ids carried in the SAMPLE record's ms_and_idx byte (bits 0-2). */
typedef enum {
	FW112_AB_MS_SCHEDULED = 0,            /* one of the exponentially spaced schedule points */
	FW112_AB_MS_REVOKED = 1,              /* the episode OPEN - the CONFIG record IS this marker */
	FW112_AB_MS_GRANTED = 2,              /* session re-entered ACTIVE (sample_idx 0, offset 0) */
	FW112_AB_MS_TORQUE_POSITIVE = 3,      /* torque_for_assist_mv first positive since REVOKED */
	FW112_AB_MS_MODE_DEMAND_POSITIVE = 4, /* assist_modes iq_request first positive */
	FW112_AB_MS_IQ_REQUEST_POSITIVE = 5,  /* ride-latch iq_after_latch_floor first positive */
	FW112_AB_MS_PRE_RAMP_POSITIVE = 6,    /* iq_pre_ramp first positive */
	FW112_AB_MS_SETPOINT_POSITIVE = 7     /* MS.i_q_setpoint first positive - closes the episode */
} fw112_ab_milestone_t;

/* CONFIG record flags. */
#define FW112_AB_CFG_BASED_ON_POWER       0x01U  /* mode runs on pedalled power, not torque */
#define FW112_AB_CFG_CADENCE_COMP         0x02U  /* cadence compensation enabled (FW-057) */
#define FW112_AB_CFG_ASSIST_WITHOUT_ROT   0x04U  /* assist-without-rotation allowed */
#define FW112_AB_CFG_LEVEL_ZERO           0x08U  /* assist level 0 - the chain was forced off */

/* SAMPLE record flags. */
#define FW112_AB_FLAG_LATCHED       0x01U  /* session latched this tick */
#define FW112_AB_FLAG_PWM_ON        0x02U  /* PWM bridge driving this tick */
#define FW112_AB_FLAG_SENSOR_VALID  0x04U  /* torque sensor reported valid this tick */
#define FW112_AB_FLAG_WHEEL_VALID   0x08U  /* wheel rolling this tick */
#define FW112_AB_FLAG_ROLLING_COAST 0x10U  /* PAS stopped AND wheel rolling (coast suspension) */
/* FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2): recovery_state != IDLE, restated as a flag bit so
 * a decoder can classify REARM PATH (this bit set - afilt/arun are the SAME live-substituted
 * value, see torque_input_recovery_run_native()) vs ORDINARY RUN PATH (this bit clear - arun is
 * the independent 48-step window average) without a recovery_state lookup table. Purely
 * derived - recovery_state (already in the record) remains the authoritative source; this bit
 * only restates it for convenience and can never disagree with it. */
#define FW112_AB_FLAG_RECOVERY_ACTIVE 0x20U

/* SAMPLE ms_and_idx: milestone_id in bits 0-2, sample_idx in bits 3-7 (0=GRANTED, 1..14=schedule). */
#define FW112_AB_MS_AND_IDX(ms, idx) (uint8_t)(((idx) << 3) | (ms))
#define FW112_AB_MS_OF(b)   ((uint8_t)((b) & 0x07U))
#define FW112_AB_IDX_OF(b)  ((uint8_t)((b) >> 3))

/* The episode window and schedule (ticks since GRANTED). GRANTED itself is sample_idx 0; the
 * remaining points are exponentially spaced so a lingering rearm is sampled at its own pace. */
#define FW112_AB_WINDOW_TICKS 8000U

/* How many milestones can fire before permission is granted (TORQUE/MODE_DEMAND/IQ_REQUEST/
 * PRE_RAMP - SETPOINT cannot, the setpoint stays zero while suspended). Their queued samples are
 * recorded with an open-relative offset and re-stamped grant-relative at the GRANTED edge. */
#define FW112_AB_MAX_PRE_GRANT 4U

/* One queued CONFIG record - exactly 32 B, no padding (asserted in fw112_ab.c). The first four
 * bytes mirror the SAMPLE record's header (episode_id/record_type/session_id plus one spare byte
 * at offset 4) so the queue can read session_id, record_type and episode_id through the union at
 * the same offsets for both record types. Field order is chosen so every member is naturally
 * aligned (u16 at even offsets, the u32 arm_tick at 28) and the total is 32 B. */
typedef struct {
	uint16_t episode_id;               /* monotonic episode id - a key, never a count */
	uint8_t  record_type;              /* FW112_AB_REC_CONFIG */
	uint8_t  session_id;
	uint8_t  reserved;                 /* offset 4: mirrors the SAMPLE record's ms_and_idx byte */
	uint8_t  assist_level;             /* 0..5 (level_to_array_element[MS.assist_level]) */
	uint8_t  bank_index;
	uint8_t  mode_type;                /* assist_mode_type_t as a byte */
	uint8_t  emtb_parameter;           /* eMTB response parameter as configured */
	uint8_t  max_iq_pct;
	uint8_t  flags;                    /* FW112_AB_CFG_* */
	uint8_t  reserved2;
	uint16_t support_ratio_pct;
	uint16_t emtb_reference_voltage_mv;
	uint16_t max_motor_power_w;
	int16_t  controller_temperature_c;
	uint16_t battery_voltage_mv_div10;
	uint16_t load_threshold_centikg;   /* the start gate's engage threshold at the REVOKED tick */
	uint8_t  required_steps;           /* the start gate's required_steps at the REVOKED tick */
	uint8_t  start_steps;              /* tuning_config_start_steps() */
	uint8_t  cadence_rpm_at_arm;
	uint8_t  reserved3;
	uint32_t arm_tick;                 /* the REVOKED tick that opened the episode */
} fw112_ab_config_record_t;

/* One queued SAMPLE record - exactly 32 B, no padding (asserted in fw112_ab.c). The first four
 * bytes (episode_id/record_type/session_id/ms_and_idx) are the shared header layout. */
typedef struct {
	uint16_t episode_id;
	uint8_t  record_type;              /* FW112_AB_REC_SAMPLE */
	uint8_t  session_id;
	uint8_t  ms_and_idx;               /* FW112_AB_MS_* (bits 0-2) | sample_idx (bits 3-7) */
	uint8_t  assist_level;             /* echoed so a mid-episode level change is visible */
	int16_t  tick_offset;              /* ticks since GRANTED (negative = before permission).
	                                     * In an episode that never granted (no GRANTED record)
	                                     * every offset stays relative to the episode OPEN, so it
	                                     * is always positive - the reader picks the reference by
	                                     * the presence of the episode's GRANTED record. */
	uint16_t torque_for_assist_mv;
	uint16_t load_centikg;             /* raw pedal load, same scale as the start gate */
	uint8_t  cadence_rpm;
	uint8_t  session_state;
	uint8_t  recovery_state;
	uint8_t  dir_state;
	uint8_t  flags;                    /* FW112_AB_FLAG_* */
	uint8_t  assist_hold_ticks;        /* the ride-latch hold grace counter (low 8 bits) */
	int16_t  iq_request;               /* assist_modes asked for, before limits */
	int16_t  iq_after_latch_floor;     /* after the ride-latch block and the min-Iq floor */
	int16_t  iq_pre_ramp;              /* the final pre-ramp target */
	int16_t  iq_setpoint;              /* what reached the motor command */
	int8_t   controller_temperature_c;
	/* FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2, was "reserved", always 0 in schema 1): arun
	 * (torque_run_filtered / snapshot.assist_delta_run_native), CLAMPED to native units 0..255
	 * (saturating, never wraps). 255 native ~= 13.4 kg on the default span - the deadband this
	 * card cares about (TORQUE_ASSIST_DEADBAND_NATIVE = 10 native ~= 0.53 kg) sits well inside
	 * the unclamped range, so nothing in the range this diagnostic is FOR loses precision; only
	 * a genuinely hard push saturates, and a saturated reading still unambiguously means "arun
	 * is not the lagging signal here", which is all a saturated sample needs to prove. Chosen
	 * over growing the record so schema 2 costs zero additional wire bytes and zero RAM (see
	 * inc/fw112_ab.h's schema comment). */
	uint8_t  arun_native_clamped;
	uint16_t battery_voltage_mv_div10;
	/* FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2, was "reserved2", always 0 in schema 1): afilt
	 * (assist_delta_filtered_native) at FULL native-unit precision - the exact value
	 * WAIT_FRESH_LOAD -> TRACK_FAST tests against TORQUE_ASSIST_DEADBAND_NATIVE
	 * (src/torque_input.c's torque_input_update()). This is the direct signal §11's
	 * root-cause decision needs; tq_mv (torque_for_assist_mv) is a boosted, live-substituted
	 * PROXY for it and must not be used for the decision once this field is present. */
	uint16_t afilt_native;
} fw112_ab_sample_record_t;

/* The queued record as a union - exactly 32 B total. */
typedef union {
	fw112_ab_config_record_t cfg;
	fw112_ab_sample_record_t smp;
} fw112_ab_record_t;

typedef struct {
	uint8_t  session_state;
	uint8_t  recovery_state;
	uint8_t  dir_state;
	uint8_t  assist_level;             /* 0..5 */
	uint8_t  bank_index;
	uint8_t  mode_type;
	uint8_t  emtb_parameter;
	uint16_t support_ratio_pct;
	uint8_t  max_iq_pct;
	bool     emtb_based_on_power;
	bool     cadence_comp_enabled;
	bool     assist_without_rotation;
	bool     latched;
	bool     pwm_on;
	bool     torque_sensor_valid;
	bool     wheel_valid;
	bool     rolling_coast;
	uint16_t emtb_reference_voltage_mv;
	uint16_t max_motor_power_w;
	uint16_t battery_voltage_mv;
	int16_t  controller_temperature_c;
	uint16_t load_threshold_centikg;
	uint8_t  required_steps;
	uint8_t  start_steps;
	uint8_t  cadence_rpm;
	uint16_t torque_for_assist_mv;
	uint16_t load_centikg;
	int16_t  iq_request;
	int16_t  iq_after_latch_floor;
	int16_t  iq_pre_ramp;
	int16_t  iq_setpoint;
	uint16_t assist_hold_ticks;
	/* FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2): the direct torque_input.c signals, passed in
	 * at FULL native-unit precision - src/fw112_ab.c clamps arun_native to the record's 8-bit
	 * wire field (see fw112_ab_sample_record_t.arun_native_clamped); afilt_native is stored at
	 * full 16-bit precision unchanged. */
	uint16_t afilt_native;   /* assist_delta_filtered_native (torque_input_get_snapshot()) */
	/* NOT the raw snapshot.assist_delta_run_native - the caller must pass what
	 * assist_modes_calculate() actually receives: torque_input_recovery_run_native() while a
	 * recovery is active (live afilt, every tick), the raw snapshot field otherwise. The raw
	 * field alone is only reseeded on forward steps during WAIT_FRESH_LOAD and can be stale by
	 * up to one step interval between them - see src/main.c's ab_in construction. */
	uint16_t arun_native;
} fw112_ab_input_t;

void fw112_ab_init(void);
/* The session id stamped into every record opened from now on - call with diag_session_current_id(). */
void fw112_ab_set_session_id(uint8_t session_id);
/* One observation per control tick; every edge is detected internally. */
void fw112_ab_tick(const fw112_ab_input_t *in, uint32_t now_tick);

/* diag_record_source bridge - same shape as fw112_diag's. */
uint16_t fw112_ab_queue_count_session(uint8_t session_id);
bool fw112_ab_queue_peek_session(uint8_t session_id, fw112_ab_record_t *out);
void fw112_ab_queue_release_session(uint8_t session_id);
uint32_t fw112_ab_queue_enqueued(void);
uint32_t fw112_ab_queue_rejected(void);

#endif /* FW112_AB_H_ */
