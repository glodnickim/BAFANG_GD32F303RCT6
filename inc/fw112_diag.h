#ifndef FW112_DIAG_H_
#define FW112_DIAG_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * FW-112-DIAG: a per-session EVENT RECORDER for the whole pedal -> Iq chain, the diagnostic
 * half of the FW-112 card. See src/fw112_diag.c for the implementation notes.
 *
 * WHY ANOTHER RECORDER. The 0x102xx aggregate frames are no longer sent live during the ride
 * (FW-106 removed print_debug_on_CAN()); they are frozen at the quiet-candidate moment and
 * dumped only while the bike stands still. A rearm, a reverse, a recovery collapse or a
 * hold expiry lasts a few control ticks - far less than one frame period, and far inside a
 * single session. The only thing that survives from the middle of the ride is what a recorder
 * captured AT THE TRANSITION, into a per-session queue. FW-111's rearm_delay_diag records the
 * rearm saga specifically; this module records the wider question the FW-112 card asked: at
 * every moment the rider is pedalling forward, WHICH stage of the chain is holding current at 0.
 *
 * RECORD. One record = one EVENT, captured at the tick it first fires (edge, never a per-tick
 * spam), carrying a full snapshot of the chain state so a single log answers "what was true at
 * the transition": session_state, dir_state, recovery_state, fwd_run, crank_forward_steps,
 * required/start steps, hold grace, load vs threshold, iq_request / iq_pre_ramp / iq_setpoint /
 * iq_actual, the permission-reason bitfield (computed by ride_control, the layer that decides
 * permission - see ride_control_get_diag_reason()), and the tick gap since the previous event.
 *
 * EVENTS (fw112_diag_event_t), each edge-detected inside this module from the input:
 *   BLOCKED            entered a contiguous stretch of "pedalling forward but not latched"
 *                      (fwd_run > 0 && !latched) - the rider is turning the cranks and gets no
 *                      permission. reason_bits says which stage of the cold gate / direction
 *                      safety / rearm window is holding it.
 *   PERMISSION_GRANTED session re-entered ACTIVE (cold start or fast rearm, flag tells which).
 *   PERMISSION_REVOKED session left ACTIVE (direction inhibit or a terminal event).
 *   RECOVERY_ENTER     the rolling-rearm recovery automaton left IDLE (begin_rolling_rearm).
 *   RECOVERY_EXIT      the recovery automaton returned to IDLE (completed, or cancelled).
 *   RECOVERY_COLLAPSE  TRACK_FAST -> WAIT_FRESH_LOAD (pressure lost mid-recovery).
 *   ZEROED             entered a contiguous stretch of "permission present but iq_setpoint 0"
 *                      (fwd_run > 0 && latched && iq_setpoint == 0) - the WHO-ZEROED question,
 *                      with the reason from the deciding layer.
 *   HOLD_ARMED / HOLD_EXPIRED  the ride-latch hold grace was armed by a positive mode demand,
 *                      or counted down to 0 while still latched.
 *
 * The FW-112 card's "no TARGET_RECOVERED as proof" point is respected structurally: nothing in
 * this module infers success from any derived "recovered" marker - the log only ever records
 * what was true at a transition, and the same tick's iq_setpoint/iq_request speak for
 * themselves.
 *
 * QUEUE. Ring of FW112_DIAG_RECORDS per session, keyed by session id like every other dump
 * source (queue_count_session/peek_session/release_session). event_id is a monotonic per-record
 * id (wraps at 65536; a key, never a count), so a reader detects each event and can join it to
 * a FW-111 rearm record via the (session_id, event_id) pair.
 *
 * RETENTION (FW-112-STABILITY). A recovery saga runs from a PERMISSION_REVOKED (its start) to
 * RECOVERY_EXIT (its terminal), or is closed by the next PERMISSION_REVOKED arriving before it
 * terminated. When the ring is full, the OLDEST COMPLETED saga is evicted and the new event is
 * appended - the in-flight saga is never evicted - so late-ride recovery evidence survives the
 * 24-record ceiling. An event is refused (rejected_total++, surfaced as DIAG_ERR_CAPTURES_FULL
 * + DIAG_TRAILER_F_FW112_REJECTED by the dump layer) only when no completed saga exists to
 * evict (a single open saga fills the whole ring and the incoming event is not the REVOKED that
 * closes it) - a refusal is honest saturation, never silent loss. The evictions are counted by
 * fw112_diag_queue_evicted_sagas()/evicted_records() (internal only, never on the wire).
 *
 * The module is driven entirely through fw112_diag_input_t - the session/recovery/direction
 * states arrive as plain bytes, the torque/Iq chain as plain scalars, and nothing is linked
 * from ride_session.c, ride_control.c or main.c (same discipline as rearm_delay_diag.c).
 */

/* One byte on the wire of every record header frame.
 * FW-112.2: 1 -> 2 - the flags byte gained FW112_FLAG_WHEEL_VALID / FW112_FLAG_ROLLING_COAST
 * (the reason a PERMISSION_REVOKED record suspended the session: a direction hold vs a
 * rolling coast), and the record already distinguishes FAST_REARM after either.
 * FW-112-STABILITY: 2 -> 3 - the two spare header bytes (data[6..7]) and the two spare bytes at
 * frag4 data[4..5] (both always zero before) now carry the recovery-stability evidence:
 *   RECOVERY_COLLAPSE  header[6..7] = the stability streak immediately before the collapse
 *                       (recovery_stable_ticks_at_edge); frag4[4..5] = assist_delta_filtered_native
 *                       on the collapse tick (the filtered assist signal that drives the streak).
 *   RECOVERY_EXIT      header[6..7] = the streak as it stood at close (== the required dwell of
 *                       TORQUE_ROLLING_REARM_STABLE_TICKS for a COMPLETED recovery, less for a
 *                       cancel/reset); frag4[4..5] = the episode's TRACK_FAST->WAIT_FRESH_LOAD
 *                       collapse count.
 *   every other event  both fields stay zero.
 * Schema 1-2 logs leave both fields zero and decode exactly as before.
 * C0-PROOF (schema 4): crank_forward_steps/required_steps/start_steps dropped (available from
 * fw112_ab per-episode records). New fields: packed_state, permission_bits, flags2,
 * hold_ticks_sat, motor_voltage_utilization, iq_before_pu. elapsed_ticks u32 -> u16 saturated.
 * Recovery evidence stays in header spare bytes (schema 3 wire semantics unchanged). */
#define FW112_DIAG_SCHEMA_VERSION 4U

/* Ring depth - see the RAM budget note in inc/diag_budget.h.
 * FW-112-DIAG.1: 8 -> 24. The first hardware capture filled all 8 slots exactly at
 * RECOVERY_ENTER, hiding the recovery tail (WAIT/TRACK/COLLAPSE, demand, final setpoint). 24
 * x 32 B = 768 B must be enough for a full short episode. reject-on-full is kept. */
#define FW112_DIAG_RECORDS 24U

/* Record wire frames (all free ids: 0x1021E trailer, 0x1021F..0x10227 rearm, 0x10228 WA). */
#define FW112_DIAG_EFID_HEADER    0x0001022AU  /* one per record: schema/session/event/type/reason */
#define FW112_DIAG_EFID_SNAP_BASE 0x0001022BU  /* +0..+3: 4 x 8 B chain-snapshot fragments */

typedef enum {
	FW112_EVT_NONE = 0,
	FW112_EVT_BLOCKED = 1,
	FW112_EVT_PERMISSION_GRANTED = 2,
	FW112_EVT_PERMISSION_REVOKED = 3,
	FW112_EVT_RECOVERY_ENTER = 4,
	FW112_EVT_RECOVERY_EXIT = 5,
	FW112_EVT_RECOVERY_COLLAPSE = 6,
	FW112_EVT_ZEROED = 7,
	FW112_EVT_HOLD_ARMED = 8,
	FW112_EVT_HOLD_EXPIRED = 9
} fw112_diag_event_t;

/*
 * The permission / WHO-ZEROED reason bitfield, computed by ride_control.c (the layer that
 * decides permission and demand) and echoed by the recorder. Multiple bits can be set - a
 * reader gets every cause that was true on the event tick, not a single guessed label.
 */
#define FW112_REASON_NONE          0x00U
#define FW112_REASON_LEVEL_ZERO    0x01U  /* assist level 0 - nothing is asked for at all */
#define FW112_REASON_DIRECTION     0x02U  /* pas_direction automaton not FORWARD_SAFE */
#define FW112_REASON_START_STEPS   0x04U  /* forward but crank_forward_steps < required_steps */
#define FW112_REASON_LOAD_BELOW    0x08U  /* steps met but load < engage threshold */
#define FW112_REASON_REARM_GRANT   0x10U  /* rearm permission window open (fast rearm catch-up) */
#define FW112_REASON_RECOVERY_WAIT 0x20U  /* recovery automaton in WAIT_FRESH_LOAD */
#define FW112_REASON_MODE_ZERO     0x40U  /* the mode calculation itself returned 0 that tick */
#define FW112_REASON_SAFETY        0x80U  /* brake / overtemp / torque fault / cal / hard cut */

/* Per-record flags. */
#define FW112_FLAG_LATCHED       0x01U  /* session latched this tick */
#define FW112_FLAG_FAST_REARM    0x02U  /* GRANTED was a fast rearm, not a cold start */
#define FW112_FLAG_COLD_ARM      0x04U  /* GRANTED was a cold start, not a fast rearm */
#define FW112_FLAG_PWM_ON        0x08U  /* PWM bridge driving this tick */
#define FW112_FLAG_SENSOR_VALID  0x10U  /* torque sensor reported valid this tick */
#define FW112_FLAG_CAL_USER      0x20U  /* user torque calibration in force this tick */
#define FW112_FLAG_WHEEL_VALID   0x40U  /* wheel rolling this tick (SPEED_STOP_TICKS freshness) */
#define FW112_FLAG_ROLLING_COAST 0x80U  /* SUSPEND_REASON_ROLLING_COAST: PAS stopped AND wheel
                                           still rolling - a PERMISSION_REVOKED with this bit set
                                           was a rolling-coast retention, not a direction hold */
/* FW-112.2: the two suspension reasons are read back from a PERMISSION_REVOKED record as
 *   direction hold:    FW112_REASON_DIRECTION set, FW112_FLAG_ROLLING_COAST clear
 *   rolling coast:     FW112_FLAG_ROLLING_COAST set (no direction reason - the direction
 *                      automaton stayed FORWARD_SAFE through the coast) */

/* C0-PROOF (schema 4): permission_bits — diagnostic mirror of the permission gate chain.
 * NOT a new gate; this is a measurement-only observation of what ride_control.c already
 * decided. Bit 7 is the aggregate ASSIST_PERMISSION: all conditions met for assist to flow. */
#define FW112_PERM_GATE_OPEN        0x01U  /* ride latch armed (session ACTIVE + direction + steps + load) */
#define FW112_PERM_MODE_SUPPORTED   0x02U  /* assist_modes_calculate() returned true */
#define FW112_PERM_IQ_REQUEST_POS   0x04U  /* mode_output.iq_request > 0 (before any limiter) */
#define FW112_PERM_HARD_CUT         0x08U  /* safety_cut_non_direction || direction_inhibit */
#define FW112_PERM_START_PHASE      0x10U  /* cranks starting from standstill */
#define FW112_PERM_SPEED_LIMIT_OK   0x20U  /* speed_x100 < speed_limit_x100 */
#define FW112_PERM_THROTTLE_PRESENT 0x40U  /* input->throttle_iq > 0 */
#define FW112_PERM_ASSIST_PERMISSION 0x80U /* aggregate: !hard_cut && gate_open && mode_supported
                                              && iq_request_pos — all conditions met for assist */

/* C0-PROOF (schema 4): flags2 — additional diagnostic observations per tick. */
#define FW112_FLAG2_ELAPSED_SAT    0x01U  /* elapsed_ticks saturated at 0xFFFF */
#define FW112_FLAG2_HOLD_SAT       0x02U  /* assist_hold_ticks saturated at 255 */
#define FW112_FLAG2_FORCE_ZERO     0x04U  /* force_zero_reference active (ride_control.c) */
#define FW112_FLAG2_HARD_CUT_SET   0x08U  /* hard_cut was true this tick */
#define FW112_FLAG2_RECOVERY_WAIT  0x10U  /* recovery_wait was true this tick */
#define FW112_FLAG2_FINAL_ZERO     0x20U  /* ASSIST_PERMISSION && (cadence>0 || start_phase)
                                             && iq_setpoint==0 — assist should flow but doesn't */
#define FW112_FLAG2_PU_CLAMPED     0x40U  /* iq_before_pu > iq_request — P/U ceiling clamped */
#define FW112_FLAG2_SPARE          0x80U

/* C0-PROOF (schema 4): one queued event record — exactly 32 B (see sizeof assert in fw112_diag.c).
 * 4 x 8 B CAN data frames. Dropped from schema 3: crank_forward_steps, required_steps,
 * start_steps (available from fw112_ab per-episode records). Added: packed_state,
 * permission_bits, flags2, hold_ticks_sat, motor_voltage_utilization, iq_before_pu.
 * elapsed_ticks narrowed u32 -> u16 with saturation. Recovery evidence stays in header spare
 * bytes (schema 3 wire semantics unchanged). */
typedef struct {
	/* ---- Frag 0: header (wire offset 0..7) ---- */
	uint16_t event_id;            /* monotonic, wraps at 65536 - a key, never a count */
	uint8_t  session_id;          /* the diag session this event belongs to */
	uint8_t  event_type;          /* fw112_diag_event_t */
	uint8_t  reason_bits;         /* FW112_REASON_* */
	uint8_t  flags;               /* FW112_FLAG_* */
	uint8_t  recovery_ev_lo;      /* schema 3 recovery evidence (spare header bytes) */
	uint8_t  recovery_ev_hi;
	/* ---- Frag 1: state (wire offset 8..15) ---- */
	uint8_t  packed_state;        /* [recovery:2][dir:2][session:2][spare:2] */
	uint8_t  permission_bits;     /* FW112_PERM_* — diagnostic mirror of gate chain */
	uint8_t  cadence_rpm;
	uint8_t  fwd_run;             /* consecutive-forward-step counter */
	uint8_t  hold_ticks_sat;      /* assist_hold_ticks saturated to u8 (min(val, 255)) */
	uint8_t  flags2;              /* FW112_FLAG2_* — additional per-tick observations */
	uint16_t elapsed_ticks;       /* control ticks since prev event, saturated at 0xFFFF */
	/* ---- Frag 2: torque + PU chain (wire offset 16..23) ---- */
	uint16_t load_centikg;        /* raw pedal load, same scale as the start gate */
	uint16_t load_threshold_centikg; /* the start gate's engage threshold this tick */
	uint16_t motor_voltage_utilization; /* C0-PROOF: u_abs clamped [0, 2048] */
	int16_t  iq_before_pu;        /* C0-PROOF: phase_iq_request BEFORE P/U ceiling */
	/* ---- Frag 3: Iq chain (wire offset 24..31) ---- */
	int16_t  iq_request;          /* what assist_modes asked for, AFTER P/U ceiling */
	int16_t  iq_pre_ramp;         /* the final pre-ramp target (after limits, before ramp) */
	int16_t  iq_setpoint;         /* what reached the motor command */
	int16_t  iq_actual;           /* measured motor current */
} fw112_diag_record_t;

typedef struct {
	uint8_t  session_state;
	uint8_t  recovery_state;
	uint8_t  dir_state;
	uint8_t  fwd_run;
	uint8_t  cadence_rpm;
	bool     latched;
	bool     pwm_on;
	bool     torque_sensor_valid;
	bool     cal_user;
	/* FW-112.2: why the session may be SUSPENDED - the wheel's SPEED_STOP_TICKS freshness and
	 * the rolling-coast verdict (real_stop && wheel_valid). main.c computes them from the same
	 * production facts ride_session used; the recorder only echoes them into the flags byte. */
	bool     wheel_valid;
	bool     rolling_coast;
	uint16_t assist_hold_ticks;
	uint16_t load_centikg;
	uint16_t load_threshold_centikg;
	/* FW-112-STABILITY: the recovery stability streak and the filtered assist signal that drives
	 * it - added to the input (and per-slot edge metadata) so a RECOVERY_COLLAPSE/EXIT record can
	 * carry the pre-transition evidence (see the schema note). Measurement only; the recorder
	 * never feeds a decision. */
	uint16_t recovery_stable_ticks;          /* live streak this tick (torque_input) */
	uint16_t recovery_stable_ticks_at_edge;  /* streak as it stood on the automaton's exit tick */
	uint16_t assist_delta_filtered_native;   /* the 35 ms filtered assist signal this tick */
	int16_t  iq_request;
	int16_t  iq_pre_ramp;
	int16_t  iq_setpoint;
	int16_t  iq_actual;
	uint8_t  reason_bits;
	/* C0-PROOF (schema 4): new diagnostic inputs. permission_bits is a diagnostic mirror of the
	 * gate chain decision (NOT a new gate). flags2 carries per-tick observations computed by
	 * ride_control.c. iq_before_pu is the phase_iq_request BEFORE the P/U ceiling clamp
	 * (measurement-only, from assist_modes.c finish_power_request). motor_voltage_utilization is
	 * the u_abs clamped [0, 2048] from the FOC loop (main.c). All measurement-only. */
	uint8_t  permission_bits;               /* FW112_PERM_* — diagnostic mirror */
	uint8_t  flags2;                        /* FW112_FLAG2_* — per-tick observations */
	int16_t  iq_before_pu;                  /* phase_iq before P/U ceiling (from assist_mode_output) */
	uint16_t motor_voltage_utilization;     /* u_abs clamped [0, 2048] */
} fw112_diag_input_t;

void fw112_diag_init(void);
/* The session id stamped into every record opened from now on - call with diag_session_current_id(). */
void fw112_diag_set_session_id(uint8_t session_id);
/* One observation per control tick; every edge is detected internally. */
void fw112_diag_tick(const fw112_diag_input_t *in, uint32_t now_tick);

/* diag_record_source bridge - same shape as rearm_delay_diag's. */
uint16_t fw112_diag_queue_count_session(uint8_t session_id);
bool fw112_diag_queue_peek_session(uint8_t session_id, fw112_diag_record_t *out);
void fw112_diag_queue_release_session(uint8_t session_id);
uint32_t fw112_diag_queue_enqueued(void);
uint32_t fw112_diag_queue_rejected(void);
/* Retention counters (FW-112-STABILITY, internal only - never on the wire): how many completed
 * sagas and how many records the full-ring eviction has removed. Read by the host tests to
 * prove the eviction engaged and never lost an in-flight saga. */
uint32_t fw112_diag_queue_evicted_sagas(void);
uint32_t fw112_diag_queue_evicted_records(void);

/* FW-112-STABILITY: the edge metadata stored beside a queued COLLAPSE/EXIT record (schema 3) -
 * edge_stable_ticks and event_value as documented above. Returns false if the session has no
 * queued record. */
bool fw112_diag_queue_peek_meta(uint8_t session_id, uint16_t *edge_stable_ticks, uint16_t *event_value);
/* FW-112-STABILITY: the current recovery episode's tracked state, as maintained by the module
 * (reset on RECOVERY_ENTER, closed on RECOVERY_EXIT). Read by the host tests to cross-check the
 * wire. */
uint16_t fw112_diag_episode_stable_current(void);
uint16_t fw112_diag_episode_stable_max(void);
uint16_t fw112_diag_episode_collapse_count(void);

#endif /* FW112_DIAG_H_ */
