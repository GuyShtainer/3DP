/* port-shim: MSL <extras.h>, pulled in by NitroDWC gs/nonport.h (_NITRO branch) in every TU that reaches
   it. MSL's extras.h declared the non-ISO string/number helpers (strcasecmp/stricmp/strnicmp, itoa,
   strlwr/strupr, strdup ...). A grep of src/, lib/, include/ and the NitroDWC/NitroSystem/NitroSDK trees
   finds NO caller of any of them in the compiled sources (they live in the prebuilt DWC/GameSpy
   libraries), so nothing is declared here beyond the standard headers MSL's file pulled in (time.h is needed for
   the time_t in gs/nonport.h).
   If a link error ever asks for one, provide it in the hardware layer and declare it here. */
#ifndef PORT_SHIM_EXTRAS_H
#define PORT_SHIM_EXTRAS_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   /* gs/nonport.h declares `time_t time(time_t *)` right after including this file */
#endif
