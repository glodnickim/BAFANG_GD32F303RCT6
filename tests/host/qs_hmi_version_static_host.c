#include <stdio.h>
#include <string.h>
static int file_has(const char *p, const char *needle) { FILE *f=fopen(p,"rb"); char b[2048]; size_t n; if(!f)return 0; while((n=fread(b,1,sizeof(b)-1,f)) != 0U) { b[n]=0; if(strstr(b,needle)) { fclose(f); return 1; } } fclose(f); return 0; }
int main(void) {
	int fail=0;
	if(!file_has("src/CAN_Display.c","EBICS_BUILD_VERSION")){puts("T-V1/T-V2 FAIL");fail++;}
	if(!file_has("src/CAN_Display.c","eVD %s")){puts("T-V2 FAIL");fail++;}
	if(file_has("inc/build_version.h","\"0.0408\"")){puts("T-V3 FAIL");fail++;}
	if(!file_has("inc/build_version.h","DEV-NONCANONICAL")){puts("T-V3 FAIL");fail++;}
	puts(fail?"qs_hmi_version_static_host: FAIL":"qs_hmi_version_static_host: PASS"); return fail;
}
