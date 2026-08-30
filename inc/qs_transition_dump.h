#ifndef QS_TRANSITION_DUMP_H_
#define QS_TRANSITION_DUMP_H_
#include <stdbool.h>
#include <stdint.h>
#include "diag_session.h"
void qs_transition_dump_init(const diag_can_ops_t *can_ops);
bool qs_transition_dump_request(void);
bool qs_transition_dump_busy(void);
void qs_transition_dump_step(uint32_t now_tick, bool allow_new_tx, bool session_active);
#endif
