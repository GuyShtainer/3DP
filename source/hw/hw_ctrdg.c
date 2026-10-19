/* hw_ctrdg.c -- replaces libraries/ctrdg/src/ctrdg_proc.c (the GBA-cartridge PXI service, 8 symbols). The 3DS has no GBA slot: the
 * module-info page says "no cartridge" (moduleID 0xffff, written by hw_sys.c), so CTRDG_IsExisting / CTRDG_IsPulledOut / the flash
 * reads the game makes all see an empty slot. SDK-world file. */
#include <assert.h>

#include <nitro.h>

#include "hw_log.h"

extern void CTRDGi_InitCommon(void);

CTRDGPulledOutCallback CTRDG_UserCallback = NULL;

void CTRDG_Init(void)
{
    static BOOL done;
    CTRDGModuleInfo *mi = (CTRDGModuleInfo *)HW_CTRDG_MODULE_INFO_BUF;

    if (done) {
        return;
    }
    done = TRUE;
    CTRDGi_InitCommon();
    CTRDGi_InitModuleInfo();
    assert(mi->moduleID.raw == 0xffffu);
    CTRDG_UserCallback = NULL;
    HWLOG("CTRDG_Init: no GBA cartridge on the 3DS");
}

void CTRDGi_InitModuleInfo(void)
{
    CTRDGModuleInfo *mi = (CTRDGModuleInfo *)HW_CTRDG_MODULE_INFO_BUF;

    assert(mi != NULL);
    mi->moduleID.raw = 0xffffu;
    mi->isAgbCartridge = 0;
    mi->detectPullOut = 0;
    mi->gameCode = 0;
    mi->makerCode = 0;
    *(u8 *)HW_IS_CTRDG_EXIST = 0;
}

void CTRDG_SetPulledOutCallback(CTRDGPulledOutCallback callback)
{
    CTRDG_UserCallback = callback;
}

void CTRDG_TerminateForPulledOut(void)
{
    HWLOG("CTRDG_TerminateForPulledOut: cannot happen without a cartridge");
    OS_Terminate();
}

void CTRDG_SendToARM7(void *arg)
{
    (void)arg;
    HWLOG("CTRDG_SendToARM7: no ARM7 cartridge service");
}

void CTRDG_CheckPulledOut(void)
{
    assert(CTRDG_IsPulledOut() == FALSE);
}

void CTRDG_SetPhiClock(CTRDGPhiClock clock)
{
    assert((u32)clock <= CTRDG_PHI_CLOCK_16MHZ);
    MIi_SetPhiClock((MIiPhiClock)clock);   /* the register write the DS made; no ARM7 acknowledge to wait for */
}
