#ifndef FW1264_PROBE_H_
#define FW1264_PROBE_H_

#include <stdint.h>

/*
 * FW-126.4: does the ADC read a different domain when ONLY the conversion producer changes?
 *
 * Startup calibration (software trigger, MOE off) reads ~1850 on all three phases. The runtime
 * neutral dwell (CH3 hardware trigger, MOE on) reads ~0 through the same JDR registers.
 * FW-126.3 measured that with MOE off the CC3 event drives no injected conversion at all - on
 * any of the three ADCs - so the CC3 path could not be compared against software at all.
 *
 * What FW-126.3 could NOT test is the path the stock M820 actually uses:
 *
 *     TIMER0 CH3 compare -> O3CPRE -> TIMER0 TRGO -> ADC inserted trigger
 *
 * That is a DIFFERENT selector from the one EVistDrive uses. The register audit (see the card)
 * found EVistDrive on ADC_CTL1.ETSIC = 1 (T0_CH3) with TIMER0 CTL1.MMC still at its reset value
 * 0 (UPG as TRGO), i.e. O3CPRE -> TRGO is not configured at all. GD32F303 offers ETSIC = 0
 * (T0_TRGO) for ADC0/ADC1 AND for ADC2, so the same internal event can drive all three.
 *
 * This probe therefore runs ONE A/B pair with everything else held identical - same boot, same
 * channels, same ranks, same sample times, MOE off throughout, motor stationary:
 *
 *     A: software trigger      (the calibration producer)
 *     B: TIMER0 TRGO from O3CPRE (the stock producer)
 *
 * and reports the per-phase medians of both plus the delta. Only the producer differs, so a
 * delta is a property of the trigger path and a zero delta exonerates it.
 *
 * SAFETY, BY CONSTRUCTION: it never writes CCHP/POEN, never writes CCR0/1/2, never calls FOC,
 * never asks for torque. It changes exactly two things, both restored and read back afterwards:
 * ADC_CTL1.ETSIC on ADC0/ADC2, and TIMER0 CTL1.MMC.
 *
 * WHAT IT REPLACES. The FW-126.3 probe (fw1263_probe.h) is gone rather than extended: its
 * question is answered, and its storage was built around a per-ADC event survey that this A/B
 * test does not need in the same shape. Old logs stay readable - the decoders still know
 * schemas 1 and 2 - but the firmware carries only the current model.
 */
#define FW1264_SAMPLES 16U      /* accepted samples per phase, per capture */
#define FW1264_PHASES  3U       /* A = ADC2, B = ADC1, C = ADC0 - as the FOC ISR reads them */
#define FW1264_ADCS    3U

/* One capture. Statistics only: the raw samples live on the stack during the capture and are
 * not worth 96 bytes of permanent RAM once the median is known. */
typedef struct {
	int16_t  median[FW1264_PHASES];
	int16_t  smin[FW1264_PHASES];
	int16_t  smax[FW1264_PHASES];
	uint8_t  n[FW1264_PHASES];      /* accepted samples per phase                        */
	uint16_t events[FW1264_ADCS];   /* EOIC events per ADC, observed INDEPENDENTLY        */
} fw1264_capture_t;

typedef struct {
	fw1264_capture_t sw;      /* test A: software trigger                                 */
	fw1264_capture_t trgo;    /* test B: TIMER0 TRGO from O3CPRE                          */
	uint16_t ch3;             /* TIMER0 CH3CV during the probe                            */
	uint8_t  adc0_src_before;     /* ADC_CTL1.ETSIC snapshots, taken before any change    */
	uint8_t  adc2_src_before;
	uint8_t  adc0_src_restored;   /* ...and read BACK after the restore                   */
	uint8_t  adc2_src_restored;
	uint8_t  trgo_before;         /* TIMER0 CTL1.MMC before / during / after              */
	uint8_t  trgo_during;
	uint8_t  trgo_restored;
	uint8_t  moe_off;         /* CCHP.POEN clear before AND after                         */
	uint8_t  timer_running;   /* TIMER0 CTL0.CEN                                          */
	uint8_t  restore_ok;      /* every changed register read back equal to its snapshot   */
	uint8_t  done;
} fw1264_probe_t;

/* main.c owns the storage; the serializer only reads it. */
const fw1264_probe_t *fw1264_probe_state(void);

#endif /* FW1264_PROBE_H_ */
