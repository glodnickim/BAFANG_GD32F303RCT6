#ifndef ROLLING_NO_ASSIST_DUMP_H_
#define ROLLING_NO_ASSIST_DUMP_H_

#include <stdbool.h>
#include <stdint.h>

#include "diag_session.h"

/*
 * Explicit, replayable transport for a FROZEN rolling_no_assist capture.
 *
 * This deliberately is not a diag_session record source: diag_session retires
 * records after a successful automatic dump, while a real-bike capture must
 * stay in RAM until a technician asks for it and must be sendable again after
 * a bad downstream/Canable capture. The recorder itself remains read-only.
 */

void rolling_no_assist_dump_init(const diag_can_ops_t *can_ops);

/*
 * Accept one explicit CAN dump request. It is accepted only for a frozen
 * capture and only when no earlier request/replay is outstanding. A completed
 * replay leaves the capture frozen, so a later request starts again at sample
 * zero. Returns false for NO FROZEN capture or a busy replay.
 */
bool rolling_no_assist_dump_request(void);

/* True while an accepted request is waiting for a safe standstill or sending. */
bool rolling_no_assist_dump_busy(void);

/*
 * Non-blocking transport step. `allow_new_tx` gives critical HMI/Canable
 * traffic first refusal of the CAN mailbox. `session_active` pauses a replay
 * while the rider is active; it never discards the frozen capture or cursor.
 */
void rolling_no_assist_dump_step(uint32_t now_tick, bool allow_new_tx, bool session_active);

#endif /* ROLLING_NO_ASSIST_DUMP_H_ */
