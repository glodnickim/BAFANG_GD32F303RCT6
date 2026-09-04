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
 * BUMPLESS HANDOVER falls out of the centre rule. The two formulas agree at exactly one place -
 * where the interpolation passes the sector midpoint - so a change of trust waits for that point.
 * The only exception is losing trust because the rotor stopped or the timing went invalid: no
 * further edge is coming, so it happens at once. That is the zone where FW-048 keeps the release
 * current-free anyway.
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

typedef struct {
	uint8_t trusted;         /* 1 = live interpolation, 0 = sector centre */
	uint8_t edges;           /* valid Hall edges since the anchor, saturating at the trust count */
	int8_t last_direction;   /* latched: a standstill keeps the sign it last measured */
	uint16_t prev_tim2;      /* previous tick's time-since-edge, for edge detection */
	uint8_t have_prev_tim2;
} rotor_angle_state_t;

typedef struct {
	int32_t hall_angle;         /* q31 calibrated angle of the sector boundary just crossed */
	int32_t angle_correction;   /* q31 calibration offset (MP.angle_correction) */
	int32_t direction;          /* measured rotor direction: +1, -1, or 0 when unknown */
	uint32_t tim2_recent;       /* Hall-timer ticks since the last edge */
	uint32_t tics_filtered_8;   /* filtered sector period, shifted left by 3 (uint32_tics_filtered) */
	bool want_untrusted;        /* speed hysteresis says the timing is no longer trustworthy */
	bool stalled;               /* no edge for far longer than a sector: give up without waiting */
	/*
	 * FW-131.1: the sign to use for the sector centre before this session has ever measured a
	 * direction - a cold boot at standstill. The caller passes -MP.reverse, which is exactly what
	 * the legacy six-step branch used, so a cold start behaves as it has for years. The first
	 * MEASURED direction latches and rules from then on.
	 */
	int32_t fallback_sign;
} rotor_angle_input_t;

void rotor_angle_reset(rotor_angle_state_t *state);

/* Returns the q31 electrical angle. Pure: no globals, so the whole thing runs on a host. */
int32_t rotor_angle_update(rotor_angle_state_t *state, const rotor_angle_input_t *input);

/* Exposed for the tests: the live interpolated offset, bounded to one sector. */
int32_t rotor_angle_sector_offset(const rotor_angle_input_t *input);

#endif /* ROTOR_ANGLE_H_ */
