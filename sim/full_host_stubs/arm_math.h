#ifndef ARM_MATH_H_
#define ARM_MATH_H_
#include <stdint.h>
#include <math.h>
#include <limits.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef int32_t q31_t;
typedef int64_t q63_t;
static inline q31_t sat32(q63_t x){return x>INT32_MAX?INT32_MAX:(x<INT32_MIN?INT32_MIN:(q31_t)x);}
static inline q31_t __QADD(q31_t a,q31_t b){return sat32((q63_t)a+b);}
static inline q31_t __QSUB(q31_t a,q31_t b){return sat32((q63_t)a-b);}
static inline void arm_clarke_q31(q31_t Ia,q31_t Ib,q31_t *a,q31_t *b){
    q31_t p1=(q31_t)(((q63_t)Ia*0x24F34E8B)>>30);
    q31_t p2=(q31_t)(((q63_t)Ib*0x49E69D16)>>30);
    *a=Ia;*b=__QADD(p1,p2);
}
static inline void arm_park_q31(q31_t a,q31_t b,q31_t *d,q31_t *q,q31_t s,q31_t c){
    q31_t p1=(q31_t)(((q63_t)a*c)>>31),p2=(q31_t)(((q63_t)b*s)>>31);
    q31_t p3=(q31_t)(((q63_t)a*s)>>31),p4=(q31_t)(((q63_t)b*c)>>31);
    *d=__QADD(p1,p2);*q=__QSUB(p4,p3);
}
static inline void arm_inv_park_q31(q31_t d,q31_t q,q31_t *a,q31_t *b,q31_t s,q31_t c){
    q31_t p1=(q31_t)(((q63_t)d*c)>>31),p2=(q31_t)(((q63_t)q*s)>>31);
    q31_t p3=(q31_t)(((q63_t)d*s)>>31),p4=(q31_t)(((q63_t)q*c)>>31);
    *a=__QSUB(p1,p2);*b=__QADD(p4,p3);
}
static inline void arm_sin_cos_q31(q31_t theta,q31_t *s,q31_t *c){
    double a=((double)theta/2147483648.0)*M_PI;
    double sd=sin(a),cd=cos(a);
    long long si=llround(sd*2147483647.0),ci=llround(cd*2147483647.0);
    if(si>INT32_MAX) si=INT32_MAX;
    if(si<INT32_MIN) si=INT32_MIN;
    if(ci>INT32_MAX) ci=INT32_MAX;
    if(ci<INT32_MIN) ci=INT32_MIN;
    *s=(q31_t)si;*c=(q31_t)ci;
}
#endif
