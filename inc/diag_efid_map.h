#ifndef DIAG_EFID_MAP_H_
#define DIAG_EFID_MAP_H_

/*
 * FW-121.0: the diagnostic CAN id map, checked by the COMPILER.
 *
 * WHY THIS FILE EXISTS. Every diagnostic module picks its own extended CAN ids and states in a
 * comment which neighbouring range it must not touch. Those comments were true when written and
 * then went stale: fw112_ab.h still says "0x1022F onwards free", which it was until fw112_ab
 * itself took 0x1022F..0x10233 and fw117_trace took 0x10234..0x10238. FW-121.0 then read the
 * comment, claimed 0x1022F..0x10235, and shipped. The result was not a build error and not a
 * missing frame - it was a ride log full of frames under the expected ids carrying somebody
 * else's payload, which took a full decode session to unpick.
 *
 * Nothing here invents an id. Every range below is expressed in terms of the owning module's own
 * defines, so a module that moves or grows its block moves its entry here with it and the asserts
 * re-check the whole neighbourhood on the next build.
 *
 * ADDING A NEW DIAGNOSTIC BLOCK: define its base in its own header, add a DIAG_EFID_* pair here,
 * and add it to the assert list. If it overlaps anything, the build fails instead of the bike.
 */

#include "rearm_delay_diag.h"  /* REARM_DELAY_EFID_*      (FW-111) */
#include "fw112_diag.h"        /* FW112_DIAG_EFID_*       (FW-112-DIAG) */
#include "fw112_ab.h"          /* FW112_AB_EFID_*         (FW-112 A/B) */
#include "fw117_trace.h"       /* FW117_TRACE_EFID_*      (FW-117, TEMPORARY) */
#include "rolling_no_assist_diag.h" /* ROLLING_NO_ASSIST_EFID_* (rolling no-assist diagnostic) */

/* --- the occupied ranges, inclusive on both ends ------------------------------------------- */

/* FW-111 delayed-rearm recorder: three timing frames, one 4-fragment snapshot, one capture frame */
#define DIAG_EFID_REARM_LO        (REARM_DELAY_EFID_TIMING)
#define DIAG_EFID_REARM_HI        (REARM_DELAY_EFID_CAPTURE)

/* FW-112-DIAG whole-chain event recorder: header + 4 snapshot fragments */
#define DIAG_EFID_FW112_DIAG_LO   (FW112_DIAG_EFID_HEADER)
#define DIAG_EFID_FW112_DIAG_HI   (FW112_DIAG_EFID_SNAP_BASE + 3U)

/* FW-112 A/B rearm-episode logger: header + 4 record fragments */
#define DIAG_EFID_FW112_AB_LO     (FW112_AB_EFID_HEADER)
#define DIAG_EFID_FW112_AB_HI     (FW112_AB_EFID_SNAP_BASE + 3U)

/* FW-126 compact FOC-start trace on the FW-117 transport: header + 6 fragments */
#define DIAG_EFID_FW117_LO        (FW117_TRACE_EFID_HEADER)
#define DIAG_EFID_FW117_HI        (FW117_TRACE_EFID_DATA_BASE + FW117_TRACE_DATA_FRAGMENTS - 1U)

/* Rolling no-assist diagnostic schema v2: header + 6 data fragments */
#define DIAG_EFID_RNA_LO          (ROLLING_NO_ASSIST_EFID_HEADER)
#define DIAG_EFID_RNA_HI          (ROLLING_NO_ASSIST_EFID_DATA_BASE + ROLLING_NO_ASSIST_DATA_FRAGMENTS - 1U)

/* --- the check ----------------------------------------------------------------------------- */

#define DIAG_EFID_DISJOINT(a_lo, a_hi, b_lo, b_hi) (((a_hi) < (b_lo)) || ((b_hi) < (a_lo)))

_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_RNA_LO, DIAG_EFID_RNA_HI,
                                  DIAG_EFID_REARM_LO, DIAG_EFID_REARM_HI),
	"Rolling no-assist diagnostic CAN ids overlap the FW-111 rearm recorder's block");
_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_RNA_LO, DIAG_EFID_RNA_HI,
                                  DIAG_EFID_FW112_DIAG_LO, DIAG_EFID_FW112_DIAG_HI),
	"Rolling no-assist diagnostic CAN ids overlap FW-112-DIAG's block");
_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_RNA_LO, DIAG_EFID_RNA_HI,
                                  DIAG_EFID_FW112_AB_LO, DIAG_EFID_FW112_AB_HI),
	"Rolling no-assist diagnostic CAN ids overlap FW-112 A/B's block");
_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_RNA_LO, DIAG_EFID_RNA_HI,
                                  DIAG_EFID_FW117_LO, DIAG_EFID_FW117_HI),
	"Rolling no-assist diagnostic CAN ids overlap the FW-117 bridge trace's block");

/* The pre-existing neighbours must stay disjoint from each other too - otherwise this map would
 * only ever protect the newest arrival, which is precisely how the last collision happened. */
_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_REARM_LO, DIAG_EFID_REARM_HI,
                                  DIAG_EFID_FW112_DIAG_LO, DIAG_EFID_FW112_DIAG_HI),
	"FW-111 and FW-112-DIAG CAN id blocks overlap");
_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_FW112_DIAG_LO, DIAG_EFID_FW112_DIAG_HI,
                                  DIAG_EFID_FW112_AB_LO, DIAG_EFID_FW112_AB_HI),
	"FW-112-DIAG and FW-112 A/B CAN id blocks overlap");
_Static_assert(DIAG_EFID_DISJOINT(DIAG_EFID_FW112_AB_LO, DIAG_EFID_FW112_AB_HI,
                                  DIAG_EFID_FW117_LO, DIAG_EFID_FW117_HI),
	"FW-112 A/B and FW-117 CAN id blocks overlap");

/*
 * The aggregate block's own frames (0x10203..0x1020F, 0x10219 and the Walk Assist frame 0x10228)
 * and the episode/session frames (0x10210..0x1021E) are NOT one contiguous run below the record
 * blocks - 0x10228 sits above FW-111's 0x10220..0x10227. So the rule that is actually true, and
 * the one worth pinning, is simpler: the newest block goes above everything already taken.
 */
#define DIAG_EFID_AGG_WALK_ASSIST 0x00010228U

#endif /* DIAG_EFID_MAP_H_ */
