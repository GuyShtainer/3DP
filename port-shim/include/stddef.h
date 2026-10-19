/* port-shim: wrapper over the compiler's <stddef.h> that keeps the MSL-faithful NULL (plain 0).
   GCC's stddef.h #undefs and redefines NULL as ((void *)0) on its first inclusion, which would undo
   the definition in nitro/types.h whenever a libc header is reached first. See
   the private design notes ("NULL"). Hardware-layer files that include libctru must
   not put this directory on their include path. */
/* No include guard on purpose: newlib re-includes <stddef.h> with __need_wint_t/__need_size_t/... set,
   and each of those inclusions must reach the real header. */
#include_next <stddef.h>
#ifndef __cplusplus
#undef NULL
#define NULL 0
#endif
