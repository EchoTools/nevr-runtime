// CMatSymHashes.h
// CMatSym_Hash intermediate values (Stage 1 of SNS hashing)
//
// This header contains the INTERMEDIATE hash values from CMatSym_Hash
// before the SMatSymData_HashA finalizer is applied.
//
// Algorithm: CRC64-ECMA-182 @ 0x140107f80 with table @ 0x1416d0120
// Source: evr-reconstruction/src/NRadEngine/Social/SNSHash.cpp

#pragma once
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace EchoVR {
namespace CMatSym {

// CMatSym_Hash intermediate values (before SMatSymData_HashA)
// 89 message types

constexpr uint64_t S_N_S_ADD_TOURNAMENT = 0xfd1eb32311ca7a35;  // "SNSAddTournament"
constexpr uint64_t S_N_S_CHANNEL_INFO_REQUEST = 0xcd0f7ea820370dd6;  // "SNSChannelInfoRequest"
constexpr uint64_t S_N_S_CHANNEL_INFO_RESPONSE = 0xbdc9d5a994c75ca2;  // "SNSChannelInfoResponse"
constexpr uint64_t S_N_S_CONFIG_FAILUREV2 = 0xbd51368f9b2f1ca4;  // "SNSConfigFailurev2"
constexpr uint64_t S_N_S_CONFIG_REQUESTV2 = 0xfe46e4415441768c;  // "SNSConfigRequestv2"
constexpr uint64_t S_N_S_CONFIG_SUCCESSV2 = 0xd8e5682c53afee94;  // "SNSConfigSuccessv2"
constexpr uint64_t S_N_S_DOCUMENT_FAILURE = 0x398420f85c618ab2;  // "SNSDocumentFailure"
constexpr uint64_t S_N_S_DOCUMENT_REQUESTV2 = 0xbe4b45a5a595847f;  // "SNSDocumentRequestv2"
constexpr uint64_t S_N_S_DOCUMENT_SUCCESS = 0x24f76c84f2a445bb;  // "SNSDocumentSuccess"
constexpr uint64_t S_N_S_EARLY_QUIT_CONFIG = 0x65541b790d55818f;  // "SNSEarlyQuitConfig"
constexpr uint64_t S_N_S_EARLY_QUIT_FEATURE_FLAGS = 0x31ebc9f016407384;  // "SNSEarlyQuitFeatureFlags"
constexpr uint64_t S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION = 0x1cc1af14b13f3d25;  // "SNSEarlyQuitUpdateNotification"
constexpr uint64_t S_N_S_LEADERBOARD_REQUESTV2 = 0xd2f2508018dad043;  // "SNSLeaderboardRequestv2"
constexpr uint64_t S_N_S_LEADERBOARD_RESPONSE = 0xeafcf7c8d68bc139;  // "SNSLeaderboardResponse"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2 = 0xe2b6499c83b2a1ad;  // "SNSLobbyAcceptPlayersFailurev2"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2 = 0x8702173f4a3a539d;  // "SNSLobbyAcceptPlayersSuccessv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2 = 0xc4d0df5f1a85bbae;  // "SNSLobbyAddEntrantAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2 = 0x73cb2775074f0558;  // "SNSLobbyAddEntrantRejectedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4 = 0x562cd7cf52c76db9;  // "SNSLobbyAddEntrantRequestv4"
constexpr uint64_t S_N_S_LOBBY_CHAT_ENTRY = 0x2cdba6ba204222d5;  // "SNSLobbyChatEntry"
constexpr uint64_t S_N_S_LOBBY_DIRECTORY_JSON = 0xb417ecceac72ee1d;  // "SNSLobbyDirectoryJson"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ACCEPTED = 0x92e23d69dc734dd2;  // "SNSLobbyEntrantAccepted"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ADDED = 0xe2a25fac7bb31f7c;  // "SNSLobbyEntrantAdded"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_DATA_CHANGED = 0x1c7dea393249fd5a;  // "SNSLobbyEntrantDataChanged"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_REMOVED = 0x382a879abdc54abb;  // "SNSLobbyEntrantRemoved"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_STATEV2 = 0x407e8017512fc232;  // "SNSLobbyEntrantStatev2"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_TEAM_CHANGED = 0x73d4fa737556ccad;  // "SNSLobbyEntrantTeamChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECT_FAILED = 0xbdfa2f975f40572c;  // "SNSLobbyHostConnectFailed"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECTED = 0xe830f4f24592c2aa;  // "SNSLobbyHostConnected"
constexpr uint64_t S_N_S_LOBBY_HOST_DATA_CHANGED = 0x84bacce2d4f9ff60;  // "SNSLobbyHostDataChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_DISCONNECTED = 0xd16eb0db08a6de88;  // "SNSLobbyHostDisconnected"
constexpr uint64_t S_N_S_LOBBY_HOST_STATE = 0xcf20c53deb6178f1;  // "SNSLobbyHostState"
constexpr uint64_t S_N_S_LOBBY_JOIN_ACCEPTEDV2 = 0x9a1fdab16df217ac;  // "SNSLobbyJoinAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_JOIN_COMPLETE = 0x863d77c9cca695ab;  // "SNSLobbyJoinComplete"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED = 0x7ab5c584616cc10a;  // "SNSLobbyJoinRejected"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_INTERNAL = 0x022fbeacae966dbe;  // "SNSLobbyJoinRejectedInternal"
constexpr uint64_t S_N_S_LOBBY_JOIN_REQUESTV4 = 0x85144c8563b90f91;  // "SNSLobbyJoinRequestv4"
constexpr uint64_t S_N_S_LOBBY_JOIN_SETUP = 0x8c95f68eaaa037f0;  // "SNSLobbyJoinSetup"
constexpr uint64_t S_N_S_LOBBY_LEFT = 0x1c18e76b55b6a179;  // "SNSLobbyLeft"
constexpr uint64_t S_N_S_LOBBY_LOCK_ENTRANTS = 0x2c5c678c3992cfc8;  // "SNSLobbyLockEntrants"
constexpr uint64_t S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED = 0x7087efe054e8a9a2;  // "SNSLobbyMatchmakerStatusReceived"
constexpr uint64_t S_N_S_LOBBY_OWNER_DATA_CHANGED = 0x706d8ba60811273e;  // "SNSLobbyOwnerDataChanged"
constexpr uint64_t S_N_S_LOBBY_OWNER_STATE = 0x8deb4a77934e47b7;  // "SNSLobbyOwnerState"
constexpr uint64_t S_N_S_LOBBY_PASS_OWNERSHIP = 0x418c74cbbd04c9bc;  // "SNSLobbyPassOwnership"
constexpr uint64_t S_N_S_LOBBY_PING_REQUESTV3 = 0x2efc074b259599c8;  // "SNSLobbyPingRequestv3"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_FAILURE = 0xb5f231cfec7b39f0;  // "SNSLobbyRegistrationFailure"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_SUCCESS = 0x6c89990f3286eea8;  // "SNSLobbyRegistrationSuccess"
constexpr uint64_t S_N_S_LOBBY_REMOVING_ENTRANT = 0x79609caea870754d;  // "SNSLobbyRemovingEntrant"
constexpr uint64_t S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS = 0x2c0aacd18d7aab5a;  // "SNSLobbySendClientLobbySettings"
constexpr uint64_t S_N_S_LOBBY_SESSION_ERROR = 0x25223860964ff50d;  // "SNSLobbySessionError"
constexpr uint64_t S_N_S_LOBBY_SESSION_FAILUREV4 = 0xabdb92b911f8805d;  // "SNSLobbySessionFailurev4"
constexpr uint64_t S_N_S_LOBBY_SESSION_STARTING = 0x9265072739e50601;  // "SNSLobbySessionStarting"
constexpr uint64_t S_N_S_LOBBY_SESSION_SUCCESSV5 = 0x8c9f2df1709244fe;  // "SNSLobbySessionSuccessv5"
constexpr uint64_t S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER = 0x328bcb07ba52b1d6;  // "SNSLobbySetSpawnBotOnServer"
constexpr uint64_t S_N_S_LOBBY_SETTINGS_RESPONSE = 0xe292bf6e0d318f99;  // "SNSLobbySettingsResponse"
constexpr uint64_t S_N_S_LOBBY_SMITE_ENTRANT = 0xe1130f3283330dea;  // "SNSLobbySmiteEntrant"
constexpr uint64_t S_N_S_LOBBY_START_SESSIONV4 = 0x14629bcb3ab4fa6d;  // "SNSLobbyStartSessionv4"
constexpr uint64_t S_N_S_LOBBY_STATUS_NOTIFYV2 = 0xeabfa9dfc93e00b5;  // "SNSLobbyStatusNotifyv2"
constexpr uint64_t S_N_S_LOBBY_TERMINATE_PROCESS = 0x307c2afd7ea03349;  // "SNSLobbyTerminateProcess"
constexpr uint64_t S_N_S_LOBBY_UNLOCK_ENTRANTS = 0x79f27e4868e7c892;  // "SNSLobbyUnlockEntrants"
constexpr uint64_t S_N_S_LOBBY_UPDATE_PINGS = 0xa04f8dc961a17959;  // "SNSLobbyUpdatePings"
constexpr uint64_t S_N_S_LOBBY_VOICE_ENTRY = 0xbee38f67e88ea9fa;  // "SNSLobbyVoiceEntry"
constexpr uint64_t S_N_S_LOGGED_IN_USER_PROFILE_REQUEST = 0xc56b55188c0800ce;  // "SNSLoggedInUserProfileRequest"
constexpr uint64_t S_N_S_LOGIN_PROFILE_RESULT = 0x1dd951c11e96cad9;  // "SNSLoginProfileResult"
constexpr uint64_t S_N_S_LOGIN_SETTINGS = 0xebb8159c87017c91;  // "SNSLoginSettings"
constexpr uint64_t S_N_S_MATCH_ENDEDV5 = 0x405845e5586ff84f;  // "SNSMatchEndedv5"
constexpr uint64_t S_N_S_NEW_UNLOCKS_NOTIFICATION = 0x57815184ab98e611;  // "SNSNewUnlocksNotification"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_FAILURE = 0x705fb14dd9ce6058;  // "SNSOtherUserProfileFailure"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_REQUEST = 0x0abccfd61f410583;  // "SNSOtherUserProfileRequest"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_SUCCESS = 0x6d2cfd31770bae59;  // "SNSOtherUserProfileSuccess"
constexpr uint64_t S_N_S_PROFILE_RESPONSE = 0x8d1f9e2f1770ae96;  // "SNSProfileResponse"
constexpr uint64_t S_N_S_PURCHASE_ITEMS = 0x4223faa0030d7050;  // "SNSPurchaseItems"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_RESULT = 0x31270b10f1d4fb6d;  // "SNSPurchaseItemsResult"
constexpr uint64_t S_N_S_RECONCILE_I_A_P = 0x24f52cbd6ae5461b;  // "SNSReconcileIAP"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_RESULT = 0x6495220b88e02095;  // "SNSReconcileIAPResult"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FOR_USER = 0xe867a8314035f0a0;  // "SNSRefreshProfileForUser"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FROM_SERVER = 0x92cf95d8c6ba758b;  // "SNSRefreshProfileFromServer"
constexpr uint64_t S_N_S_REFRESH_PROFILE_RESULT = 0x6deb0c3773e06542;  // "SNSRefreshProfileResult"
constexpr uint64_t S_N_S_REMOTE_LOG_SETV3 = 0xfccb13c93e07aedd;  // "SNSRemoteLogSetv3"
constexpr uint64_t S_N_S_REMOVE_TOURNAMENT = 0x156460131ece7bd6;  // "SNSRemoveTournament"
constexpr uint64_t S_N_S_REWARDS_SETTINGS = 0xa0618e27856418b1;  // "SNSRewardsSettings"
constexpr uint64_t S_N_S_SERVER_SETTINGS_RESPONSEV2 = 0x70a253142e853440;  // "SNSServerSettingsResponsev2"
constexpr uint64_t S_N_S_TELEMETRY_EVENT = 0x731857d1fd9f83c9;  // "SNSTelemetryEvent"
constexpr uint64_t S_N_S_TELEMETRY_NOTIFY = 0x4aede1efe0bd3640;  // "SNSTelemetryNotify"
constexpr uint64_t S_N_S_UPDATE_PROFILE = 0x2cf5d4ee89f2df89;  // "SNSUpdateProfile"
constexpr uint64_t S_N_S_UPDATE_PROFILE_FAILURE = 0xa0f37564ee33970b;  // "SNSUpdateProfileFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE = 0x2e404a73a5915696;  // "SNSUserServerProfileUpdateFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST = 0x54a334e8631f3b5d;  // "SNSUserServerProfileUpdateRequest"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS = 0x3333060f0b54999f;  // "SNSUserServerProfileUpdateSuccess"

// Reverse lookup map (intermediate hash -> name)
inline const std::unordered_map<uint64_t, std::string_view> HASH_TO_NAME = {
    {S_N_S_ADD_TOURNAMENT, "SNSAddTournament"},
    {S_N_S_CHANNEL_INFO_REQUEST, "SNSChannelInfoRequest"},
    {S_N_S_CHANNEL_INFO_RESPONSE, "SNSChannelInfoResponse"},
    {S_N_S_CONFIG_FAILUREV2, "SNSConfigFailurev2"},
    {S_N_S_CONFIG_REQUESTV2, "SNSConfigRequestv2"},
    {S_N_S_CONFIG_SUCCESSV2, "SNSConfigSuccessv2"},
    {S_N_S_DOCUMENT_FAILURE, "SNSDocumentFailure"},
    {S_N_S_DOCUMENT_REQUESTV2, "SNSDocumentRequestv2"},
    {S_N_S_DOCUMENT_SUCCESS, "SNSDocumentSuccess"},
    {S_N_S_EARLY_QUIT_CONFIG, "SNSEarlyQuitConfig"},
    {S_N_S_EARLY_QUIT_FEATURE_FLAGS, "SNSEarlyQuitFeatureFlags"},
    {S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION, "SNSEarlyQuitUpdateNotification"},
    {S_N_S_LEADERBOARD_REQUESTV2, "SNSLeaderboardRequestv2"},
    {S_N_S_LEADERBOARD_RESPONSE, "SNSLeaderboardResponse"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2, "SNSLobbyAcceptPlayersFailurev2"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2, "SNSLobbyAcceptPlayersSuccessv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2, "SNSLobbyAddEntrantAcceptedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2, "SNSLobbyAddEntrantRejectedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4, "SNSLobbyAddEntrantRequestv4"},
    {S_N_S_LOBBY_CHAT_ENTRY, "SNSLobbyChatEntry"},
    {S_N_S_LOBBY_DIRECTORY_JSON, "SNSLobbyDirectoryJson"},
    {S_N_S_LOBBY_ENTRANT_ACCEPTED, "SNSLobbyEntrantAccepted"},
    {S_N_S_LOBBY_ENTRANT_ADDED, "SNSLobbyEntrantAdded"},
    {S_N_S_LOBBY_ENTRANT_DATA_CHANGED, "SNSLobbyEntrantDataChanged"},
    {S_N_S_LOBBY_ENTRANT_REMOVED, "SNSLobbyEntrantRemoved"},
    {S_N_S_LOBBY_ENTRANT_STATEV2, "SNSLobbyEntrantStatev2"},
    {S_N_S_LOBBY_ENTRANT_TEAM_CHANGED, "SNSLobbyEntrantTeamChanged"},
    {S_N_S_LOBBY_HOST_CONNECT_FAILED, "SNSLobbyHostConnectFailed"},
    {S_N_S_LOBBY_HOST_CONNECTED, "SNSLobbyHostConnected"},
    {S_N_S_LOBBY_HOST_DATA_CHANGED, "SNSLobbyHostDataChanged"},
    {S_N_S_LOBBY_HOST_DISCONNECTED, "SNSLobbyHostDisconnected"},
    {S_N_S_LOBBY_HOST_STATE, "SNSLobbyHostState"},
    {S_N_S_LOBBY_JOIN_ACCEPTEDV2, "SNSLobbyJoinAcceptedv2"},
    {S_N_S_LOBBY_JOIN_COMPLETE, "SNSLobbyJoinComplete"},
    {S_N_S_LOBBY_JOIN_REJECTED, "SNSLobbyJoinRejected"},
    {S_N_S_LOBBY_JOIN_REJECTED_INTERNAL, "SNSLobbyJoinRejectedInternal"},
    {S_N_S_LOBBY_JOIN_REQUESTV4, "SNSLobbyJoinRequestv4"},
    {S_N_S_LOBBY_JOIN_SETUP, "SNSLobbyJoinSetup"},
    {S_N_S_LOBBY_LEFT, "SNSLobbyLeft"},
    {S_N_S_LOBBY_LOCK_ENTRANTS, "SNSLobbyLockEntrants"},
    {S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED, "SNSLobbyMatchmakerStatusReceived"},
    {S_N_S_LOBBY_OWNER_DATA_CHANGED, "SNSLobbyOwnerDataChanged"},
    {S_N_S_LOBBY_OWNER_STATE, "SNSLobbyOwnerState"},
    {S_N_S_LOBBY_PASS_OWNERSHIP, "SNSLobbyPassOwnership"},
    {S_N_S_LOBBY_PING_REQUESTV3, "SNSLobbyPingRequestv3"},
    {S_N_S_LOBBY_REGISTRATION_FAILURE, "SNSLobbyRegistrationFailure"},
    {S_N_S_LOBBY_REGISTRATION_SUCCESS, "SNSLobbyRegistrationSuccess"},
    {S_N_S_LOBBY_REMOVING_ENTRANT, "SNSLobbyRemovingEntrant"},
    {S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS, "SNSLobbySendClientLobbySettings"},
    {S_N_S_LOBBY_SESSION_ERROR, "SNSLobbySessionError"},
    {S_N_S_LOBBY_SESSION_FAILUREV4, "SNSLobbySessionFailurev4"},
    {S_N_S_LOBBY_SESSION_STARTING, "SNSLobbySessionStarting"},
    {S_N_S_LOBBY_SESSION_SUCCESSV5, "SNSLobbySessionSuccessv5"},
    {S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER, "SNSLobbySetSpawnBotOnServer"},
    {S_N_S_LOBBY_SETTINGS_RESPONSE, "SNSLobbySettingsResponse"},
    {S_N_S_LOBBY_SMITE_ENTRANT, "SNSLobbySmiteEntrant"},
    {S_N_S_LOBBY_START_SESSIONV4, "SNSLobbyStartSessionv4"},
    {S_N_S_LOBBY_STATUS_NOTIFYV2, "SNSLobbyStatusNotifyv2"},
    {S_N_S_LOBBY_TERMINATE_PROCESS, "SNSLobbyTerminateProcess"},
    {S_N_S_LOBBY_UNLOCK_ENTRANTS, "SNSLobbyUnlockEntrants"},
    {S_N_S_LOBBY_UPDATE_PINGS, "SNSLobbyUpdatePings"},
    {S_N_S_LOBBY_VOICE_ENTRY, "SNSLobbyVoiceEntry"},
    {S_N_S_LOGGED_IN_USER_PROFILE_REQUEST, "SNSLoggedInUserProfileRequest"},
    {S_N_S_LOGIN_PROFILE_RESULT, "SNSLoginProfileResult"},
    {S_N_S_LOGIN_SETTINGS, "SNSLoginSettings"},
    {S_N_S_MATCH_ENDEDV5, "SNSMatchEndedv5"},
    {S_N_S_NEW_UNLOCKS_NOTIFICATION, "SNSNewUnlocksNotification"},
    {S_N_S_OTHER_USER_PROFILE_FAILURE, "SNSOtherUserProfileFailure"},
    {S_N_S_OTHER_USER_PROFILE_REQUEST, "SNSOtherUserProfileRequest"},
    {S_N_S_OTHER_USER_PROFILE_SUCCESS, "SNSOtherUserProfileSuccess"},
    {S_N_S_PROFILE_RESPONSE, "SNSProfileResponse"},
    {S_N_S_PURCHASE_ITEMS, "SNSPurchaseItems"},
    {S_N_S_PURCHASE_ITEMS_RESULT, "SNSPurchaseItemsResult"},
    {S_N_S_RECONCILE_I_A_P, "SNSReconcileIAP"},
    {S_N_S_RECONCILE_I_A_P_RESULT, "SNSReconcileIAPResult"},
    {S_N_S_REFRESH_PROFILE_FOR_USER, "SNSRefreshProfileForUser"},
    {S_N_S_REFRESH_PROFILE_FROM_SERVER, "SNSRefreshProfileFromServer"},
    {S_N_S_REFRESH_PROFILE_RESULT, "SNSRefreshProfileResult"},
    {S_N_S_REMOTE_LOG_SETV3, "SNSRemoteLogSetv3"},
    {S_N_S_REMOVE_TOURNAMENT, "SNSRemoveTournament"},
    {S_N_S_REWARDS_SETTINGS, "SNSRewardsSettings"},
    {S_N_S_SERVER_SETTINGS_RESPONSEV2, "SNSServerSettingsResponsev2"},
    {S_N_S_TELEMETRY_EVENT, "SNSTelemetryEvent"},
    {S_N_S_TELEMETRY_NOTIFY, "SNSTelemetryNotify"},
    {S_N_S_UPDATE_PROFILE, "SNSUpdateProfile"},
    {S_N_S_UPDATE_PROFILE_FAILURE, "SNSUpdateProfileFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE, "SNSUserServerProfileUpdateFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST, "SNSUserServerProfileUpdateRequest"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS, "SNSUserServerProfileUpdateSuccess"},
};

}  // namespace CMatSym
}  // namespace EchoVR