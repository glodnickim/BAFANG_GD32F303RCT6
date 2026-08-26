#include "rolling_no_assist_dump.h"
#include "rolling_no_assist_diag.h"
#include "diag_budget.h"

#if CAN_DIAGNOSTICS_ENABLE && ROLLING_NO_ASSIST_DIAG_ENABLE

#include <string.h>

/*
 * The manual replay has its own cursor. It NEVER calls a recorder release
 * function: release means "forget this record" in diag_session, which is the
 * exact behaviour this transport must not have after FROZEN.
 */
#define RNA_DUMP_TX_IDLE    0U
#define RNA_DUMP_TX_PENDING 1U

static struct {
	const diag_can_ops_t *can_ops;
	uint16_t sample_index;
	uint8_t fragment;
	uint8_t tx_state;
	uint8_t tx_mailbox;
	uint8_t tx_retry;
	bool request_armed;
	bool replay_active;
	uint32_t tx_pace_last_tick;
} D;

_Static_assert(sizeof(D) <= DIAG_BUDGET_ROLLING_NO_ASSIST_DUMP_BYTES,
	"rolling_no_assist_dump: total state exceeds its RAM line item");

static void stop_replay(void)
{
	D.replay_active = false;
	D.tx_state = RNA_DUMP_TX_IDLE;
	D.tx_retry = 0U;
}

static void advance_cursor(void)
{
	D.tx_retry = 0U;
	D.fragment++;
	if (D.fragment < ROLLING_NO_ASSIST_FRAMES_PER_SAMPLE) return;

	D.fragment = 0U;
	D.sample_index++;
	if (D.sample_index >= ROLLING_NO_ASSIST_DIAG_SAMPLES) {
		/* The immutable FROZEN ring is intentionally retained for the next request. */
		stop_replay();
	}
}

void rolling_no_assist_dump_init(const diag_can_ops_t *can_ops)
{
	memset(&D, 0, sizeof(D));
	D.can_ops = can_ops;
}

bool rolling_no_assist_dump_request(void)
{
	if (!rolling_no_assist_diag_is_frozen()) return false;
	if (D.request_armed || D.replay_active) return false;
	D.request_armed = true;
	return true;
}

bool rolling_no_assist_dump_busy(void)
{
	return D.request_armed || D.replay_active;
}

void rolling_no_assist_dump_step(uint32_t now_tick, bool allow_new_tx, bool session_active)
{
	if (D.can_ops == 0) return;

	/* A reset/reinitialisation is the sole RAM-lifetime boundary. Never send stale data. */
	if (!rolling_no_assist_diag_is_frozen()) {
		D.request_armed = false;
		stop_replay();
		return;
	}

	/* A confirmed transmit may be observed even after the rider starts moving. Poll it; only
	 * starting the NEXT frame is paused. This keeps the mailbox state honest and sends no new
	 * diagnostic traffic during riding. */
	if (D.tx_state == RNA_DUMP_TX_PENDING) {
		uint8_t state = D.can_ops->state(D.tx_mailbox);
		if (state == DIAG_CAN_PENDING) return;
		D.tx_state = RNA_DUMP_TX_IDLE;
		if (state == DIAG_CAN_OK) {
			advance_cursor();
		} else {
			D.tx_retry++;
			/* A failed replay remains fully retained. Stop rather than skip one frame; a later
			 * explicit request starts a clean, complete capture at sample zero. */
			if (D.tx_retry > DIAG_TX_MAX_RETRY) {
				stop_replay();
				return;
			}
		}
	}

	if (!D.replay_active && D.request_armed && !session_active) {
		D.request_armed = false;
		D.replay_active = true;
		D.sample_index = 0U;
		D.fragment = 0U;
		D.tx_retry = 0U;
	}

	if (!D.replay_active || session_active || !allow_new_tx) return;
	if ((uint32_t)(now_tick - D.tx_pace_last_tick) < DIAG_TX_FRAME_INTERVAL_TICKS) return;

	rolling_no_assist_sample_t sample;
	uint32_t efid = 0U;
	uint8_t data[8];
	bool last = false;
	if (!rolling_no_assist_diag_capture_sample_at(D.sample_index, &sample) ||
	    !rolling_no_assist_diag_encode_fragment(
		&sample, rolling_no_assist_diag_capture_session_id(),
		rolling_no_assist_diag_capture_id(), rolling_no_assist_diag_trigger_case(),
		D.fragment, &efid, data, &last)) {
		/* The capture must be immutable for the whole replay. Failing closed retains it. */
		stop_replay();
		return;
	}
	(void)last; /* Cursor geometry is checked independently above. */

	uint8_t mailbox = D.can_ops->transmit(efid, data);
	if (mailbox == DIAG_CAN_NOMAILBOX) return;
	D.tx_pace_last_tick = now_tick;
	D.tx_mailbox = mailbox;
	D.tx_state = RNA_DUMP_TX_PENDING;
}

#else

typedef int rolling_no_assist_dump_not_compiled_in;

#endif /* CAN_DIAGNOSTICS_ENABLE && ROLLING_NO_ASSIST_DIAG_ENABLE */
