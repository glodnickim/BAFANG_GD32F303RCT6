#ifndef ROTOR_ANGLE_H_
#define ROTOR_ANGLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/*
 * FW-131 / FW-131.1 - one canonical rotor angle.
 *
 * THE DEFECT FW-131 REPLACED. main.c computed the electrical angle from two formulas and switched
 * between them on speed alone:
 *
 *     fast:  hall + angle_correction + interpolation(0..60deg across the sector)
 *     slow:  hall - reverse * 30deg
 *
 * They did not describe the same zero. The slow branch omitted angle_correction entirely and took
 * its sign from the configured MP.reverse while the fast one used the MEASURED direction, so at
 * the switch the angle stepped - measured at 24 degrees even exactly ON a Hall edge and up to 36
 * mid-sector. Any current flowing at that instant stepped with it.
 *
 * WHAT THIS MODULE DOES. One base, one offset, one place:
 *
 *     theta = (hall + angle_correction) + sector_offset
 *
 * The base is shared by every state, so there is only one zero. Only the offset changes with
 * confidence, and it is one of exactly two things:
 *
 *     TRUSTED    the live interpolation, bounded to one sector
 *     UNTRUSTED  the SECTOR CENTRE (direction * 30deg)
 *
 * FW-131.1 - WHY THE CENTRE, AND WHY A LEARNING GATE. Both come from the manufacturer's own
 * firmware for this hardware class (Fake Taxi, CONFIRMED in the reverse guide):
 *
 *     0 Hall transitions   -> theta = centre of the current sector
 *     1 transition         -> still the centre of the new sector
 *     >= 2 transitions     -> only now may timed interpolation be used
 *     Hall timeout         -> interpolation invalid, back to the sector centre
 *
 * FW-131 v1 got both of these wrong, and both mistakes land exactly where a start begins:
 *
 *   - it fell back to the sector BOUNDARY (offset 0) instead of the centre. The rotor is somewhere
 *     INSIDE the sector, so the boundary is wrong by 0..60 degrees where the centre is wrong by at
 *     most 30. The legacy code it replaced had this right - "hall - reverse*30deg" IS the centre.
 *   - it trusted the interpolation from the first tick after a reset, when the only sector period
 *     available is a stale one left over from before the rotor stopped. The manufacturer refuses
 *     to estimate speed from a single uncertain sample after a standstill, and so does this now.
 *
 * STOP-CLICK: a midpoint threshold alone did not guarantee continuity: a late switch could
 * replace a 60-degree offset with 30 degrees. Every formula change now preserves the previous
 * output and removes the difference at a bounded rate. Timeout always clears edge history,
 * including when already untrusted. Production supplies real Hall sequence numbers; timer
 * rollover is not movement. The fallback timer comparison remains for legacy host callers.
 *
 * FW-131.2 - THE EDGE RACE. Making the sequence number the sole definition of an edge introduced a
 * regression: the timer restart and the sequence number come from DIFFERENT contexts. The timer is
 * restarted by the hardware on the edge itself; the sequence number and hall_angle are published
 * by the Hall ISR. A FOC tick that lands between them sees "timer restarted, sequence unchanged"
 * and the old code read that as lost timing - so it cleared trust, and every such flip latched a
 * fresh transfer_offset for a sector that was about to arrive anyway. Recomputing during that
 * window is just as wrong: hall_angle still names the PREVIOUS sector while the timer has already
 * restarted, so the interpolation collapses to zero and the angle steps a whole sector backwards.
 * The window is now recognised for what it is and the previous output is HELD across it, bounded
 * by ROTOR_ANGLE_EDGE_PENDING_TICKS so a genuine rollover still loses trust.
 *
 * A/B: CANONICAL_ANGLE_ENABLE in inc/config.h. 0 keeps main.c's legacy branches untouched.
 */

/*
 * One Hall sector, in the SAME arithmetic the interpolation uses: 10923 << 16. main.c's comment
 * called this 715827883, which is the exact q31 60 degrees - the real value is 715849728, larger
 * by 0.0018 degrees. Physically irrelevant, but the handover compares against this, so the module
 * uses the number the arithmetic actually produces rather than the one the comment claimed.
 */
#define ROTOR_ANGLE_DEG_60  (10923 << 16)
#define ROTOR_ANGLE_DEG_30  (ROTOR_ANGLE_DEG_60 / 2)

/* Hall edges needed since the anchor before the timing may be believed (Fake Taxi: >= 2). */
#define ROTOR_ANGLE_TRUST_EDGES 2U

/*
 * FW-131.2: how many FOC ticks a restarted Hall timer may wait for the Hall ISR to publish the
 * sector that goes with it. The hardware restarts the timer ON the edge; the sequence number and
 * hall_angle are written by the ISR, so a FOC tick landing between the two sees a restarted timer
 * against the previous sector's angle. That is one ISR ordering, not a lost signal.
 *
 * 4 ticks is 250us at 16 kHz. One sector is shorter than that only above ~667 ERPS, far beyond
 * this drive, so the bound can never swallow a whole sector. Past the bound the restart is treated
 * as lost timing exactly as before, which is what still catches a real 16-bit timer rollover.
 */
#define ROTOR_ANGLE_EDGE_PENDING_TICKS 4U

/* Limit only the change of angle formula, not normal rotor rotation.
 * A 30-degree fallback takes 160 FOC ticks (10 ms at 16 kHz). */
#define ROTOR_ANGLE_TRANSFER_STEP (ROTOR_ANGLE_DEG_30 / 160)

typedef struct {
	uint8_t trusted;         /* 1 = live interpolation, 0 = sector centre */
	uint8_t edges;           /* valid Hall edges since the anchor, saturating at the trust count */
	int8_t last_direction;   /* latched: a standstill keeps the sign it last measured */
	uint16_t prev_tim2;      /* previous tick's time-since-edge, for edge detection */
	uint8_t have_prev_tim2;
	uint8_t pending_ticks;   /* FOC ticks a restarted timer has waited for its sector, 0 = none */
	uint8_t have_output;
	int32_t last_output, transfer_offset;
	uint32_t prev_hall_sequence;
} rotor_angle_state_t;

typedef struct {
	int32_t hall_angle;         /* q31 calibrated angle of the sector boundary just crossed */
	int32_t angle_correction;   /* q31 calibration offset (MP.angle_correction) */
	int32_t direction;          /* measured rotor direction: +1, -1, or 0 when unknown */
	uint32_t tim2_recent;       /* Hall-timer ticks since the last edge */
	uint32_t tics_filtered_8;   /* filtered sector period, shifted left by 3 (uint32_tics_filtered) */
	bool want_untrusted;        /* speed hysteresis says the timing is no longer trustworthy */
	bool stalled;               /* real 4 kHz Hall age exceeded 4 measured periods; clear history */
	/*
	 * FW-131.1: the sign to use for the sector centre before this session has ever measured a
	 * direction - a cold boot at standstill. The caller passes -MP.reverse, which is exactly what
	 * the legacy six-step branch used, so a cold start behaves as it has for years. The first
	 * MEASURED direction latches and rules from then on.
	 */
	int32_t fallback_sign;
	/* Explicit accepted Hall-edge sequence avoids mistaking 16-bit timer wrap for motion. */
	uint32_t hall_sequence;
	bool hall_sequence_valid;
} rotor_angle_input_t;

void rotor_angle_reset(rotor_angle_state_t *state);

/* Returns the q31 electrical angle. Pure: no globals, so the whole thing runs on a host. */
int32_t rotor_angle_update(rotor_angle_state_t *state, const rotor_angle_input_t *input);

/* Exposed for the tests: the live interpolated offset, bounded to one sector. */
int32_t rotor_angle_sector_offset(const rotor_angle_input_t *input);

#endif /* ROTOR_ANGLE_H_ */
