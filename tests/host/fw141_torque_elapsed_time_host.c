/* FW-141: torque filtering is a real-time property, not a foreground-call-count property. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "torque_input.h"

#ifndef MAIN_C_PATH
#define MAIN_C_PATH "src/main.c"
#endif

#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); return 1; } } while (0)

typedef struct { uint16_t fast, run; } result_t;

static void one(uint16_t raw, uint32_t elapsed)
{
    int16_t corrected = torque_input_correct(raw);
    torque_input_update_elapsed(raw, corrected, true, elapsed);
}

static result_t rise_case(uint32_t calls, uint32_t elapsed)
{
    torque_input_init();
    torque_input_startup_zero(TORQUE_ZERO_TARGET_NATIVE);
    const uint16_t raw = TORQUE_ZERO_TARGET_NATIVE + 400U;
    for (uint32_t i=0;i<calls;i++) one(raw, elapsed);
    const torque_snapshot_t *s=torque_input_get_snapshot();
    return (result_t){s->assist_delta_filtered_native,s->assist_delta_run_native};
}

static result_t fall_case(uint32_t calls, uint32_t elapsed)
{
    torque_input_init();
    torque_input_startup_zero(TORQUE_ZERO_TARGET_NATIVE);
    const uint16_t high = TORQUE_ZERO_TARGET_NATIVE + 400U;
    for (uint32_t i=0;i<2000U;i++) one(high,1U);
    for (uint32_t i=0;i<calls;i++) one(TORQUE_ZERO_TARGET_NATIVE,elapsed);
    const torque_snapshot_t *s=torque_input_get_snapshot();
    return (result_t){s->assist_delta_filtered_native,s->assist_delta_run_native};
}

static char *read_all(const char *path)
{
    FILE *f=fopen(path,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    char *b=(char*)malloc((size_t)n+1U); if(!b){fclose(f);return NULL;}
    fread(b,1,(size_t)n,f); b[n]=0; fclose(f); return b;
}

int main(void)
{
    /* Same 35 ms physical interval: 140 dense calls vs 35 calls each carrying 4 real ticks. */
    result_t rd=rise_case(140U,1U), rs=rise_case(35U,4U);
    CHECK(rd.fast==rs.fast,"35 ms FAST rise changed with call density");
    CHECK(rd.run==rs.run,"ordinary RUN rise changed with call density");

    /* Same 100 ms physical ease-off after an identical steady state. */
    result_t fd=fall_case(400U,1U), fs=fall_case(100U,4U);
    CHECK(fd.fast==fs.fast,"FAST fall changed with call density");
    CHECK(fd.run==fs.run,"ordinary RUN fall changed with call density");

    /* elapsed=0 is defensive one-tick semantics, never a frozen filter. */
    result_t z1=rise_case(1U,1U), z0=rise_case(1U,0U);
    CHECK(z1.fast==z0.fast && z1.run==z0.run,"elapsed=0 defensive semantics differ from one tick");

    char *main_c=read_all(MAIN_C_PATH);
    CHECK(main_c!=NULL,"cannot read main.c");
    CHECK(strstr(main_c,"torque_input_update_elapsed(torque_raw_mv, MS.torque_on_crank, torque_fault==0, control_delta)")!=NULL,
          "production main.c does not feed control_delta into torque filtering");
    free(main_c);

    printf("FW-141 torque elapsed-time filter PASS: dense/sparse rise and fall are identical\n");
    return 0;
}
