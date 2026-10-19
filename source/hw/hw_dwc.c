/* hw_dwc.c -- binary-only Nintendo WFC library (DWC) replaced by stubs: every one of the 82 entry points the game links returns its
 * documented "no connection" result. DWC_Init succeeds (the game's boot expects DWC_INIT_RESULT_NOERROR); DWC_ConnectInetAsync puts
 * the connection state machine into DWC_CONNECTINET_STATE_ERROR so a caller that polls DWC_GetInetStatus reaches a terminal state
 * instead of waiting forever. Each stub is logged once per CALLER (hwlog), later calls only count. SDK-world file.
 * Generated once from the prototypes in port/subprojects/NitroDWC-2.2.30008/include/base; maintained by hand from here. */
#include <string.h>

#include <dwc.h>

#include "hw_log.h"
#include "hw_world.h"

static DWCAllocEx s_alloc;
static DWCFreeEx s_free;
static DWCInetResult s_inet = DWC_CONNECTINET_STATE_NOT_INITIALIZED;
static DWCError s_err = DWC_ERROR_NONE;

/* Soft reset: the allocator pointers point into the previous boot's heap (game RAM is zeroed and re-run), and the connection state belongs to it. */
void hw_dwc_reset(void)
{
    s_alloc = NULL;
    s_free = NULL;
    s_inet = DWC_CONNECTINET_STATE_NOT_INITIALIZED;
    s_err = DWC_ERROR_NONE;
}

u8 DWC_AddMatchKeyString(u8 keyID, const char * keyString, const char * valueSrc)
{
    HWLOG_CALL("DWC_AddMatchKeyString");
    return 0;
}

void * DWC_Alloc(DWCAllocType name, u32 size)
{
    HWLOG_CALL("DWC_Alloc");
    return s_alloc ? s_alloc(name, size, 4) : NULL;
}

/* GameSpy's nonport.c / gsiMemory allocator (DWCi_GsMalloc is declared in dwc_memfunc.h): same pool as DWC_Alloc, NULL until DWC_SetMemFunc ran. */
void * DWCi_GsMalloc(size_t size)
{
    HWLOG_CALL("DWCi_GsMalloc");
    return s_alloc ? s_alloc(DWC_ALLOCTYPE_BASE, (u32)size, 4) : NULL;
}

BOOL DWC_CancelMatching(void)
{
    HWLOG_CALL("DWC_CancelMatching");
    return FALSE;
}

BOOL DWC_CheckDirtyFlag(const DWCAccUserData * userdata)
{
    HWLOG_CALL("DWC_CheckDirtyFlag");
    return FALSE;
}

BOOL DWC_CheckFriendKey(const DWCAccUserData * userdata, u64 friend_key)
{
    HWLOG_CALL("DWC_CheckFriendKey");
    return FALSE;
}

BOOL DWC_CheckHasProfile(const DWCAccUserData * userdata)
{
    HWLOG_CALL("DWC_CheckHasProfile");
    return FALSE;
}

/* TRUE once the connection attempt has finished, one way or the other: ERROR, FATAL_ERROR or CONNECTED. The game polls it after
   DWC_ConnectInetAsync (wfc_init.c:423) and reads DWC_GetInetStatus only when it is TRUE, so a constant FALSE would wait forever. Before
   DWC_ConnectInetAsync (state NOT_INITIALIZED) it is FALSE, which is what the game's "not connected yet" branch expects. */
BOOL DWC_CheckInet(void)
{
    HWLOG_CALL("DWC_CheckInet");
    return (s_inet == DWC_CONNECTINET_STATE_ERROR || s_inet == DWC_CONNECTINET_STATE_FATAL_ERROR ||
            s_inet == DWC_CONNECTINET_STATE_CONNECTED) ? TRUE : FALSE;
}

BOOL DWC_CheckUserData(const DWCAccUserData * userdata)
{
    HWLOG_CALL("DWC_CheckUserData");
    return FALSE;
}

BOOL DWC_CheckValidConsole(const DWCAccUserData * userdata)
{
    HWLOG_CALL("DWC_CheckValidConsole");
    return FALSE;
}

void DWC_CleanupInet(void)
{
    s_inet = DWC_CONNECTINET_STATE_NOT_INITIALIZED;
    HWLOG_CALL("DWC_CleanupInet");
}

BOOL DWC_CleanupInetAsync(void)
{
    s_inet = DWC_CONNECTINET_STATE_NOT_INITIALIZED;
    HWLOG_CALL("DWC_CleanupInetAsync");
    return TRUE;
}

void DWC_ClearDirtyFlag(DWCAccUserData * userdata)
{
    HWLOG_CALL("DWC_ClearDirtyFlag");
}

void DWC_ClearError(void)
{
    HWLOG_CALL("DWC_ClearError");
    s_err = DWC_ERROR_NONE;
}

int DWC_CloseAllConnectionsHard(void)
{
    HWLOG_CALL("DWC_CloseAllConnectionsHard");
    return 0;
}

int DWC_CloseConnectionHardBitmap(u32 * bitmap)
{
    HWLOG_CALL("DWC_CloseConnectionHardBitmap");
    return 0;
}

void DWC_ConnectInetAsync(void)
{
    s_inet = DWC_CONNECTINET_STATE_ERROR;   /* a caller that polls DWC_GetInetStatus must reach a terminal state */
    s_err = DWC_ERROR_AC_ANY;
    HWLOG_CALL("DWC_ConnectInetAsync");
}

BOOL DWC_ConnectToAnybodyAsync(u8 numEntry, const char * addFilter, DWCMatchedCallback matchedCallback, void * matchedParam, DWCEvalPlayerCallback evalCallback, void * evalParam)
{
    HWLOG_CALL("DWC_ConnectToAnybodyAsync");
    return FALSE;
}

BOOL DWC_ConnectToGameServerAsync(int serverIndex, DWCMatchedSCCallback matchedCallback, void * matchedParam, DWCNewClientCallback newClientCallback, void * newClientParam)
{
    HWLOG_CALL("DWC_ConnectToGameServerAsync");
    return FALSE;
}

void DWC_CreateExchangeToken(const DWCAccUserData * userdata, DWCAccFriendData * token)
{
    HWLOG_CALL("DWC_CreateExchangeToken");
    if (token) {
        memset(token, 0, sizeof *token);
    }
}

u64 DWC_CreateFriendKey(const DWCAccUserData * userdata)
{
    HWLOG_CALL("DWC_CreateFriendKey");
    return 0;
}

void DWC_CreateFriendKeyToken(DWCAccFriendData * token, u64 friend_key)
{
    HWLOG_CALL("DWC_CreateFriendKeyToken");
    if (token) {
        memset(token, 0, sizeof *token);
    }
}

void DWC_CreateUserData(DWCAccUserData * userdata, u32 gamecode)
{
    HWLOG_CALL("DWC_CreateUserData");
    if (userdata) {
        memset(userdata, 0, sizeof *userdata);
    }
}

void DWC_Free(DWCAllocType name, void * ptr, u32 size)
{
    HWLOG_CALL("DWC_Free");
    if (s_free) {
        s_free(name, ptr, size);
    }
}

u32 DWC_GetAIDBitmap(void)
{
    HWLOG_CALL("DWC_GetAIDBitmap");
    return 0;
}

BOOL DWC_GetApInfo(DWCApInfo * apinfo)
{
    HWLOG_CALL("DWC_GetApInfo");
    if (apinfo) {
        memset(apinfo, 0, sizeof *apinfo);
    }
    return FALSE;
}

BOOL DWC_GetDateTime(RTCDate * date, RTCTime * time)
{
    HWLOG_CALL("DWC_GetDateTime");
    return FALSE;
}

int DWC_GetFriendDataType(const DWCAccFriendData * frienddata)
{
    HWLOG_CALL("DWC_GetFriendDataType");
    return DWC_FRIENDDATA_NODATA;
}

u64 DWC_GetFriendKey(const DWCAccFriendData * frienddata)
{
    HWLOG_CALL("DWC_GetFriendKey");
    return 0;
}

u8 DWC_GetFriendStatusData(const DWCFriendData * friendData, char * statusData, int * size)
{
    HWLOG_CALL("DWC_GetFriendStatusData");
    return DWC_STATUS_OFFLINE;
}

int DWC_GetGHTTPData(const char * url, DWCGHTTPCompletedCallback completedCallback, void * param)
{
    HWLOG_CALL("DWC_GetGHTTPData");
    return 0;
}

int DWC_GetGsProfileId(const DWCAccUserData * userdata, const DWCAccFriendData * frienddata)
{
    HWLOG_CALL("DWC_GetGsProfileId");
    return 0;
}

DWCInetResult DWC_GetInetStatus(void)
{
    HWLOG_CALL("DWC_GetInetStatus");
    return s_inet;
}

DWCError DWC_GetLastError(int * errorCode)
{
    HWLOG_CALL("DWC_GetLastError");
    if (errorCode) {
        *errorCode = s_err == DWC_ERROR_NONE ? 0 : DWC_ECODE_SEQ_LOGIN + DWC_ECODE_TYPE_NETWORK;
    }
    return s_err;
}

DWCError DWC_GetLastErrorEx(int * errorCode, DWCErrorType * errorType)
{
    HWLOG_CALL("DWC_GetLastErrorEx");
    if (errorCode) {
        *errorCode = s_err == DWC_ERROR_NONE ? 0 : DWC_ECODE_SEQ_LOGIN + DWC_ECODE_TYPE_NETWORK;
    }
    if (errorType) {
        *errorType = s_err == DWC_ERROR_NONE ? DWC_ETYPE_NO_ERROR : DWC_ETYPE_SHOW_ERROR;
    }
    return s_err;
}

WMLinkLevel DWC_GetLinkLevel(void)
{
    HWLOG_CALL("DWC_GetLinkLevel");
    return WM_LINK_LEVEL_0;
}

u8 DWC_GetMyAID(void)
{
    HWLOG_CALL("DWC_GetMyAID");
    return 0;
}

int DWC_GetNumConnectionHost(void)
{
    HWLOG_CALL("DWC_GetNumConnectionHost");
    return 0;
}

int DWC_Init(void * work)
{
    HWLOG_CALL("DWC_Init");
    return DWC_INIT_RESULT_NOERROR;
}

void DWC_InitFriendsMatch(DWCFriendsMatchControl * dwccnt, DWCUserData * userdata, int productID, const char * gameName, const char * secretKey, int sendBufSize, int recvBufSize, DWCFriendData friendList[], int friendListLen)
{
    HWLOG_CALL("DWC_InitFriendsMatch");
}

BOOL DWC_InitGHTTP(const char * gamename)
{
    HWLOG_CALL("DWC_InitGHTTP");
    return FALSE;
}

void DWC_InitInetEx(DWCInetControl * inetCntl, u16 dmaNo, u16 powerMode, u16 ssl_priority)
{
    s_inet = DWC_CONNECTINET_STATE_IDLE;
    HWLOG_CALL("DWC_InitInetEx");
}

BOOL DWC_IsBuddyFriendData(const DWCAccFriendData * frienddata)
{
    HWLOG_CALL("DWC_IsBuddyFriendData");
    return FALSE;
}

BOOL DWC_IsEqualFriendData(const DWCAccFriendData * frienddata1, const DWCAccFriendData * frienddata2)
{
    HWLOG_CALL("DWC_IsEqualFriendData");
    return FALSE;
}

BOOL DWC_IsSendableReliable(u8 aid)
{
    HWLOG_CALL("DWC_IsSendableReliable");
    return FALSE;
}

BOOL DWC_IsValidAID(u8 aid)
{
    HWLOG_CALL("DWC_IsValidAID");
    return FALSE;
}

BOOL DWC_IsValidFriendData(const DWCAccFriendData * frienddata)
{
    HWLOG_CALL("DWC_IsValidFriendData");
    return FALSE;
}

BOOL DWC_LoginAsync(const u16 * ingamesn, const char * reserved, DWCLoginCallback callback, void * param)
{
    HWLOG_CALL("DWC_LoginAsync");
    return FALSE;
}

void DWC_NASLoginAbort(void)
{
    HWLOG_CALL("DWC_NASLoginAbort");
}

BOOL DWC_NASLoginAsync(void)
{
    HWLOG_CALL("DWC_NASLoginAsync");
    return FALSE;
}

DWCNasLoginState DWC_NASLoginProcess(void)
{
    HWLOG_CALL("DWC_NASLoginProcess");
    return DWC_NASLOGIN_STATE_IDLE;
}

BOOL DWC_NdCancelAsync(void)
{
    HWLOG_CALL("DWC_NdCancelAsync");
    return FALSE;
}

BOOL DWC_NdCleanupAsync(DWCNdCleanupCallback callback)
{
    HWLOG_CALL("DWC_NdCleanupAsync");
    return FALSE;
}

BOOL DWC_NdGetFileAsync(DWCNdFileInfo * fileinfo, char * buf, unsigned int size)
{
    HWLOG_CALL("DWC_NdGetFileAsync");
    return FALSE;
}

BOOL DWC_NdGetFileListAsync(DWCNdFileInfo * filelist, unsigned int offset, unsigned int num)
{
    HWLOG_CALL("DWC_NdGetFileListAsync");
    return FALSE;
}

BOOL DWC_NdGetFileListNumAsync(int * entrynum)
{
    HWLOG_CALL("DWC_NdGetFileListNumAsync");
    return FALSE;
}

BOOL DWC_NdGetProgress(u32 * received, u32 * contentlen)
{
    HWLOG_CALL("DWC_NdGetProgress");
    return FALSE;
}

BOOL DWC_NdInitAsync(DWCNdCallback callback, char * gamecd, char * passwd)
{
    HWLOG_CALL("DWC_NdInitAsync");
    return FALSE;
}

BOOL DWC_NdSetAttr(const char * attr1, const char * attr2, const char * attr3)
{
    HWLOG_CALL("DWC_NdSetAttr");
    return FALSE;
}

void DWC_ProcessFriendsMatch(void)
{
    HWLOG_CALL("DWC_ProcessFriendsMatch");
}

BOOL DWC_ProcessGHTTP(void)
{
    HWLOG_CALL("DWC_ProcessGHTTP");
    return FALSE;
}

void DWC_ProcessInet(void)
{
    HWLOG_CALL("DWC_ProcessInet");
}

BOOL DWC_SVLGetTokenAsync(char * svl, DWCSvlResult * result)
{
    HWLOG_CALL("DWC_SVLGetTokenAsync");
    return FALSE;
}

DWCSvlState DWC_SVLProcess(void)
{
    HWLOG_CALL("DWC_SVLProcess");
    return DWC_SVL_STATE_IDLE;
}

u32 DWC_SendReliableBitmap(u32 bitmap, const void * buffer, int size)
{
    HWLOG_CALL("DWC_SendReliableBitmap");
    return 0;
}

void DWC_SetAuthServer(DWCInetAuthType type)
{
    HWLOG_CALL("DWC_SetAuthServer");
}

BOOL DWC_SetBuddyFriendCallback(DWCBuddyFriendCallback callback, void * param)
{
    HWLOG_CALL("DWC_SetBuddyFriendCallback");
    return FALSE;
}

BOOL DWC_SetConnectionClosedCallback(DWCConnectionClosedCallback callback, void * param)
{
    HWLOG_CALL("DWC_SetConnectionClosedCallback");
    return FALSE;
}

int DWC_SetMatchingOption(DWCMatchOptType opttype, const void * optval, int optlen)
{
    HWLOG_CALL("DWC_SetMatchingOption");
    return 0;
}

void DWC_SetMemFunc(DWCAllocEx alloc, DWCFreeEx free)
{
    HWLOG_CALL("DWC_SetMemFunc");
    s_alloc = alloc;
    s_free = free;
}

BOOL DWC_SetOwnStatusData(const char * statusData, u32 size)
{
    HWLOG_CALL("DWC_SetOwnStatusData");
    return FALSE;
}

BOOL DWC_SetRecvBuffer(u8 aid, void * recvBuffer, int size)
{
    HWLOG_CALL("DWC_SetRecvBuffer");
    return FALSE;
}

BOOL DWC_SetRecvTimeoutTime(u8 aid, u32 time)
{
    HWLOG_CALL("DWC_SetRecvTimeoutTime");
    return FALSE;
}

BOOL DWC_SetUserRecvCallback(DWCUserRecvCallback callback)
{
    HWLOG_CALL("DWC_SetUserRecvCallback");
    return FALSE;
}

BOOL DWC_SetUserRecvTimeoutCallback(DWCUserRecvTimeoutCallback callback)
{
    HWLOG_CALL("DWC_SetUserRecvTimeoutCallback");
    return FALSE;
}

BOOL DWC_SetUserSendCallback(DWCUserSendCallback callback)
{
    HWLOG_CALL("DWC_SetUserSendCallback");
    return FALSE;
}

BOOL DWC_SetupGameServer(u8 maxEntry, DWCMatchedSCCallback matchedCallback, void * matchedParam, DWCNewClientCallback newClientCallback, void * newClientParam)
{
    HWLOG_CALL("DWC_SetupGameServer");
    return FALSE;
}

void DWC_ShutdownFriendsMatch(void)
{
    HWLOG_CALL("DWC_ShutdownFriendsMatch");
}

BOOL DWC_ShutdownGHTTP(void)
{
    HWLOG_CALL("DWC_ShutdownGHTTP");
    return FALSE;
}

int DWC_StartUtility(void * work, int language, int param)
{
    HWLOG_CALL("DWC_StartUtility");
    return 0;
}

BOOL DWC_StopSCMatchingAsync(DWCStopSCCallback callback, void * param)
{
    HWLOG_CALL("DWC_StopSCMatchingAsync");
    return FALSE;
}

BOOL DWC_UpdateConnection(void)
{
    HWLOG_CALL("DWC_UpdateConnection");
    return FALSE;
}

BOOL DWC_UpdateServersAsync(const char * playerName, DWCUpdateServersCallback updateCallback, void * updateParam, DWCFriendStatusCallback statusCallback, void * statusParam, DWCDeleteFriendListCallback deleteCallback, void * deleteParam)
{
    HWLOG_CALL("DWC_UpdateServersAsync");
    return FALSE;
}
