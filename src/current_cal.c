#include "current_cal.h"
#include "config.h"

#include <string.h>

/*
 * FW-126.7 phase-current zero calibration. See inc/current_cal.h for the domain contract, the
 * evidence that replaced the old dark-bridge calibration, and the deadlock this design avoids.
 *
 * Everything here is pure arithmetic on values handed in by main.c. No register is touched.
 */

static void cal_restart_collection(current_cal_t *s)
{
	uint8_t i;
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		s->acc[i].sum = 0;
		s->acc[i].min = 0;
		s->acc[i].max = 0;
	}
	s->eligible = 0;
	s->stable_count = 0;
}

void current_cal_init(current_cal_t *s)
{
	if (!s) return;
	memset(s, 0, sizeof(*s));
	s->state = CURRENT_CAL_ST_UNCALIBRATED;
	s->failure_reason = CURRENT_CAL_FAIL_NONE;
	s->source = CURRENT_CAL_SRC_NONE;
	/* valid = 0 and offset[] = 0: the ISR applies no software correction and active FOC is
	 * forbidden until something has actually been measured. */
}

uint8_t current_cal_needs_calibration(const current_cal_t *s)
{
	if (!s) return 0U;
	if (s->state == CURRENT_CAL_ST_VALID) return 0U;
	return (uint8_t)(s->attempts < (uint8_t)CURRENT_CAL_MAX_ATTEMPTS);
}

void current_cal_begin_attempt(current_cal_t *s)
{
	if (!s) return;
	if (!current_cal_needs_calibration(s)) return;

	if (s->attempts < 0xFFu) s->attempts++;
	s->state = CURRENT_CAL_ST_SETTLING;
	s->failure_reason = CURRENT_CAL_FAIL_NONE;
	s->cycles = 0;
	s->restarts = 0;
	s->timeout_hit = 0;
	s->neutral_ok = 1U;
	s->seen_sample = 0;
	cal_restart_collection(s);
}

uint8_t current_cal_wants_dwell(const current_cal_t *s)
{
	if (!s) return 0U;
	return (uint8_t)(s->state == CURRENT_CAL_ST_SETTLING || s->state == CURRENT_CAL_ST_COLLECTING);
}

uint8_t current_cal_foc_allowed(const current_cal_t *s)
{
	if (!s) return 0U;
	return (uint8_t)(s->state == CURRENT_CAL_ST_VALID && s->valid);
}

/* Fail this attempt. Nothing the ISR depends on is written: a failed attempt can never install,
 * degrade or resurrect an offset set. */
static current_cal_state_t cal_fail(current_cal_t *s, current_cal_fail_t why)
{
	s->state = CURRENT_CAL_ST_FAILED;
	s->failure_reason = why;
	return s->state;
}

/*
 * Validate and publish. Range is checked BEFORE noise, because a wildly wrong zero is the more
 * actionable failure on the bench - and because the whole point of this card is that a quiet
 * signal in the wrong place must not pass.
 */
static current_cal_state_t cal_finish(current_cal_t *s)
{
	uint8_t i;
	int16_t mean[CURRENT_CAL_PHASES];
	uint16_t p2p[CURRENT_CAL_PHASES];

	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		/*
		 * Round to NEAREST, not toward zero. Plain integer division truncates toward zero, which
		 * rounds a negative phase up and a positive phase down - and the three phases genuinely
		 * do straddle zero (measured -16 / -5 / +8). That asymmetry would inject a sub-LSB
		 * DIFFERENTIAL offset between phases, and a differential offset is precisely what Clarke
		 * and Park turn into a spurious current vector. Symmetric rounding costs two
		 * instructions, once per calibration.
		 */
		{
			const int32_t n = (int32_t)s->eligible;
			const int32_t half = n / 2;
			const int32_t sum = s->acc[i].sum;
			mean[i] = (int16_t)((sum >= 0) ? ((sum + half) / n) : ((sum - half) / n));
		}
		p2p[i] = (s->acc[i].max >= s->acc[i].min)
		         ? (uint16_t)(s->acc[i].max - s->acc[i].min) : 0u;
		s->mean[i] = mean[i];
		s->p2p[i] = p2p[i];
	}

	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		if (mean[i] < (int16_t)CURRENT_CAL_RESIDUAL_MIN ||
		    mean[i] > (int16_t)CURRENT_CAL_RESIDUAL_MAX) {
			return cal_fail(s, CURRENT_CAL_FAIL_OUT_OF_RANGE);
		}
	}
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		if (p2p[i] > (uint16_t)CURRENT_CAL_MAX_P2P) {
			return cal_fail(s, CURRENT_CAL_FAIL_TOO_NOISY);
		}
	}

	/* Publication order matters: the ISR reads `valid` and then offset[]. Clearing valid first
	 * means an ISR firing mid-update applies no correction rather than a half-written set. */
	s->valid = 0;
	for (i = 0; i < CURRENT_CAL_PHASES; i++) s->offset[i] = mean[i];
	s->valid = 1;

	s->source = CURRENT_CAL_SRC_NEUTRAL_DWELL;
	s->failure_reason = CURRENT_CAL_FAIL_NONE;
	s->state = CURRENT_CAL_ST_VALID;
	return s->state;
}

current_cal_state_t current_cal_sample(current_cal_t *s, const int16_t jdr[CURRENT_CAL_PHASES],
                                       uint8_t bridge_neutral)
{
	uint8_t i, in_window = 1U, steady = 1U;

	if (!s || !jdr) return CURRENT_CAL_ST_FAILED;
	if (s->state != CURRENT_CAL_ST_SETTLING && s->state != CURRENT_CAL_ST_COLLECTING) {
		return s->state;
	}

	/* The caller verifies the electrical state; this module refuses to measure without it. */
	if (!bridge_neutral) {
		s->neutral_ok = 0U;
		return cal_fail(s, CURRENT_CAL_FAIL_NOT_NEUTRAL);
	}

	if (s->cycles < 0xFFFFu) s->cycles++;
	if (s->cycles > (uint16_t)CURRENT_CAL_MAX_CYCLES) {
		s->timeout_hit = 1U;
		return cal_fail(s, CURRENT_CAL_FAIL_TIMEOUT);
	}

	/*
	 * GATE 1 - MIDPOINT / RESIDUAL WINDOW, checked first and on its own merit.
	 * The saturated dark reading (JDR ~ +1850) and the bridge-enable transient (JDR ~ +2050)
	 * are both far outside this window, so neither can ever reach the estimator.
	 */
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		if (jdr[i] < (int16_t)CURRENT_CAL_RESIDUAL_MIN ||
		    jdr[i] > (int16_t)CURRENT_CAL_RESIDUAL_MAX) {
			in_window = 0U;
			break;
		}
	}

	/*
	 * GATE 2 - STABILITY, and only as a SECOND condition. Saturation was stable too, so this
	 * can never be the whole test. Its job is to catch the tail of the rail->midpoint ramp:
	 * during that ramp the reading moves by hundreds of LSB per cycle, which this band rejects
	 * decisively, while settled noise (measured spread ~10 LSB over 16 samples) passes.
	 */
	if (s->seen_sample) {
		for (i = 0; i < CURRENT_CAL_PHASES; i++) {
			int32_t d = (int32_t)jdr[i] - (int32_t)s->prev[i];
			if (d < 0) d = -d;
			if (d > (int32_t)CURRENT_CAL_STABLE_BAND) { steady = 0U; break; }
		}
	} else {
		steady = 0U;   /* the very first cycle cannot prove stability against anything */
	}

	for (i = 0; i < CURRENT_CAL_PHASES; i++) s->prev[i] = jdr[i];
	s->seen_sample = 1U;

	if (!in_window || !steady) {
		/*
		 * Gate lost. Policy: RESTART the collection, never blend. A set mixing settled samples
		 * with transient ones would still pass a mean test while being wrong by whatever the
		 * transient contributed - exactly the class of quiet-but-wrong result this card exists
		 * to eliminate. Restarting is safe because the whole attempt is bounded by
		 * CURRENT_CAL_MAX_CYCLES, so this cannot loop forever.
		 */
		if (s->state == CURRENT_CAL_ST_COLLECTING && s->restarts < 0xFFFFu) s->restarts++;
		s->state = CURRENT_CAL_ST_SETTLING;
		cal_restart_collection(s);
		return s->state;
	}

	if (s->stable_count < 0xFFFFu) s->stable_count++;

	/* Not yet proven steady for long enough - eligible samples do not start here. */
	if (s->stable_count < (uint16_t)CURRENT_CAL_STABLE_CYCLES) {
		s->state = CURRENT_CAL_ST_SETTLING;
		return s->state;
	}

	/* GATE SATISFIED: this sample, and only from here, is eligible. */
	s->state = CURRENT_CAL_ST_COLLECTING;
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		if (s->eligible == 0u) {
			s->acc[i].min = jdr[i];
			s->acc[i].max = jdr[i];
		} else {
			if (jdr[i] < s->acc[i].min) s->acc[i].min = jdr[i];
			if (jdr[i] > s->acc[i].max) s->acc[i].max = jdr[i];
		}
		s->acc[i].sum += (int32_t)jdr[i];
	}
	s->eligible++;

	if (s->eligible >= (uint16_t)CURRENT_CAL_COLLECT_SAMPLES) return cal_finish(s);
	return s->state;
}
