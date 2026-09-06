/* FW-139: startup torque must have one owner. The deleted gear preload must not return as a
 * Hall-gated current cap in ride_control. This source guard complements sim/evist_sil.c's
 * closed-loop loaded-start scenario. */
#include "../common/check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef RIDE_CONTROL_C_PATH
#error RIDE_CONTROL_C_PATH required
#endif
#define S2(x) #x
#define S(x) S2(x)
static char *read_all(const char *p){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*b=malloc((size_t)n+1);if(!b){fclose(f);return NULL;}size_t g=fread(b,1,(size_t)n,f);fclose(f);b[g]=0;return b;}
int main(void){
 char *s=read_all(S(RIDE_CONTROL_C_PATH)); CHECK(s!=NULL,"source readable"); if(!s)return 1;
 CHECK(strstr(s,"PRELOAD_IQ_CAP")==NULL,"no second startup current cap PRELOAD_IQ_CAP");
 CHECK(strstr(s,"PRELOAD_TIMEOUT_TICKS")==NULL,"no 300 ms Hall-wait timeout in ride control");
 CHECK(strstr(s,"preload_active")==NULL,"no separate preload state machine");
 CHECK(strstr(s,"fast_iq_slew_publish")!=NULL,"final Iq mailbox remains the startup trajectory owner");
 free(s);
 if(host_test_failures==0){puts("FW-139 startup ownership guard passed.");return 0;} return 1;
}
