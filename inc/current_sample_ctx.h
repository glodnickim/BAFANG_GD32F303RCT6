#ifndef CURRENT_SAMPLE_CTX_H_
#define CURRENT_SAMPLE_CTX_H_

#include <stdint.h>

/*
 * FW-127B: one object describing one PWM/ADC transaction.
 *
 * THE PROBLEM THIS REPLACES
 * -------------------------
 * Everything needed to interpret a current sample used to live in four unrelated places:
 * the sampling sector in MS.char_dyn_adc_state, the direct-phase ownership implicit in a switch
 * inside dyn_adc_state_reconstruct(), the trigger only in a timer register, and the sequence in
 * a separate free-running counter. Nothing tied them to the same conversion, so nothing could
 * detect it when they disagreed - and the pre-audit showed they systematically did, because the
 * trigger was derived from the previous period's geometry.
 *
 * THE MODEL
 * ---------
 * Exactly two contexts exist:
 *
 *   LATCHED  describes the ADC conversion that is about to be consumed.  seq = N
 *   NEXT     is being prepared for the transaction after this one.       seq = N+1
 *
 * The publish point is the single place where NEXT becomes LATCHED, and it happens with the
 * compare writes that make that geometry real. A consumer therefore never sees a context whose
 * sector came from one transaction and whose trigger came from another.
 *
 * ATOMICITY, honestly stated. Both the publish and the consume run inside ADC0_1_IRQHandler,
 * and that ISR cannot preempt itself, so between them there is no window in which a partially
 * written context could be consumed by the control path. The publish is a plain structure
 * assignment for that reason, not because tearing was ruled out in general: a DIAGNOSTIC reader
 * in the main loop can still race it, which is why current_sample_ctx_snapshot() re-checks seq
 * and reports a torn read instead of returning half a context.
 */

#define CURRENT_SAMPLE_PHASES 3

/*
 * Validity of the sample a context describes. Deliberately NOT the same question as "did the
 * conversion complete" - EOIC proves completion, nothing more. FW-127D gives these teeth; B only
 * carries the field so the model is in place before anything depends on it.
 */
typedef enum {
	CURRENT_SAMPLE_INVALID = 0,   /* no trustworthy sampling window for this transaction   */
	CURRENT_SAMPLE_PRIMARY,       /* the preferred window exists                            */
	CURRENT_SAMPLE_ALTERNATE      /* primary unavailable, another valid window was used      */
} current_sample_state_t;

/* direct_mask bits: which phases were measured directly rather than reconstructed. */
#define CURRENT_SAMPLE_DIRECT_A  0x01U
#define CURRENT_SAMPLE_DIRECT_B  0x02U
#define CURRENT_SAMPLE_DIRECT_C  0x04U

typedef struct {
	uint8_t  sector;        /* SAMPLING sector, from the APPLIED geometry - not the Hall sector */
	uint8_t  state;         /* current_sample_state_t                                          */
	uint8_t  direct_mask;   /* CURRENT_SAMPLE_DIRECT_*                                         */
	uint16_t trigger_ccr;   /* the CH3 compare actually programmed for this transaction        */
	uint32_t seq;           /* transaction number; 0 means "no transaction yet"                */
} current_sample_context_t;

/*
 * Power-on / bridge-enable. Leaves both contexts INVALID with seq 0, so the first conversion
 * after a start can never be interpreted with state left over from a previous run.
 *
 *   PRODUCER : current_sample_ctx_reset()
 *   CONSUMER : the ISR consume path and the diagnostics
 *   RESET    : here, and at every bridge-disable/failsafe boundary
 *   PURPOSE  : a deterministic first state, so rolling start cannot inherit a stale context
 */
void current_sample_ctx_reset(void);

/*
 * Publish the context for the NEXT transaction. `seq` is assigned here and only here, so the
 * numbering cannot drift from the publishes: each call is exactly one transaction.
 * Returns the seq that was assigned.
 */
uint32_t current_sample_ctx_publish(uint8_t sector, uint8_t state, uint8_t direct_mask,
                                    uint16_t trigger_ccr);

/*
 * Take ownership of the context describing the conversion now in hand. Moves NEXT to LATCHED
 * and returns it. Call exactly once per ADC ISR, before anything interprets the sample.
 */
const current_sample_context_t *current_sample_ctx_consume(void);

/* The context currently owning the sample being consumed. Valid after _consume(). */
const current_sample_context_t *current_sample_ctx_latched(void);

/* The context prepared for the next transaction but not yet consumed. */
const current_sample_context_t *current_sample_ctx_next(void);

/*
 * Diagnostic snapshot for a reader outside the ISR. Returns 1 on a coherent copy, 0 if the
 * context changed underneath it - the caller then knows the read was torn rather than being
 * handed a mixture of two transactions.
 */
uint8_t current_sample_ctx_snapshot(current_sample_context_t *out);

/* How many transactions have been published this power cycle (passive evidence). */
uint32_t current_sample_ctx_published_count(void);

/* Consumes that found no published context waiting - i.e. a conversion nobody had described. */
uint32_t current_sample_ctx_orphan_count(void);

#endif /* CURRENT_SAMPLE_CTX_H_ */
