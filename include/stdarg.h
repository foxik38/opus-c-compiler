/* stdarg.h - variable arguments (C23 7.16), provided by occ.
 *
 * The layout matches the System V AMD64 ABI, so va_list values can be
 * passed to and from libc functions such as vprintf. */
#ifndef __OCC_STDARG_H
#define __OCC_STDARG_H

#define __STDC_VERSION_STDARG_H__ 202311L

typedef struct {
  unsigned int gp_offset;
  unsigned int fp_offset;
  void *overflow_arg_area;
  void *reg_save_area;
} __occ_va_elem;

typedef __occ_va_elem va_list[1];
typedef va_list __gnuc_va_list;
#define __GNUC_VA_LIST 1

/* C23: the second argument of va_start is optional and ignored. */
#define va_start(ap, ...) __builtin_va_start(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_copy(dest, src) ((void)(*(dest) = *(src)))
#define va_end(ap) ((void)(ap))

#endif
