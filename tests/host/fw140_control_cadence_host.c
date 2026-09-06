/* FW-140: one conditioned cadence for motor demand; raw cadence remains diagnostic. */
#include "cadence_filter.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAIN_C_PATH
#error MAIN_C_PATH required
#endif
#define S2(x) #x
#define S(x) S2(x)

static char *read_all(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1U);
    if (!b) { fclose(f); return NULL; }
    size_t g = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[g] = 0;
    return b;
}

static unsigned count_text(const char *s, const char *needle)
{
    unsigned n = 0U;
    size_t len = strlen(needle);
    while ((s = strstr(s, needle)) != NULL) { n++; s += len; }
    return n;
}

int main(void)
{
    cadence_filter_reset();
    CHECK(cadence_filter_get() == 0U, "reset cadence is zero");
    CHECK(cadence_filter_get_x8() == 0U, "reset x8 state is zero");

    /* First real measurement must seed directly; no fake 7.5 rpm control value from a 60 rpm
     * first sample. This is what makes the filter safe to put in the production control path. */
    CHECK(cadence_filter_update(60U) == 60U, "first 60 rpm sample seeds at 60 rpm");
    CHECK(cadence_filter_get_x8() == 480U, "first sample seeds exact Q3 state");

    CHECK(cadence_filter_update(80U) == 62U, "80 rpm local-speed peak is smoothed");
    CHECK(cadence_filter_update(40U) == 59U, "40 rpm local-speed trough is smoothed");

    cadence_filter_reset();
    for (unsigned i = 0; i < 64U; i++) {
        uint8_t raw = (i & 1U) ? 80U : 40U;
        uint8_t filtered = cadence_filter_update(raw);
        CHECK(filtered >= 40U && filtered <= 80U, "IIR never leaves raw physical envelope");
    }
    CHECK(cadence_filter_get() >= 55U && cadence_filter_get() <= 65U,
        "alternating 40/80 local speed converges near 60 rpm mean");

    char *main_c = read_all(S(MAIN_C_PATH));
    CHECK(main_c != NULL, "main.c readable");
    if (main_c) {
        CHECK(strstr(main_c, "cadence_filter_update(MS.cadence)") != NULL,
            "real cadence measurements feed the production conditioner");
        CHECK(count_text(main_c, ".cadence_rpm = cadence_filter_get()") >= 2U,
            "rider demand and ride dynamics use the same conditioned cadence");
        /* Raw cadence is intentionally still copied into diagnostic recorder structs. What
         * matters for control ownership is that the two production initializers above use the
         * same cadence_filter_get() source. */
        CHECK(strstr(main_c, "uint16_cadence_filtered") == NULL,
            "old ad-hoc cadence IIR state removed from main.c");
        CHECK(strstr(main_c, "pc->rpm") != NULL,
            "raw cadence remains explicitly available in diagnostics");
        free(main_c);
    }

    if (host_test_failures == 0) {
        puts("FW-140 control cadence test passed.");
        return 0;
    }
    return 1;
}
