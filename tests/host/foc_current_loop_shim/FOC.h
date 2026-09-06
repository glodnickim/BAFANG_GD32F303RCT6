#ifndef FOC_H_
#define FOC_H_
#include "foc_current_loop.h"
#define _U_MAX 1920L
#define FOC_AW_SAT_ERROR_MAX 4096L
int32_t PI_control(PI_control_t *PI_c);
#endif
