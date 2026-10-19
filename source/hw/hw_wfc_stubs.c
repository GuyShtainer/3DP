/* hw_wfc_stubs.c -- bet 1.6a: the 41 symbols the real link left undefined (the private design notes; the 10 crt0/specs ones come from libctru's
 * crt0 and 3dsx.specs, not from here). All are wireless / WFC / voice-chat entry points of binary-only libraries (NitroWiFi SOC/SOCL/WCM, the SDK's WVR, libvct,
 * the ppwlobby glue ov66_): the console has no radio in this port, so each returns the value its callers treat as a clean failure. Every call is
 * logged once per (stub, caller) pair by HWLOG_CALL. Per-symbol rationale and call sites: the private design notes ("The 41 stubs").
 * SDK-world file. DWCi_GsMalloc is in hw_dwc.c (it needs the allocator state). */
#include <string.h>

#include <nitro.h>
#include <nitroWiFi/soc.h>
#include <nitroWiFi/socl.h>
#include <nitroWiFi/wcm.h>
#include <nitro/wvr.h>
#include <ppwlobby/ppw_lobby.h>
#include <vct.h>

#include "hw_log.h"

/* ---- SOC_ (the DWC/GameSpy BSD-socket layer, nonport.c, and the GDS client ov61_0222D498.c) ------------------------------------------ */
/* Failure is a negative errno (nonport.c CheckRcode keeps rcode >= 0 as success). SOC_Socket returns -1: ov61_0222D6A0 tests `== -1` literally. */
int SOC_Socket(int pf, int type, int protocol)
{
    HWLOG_CALL("SOC_Socket");
    return -1;
}

int SOC_Bind(int s, const void *sockAddr)
{
    HWLOG_CALL("SOC_Bind");
    return SOC_ENETDOWN;
}

int SOC_Connect(int s, const void *sockAddr)
{
    HWLOG_CALL("SOC_Connect");
    return SOC_ENETDOWN;   /* not SOC_EINPROGRESS: ov61_0222D8F8 would wait for the connect timeout */
}

int SOC_Listen(int s, int backlog)
{
    HWLOG_CALL("SOC_Listen");
    return SOC_ENETDOWN;
}

int SOC_Accept(int s, void *sockAddr)
{
    HWLOG_CALL("SOC_Accept");
    return SOC_ENETDOWN;
}

int SOC_Close(int s)
{
    HWLOG_CALL("SOC_Close");
    return 0;
}

int SOC_Shutdown(int s, int how)
{
    HWLOG_CALL("SOC_Shutdown");
    return 0;
}

int SOC_Recv(int s, void *buf, int len, int flags)
{
    HWLOG_CALL("SOC_Recv");
    return SOC_ENETDOWN;
}

int SOC_RecvFrom(int s, void *buf, int len, int flags, void *sockFrom)
{
    HWLOG_CALL("SOC_RecvFrom");
    return SOC_ENETDOWN;
}

int SOC_Send(int s, const void *buf, int len, int flags)
{
    HWLOG_CALL("SOC_Send");
    return SOC_ENETDOWN;
}

int SOC_SendTo(int s, const void *buf, int len, int flags, const void *sockTo)
{
    HWLOG_CALL("SOC_SendTo");
    return SOC_ENETDOWN;
}

int SOC_Fcntl(int s, int cmd, int mode)
{
    HWLOG_CALL("SOC_Fcntl");
    return SOC_ENETDOWN;
}

int SOC_GetSockName(int s, void *sockAddr)
{
    HWLOG_CALL("SOC_GetSockName");
    return SOC_ENETDOWN;
}

int SOC_Poll(SOCPollFD fds[], unsigned nfds, OSTick timeout)
{
    HWLOG_CALL("SOC_Poll");
    return SOC_ENETDOWN;
}

SOCHostEnt *SOC_GetHostByName(const char *name)
{
    HWLOG_CALL("SOC_GetHostByName");
    return NULL;   /* ov61_0222D638 returns FALSE for NULL */
}

int SOC_InetAtoN(const char *cp, SOCInAddr *inp)
{
    HWLOG_CALL("SOC_InetAtoN");
    return FALSE;   /* nonport.c inet_addr: FALSE -> INADDR_NONE */
}

char *SOC_InetNtoA(SOCInAddr in)
{
    static char s_empty[1] = "";

    HWLOG_CALL("SOC_InetNtoA");
    return s_empty;
}

/* Pure: split a 32-bit address into four bytes, most significant first (the SOC_GetAddr inline feeds it SOCL_GetHostID() = 0 here). */
void SOC_U32to4U8(u32 adr_u32, u8 *adr_4u8)
{
    HWLOG_CALL("SOC_U32to4U8");
    adr_4u8[0] = (u8)(adr_u32 >> 24);
    adr_4u8[1] = (u8)(adr_u32 >> 16);
    adr_4u8[2] = (u8)(adr_u32 >> 8);
    adr_4u8[3] = (u8)adr_u32;
}

SOCLInAddr SOCL_GetHostID(void)
{
    SOCLInAddr none;

    HWLOG_CALL("SOCL_GetHostID");
    memset(&none, 0, sizeof none);   /* 0.0.0.0: no address */
    return none;
}

/* ---- WCM / WVR (the wireless driver, game side: unk_02033200.c, wireless_manager.c, gts networking.c, battle tower http.c) ------------- */
s32 WCM_GetPhase(void)
{
    HWLOG_CALL("WCM_GetPhase");
    return WCM_PHASE_NULL;   /* != WCM_PHASE_DCF: networking.c:782 / http.c:421 turn a network result into "not in DCF phase", then DWC_ClearError() */
}

/* WirelessDriver_Init (unk_02033200.c:207) calls OS_Terminate() unless this returns OPERATING, and WirelessDriver_InitCallback calls OS_Terminate() on any
   result but SUCCESS. So the driver "starts" instantly and successfully (callback inline); everything above it (WM_*, DWC) then finds no network. */
WVRResult WVR_StartUpAsync(GXVRamARM7 vram, WVRCallbackFunc callback, void *arg)
{
    HWLOG_CALL("WVR_StartUpAsync");
    if (callback != NULL) {
        callback(arg, WVR_RESULT_SUCCESS);
    }
    return WVR_RESULT_OPERATING;
}

WVRResult WVR_TerminateAsync(WVRCallbackFunc callback, void *arg)
{
    HWLOG_CALL("WVR_TerminateAsync");
    if (callback != NULL) {
        callback(arg, WVR_RESULT_SUCCESS);
    }
    return WVR_RESULT_OPERATING;
}

/* ---- ov66_ (the lobby overlay's glue: maps a PPW_LOBBY_RESULT to a PPW_LOBBY_ERROR, ov66_022324F0.c:263) -------------------------------- */
PPW_LOBBY_ERROR ov66_02237134(PPW_LOBBY_RESULT result)
{
    HWLOG_CALL("ov66_02237134");
    return PPW_LOBBY_ERROR_SESSION;   /* hw_ppwlobby.c reports the same error: the lobby never gets a session */
}

/* ---- VCT_ (libvct voice chat, nintendo_wfc/voice_chat.c; the lobby/chat feature cannot start without a connection) -------------------- */
BOOL VCT_Init(VCTConfig *config)
{
    HWLOG_CALL("VCT_Init");
    return FALSE;
}

void VCT_Cleanup(void)
{
    HWLOG_CALL("VCT_Cleanup");
}

void VCT_Main(void)
{
    HWLOG_CALL("VCT_Main");
}

BOOL VCT_HandleData(u8 aid, u8 *buffer, int size)
{
    HWLOG_CALL("VCT_HandleData");
    return FALSE;   /* not consumed */
}

int VCT_Request(VCTSession *session, VCTRequestCode request)
{
    HWLOG_CALL("VCT_Request");
    return VCT_ERROR_SEND_FAIL;
}

int VCT_Response(VCTSession *session, VCTResponseCode response)
{
    HWLOG_CALL("VCT_Response");
    return VCT_ERROR_SEND_FAIL;
}

int VCT_AddConferenceClient(u8 aid)
{
    HWLOG_CALL("VCT_AddConferenceClient");
    return VCT_ERROR_EXHAUST_CLIENTS;
}

VCTSession *VCT_CreateSession(u8 aid)
{
    HWLOG_CALL("VCT_CreateSession");
    return NULL;
}

BOOL VCT_DeleteSession(VCTSession *session)
{
    HWLOG_CALL("VCT_DeleteSession");
    return FALSE;
}

BOOL VCT_StartStreaming(VCTSession *session)
{
    HWLOG_CALL("VCT_StartStreaming");
    return FALSE;
}

void VCT_StopStreaming(VCTSession *session)
{
    HWLOG_CALL("VCT_StopStreaming");
}

BOOL VCT_SendAudio(void *audio_data, u32 length)
{
    HWLOG_CALL("VCT_SendAudio");
    return FALSE;
}

BOOL VCT_ReceiveAudio(void *audio_data, u32 length, u32 *outAIDBitmap)
{
    HWLOG_CALL("VCT_ReceiveAudio");
    if (outAIDBitmap != NULL) {
        *outAIDBitmap = 0;
    }
    return FALSE;
}

BOOL VCT_SetCodec(VCTCodec codec)
{
    HWLOG_CALL("VCT_SetCodec");
    return FALSE;
}

void VCT_EnableVAD(BOOL flag)
{
    HWLOG_CALL("VCT_EnableVAD");
}

void VCT_EnableEchoCancel(BOOL flag)
{
    HWLOG_CALL("VCT_EnableEchoCancel");
}

void VCT_GetVADInfo(VCTVADInfo *outInfo)
{
    HWLOG_CALL("VCT_GetVADInfo");
    if (outInfo != NULL) {
        memset(outInfo, 0, sizeof *outInfo);   /* nobody is speaking */
    }
}
