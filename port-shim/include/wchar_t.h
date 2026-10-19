/* port-shim: MSL <wchar_t.h> (only NitroSystem g2di_SplitChar.c uses it). It supplies wchar_t, which the
   tree treats as 16-bit (-fshort-wchar); <stddef.h> provides it under that flag. */
#include <stddef.h>
