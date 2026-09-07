#ifndef STOP_TRACE_H_
#define STOP_TRACE_H_

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* STOP-TRACE: an explicitly armed, passive recorder. The large session DIAG
 * image has no room for this buffer; NORMAL provides this on-demand instrument. */
#ifndef STOP_TRACE_ENABLE
#define STOP_TRACE_ENABLE (!CAN_DIAGNOSTICS_ENABLE)
#endif
#if STOP_TRACE_ENABLE && CAN_DIAGNOSTICS_ENABLE
#error "STOP-TRACE and full session diagnostics cannot share the RAM budget"
#endif

#define STOP_TRACE_SCHEMA 1U
#define STOP_TRACE_OVERVIEW 384U
#define STOP_TRACE_FAST 96U
#define STOP_TRACE_PRE 32U
#define STOP_TRACE_INTERVAL 320U /* 20 ms; peaks/events still evaluated every ISR */
#define STOP_TRACE_BYTES 40U
#define STOP_TRACE_DATA_ID 0x00010300U /* eight fragments, IDs 10300..10307 */

enum { ST_IDLE, ST_ARMED, ST_CAPTURING, ST_FROZEN };
enum { ST_END_NONE, ST_END_STOPPED, ST_END_TIMEOUT, ST_END_FULL, ST_END_NO_TRIGGER };
enum {
    ST_MOE=1U, ST_PWM=2U, ST_QUIET=4U, ST_FRESH=8U,
    ST_VALID=16U, ST_TRUSTED=32U, ST_REVERSE=64U,
    ST_ENTRY=128U, ST_HANDBACK=256U, ST_EXIT=512U,
    ST_ABORT=1024U, ST_ANGLE_JUMP=2048U, ST_SATURATED=4096U,
    ST_BAD_SAMPLE=8192U, ST_FOC_RAN=16384U
};

/* All currents are firmware counts, signed; CAL_I mA/count. Angle is one
 * unsigned 16-bit electrical turn. PI and voltage values are controller units.
 * A sample pairs current consumed in this ISR with voltage commanded by it. */
typedef struct {
    uint32_t tick;
    int16_t iq_ref, iq, id, uq, ud, piq, pid;
    uint16_t theta, hall_age, hall_timer, erps, vbus_10mv;
    int16_t iq_min, iq_max;
    uint16_t id_peak, theta_step_peak, flags;
    uint8_t hall, qzero;
} stop_trace_sample_t;

bool stop_trace_fast_needed(void);
/* Measured ADC handler body; core_hz is CMSIS SystemCoreClock. */
void stop_trace_timing(uint32_t cycles, uint32_t core_hz);
void stop_trace_tick(const stop_trace_sample_t *input); /* ADC ISR only */
bool stop_trace_arm(void); /* request only; ISR resets metadata, not buffers */
void stop_trace_status(uint8_t data[8]);
bool stop_trace_dump_request(void);
bool stop_trace_dump_busy(void);
void stop_trace_dump_step(uint32_t now_4k, bool allow_tx); /* foreground only */

/* Immutable access, also exercised by the host tests and serializer. */
uint16_t stop_trace_count(uint8_t stream);
bool stop_trace_sample(uint8_t stream, uint16_t index, stop_trace_sample_t *out);

#endif
