#ifndef ADC_TRIGGER_DIAG_H_
#define ADC_TRIGGER_DIAG_H_

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/*
 * FW-121.0 / FW-126.0: MEASURE the injected-ADC trigger chain. FW-121's original dark-bridge
 * sweep is retained only as a host-testable legacy mode; FW-126.0 arms the real measurement in
 * the existing FW-117 neutral dwell, before FOC can release torque. It changes no phase PWM
 * compare and no motor-control algorithm.
 *
 * WHY THIS EXISTS. The FW-121 audit could not decide, from the code or the vendor headers, WHICH
 * edge of TIMER0_CH3 actually starts the injected conversion:
 *
 *   hypothesis A - the CC3 EVENT (what the vendor header says the trigger is). Under CAM=11 that
 *                  event fires on the up-count match AND the down-count match; the ADC is idle at
 *                  the first one, so the UP match wins and the second is swallowed by the
 *                  in-progress conversion.
 *   hypothesis B - the rising edge of OC3REF, i.e. the DOWN-count match only.
 *
 * The two differ by 2 x (_T - CCR3) counts, and they demand OPPOSITE corrections to CCR3. Guessing
 * has a 50% chance of moving the worst-case sample OUT of the low-side conduction window instead
 * of into the middle of it, so FW-121 proper is blocked until this is measured.
 *
 * HOW IT DECIDES. With the counter position at ISR entry called CNT and the trigger-to-ISR
 * latency called L (unknown, but CONSTANT):
 *
 *   A:  CNT = 2*_T - CCR3 - CONV - L      ->  dCNT/dCCR3 = -1
 *   B:  CNT = CCR3 - CONV - L             ->  dCNT/dCCR3 = +1
 *
 * So the SIGN OF THE SLOPE of CNT against CCR3 answers the question without knowing L at all, and
 * the intercept then hands L over for free. Sweeping CCR3 from 3740 down to 3540 separates the two
 * predictions by 400 counts - far beyond any plausible interrupt-latency uncertainty.
 *
 * A second, independent discriminator comes free with the same sweep: under hypothesis A the two
 * CC3 events are 2 x (_T - CCR3) counts apart, so once that exceeds the conversion length (~408
 * counts) the second event is NO LONGER swallowed and a SECOND injected conversion starts in the
 * same PWM period. The ISR rate would then double. Counting ISRs per control tick is immune to
 * latency uncertainty entirely: 4 per tick means one conversion per PWM period, 8 means two.
 *
 * WHAT IT IS NOT. It measures the TRIGGER chain, not the analogue front end. The switching
 * settling time (t_settle) still needs an oscilloscope and stays HW_PENDING.
 *
 * FW-126.0 SAFETY. The production main loop no longer calls the dark-bridge arm entry: the
 * real-bike log proved that it yields no injected ISR data. The new arm entry is called only
 * after neutral CCR0/1/2 are committed, while neutral_dwell_active is set, and before POEN is
 * enabled. At every ISR it re-checks that the dwell is still active and all three phase CCRs are
 * neutral. A failed check restores production CCR3 immediately. The three-point sweep therefore
 * completes (or aborts) before the main loop can release FOC torque.
 */

/*
 * FW-126.2. The first real-bike sweep (log 2026-08-25 17:44) RAN but could not decide: the
 * three points gave slopes of -4 and +1.1 instead of a consistent -1 or +1. The log named all
 * three reasons itself, and every one of them is a property of taking ONE sample per CH3:
 *
 *   - one interrupt was rejected (ADC1_LATE=1), so the ISR sequence went 1 -> 3 -> 4 and two
 *     PWM periods, not one, separated the first two points;
 *   - point 0 was the FIRST interrupt after MOE ON and the only one with DIR=up. Both
 *     hypotheses predict entry on the DOWN slope, so that sample was measuring the bridge
 *     coming up, not the trigger. It alone produced the -4;
 *   - that left ONE usable pair (+1.1). A single pair cannot separate a real slope from a
 *     single disturbed reading, and section 18 forbids settling polarity without proof.
 *
 * So this card changes WHAT IS SAMPLED, never how the trigger works:
 *   1. the first interrupt after MOE ON is discarded outright;
 *   2. each CH3 value collects SAMPLES_TARGET accepted conversions and reports their MEDIAN,
 *      so one disturbed reading can no longer move the answer;
 *   3. a sample whose ADC1 group had not completed never enters the median - it is counted
 *      as rejected and the point simply waits for another interrupt;
 *   4. min/max/spread travel with every point, so "the numbers are stable" stops being an
 *      assumption;
 *   5. the CH3 actually in the compare register is read BACK at ISR entry, which turns "the
 *      value the module believes it set" into a measured fact.
 *
 * NOT changed by this card: NORMAL image, current calibration, the legacy hardware offsets,
 * FOC, and the production neutral-dwell length. The longer dwell below exists only in the
 * DIAG image and only while the sweep is actually running.
 */
#define ADC_TRIGGER_DIAG_NEUTRAL_POINTS   3U   /* 3740 / 3700 / 3660                        */
#define ADC_TRIGGER_DIAG_SAMPLES_TARGET   7U   /* accepted conversions per CH3 value        */
#define ADC_TRIGGER_DIAG_SAMPLES_MIN      5U   /* below this a point is not evidence        */

/*
 * DIAG-ONLY dwell length, in ISR cycles, used ONLY on the one bridge start that arms the
 * sweep. Everything else - including every later start in the same DIAG image - keeps
 * START_NEUTRAL_DWELL_CYCLES. Budget: 1 discarded + 3 x 7 accepted = 22 interrupts minimum,
 * and the rest is headroom for rejected ones. At 16 kHz, 40 cycles is 2.5 ms of neutral
 * vector - no torque, no current, all three phases at 50%.
 */
#define ADC_TRIGGER_DIAG_DWELL_CYCLES     40U

/* The sweep, exactly as specified by the card. Point 0 is the production value, i.e. the
 * baseline: at CCR3 = TRIGGER_DEFAULT nothing changes at all and the point is pure measurement. */
#define ADC_TRIGGER_DIAG_POINTS       6U
#define ADC_TRIGGER_DIAG_CCR3_MAX     3740U   /* = TRIGGER_DEFAULT, the production value */
#define ADC_TRIGGER_DIAG_CCR3_MIN     3540U   /* the card's floor - do not go lower without need */
#define ADC_TRIGGER_DIAG_CCR3_STEP    40U

/* How long each point is held. 400 control ticks = 100 ms = ~1600 injected conversions at 16 kHz,
 * which is far more than enough to separate 4 from 8 ISRs per tick. Whole sweep: 600 ms. */
#define ADC_TRIGGER_DIAG_POINT_TICKS  400U

/* adc_flags bits, captured at ISR entry (see adc_trigger_diag_sample_t) */
#define ADC_TRIGGER_DIAG_F_ADC0_EOIC  0x01U  /* ADC0 (phase C) had finished its injected conversion */
#define ADC_TRIGGER_DIAG_F_ADC2_EOIC  0x02U  /* ADC2 (phase A) had finished its injected conversion */
#define ADC_TRIGGER_DIAG_F_ADC0_STRC  0x04U  /* ADC0 regular (DMA scan) channel start flag was set */
#define ADC_TRIGGER_DIAG_F_ADC0_EOC   0x08U  /* ADC0 regular end-of-conversion flag was set */
#define ADC_TRIGGER_DIAG_F_ADC1_EOIC  0x10U  /* ADC1 (phase B / ISR source) completed this group       */

/*
 * Wire identity. These live HERE, not in main.c, because the first version of this card put raw
 * EFIDs in the aggregate builder and picked a block that was already owned by FW-112 A/B
 * (0x1022F..0x10233) and FW-117 (0x10234..0x10238). The bike then logged seven frames under
 * those ids that looked like sweep frames and were not. inc/diag_efid_map.h now asserts at
 * compile time that this block collides with nothing.
 */
#define ADC_TRIGGER_DIAG_EFID_STATUS      0x00010240U   /* sweep status                       */
#define ADC_TRIGGER_DIAG_EFID_POINT_BASE  0x00010241U   /* +0..+5: one frame per sweep point  */

/* How many aggregate frames this module contributes, and where they start in the block. main.c
 * asserts its own fixed-frame count against ADC_TRIGGER_DIAG_AGG_FIRST_INDEX, so the absolute
 * index of every frame is a compile-time fact rather than something only a ride log can reveal. */
#define ADC_TRIGGER_DIAG_AGG_FRAMES       (1U + ADC_TRIGGER_DIAG_POINTS)
#define ADC_TRIGGER_DIAG_AGG_FIRST_INDEX  14U

/*
 * FW-121.0B: the environment snapshot. The module stays hardware-free - main.c reads the
 * registers and globals and hands the result in as plain data, which is what keeps the whole
 * arm/abort chronology executable in a host test.
 *
 * WHY IT EXISTS. FW-121.0A found a hard contradiction the existing frames cannot resolve: the
 * arm gate can only pass with POEN clear, yet the ride log shows the sweep aborting on POEN with
 * zero samples counted - while the only runtime POEN enable is preceded by a blocking 25 ms
 * delay that should have produced ~400 counted interrupts first. One of those premises is false
 * and nothing recorded today says which. These snapshots record WHEN each event happened, at
 * 250 us resolution on the control tick and 8.3 ns inside the PWM period.
 */
#define ADC_TRIGGER_DIAG_ENV_POEN        0x01U  /* TIMER0 CCHP.POEN - bridge output enabled   */
#define ADC_TRIGGER_DIAG_ENV_DIR_DOWN    0x02U  /* TIMER0 CTL0.DIR - counter counting down     */
#define ADC_TRIGGER_DIAG_ENV_PWM_ON      0x04U  /* ui_8_PWM_ON_Flag                            */
#define ADC_TRIGGER_DIAG_ENV_CUTOFF      0x08U  /* pwm_cutoff_active (soft-cutoff ramp RUNNING) */
#define ADC_TRIGGER_DIAG_ENV_STANDSTILL  0x10U  /* hall_calibration_standstill_confirmed()     */
#define ADC_TRIGGER_DIAG_ENV_RUNNING     0x20U  /* the sweep was RUNNING when this was taken   */
#define ADC_TRIGGER_DIAG_ENV_IQ_NONZERO  0x40U  /* MS.i_q_setpoint != 0                        */
#define ADC_TRIGGER_DIAG_ENV_DWELL       0x80U  /* neutral_dwell_active at the exact snapshot   */

/* These are kept outside adc_trigger_diag_env_t.flags because the original byte is full. They
 * are supplied with each FW-126 neutral-dwell sample and are the hard interlock for CH3 ownership. */
#define ADC_TRIGGER_DIAG_SAFE_NEUTRAL_CCR 0x01U /* CCR0 == CCR1 == CCR2 == _T/2                 */
#define ADC_TRIGGER_DIAG_SAFE_FOC_HELD    0x02U /* neutral dwell still blocks FOC_calculation() */

typedef struct {
	uint32_t tick;             /* control_time_ticks, 250 us/LSB                          */
	uint16_t cnt;              /* TIMER0 CNT at the instant, 8.333 ns/LSB, 0..3750         */
	uint16_t iq_setpoint_abs;  /* |MS.i_q_setpoint|, saturating                            */
	uint8_t  lifecycle;        /* bridge_lifecycle (0 IDLE .. 5 RUN)                       */
	uint8_t  flags;            /* ADC_TRIGGER_DIAG_ENV_*                                   */
} adc_trigger_diag_env_t;

/* Which frozen snapshots the status frame says are present. */
#define ADC_TRIGGER_DIAG_SNAP_ARM        0x01U
#define ADC_TRIGGER_DIAG_SNAP_ABORT      0x02U
#define ADC_TRIGGER_DIAG_SNAP_POEN       0x04U
/* The discriminator FW-121.0A actually needs: a POEN enable was observed WHILE the sweep was
 * RUNNING. Clear + an abort means POEN was already high when the arm gate said it was low. */
#define ADC_TRIGGER_DIAG_SNAP_POEN_WHILE_RUNNING 0x08U

#define ADC_TRIGGER_DIAG_ABORT_NONE      0U
#define ADC_TRIGGER_DIAG_ABORT_POEN      1U
#define ADC_TRIGGER_DIAG_ABORT_SAFETY    2U /* dwell/neutral/FOC-held interlock broke */

/* Snapshot frame tags in Data0. Point frames carry 0x80|index (0x80..0x85), so these can never
 * be confused with one - the decoder never has to know the state to read a frame. */
#define ADC_TRIGGER_DIAG_TAG_ARM         0xA1U
#define ADC_TRIGGER_DIAG_TAG_ABORT       0xA2U
#define ADC_TRIGGER_DIAG_TAG_POEN        0xA3U

/* state */
#define ADC_TRIGGER_DIAG_IDLE         0U
#define ADC_TRIGGER_DIAG_RUNNING      1U
#define ADC_TRIGGER_DIAG_DONE         2U
#define ADC_TRIGGER_DIAG_ABORTED_MOE  3U  /* the bridge came up mid-sweep */

typedef struct {
	uint16_t ccr3;             /* the CCR3 this point was measured at                          */
	uint16_t isr_count;        /* injected ISR entries during the point (saturating)           */
	uint16_t tick_span;        /* control ticks the point spanned - == POINT_TICKS if complete */
	uint16_t cnt_first;        /* TIMER0 CNT at the first ISR of the point                     */
	uint16_t cnt_min;          /* ... and the range over the point: max-min is the jitter      */
	uint16_t cnt_max;
	uint16_t dir_up_count;     /* ISRs seen while TIMER0 was counting UP (saturating)          */
	uint16_t adc0_late_count;  /* ISRs where ADC0's injected conversion had NOT finished yet   */
	uint16_t adc2_late_count;  /* ... same for ADC2                                            */
	/* FW-126.0 neutral-dwell point: one real injected conversion, captured at ISR entry. */
	uint32_t tick_first;       /* exact control_time_ticks of the captured injected conversion */
	uint32_t isr_seq_first;    /* monotonically increasing ISR sequence within this sweep       */
	uint16_t raw_a;            /* ADC2 inserted JDR, before software current offset             */
	uint16_t raw_b;            /* ADC1 inserted JDR, before software current offset             */
	uint16_t raw_c;            /* ADC0 inserted JDR, before software current offset             */
	uint8_t  ready_flags;      /* ADC_TRIGGER_DIAG_F_ADC[012]_EOIC at ISR entry                 */
	uint8_t  env_flags;        /* MOE/DIR/DWELL and existing frozen environment bits             */
	uint8_t  lifecycle;        /* bridge_lifecycle at ISR entry                                 */
	uint8_t  complete;         /* the point ran its full tick span                             */
} adc_trigger_diag_point_t;

typedef struct {
	adc_trigger_diag_env_t env;  /* FW-121.0B: the full environment at ISR entry */
	uint8_t  adc_flags;          /* ADC_TRIGGER_DIAG_F_*                         */
	uint8_t  safety_flags;       /* FW-126.0: neutral CCR + FOC-held interlocks  */
	uint16_t raw_a;              /* ADC2 JDR sampled for this IRQ (before SW offset) */
	uint16_t raw_b;              /* ADC1 JDR sampled for this IRQ (before SW offset) */
	uint16_t raw_c;              /* ADC0 JDR sampled for this IRQ (before SW offset) */
	/* FW-126.2: TIMER0 CH3CV read at ISR ENTRY. The module knows which value it WROTE; only
	 * this says which value the hardware was actually comparing against for the conversion
	 * being reported, which is what rules out a whole class of off-by-one-period errors. */
	uint16_t ccr3_readback;
} adc_trigger_diag_sample_t;

/*
 * FW-126.2: one sweep point - a CH3 value and the several conversions measured at it.
 *
 * Kept separate from adc_trigger_diag_point_t so the legacy dark-bridge mode's six slots are
 * untouched: section 18 forbids deleting existing diagnostics to reclaim RAM, so this is
 * ADDED alongside rather than carved out of it.
 */
typedef struct {
	uint16_t ccr3_requested;   /* what the module programmed for this point            */
	uint16_t ccr3_readback;    /* what CH3CV actually held at the last accepted sample */
	uint16_t cnt[ADC_TRIGGER_DIAG_SAMPLES_TARGET];  /* CNT at ISR entry, per sample    */
	uint16_t cnt_median;       /* the point's answer - immune to one bad reading       */
	uint16_t cnt_min;
	uint16_t cnt_max;
	uint32_t isr_seq_first;    /* sweep-relative sequence of this point's first sample */
	uint8_t  accepted;         /* samples that entered the median                      */
	uint8_t  rejected_late;    /* interrupts refused here because ADC1 had not finished */
	uint8_t  dir_up_count;     /* accepted samples entered while counting UP           */
	uint8_t  eoic_all_count;   /* accepted samples with all three EOIC set             */
	uint8_t  env_flags;        /* POEN/DWELL from the last accepted sample             */
	uint8_t  complete;         /* reached SAMPLES_TARGET and was finalised             */
} adc_trigger_diag_npoint_t;

/* Call once at startup. `production_ccr3` is the value the module restores on finish/abort. */
void adc_trigger_diag_init(uint16_t production_ccr3);

/*
 * Main loop, once per control iteration. The sweep starts only when standstill_ok has been
 * established by the caller (the same 1 s standstill gate that guards Hall autodetect) and the
 * bridge output is off. It arms at most once per power cycle. Returns true when the caller must
 * program *ccr3_out into TIMER0 CH3.
 *
 * FW-121.0B: `env` is frozen as the ARM snapshot on the successful IDLE -> RUNNING transition and
 * never written again. The gate conditions themselves are UNCHANGED by that card.
 */
bool adc_trigger_diag_arm(bool standstill_ok, bool moe_on,
                          const adc_trigger_diag_env_t *env, uint16_t *ccr3_out);

/* FW-126.0 production entry point. Call exactly once from the existing bridge-start sequence,
 * after neutral CCR0/1/2 and neutral_dwell_active are committed but before MOE is enabled. The
 * module moves ONLY CH3 through 3740, 3700 and 3660; it never touches CCR0/1/2 or dwell policy. */
bool adc_trigger_diag_arm_neutral_dwell(const adc_trigger_diag_env_t *env,
									uint8_t safety_flags, uint16_t *ccr3_out);

/*
 * FW-121.0B: called from the one runtime `timer_primary_output_config(TIMER0, ENABLE)` site,
 * purely to record it. It changes nothing - not the enable, not the sweep, not the guard. The
 * FIRST enable seen while the sweep is RUNNING is frozen as the POEN snapshot; every enable is
 * counted.
 */
void adc_trigger_diag_note_poen_enable(const adc_trigger_diag_env_t *env);

/*
 * Injected-ADC ISR, once per conversion, ONLY when adc_trigger_diag_owns_ch3() is true.
 * Returns true when the caller must program *ccr3_out into TIMER0 CH3 (point change, finish or
 * MOE abort). The MOE check inside is the hard safety interlock, not a convenience.
 */
bool adc_trigger_diag_isr(const adc_trigger_diag_sample_t *s, uint16_t *ccr3_out);

/* Same ISR ownership entry for the FW-126.0 neutral-dwell mode. Each completed ADC1 interrupt
 * produces exactly one point, then CH3 is restored before the existing dwell can release FOC. */
bool adc_trigger_diag_neutral_dwell_isr(const adc_trigger_diag_sample_t *s, uint16_t *ccr3_out);

/* True while the module, not dyn_adc_trigger_update(), owns TIMER0 CH3. */
bool adc_trigger_diag_owns_ch3(void);

/*
 * One aggregate frame, built from the results this module holds. `sub` is 0 for the status frame
 * and 1..ADC_TRIGGER_DIAG_POINTS for the sweep points; the absolute index in the aggregate block
 * is ADC_TRIGGER_DIAG_AGG_FIRST_INDEX + sub. Returns false past the end.
 *
 * Layouts (the only place they are defined):
 *
 * FW-126.2 neutral-dwell mode (schema 8). Still exactly seven frames, so the aggregate block
 * and its compile-time index assert are unchanged - each point simply gets TWO of them:
 *
 *   0x10240 status  Data0 state, Data1 completed points, Data2-3 total sweep ISR count,
 *           Data4 max ISR/control tick, Data5-7 ADC0/ADC1/ADC2 late totals.
 *   0x10241/43/45  point N frame A:
 *           Data0 0xC0|N, Data1 CH3 requested - 3500, Data2 CH3 READBACK - 3500,
 *           Data3 accepted samples, Data4-5 MEDIAN CNT, Data6 samples rejected here,
 *           Data7 bit0 every accepted sample entered counting DOWN, bit1 any entered UP,
 *                 bit2 every accepted sample had all three EOIC, bit3 point complete,
 *                 bit4 POEN, bit5 DWELL.
 *   0x10242/44/46  point N frame B:
 *           Data0 0xB0|N, Data1-2 CNT min, Data3-4 CNT max, Data5 spread (clamped 255),
 *           Data6-7 ISR sequence of this point's first accepted sample.
 *
 *   Data0 of 0x10244 is what tells the decoder which schema it is holding, without needing
 *   any state: 0xB1 here, 0xA0 for FW-126.0 schema 7, 0xA1/0x83 for the legacy dark-bridge
 *   frames. The raw JDR triples schema 7 carried are gone from the wire - the evidence they
 *   were collected for (JDR ~0 in dwell versus ~1850 during MOE-off calibration) is frozen in
 *   the 2026-08-25 17:44 log and is the subject of FW-126.3, not of this measurement.
 *
 * FW-126.0 neutral-dwell mode (schema 7, superseded - kept so old logs stay readable):
 *   status  Data0 state, Data1 completed points, Data2-3 ISR sequence, Data4 max ISR/control
 *           tick, Data5-7 ADC0/ADC1/ADC2 late totals.
 *   0x10241..43 point Data0 point|complete, Data1 CH3-3500, Data2-3 CNT_ISR,
 *           Data4-5 ISR sequence low16, Data6 lifecycle, Data7 DIR/ADC0/ADC1/ADC2/MOE/DWELL.
 *   0x10244..46 raw Data0 0xA0/A1/A2 = A/B/C, Data1-6 three u16 raw JDR values,
 *           Data7 complete marker.
 *
 * Legacy FW-121 dark-bridge mode:
 *   status  Data0 state (0 IDLE / 1 RUNNING / 2 DONE / 3 ABORTED_MOE)
 *           Data1 point index reached
 *           Data2-3 production CCR3 restored on finish (u16, big endian)
 *           Data4 point count      Data5-6 ticks per point (u16)      Data7 CCR3 step
 *   point   Data0 point index, bit 0x80 set when the point ran its full span
 *           Data1 CCR3 - 3500      Data2-3 injected ISR entries (u16)
 *           Data4-5 TIMER0 CNT at ISR entry, minimum (u16)
 *           Data6 CNT spread (max-min), clamped to 255
 *           Data7 flags: 0x01 any sample while counting UP, 0x02 all samples while counting UP,
 *                        0x04 ADC0 (phase C) ever still converting, 0x08 ADC2 (phase A) ditto
 */
bool adc_trigger_diag_aggregate_frame(uint8_t sub, uint32_t *efid, uint8_t data[8]);

uint8_t adc_trigger_diag_state(void);
uint8_t adc_trigger_diag_index(void);
const adc_trigger_diag_point_t *adc_trigger_diag_get_point(uint8_t i);

/* FW-126.2 readback for the host tests and the frame builder. */
const adc_trigger_diag_npoint_t *adc_trigger_diag_get_npoint(uint8_t i);

/* True once the first interrupt after MOE ON has been discarded (FW-126.2 rule 1). */
bool adc_trigger_diag_first_isr_discarded(void);

/*
 * FW-126.2: true while the sweep still needs the extended DIAG dwell. main.c asks this instead
 * of tracking the sweep's state itself, so the one place that knows when the measurement is
 * finished is the module that runs it.
 */
bool adc_trigger_diag_needs_dwell(void);

/* FW-121.0B readback, for the host tests and the frame builder. */
uint32_t adc_trigger_diag_arm_isr_count(void);
uint32_t adc_trigger_diag_abort_isr_count(void);
uint8_t  adc_trigger_diag_abort_reason(void);
uint8_t  adc_trigger_diag_snap_flags(void);
uint16_t adc_trigger_diag_poen_enable_count(void);
uint32_t adc_trigger_diag_isr_per_control_max(void);
uint32_t adc_trigger_diag_adc0_late_total(void);
uint32_t adc_trigger_diag_adc1_late_total(void);
uint32_t adc_trigger_diag_adc2_late_total(void);
const adc_trigger_diag_env_t *adc_trigger_diag_snapshot(uint8_t tag);

#endif /* ADC_TRIGGER_DIAG_H_ */
