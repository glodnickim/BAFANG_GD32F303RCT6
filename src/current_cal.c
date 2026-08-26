/*
 * FW-119: retry / last-known-good / fallback policy for the FW-118 current calibration.
 *
 * See inc/current_cal.h for what this module is and, just as importantly, what it is not.
 * Nothing here touches hardware: main.c owns the ADC sampling and hands each attempt in as a
 * plain accumulation, which is what lets the whole policy be exercised on the host.
 */

#include "current_cal.h"
#include "config.h"

static void copy3_i16(int16_t *dst, const int16_t *src)
{
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
}

static void copy3_u16(uint16_t *dst, const uint16_t *src)
{
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
}

void current_cal_init(current_cal_t *s, current_cal_policy_t policy)
{
	uint8_t i;

	if (!s) return;

	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		s->offset[i] = 0;
		s->residual_mean[i] = 0;
		s->p2p[i] = 0;
		s->lkg_offset[i] = 0;
		s->lkg_residual_mean[i] = 0;
	}
	s->valid = 0;
	s->lkg_valid = 0;
	s->attempt_status = CURRENT_CAL_UNCALIBRATED;
	s->policy = policy;
	s->status = CURRENT_CAL_UNCALIBRATED;
	s->failure_reason = CURRENT_CAL_UNCALIBRATED;
	/*
	 * Before any measurement exists, the honest state is "legacy path, allowed to run": that is
	 * literally what the firmware did before FW-118. Starting from SRC_NONE / foc_allowed = 0
	 * would mean a controller that refuses to move if the calibration sequence is never reached
	 * at all - a failure mode this card must not introduce.
	 */
	s->source = CURRENT_CAL_SRC_LEGACY;
	s->attempts = 0;
	s->foc_allowed = 1;
	s->degraded = 1;
	s->last_sample_count = 0;
	s->last_moe_on = 0;
}

void current_cal_begin(current_cal_t *s)
{
	uint8_t i;

	if (!s) return;

	s->attempts = 0;
	s->attempt_status = CURRENT_CAL_UNCALIBRATED;
	s->failure_reason = CURRENT_CAL_UNCALIBRATED;
	s->last_sample_count = 0;
	s->last_moe_on = 0;
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		s->residual_mean[i] = 0;
		s->p2p[i] = 0;
	}
	/*
	 * offset[], valid, lkg_* and foc_allowed are deliberately NOT touched. Starting a fresh
	 * sequence must never pull the offsets out from under a running ISR, and must never discard
	 * a set that already passed.
	 */
}

uint8_t current_cal_attempt_allowed(const current_cal_t *s)
{
	if (!s) return 0;
	if (s->attempt_status == CURRENT_CAL_OK) return 0;
	return (uint8_t)(s->attempts < CURRENT_CAL_MAX_ATTEMPTS);
}

current_cal_status_t current_cal_submit(current_cal_t *s, const current_cal_attempt_t *a)
{
	uint8_t i;
	int16_t mean[CURRENT_CAL_PHASES];
	uint16_t p2p[CURRENT_CAL_PHASES];
	current_cal_status_t status = CURRENT_CAL_OK;

	if (!s || !a) return CURRENT_CAL_UNCALIBRATED;

	if (s->attempts < 0xFFu) s->attempts++;
	s->last_sample_count = a->samples;
	s->last_moe_on = a->moe_on ? 1U : 0U;

	/* This module never writes a timer register, but it must still refuse to bless a sample
	 * sequence collected while bridge output was active. main.c detects POEN both before and
	 * during its software-triggered conversion loop and carries that fact here. */
	if (a->moe_on) {
		s->attempt_status = CURRENT_CAL_MOE_ON;
		s->failure_reason = CURRENT_CAL_MOE_ON;
		return CURRENT_CAL_MOE_ON;
	}

	/*
	 * A timed-out attempt has no numbers at all, so it must not overwrite the recorded means
	 * and P2P of whatever was measured before - a decoder reading those must not see a zero row
	 * that looks like a real measurement.
	 */
	if (a->timed_out || a->samples == 0u) {
		s->attempt_status = CURRENT_CAL_SAMPLE_TIMEOUT;
		s->failure_reason = CURRENT_CAL_SAMPLE_TIMEOUT;
		return CURRENT_CAL_SAMPLE_TIMEOUT;
	}

	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		/*
		 * Divide by the samples actually taken, not by a hardcoded shift. FW-118 used `>> 6`
		 * against a configurable CURRENT_CAL_SAMPLES, so changing that constant would have
		 * silently produced a wrong mean and, through it, a wrong offset.
		 */
		mean[i] = (int16_t)(a->sum[i] / (int32_t)a->samples);
		p2p[i] = (a->max[i] >= a->min[i]) ? (uint16_t)(a->max[i] - a->min[i]) : 0u;
	}

	copy3_i16(s->residual_mean, mean);
	copy3_u16(s->p2p, p2p);

	/*
	 * Range before noise, so that a wildly wrong zero is reported as OUT_OF_RANGE even when it
	 * is also noisy - the range failure is the more actionable one on the bench.
	 *
	 * FW-125: mean[] is the SAME domain adc_inserted_data_read() returns at runtime (hardware
	 * IOFFx already subtracted in silicon), so the candidate offset is simply that mean - no
	 * second subtraction of the hardware offset constant. See current_cal.h.
	 */
	for (i = 0; i < CURRENT_CAL_PHASES; i++) {
		if (mean[i] < (int16_t)CURRENT_CAL_RESIDUAL_MIN || mean[i] > (int16_t)CURRENT_CAL_RESIDUAL_MAX) {
			status = CURRENT_CAL_OUT_OF_RANGE;
			break;
		}
	}
	if (status == CURRENT_CAL_OK) {
		for (i = 0; i < CURRENT_CAL_PHASES; i++) {
			if (p2p[i] > (uint16_t)CURRENT_ZERO_MAX_P2P_ADC) {
				status = CURRENT_CAL_TOO_NOISY;
				break;
			}
		}
	}

	if (status != CURRENT_CAL_OK) {
		s->attempt_status = status;
		s->failure_reason = status;
		/* offset[], valid and the LKG set are untouched: a failure never degrades a good set. */
		return status;
	}

	/*
	 * Publication order matters: the ISR reads `valid` and then offset[]. Dropping valid first
	 * means an ISR that fires mid-update sees the legacy path rather than a half-written set.
	 */
	s->valid = 0;
	copy3_i16(s->offset, mean);
	s->valid = 1;

	copy3_i16(s->lkg_offset, mean);
	copy3_i16(s->lkg_residual_mean, mean);
	s->lkg_valid = 1;

	s->attempt_status = CURRENT_CAL_OK;
	return CURRENT_CAL_OK;
}

void current_cal_finalize(current_cal_t *s)
{
	if (!s) return;

	if (s->attempt_status == CURRENT_CAL_OK) {
		s->status = CURRENT_CAL_OK;
		s->source = CURRENT_CAL_SRC_RUNTIME;
		s->degraded = 0;
		s->foc_allowed = 1;
		return;
	}

	if (s->lkg_valid) {
		/*
		 * Every attempt in THIS sequence failed but an earlier one passed. Keep running on it
		 * and say so: the offsets are real, but nobody re-confirmed them, so the ride is
		 * diagnostically degraded even though it is mechanically normal.
		 */
		s->valid = 0;
		copy3_i16(s->offset, s->lkg_offset);
		s->valid = 1;
		s->status = CURRENT_CAL_USING_LKG;
		s->source = CURRENT_CAL_SRC_LKG;
		s->degraded = 1;
		s->foc_allowed = 1;
		return;
	}

	if (s->policy == CURRENT_CAL_POLICY_STRICT) {
		s->valid = 0;
		s->status = CURRENT_CAL_HARD_FAILED;
		s->source = CURRENT_CAL_SRC_NONE;
		s->degraded = 1;
		s->foc_allowed = 0;
		return;
	}

	/*
	 * LEGACY_FALLBACK: the pre-FW-118 hardware-offset-only path. This is not "fine" - it is the
	 * old residual bias back again - so it is flagged degraded and carries its own status, but
	 * it does keep the bike rideable while the FW-118 limits are still provisional.
	 */
	s->valid = 0;
	s->status = CURRENT_CAL_LEGACY_FALLBACK;
	s->source = CURRENT_CAL_SRC_LEGACY;
	s->degraded = 1;
	s->foc_allowed = 1;
}

uint8_t current_cal_foc_allowed(const current_cal_t *s)
{
	if (!s) return 1;
	return s->foc_allowed;
}
