/* hw_ppwlobby.c -- the binary-only Pokemon Platinum WFC lobby library (ppwlobby, 20 entry points the game calls) as stubs. The lobby
 * never connects: PPW_LobbyInitializeAsync reports a session error, PPW_LobbyProcess then reports PPW_LOBBY_STATE_ERROR with
 * PPW_LOBBY_ERROR_SESSION, every other call returns PPW_LOBBY_RESULT_ERROR_STATE. Each stub is logged once per caller (hwlog).
 * SDK-world file. */
#include <ppwlobby/ppw_lobby.h>

#include "hw_log.h"
#include "hw_world.h"

static PPW_LOBBY_STATE s_state = PPW_LOBBY_STATE_NOTINITIALIZED;
static PPW_LOBBY_ERROR s_err = PPW_LOBBY_ERROR_NONE;

/* Soft reset: the lobby session belongs to the previous boot. */
void hw_ppwlobby_reset(void)
{
    s_state = PPW_LOBBY_STATE_NOTINITIALIZED;
    s_err = PPW_LOBBY_ERROR_NONE;
}

PPW_LOBBY_RESULT PPW_LobbyInitializeAsync(const char *gameName, const char *secretKey, DWC_LOBBY_CHANNEL_PREFIX prefix,
                                          const PPW_LobbyCallbacks *lobbyCallbacks, const DWCUserData *dwcUserdata, const u8 *userProfile,
                                          u32 userProfileSize)
{
    HWLOG_CALL("PPW_LobbyInitializeAsync");
    s_state = PPW_LOBBY_STATE_ERROR;
    s_err = PPW_LOBBY_ERROR_SESSION;
    return PPW_LOBBY_RESULT_ERROR_SESSION;
}

PPW_LOBBY_STATE PPW_LobbyProcess()
{
    HWLOG_CALL("PPW_LobbyProcess");
    return s_state;
}

PPW_LOBBY_ERROR PPW_LobbyGetLastError()
{
    HWLOG_CALL("PPW_LobbyGetLastError");
    return s_err;
}

s32 PPW_LobbyToErrorCode(PPW_LOBBY_ERROR err)
{
    HWLOG_CALL("PPW_LobbyToErrorCode");
    return PPW_LOBBY_ERROR_CODE_BASE + (s32)err;
}

PPW_LOBBY_RESULT PPW_LobbyShutdownAsync()
{
    HWLOG_CALL("PPW_LobbyShutdownAsync");
    s_state = PPW_LOBBY_STATE_NOTINITIALIZED;
    return PPW_LOBBY_RESULT_SUCCESS;
}

s32 PPW_LobbyGetMyUserId()
{
    HWLOG_CALL("PPW_LobbyGetMyUserId");
    return PPW_LOBBY_INVALID_USER_ID;
}

PPW_LOBBY_CHANNEL_STATE PPW_LobbyGetSubChannelState()
{
    HWLOG_CALL("PPW_LobbyGetSubChannelState");
    return PPW_LOBBY_CHANNEL_STATE_NONE;
}

PPW_LOBBY_RESULT PPW_LobbyGetChannelDataAsync(s32 userId, PPW_LOBBY_CHANNEL_KIND channelKind, const char *key, void *param)
{
    HWLOG_CALL("PPW_LobbyGetChannelDataAsync");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyGetSchedule(PPW_LobbySchedule *schedule, u32 *scheduleSize)
{
    HWLOG_CALL("PPW_LobbyGetSchedule");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyGetTimeInfo(PPW_LobbyTimeInfo *timeInfo)
{
    HWLOG_CALL("PPW_LobbyGetTimeInfo");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyJoinSubChannelAsync(PPW_LOBBY_CHANNEL_KIND channelKind)
{
    HWLOG_CALL("PPW_LobbyJoinSubChannelAsync");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyLeaveSubChannel()
{
    HWLOG_CALL("PPW_LobbyLeaveSubChannel");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbySendChannelBinaryMessage(PPW_LOBBY_CHANNEL_KIND channelKind, s32 type, const u8 *data, u32 dataSize)
{
    HWLOG_CALL("PPW_LobbySendChannelBinaryMessage");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbySendPlayerBinaryMessage(s32 userId, s32 type, const u8 *data, u32 dataSize)
{
    HWLOG_CALL("PPW_LobbySendPlayerBinaryMessage");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbySetChannelData(s32 userId, PPW_LOBBY_CHANNEL_KIND channelKind, const char *key, const u8 *data, u32 dataSize)
{
    HWLOG_CALL("PPW_LobbySetChannelData");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyStartRecruit(PPW_LobbyRecruitInfo *recruitInfo)
{
    HWLOG_CALL("PPW_LobbyStartRecruit");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyStopRecruit()
{
    HWLOG_CALL("PPW_LobbyStopRecruit");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbySubmitQuestionnaire(s32 answerNo)
{
    HWLOG_CALL("PPW_LobbySubmitQuestionnaire");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyUpdateMyProfile(const u8 *userProfile, u32 userProfileSize, BOOL force)
{
    HWLOG_CALL("PPW_LobbyUpdateMyProfile");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}

PPW_LOBBY_RESULT PPW_LobbyUpdateRecruitInfo(const PPW_LobbyRecruitInfo *recruitInfo)
{
    HWLOG_CALL("PPW_LobbyUpdateRecruitInfo");
    return PPW_LOBBY_RESULT_ERROR_STATE;
}
