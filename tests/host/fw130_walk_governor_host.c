/*
 * FW-130: Walk Assist in the G532 character - one ramped demand, two soft ceilings, no integrator.
 *
 * Links the real walk_assist_motor.c + walk_speed_controller.c (no stubs) and drives them tick by
 * tick with a synthetic Hall input, the same way FW-113.1/113.2 do. Everything here is about the
 * behaviour the ride complaint named, so each case is written against the symptom it answers:
 *
 *   T1 RAMP CHARACTER   rise ~550 ms and fall ~110 ms of FULL SCALE. Law A rose in 427 ms but fell
 *                       in 1280 ms - it fell 3x slower than it rose, where the stock G532 falls 5x
 *                       FASTER. That asymmetry is the overshoot.
 *   T2 BAND IS CENTRED  the owner's requirement (2026-09-03): the governor must oscillate AROUND
 *                       the configured rpm, so at measured == target the ceiling is exactly half.
 *   T3 BAND SCALES      every supported 10..60 chainring-rpm target must be accepted and the
 *                       centred band must remain proportional across the range.
 *   T4 NO WIND-UP       after a long spell below target - which is exactly what wound law A's
 *                       integrator - crossing above the band must still collapse to zero in about
 *                       one fall ramp, not in seconds.
 *   T5 WHEEL TAPER      the fuse takes power away progressively BELOW the cut-off.
 *   T6 WHEEL CUT KEEPS  at the cut-off the current is zero in the same tick, but the session is NOT
 *      THE SESSION      torn down: no re-armed start, and dropping back below it is one rise ramp.
 *                       Law A zeroed AND reset here, which is the reported "cutting out".
 *   T7 HARD STOP        above cut-off + margin it IS a real stop, with WA_REASON_SPEED_GATE.
 *   T8 STRENGTH         the bank's Walk current finally reaches the motor, with the absolute WA
 *                       ceiling still on top of it and the old fixed 40 Iq as the unset fallback.
 *   T9 JAM GRACE        3 s of grace covers the longer ramp, and a genuine stall still latches.
 *
 * The suite is meaningful only for the FW-130 law; with WALK_GOVERNOR_ENABLE=0 it reports SKIPPED
 * rather than passing vacuously.
 */

#include "../common/check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "walk_assist_motor.h"

#define MAX_WHEEL_X100    700
#define TARGET_RPM_LOW     10U   /* -> 13 ERPS; 4-ERPS stability floor -> full 9, zero 17 */
#define TARGET_ERPS_LOW    13
#define TARGET_RPM_HIGH    60U   /* -> 80 ERPS, band 12 -> full 68, zero 92 */
#define TARGET_ERPS_HIGH   80
#define WALK_IQ_MAX_TEST  105    /* 15 % of PH_CURRENT_MAX 700: the new default strength */
#define KEEPALIVE_IQ        2    /* FW-130.1 WA_MOTOR_KEEPALIVE_IQ: the floor while a speed is known */
#define WA_MOTOR_REACQUIRE_CEILING_TEST 24  /* the module's bounded Hall-recovery ceiling */

#if (WALK_GOVERNOR_ENABLE == 0)

int main(void)
{
	(void)host_test_check;   /* law A build links no checks; keep the header warning-free */
	printf("FW-130 walk governor: SKIPPED (WALK_GOVERNOR_ENABLE=0, law A build)\n");
	return 0;
}

#else

static uint16_t hall_ticks_500k(uint16_t erps)
{
	return (uint16_t)(500000U / (6U * (uint32_t)erps));
}

static uint16_t hall_period_ticks_4k(uint16_t erps)
{
	uint32_t denom = 6U * (uint32_t)erps;
	return (uint16_t)((denom > 0U) ? (4000U / denom) : 0U);
}

typedef struct {
	walk_motor_input_t in;
	uint16_t age;
	uint16_t period;
	uint16_t hall_ticks;
	int32_t last_iq;
	int32_t iq_actual;
} drive_t;

static void drive_reset(drive_t *d, uint16_t target_rpm)
{
	memset(d, 0, sizeof *d);
	d->in.active = 1;
	d->in.target_chainring_rpm = target_rpm;
	d->in.max_wheel_speed_x100 = MAX_WHEEL_X100;
	d->in.walk_iq_max = WALK_IQ_MAX_TEST;
	d->age = 0xFFFFU;              /* no Hall: rotor stopped */
}

static void drive_set_erps(drive_t *d, uint16_t erps)
{
	d->period = hall_period_ticks_4k(erps);
	d->hall_ticks = hall_ticks_500k(erps);
}

static void drive_tick(drive_t *d, walk_motor_output_t *out)
{
	if (d->period > 0U) {
		if (++d->age >= d->period) {
			d->age = 0;
			d->in.motor_hall_ticks = d->hall_ticks;
		}
		d->in.motor_erps_age_ticks = d->age;
	} else {
		d->in.motor_erps_age_ticks = 0xFFFFU;
		d->in.motor_hall_ticks = 0;
	}
	d->in.motor_iq_reference = d->last_iq;
	d->in.motor_iq_actual = d->iq_actual;
	d->last_iq = walk_motor_update(&d->in, out);
}

/* Ticks needed for the command to first reach `iq`, or -1 if it never does. */
static int32_t ticks_until_iq(drive_t *d, walk_motor_output_t *out, int32_t iq,
	uint32_t budget, bool rising)
{
	for (uint32_t i = 0; i < budget; i++) {
		drive_tick(d, out);
		if (rising ? (out->iq_target >= iq) : (out->iq_target <= iq)) {
			return (int32_t)(i + 1U);
		}
	}
	return -1;
}

/* Bring a fresh session up to its full ceiling with the rotor stopped. */
static void spin_up(drive_t *d, walk_motor_output_t *out, uint16_t target_rpm)
{
	walk_motor_release();
	drive_reset(d, target_rpm);
	for (uint32_t i = 0; i < 3000U; i++) {
		drive_tick(d, out);
	}
}

int main(void)
{
	printf("FW-130 walk governor - ramp 550/110 ms, centred gear band, wheel fuse\n");
	printf("  walk_iq_max %d, target %u rpm -> %d ERPS, cut-off %d, margin %d\n",
		WALK_IQ_MAX_TEST, TARGET_RPM_LOW, TARGET_ERPS_LOW,
		MAX_WHEEL_X100, WA_WHEEL_HARD_MARGIN_X100);

	walk_motor_release();

	/* ==================================================================
	 * T1: ramp character. Rise 550 ms full scale, fall 110 ms full scale.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		walk_motor_release();
		drive_reset(&d, TARGET_RPM_LOW);
		int32_t rise = ticks_until_iq(&d, &out, WALK_IQ_MAX_TEST, 6000U, true);
		CHECK(rise > 0, "T1: the demand ramp reached the full walk ceiling");
		/* 2200 ticks = 550 ms at 4 kHz; integer step rounding costs a few percent. */
		CHECK(rise >= 1900 && rise <= 2600,
			"T1: full-scale rise is the G532 ~550 ms, not a step and not seconds");
		if (rise < 1900 || rise > 2600) {
			printf("    (rise took %d ticks = %d ms)\n", (int)rise, (int)(rise / 4));
		}

		/* Rotor suddenly well above the band -> gear factor 0 -> pure fall ramp. */
		drive_set_erps(&d, 90U);
		int32_t fall = ticks_until_iq(&d, &out, KEEPALIVE_IQ, 6000U, false);
		CHECK(fall > 0, "T1: the command fell to the keepalive floor above the band");
		/* 440 ticks = 110 ms, plus the handful of ticks the Hall estimate needs. */
		CHECK(fall >= 800 && fall <= 1400,
			"T1: full-scale fall is the FW-130.1 ~250 ms - still far FASTER than the rise");
		if (fall < 800 || fall > 1400) {
			printf("    (fall took %d ticks = %d ms)\n", (int)fall, (int)(fall / 4));
		}
		CHECK(fall < rise, "T1: the ramp is asymmetric the RIGHT way (falls faster than it rises)");
	}

	/* ==================================================================
	 * T2: the band is centred on the configured rpm (owner requirement).
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_LOW);

		/* Exactly at target: the ceiling must be half, i.e. the control oscillates AROUND it. */
		drive_set_erps(&d, (uint16_t)TARGET_ERPS_LOW);
		for (uint32_t i = 0; i < 4000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.measured_erps == (uint16_t)TARGET_ERPS_LOW,
			"T2: the Hall estimate settled exactly on the target (setup)");
		CHECK(out.gear_factor_q8 == 128,
			"T2: at measured == target the gear ceiling is exactly half - the band is CENTRED");
		if (out.gear_factor_q8 != 128) {
			printf("    (gear factor was %d/256)\n", (int)out.gear_factor_q8);
		}

		/* Below the band: nothing is taken away. */
		drive_set_erps(&d, (uint16_t)(TARGET_ERPS_LOW - 6));
		for (uint32_t i = 0; i < 4000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.gear_factor_q8 == 256,
			"T2: below the band the full walk ceiling is available");
		CHECK(out.iq_target == WALK_IQ_MAX_TEST,
			"T2: and the command climbs back to the full ceiling");

		/* Above the band: everything is taken away. */
		drive_set_erps(&d, (uint16_t)(TARGET_ERPS_LOW + 6));
		for (uint32_t i = 0; i < 4000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.gear_factor_q8 == 0, "T2: above the band the gear ceiling is zero");
		CHECK(out.iq_target == KEEPALIVE_IQ,
			"T2: and the command rests on the keepalive floor, not a dead stop");
		CHECK(out.state == (uint8_t)WA_STATE_REGULATE,
			"T2: taking all the current away is normal regulation, not a fault");
	}

	/* ==================================================================
	 * T3: every supported 10..60 rpm target is accepted and centred.
	 * ================================================================== */
	{
		static const uint16_t rpms[] = {10U, 15U, 20U, 30U, 40U, 50U, 60U};
		for (uint32_t k = 0; k < sizeof(rpms) / sizeof(rpms[0]); k++) {
			drive_t d;
			walk_motor_output_t out;
			uint16_t expected_erps = (uint16_t)(((uint32_t)rpms[k] * 4U + 1U) / 3U);
			spin_up(&d, &out, rpms[k]);
			drive_set_erps(&d, expected_erps);
			for (uint32_t i = 0; i < 4000U; i++) {
				drive_tick(&d, &out);
			}
			CHECK(out.target_erps == expected_erps,
				"T3: supported target maps to the expected chainring-derived ERPS");
			CHECK(out.gear_factor_q8 == 128,
				"T3: measured == target leaves exactly half the centred gear ceiling");
		}

		/* At the high end the proportional band is wider than at 10 rpm. */
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_HIGH);
		drive_set_erps(&d, (uint16_t)(TARGET_ERPS_HIGH + 6));
		for (uint32_t i = 0; i < 4000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.gear_factor_q8 > 0,
			"T3: the 60-rpm band still allows current 6 ERPS above target");
	}

	/* ==================================================================
	 * T4: no wind-up. A long spell below target must not delay the collapse.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_LOW);

		/* 5 s below the band at full current: this is what wound law A's integrator. */
		drive_set_erps(&d, (uint16_t)(TARGET_ERPS_LOW - 7));
		for (uint32_t i = 0; i < 20000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.iq_target == WALK_IQ_MAX_TEST,
			"T4: five seconds below target sit at the ceiling, never above it");
		CHECK(out.integral_iq == 0, "T4: there is no integrator to accumulate");

		drive_set_erps(&d, 90U);
		int32_t collapse = ticks_until_iq(&d, &out, KEEPALIVE_IQ, 20000U, false);
		CHECK(collapse > 0, "T4: the command still fell after the long low spell");
		CHECK(collapse <= 1400,
			"T4: the collapse is one fall ramp - the long spell below target cost nothing");
		if (collapse > 900) {
			printf("    (collapse took %d ticks = %d ms)\n",
				(int)collapse, (int)(collapse / 4));
		}
	}

	/* ==================================================================
	 * T5: the wheel fuse tapers BELOW the cut-off instead of switching.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_LOW);

		d.in.wheel_speed_x100 = (uint16_t)(MAX_WHEEL_X100 - WA_WHEEL_TAPER_X100 - 50);
		drive_tick(&d, &out);
		CHECK(out.wheel_factor_q8 == 256, "T5: below the taper the fuse takes nothing away");

		d.in.wheel_speed_x100 = (uint16_t)(MAX_WHEEL_X100 - (WA_WHEEL_TAPER_X100 / 2));
		drive_tick(&d, &out);
		CHECK(out.wheel_factor_q8 > 100 && out.wheel_factor_q8 < 160,
			"T5: halfway through the taper about half the ceiling is left");
		if (out.wheel_factor_q8 <= 100 || out.wheel_factor_q8 >= 160) {
			printf("    (wheel factor was %d/256)\n", (int)out.wheel_factor_q8);
		}
		CHECK((out.reason & WA_REASON_SPEED_GATE) == 0,
			"T5: a partial taper is not a gate - it must not report SPEED_GATE");

		d.in.wheel_speed_x100 = (uint16_t)(MAX_WHEEL_X100 - 20);
		drive_tick(&d, &out);
		CHECK(out.wheel_factor_q8 < 60,
			"T5: just under the cut-off almost nothing is left");
	}

	/* ==================================================================
	 * T6: AT the cut-off - zero in the same tick, but the session survives.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_LOW);
		CHECK(out.iq_target > 0, "T6: a full session is driving before the fuse (setup)");

		d.in.wheel_speed_x100 = MAX_WHEEL_X100;
		drive_tick(&d, &out);
		CHECK(out.iq_target == 0, "T6: at the cut-off the current is zero in the SAME tick");
		CHECK((out.reason & WA_REASON_SPEED_GATE) != 0,
			"T6: and the fuse names itself with WA_REASON_SPEED_GATE");
		CHECK(out.state == (uint8_t)WA_STATE_REGULATE,
			"T6: the session is NOT torn down at the cut-off");
		CHECK(out.state != (uint8_t)WA_STATE_STALL &&
			(out.reason & WA_REASON_STALL) == 0,
			"T6: no STALL latch from the fuse");

		/* Back below the cut-off: the return is a RAMP, not a step back to full power. */
		d.in.wheel_speed_x100 = 0;
		drive_tick(&d, &out);
		int32_t first = out.iq_target;
		CHECK(first < WALK_IQ_MAX_TEST / 4,
			"T6: the return below the cut-off ramps up instead of stepping to full power");
		int32_t back = ticks_until_iq(&d, &out, WALK_IQ_MAX_TEST, 6000U, true);
		CHECK(back > 0, "T6: and it does climb all the way back");
		CHECK(back >= 1900 && back <= 2600,
			"T6: the return is one normal rise ramp (~550 ms), not a fresh start");
		if (back < 1900 || back > 2600) {
			printf("    (return took %d ticks = %d ms, first tick %d Iq)\n",
				(int)back, (int)(back / 4), (int)first);
		}
	}

	/* ==================================================================
	 * T7: above cut-off + margin it is a real stop.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_LOW);
		d.in.wheel_speed_x100 =
			(uint16_t)(MAX_WHEEL_X100 + WA_WHEEL_HARD_MARGIN_X100);
		drive_tick(&d, &out);
		CHECK(out.iq_target == 0, "T7: above cut-off + margin the current is zero");
		CHECK((out.reason & WA_REASON_SPEED_GATE) != 0,
			"T7: the hard stop still reports SPEED_GATE");
		CHECK(out.state == (uint8_t)WA_STATE_OFF,
			"T7: and THIS one does tear the session down");
	}

	/* ==================================================================
	 * T8: the configured walk strength reaches the motor, bounded above.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;

		walk_motor_release();
		drive_reset(&d, TARGET_RPM_LOW);
		d.in.walk_iq_max = 0;                    /* unset -> the pre-FW-130 fixed ceiling */
		for (uint32_t i = 0; i < 6000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.iq_target == 40,
			"T8: an unset walk strength keeps the old fixed 40 Iq ceiling");

		walk_motor_release();
		drive_reset(&d, TARGET_RPM_LOW);         /* 105 from drive_reset */
		for (uint32_t i = 0; i < 6000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.iq_target == WALK_IQ_MAX_TEST,
			"T8: the configured strength is what the motor gets");

		walk_motor_release();
		drive_reset(&d, TARGET_RPM_LOW);
		d.in.walk_iq_max = 1000;                 /* absurd -> clamped by the module */
		/* Stay inside the jam grace: with no Hall this is a stalled rotor by definition, and
		 * T9 is where that is supposed to latch. Here only the ceiling is under test. */
		for (uint32_t i = 0; i < 6000U; i++) {
			drive_tick(&d, &out);
		}
		CHECK(out.iq_target == 157,
			"T8: the absolute WA ceiling still caps whatever the bank asks for");
		if (out.iq_target != 157) {
			printf("    (clamped to %d Iq)\n", (int)out.iq_target);
		}
	}

	/* ==================================================================
	 * T9: the jam grace covers the longer ramp; a real stall still latches.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		walk_motor_release();
		drive_reset(&d, TARGET_RPM_LOW);
		d.iq_actual = 30;                        /* current flows, rotor never moves */
		bool early_limit = false;
		for (uint32_t i = 0; i < 11000U; i++) {  /* < the 12000-tick grace */
			drive_tick(&d, &out);
			if (out.state == (uint8_t)WA_STATE_LIMIT ||
				out.state == (uint8_t)WA_STATE_STALL) {
				early_limit = true;
			}
		}
		CHECK(!early_limit,
			"T9: a slow start inside the 3 s grace is not called a jam");

		bool latched = false;
		for (uint32_t i = 0; i < 8000U; i++) {
			drive_tick(&d, &out);
			if (out.state == (uint8_t)WA_STATE_LIMIT ||
				out.state == (uint8_t)WA_STATE_STALL) {
				latched = true;
				break;
			}
		}
		CHECK(latched, "T9: a genuine sustained stall still latches LIMIT/STALL");
		CHECK((out.reason & WA_REASON_JAM) != 0 ||
			(out.reason & WA_REASON_LIMIT) != 0 ||
			(out.reason & WA_REASON_STALL) != 0,
			"T9: and it names itself in the reason bits");
	}

	/* ==================================================================
	 * T10 (FW-130.1): a missing speed reading must never RAISE the current.
	 *
	 * Owner requirement 2026-09-03: "as soon as it stops it cannot read speed from the Hall,
	 * because there will not be one." The controller must know that it does not know. v1 read
	 * silence as 0 rpm and answered with the full ceiling, which closes a loop with no input
	 * from the bike at all.
	 * ================================================================== */
	{
		drive_t d;
		walk_motor_output_t out;
		spin_up(&d, &out, TARGET_RPM_LOW);

		/* Settle well above the band: the governor has taken the current away on purpose. */
		drive_set_erps(&d, (uint16_t)(TARGET_ERPS_LOW + 8));
		for (uint32_t i = 0; i < 6000U; i++) {
			drive_tick(&d, &out);
		}
		int32_t settled = out.iq_target;
		CHECK(settled <= KEEPALIVE_IQ,
			"T10: settled at the floor above the band (setup)");
		CHECK(out.gear_factor_q8 == 0, "T10: the last REAL verdict was zero ceiling (setup)");

		/* Now the signal disappears entirely - exactly what a stopped rotor looks like. */
		d.in.motor_erps_age_ticks = 0xFFFFU;
		d.in.motor_hall_ticks = 0;
		d.period = 0;
		int32_t peak = 0;
		for (uint32_t i = 0; i < 4000U; i++) {
			drive_tick(&d, &out);
			if (out.iq_target > peak) peak = out.iq_target;
		}
		/* The bounded recovery is allowed to nudge the rotor; the GOVERNOR is not allowed to
		 * decide that silence means "go". Anything approaching the walk ceiling is the v1 bug. */
		CHECK(peak <= WA_MOTOR_REACQUIRE_CEILING_TEST,
			"T10: losing the speed signal never asks for more than the bounded recovery");
		if (peak > WA_MOTOR_REACQUIRE_CEILING_TEST) {
			printf("    (peak was %d Iq, walk ceiling is %d)\n",
				(int)peak, WALK_IQ_MAX_TEST);
		}
		CHECK(peak < WALK_IQ_MAX_TEST / 2,
			"T10: and nowhere near the full walk ceiling");
	}

	/* ==================================================================
	 * T11 (FW-143): configuration range is exactly 10..60 rpm. Values
	 * below/above the supported range must not become hidden 70/80-rpm
	 * Walk targets; the module falls back to the safe 30-rpm default.
	 * ================================================================== */
	{
		static const uint16_t invalid[] = {0U, 9U, 61U, 70U, 80U, 100U};
		for (uint32_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++) {
			drive_t d;
			walk_motor_output_t out;
			walk_motor_release();
			drive_reset(&d, invalid[k]);
			drive_tick(&d, &out);
			CHECK(out.target_erps == 40U,
				"T11: out-of-range Walk RPM falls back to the 30-rpm/40-ERPS default");
		}
	}

	if (host_test_failures == 0) {
		printf("All FW-130 walk governor checks passed.\n");
		return 0;
	}
	printf("\n%d FW-130 walk governor check(s) FAILED.\n", host_test_failures);
	return 1;
}

#endif /* WALK_GOVERNOR_ENABLE */
