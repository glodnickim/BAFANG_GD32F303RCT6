#include "iq_chain.h"

#include <string.h>

/*
 * FW-128A. See inc/iq_chain.h for what the audit actually found and why this module records two
 * stages rather than owning three. Pure state: no register, no arithmetic on the demand.
 */

static iq_chain_t chain;

void iq_chain_reset(void)
{
	memset(&chain, 0, sizeof(chain));
}

void iq_chain_note_requested(int32_t requested)
{
	chain.requested = requested;
	chain.valid = 1U;
}

void iq_chain_note_allowed(int32_t allowed)
{
	chain.allowed = allowed;
	chain.valid = 1U;
}

const iq_chain_t *iq_chain_get(void)
{
	return &chain;
}
