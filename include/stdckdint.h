/* stdckdint.h - checked integer arithmetic (C23 7.20), provided by occ.
 * ckd_add(&r, a, b) stores a + b into r and returns true if the
 * mathematically exact result did not fit. */
#ifndef __OCC_STDCKDINT_H
#define __OCC_STDCKDINT_H

#define __STDC_VERSION_STDCKDINT_H__ 202311L

#define ckd_add(result, a, b) __builtin_add_overflow((a), (b), (result))
#define ckd_sub(result, a, b) __builtin_sub_overflow((a), (b), (result))
#define ckd_mul(result, a, b) __builtin_mul_overflow((a), (b), (result))

#endif
