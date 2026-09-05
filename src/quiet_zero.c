/*
 * QZERO: Quiet Zero. Contract, rationale and ownership rules are in inc/quiet_zero.h.
 *
 * Implementation notes:
 *
 *   - No global state. The caller owns the quiet_zero_t instance, so the whole state machine
 *     runs on a host against the same code the ISR executes.
 *   - No division: the linear fade multiplies by a compile-time reciprocal.
 *   - The fade is computed from the CAPTURED entry value every tick (entry * remaining/N), not
 *     by repeated subtraction, so the trajectory is exactly linear and its last point is
 *     exactly 0.0f rather than an accumulated rounding residue.
 *   - The abort guard is checked before the fade is applied, so the tick that detects too much
 *     current is already a full-PI tick, not a P-only one. The low-speed handback is checked
 *     first of the two: it is the expected, benign end of a release, not a fault, and at those
 *     speeds the back-EMF is too small to have raised the current guard anyway.
 */

#include "quiet_zero.h"

#include <stddef.h>

static inline int32_t qz_abs(int32_t v)
{
	return (v < 0) ? -v : v;
}

void quiet_zero_reset(quiet_zero_t *qz)
{
	if (qz == NULL) {
		return;
	}
	qz->state = (uint32_t)QZERO_INACTIVE;
	qz->blend_tick = 0U;
	qz->iq_integral_entry = 0.0f;
	qz->id_integral_entry = 0.0f;
	qz->erps_entry = 0;
	qz->prev_iq_ref = 0;
	/*
	 * QZERO-2: seeded to 1, not 0, and that is deliberate. This reset runs where the bridge is
	 * being switched off in the same breath (cold PREPARE, dwell failsafe, hall calibration). If
	 * the producer's verdict happens to still be QUIET on the next ISR tick, a zero-seed would
	 * manufacture a rising edge out of the reset itself and arm a fade nobody asked for. Seeding
	 * it high means only a verdict that goes false and true again can arm anything.
	 */
	qz->prev_policy_quiet = 1U;
	qz->entries = 0U;
	qz->aborts = 0U;
	qz->low_speed_exits = 0U;
	qz->hold_ticks = 0U;
}

void quiet_zero_tick(
	quiet_zero_t *qz,
	const quiet_zero_input_t *in,
	quiet_zero_action_t *out)
{
	if (qz == NULL || in == NULL || out == NULL) {
		return;
	}

	out->apply_integral = false;
	out->iq_integral = 0.0f;
	out->id_integral = 0.0f;
	out->freeze_aw = false;
	out->clear_aw_edge = false;
	out->entered = false;
	out->exited = false;
	out->aborted = false;
	out->low_speed_release = false;

	/*
	 * QZERO-2: the rising edge of the QUIET verdict, captured BEFORE any return path so every
	 * exit from this function leaves the same one-tick history behind. See prev_policy_quiet in
	 * the header for why a second entry edge is needed at all.
	 */
	bool policy_edge = in->zero_policy_quiet && (qz->prev_policy_quiet == 0U);
	qz->prev_policy_quiet = in->zero_policy_quiet ? 1U : 0U;

	/*
	 * EXIT. A positive reference means the rider (or anything else entitled to ask) wants
	 * torque again. Leave on the same tick and report nothing to apply: the regulators get
	 * their integral back untouched and FOC-AW1 resumes, with the residual cleared once so the
	 * first active cycle does not correct against the held vector.
	 */
	if (in->iq_ref > 0) {
		if (qz->state != (uint32_t)QZERO_INACTIVE) {
			qz->state = (uint32_t)QZERO_INACTIVE;
			qz->blend_tick = 0U;
			out->exited = true;
			out->clear_aw_edge = true;
		}
		qz->prev_iq_ref = in->iq_ref;
		out->state = qz->state;
		return;
	}

	/*
	 * ENTRY. The reference is zero (a negative reference cannot be produced by the final slew
	 * owner, but treat it as zero rather than trusting that). Two edges arm the fade, and both
	 * describe the same physical event - "this zero is a rider release":
	 *
	 *   - the non-zero -> exact zero edge of the reference (v1), and
	 *   - QZERO-2: the rising edge of the QUIET verdict while the reference is already zero,
	 *     which is what happens when a limiter, a closing torque gate or simply easing off
	 *     reached zero before the cranks stopped.
	 *
	 * Every other zero is still left to the ordinary zero-current PI. The verdict is only ever
	 * granted after pedalling has ended, so this remains "never a limiter" as designed.
	 */
	if (qz->state == (uint32_t)QZERO_INACTIVE) {
		if ((qz->prev_iq_ref > 0 || policy_edge) && in->zero_policy_quiet &&
			in->rotor_erps >= in->min_brake_erps) {
			qz->state = (uint32_t)QZERO_BLEND;
			qz->blend_tick = 0U;
			qz->iq_integral_entry = in->iq_integral;
			qz->id_integral_entry = in->id_integral;
			qz->erps_entry = in->rotor_erps;   /* QZERO-3: the speed this integral matched */
			qz->entries++;
			out->entered = true;
			out->clear_aw_edge = true;
		} else {
			qz->prev_iq_ref = 0;
			out->state = qz->state;
			return;
		}
	}

	qz->prev_iq_ref = 0;

	/*
	 * LOW-SPEED HANDBACK. The rotor has reached the zone where the commutation angle advances in
	 * 60-degree steps (FW-048 / FW-131). Current must not be flowing when it steps.
	 *
	 * QZERO-3. v1 handed the axis back with the integral left at exactly zero and trusted the PI
	 * to "rebuild the back-EMF match again". It does - but not instantly, and until it has, the
	 * applied voltage does not match the back-EMF, so BRAKING CURRENT KEEPS FLOWING all the way
	 * down through the stepping zone. That is the same state the old foreground reset produced,
	 * which FW-048 identified as the click's precondition in the first place.
	 *
	 * The rider's own A/B settled it: a coast with no winding current at all was silent through a
	 * full stop AND a full restart, while the same passage under braking current clicks. So the
	 * last stretch has to be genuinely current-free, not merely heading that way.
	 *
	 * The value that nulls the current is the one matching the back-EMF, and back-EMF is
	 * proportional to speed - so the integral captured at entry, scaled by how much the rotor has
	 * slowed since, IS that value to a first order. Handing THAT back leaves the axis in the
	 * free-coast state directly, with no transient to hear. Energy scales with the square of
	 * speed, so surrendering the last few rev/s of braking costs well under 1 % of the run-on.
	 */
	/*
	 * FW-136: hand the axis back at a FRACTION OF THE RELEASE SPEED, not just above standstill.
	 *
	 * min_brake_erps alone means the whole run-down is braked and only the last sliver coasts -
	 * and that sliver sits in the speed zone where the reading is least trustworthy and where
	 * the decision therefore arrives late. Measured on the bike: the cut fires about 30 ms after
	 * the last Hall edge, sometimes before the last commutation steps and sometimes after, which
	 * is exactly the "clicks about half the time" the owner reported.
	 *
	 * Taking the threshold as a fraction of the speed the release started from moves the decision
	 * to where Hall edges are dense and the estimate is fresh, so there is nothing left to arrive
	 * late for. Energy goes as the square of speed, so handing back at half the speed still lets
	 * the brake take 75 % of it - the run-down stays short, and the whole low-speed zone becomes
	 * current-free, which is the condition already known to be silent.
	 *
	 * min_brake_erps stays as the floor. A release that began below it never gets a relative
	 * threshold under it, and FW-048 keeps its own meaning untouched.
	 */
	int32_t handback_erps = in->min_brake_erps;
	if (qz->erps_entry > 0) {
		int32_t relative = ((int32_t)qz->erps_entry * QZERO_HANDBACK_PCT) / 100;
		if (relative > handback_erps) {
			handback_erps = relative;
		}
	}
	if (in->rotor_erps < handback_erps) {
		qz->state = (uint32_t)QZERO_INACTIVE;
		qz->blend_tick = 0U;
		qz->low_speed_exits++;
		out->low_speed_release = true;
		out->clear_aw_edge = true;
		if (qz->erps_entry > 0 && in->rotor_erps > 0) {
			int32_t erps_now = in->rotor_erps;
			/* Only ever handed back at a lower speed; clamp so a bad reading cannot amplify. */
			if (erps_now > qz->erps_entry) {
				erps_now = qz->erps_entry;
			}
			float ratio = (float)erps_now / (float)qz->erps_entry;
			out->apply_integral = true;
			out->iq_integral = qz->iq_integral_entry * ratio;
			/*
			 * The d axis is NOT scaled: with the q current already at zero there is no
			 * cross-coupling term left for u_d to cancel, so zero is the value that nulls it.
			 */
			out->id_integral = 0.0f;
		}
		out->state = qz->state;
		return;
	}

	/*
	 * ABORT GUARD.
 During the P-only hold the only thing limiting winding current is the
	 * proportional term, so measured current is the quantity that has to be watched. Too much
	 * of it in either axis ends the hold and hands the axis back to the full zero-current PI -
	 * the integrator is then free to build the BEMF-matching voltage again, which is precisely
	 * the state that produces no current. It deliberately does not re-arm: a release that had
	 * to be aborted must not oscillate between the two behaviours.
	 */
	if (qz_abs(in->iq_measured) >= in->abort_current ||
		qz_abs(in->id_measured) >= in->abort_current) {
		qz->state = (uint32_t)QZERO_INACTIVE;
		qz->blend_tick = 0U;
		qz->aborts++;
		out->aborted = true;
		out->clear_aw_edge = true;
		out->state = qz->state;
		return;
	}

	if (qz->state == (uint32_t)QZERO_BLEND) {
		qz->blend_tick++;
		if (qz->blend_tick >= QZERO_BLEND_TICKS) {
			/* Last point of the fade is exact zero, and it is also the first HOLD tick. */
			qz->state = (uint32_t)QZERO_HOLD;
			qz->blend_tick = 0U;
			qz->hold_ticks++;
			out->iq_integral = 0.0f;
			out->id_integral = 0.0f;
		} else {
			float remaining = (float)(QZERO_BLEND_TICKS - qz->blend_tick) * QZERO_BLEND_RECIP;
			out->iq_integral = qz->iq_integral_entry * remaining;
			out->id_integral = qz->id_integral_entry * remaining;
		}
	} else {
		qz->hold_ticks++;
		out->iq_integral = 0.0f;
		out->id_integral = 0.0f;
	}

	out->apply_integral = true;
	out->freeze_aw = true;
	out->state = qz->state;
}
