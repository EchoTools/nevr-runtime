// SNSMessageHashes.h
// Final SNS message hashes (CMatSym_Hash + SMatSymData_HashA)
//
// WARNING: Since we don't have validated hash values, this header includes
// multiple prefix variations for each message name:
//   - Full name as documented (e.g., 'SNSLobbyJoinRequestv4')
//   - Without 'SNS' prefix (e.g., 'LobbyJoinRequestv4')
//   - With 'S' prefix only (e.g., 'SLobbyJoinRequestv4')
//
// Test ALL variations at runtime to determine which is correct.
//
// Algorithm:
//   1. CMatSym_Hash @ 0x140107f80 (CRC64-ECMA-182)
//   2. SMatSymData_HashA @ 0x140107fd0 (MurmurHash3 finalizer)
//   3. SNS_SEED = 0x6d451003fb4b172e
//
// Source: evr-reconstruction/src/NRadEngine/Social/SNSHash.cpp

#pragma once
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace EchoVR {
namespace SNS {

// ============================================================================
// VARIATION 1: Full names as documented (e.g., 'SNSLobbyJoinRequestv4')
// ============================================================================

constexpr uint64_t S_N_S_ADD_TOURNAMENT_FULL = 0x7a7d209b6f74d24e;  // "SNSAddTournament"
constexpr uint64_t S_N_S_CHANNEL_INFO_REQUEST_FULL = 0xfbe3e20a1c99543d;  // "SNSChannelInfoRequest"
constexpr uint64_t S_N_S_CHANNEL_INFO_RESPONSE_FULL = 0x2f079c1f5ab76838;  // "SNSChannelInfoResponse"
constexpr uint64_t S_N_S_CONFIG_FAILUREV2_FULL = 0xcd4b35df682d601a;  // "SNSConfigFailurev2"
constexpr uint64_t S_N_S_CONFIG_REQUESTV2_FULL = 0x1260710b767e27cd;  // "SNSConfigRequestv2"
constexpr uint64_t S_N_S_CONFIG_SUCCESSV2_FULL = 0xd7b2b6f241ab8d2d;  // "SNSConfigSuccessv2"
constexpr uint64_t S_N_S_DOCUMENT_FAILURE_FULL = 0x9be41ff820447a13;  // "SNSDocumentFailure"
constexpr uint64_t S_N_S_DOCUMENT_REQUESTV2_FULL = 0xb53fe97680d9a48b;  // "SNSDocumentRequestv2"
constexpr uint64_t S_N_S_DOCUMENT_SUCCESS_FULL = 0xa55c4d49813b4ec3;  // "SNSDocumentSuccess"
constexpr uint64_t S_N_S_EARLY_QUIT_CONFIG_FULL = 0xe4a485b66a6a778f;  // "SNSEarlyQuitConfig"
constexpr uint64_t S_N_S_EARLY_QUIT_FEATURE_FLAGS_FULL = 0xc2bbab161abd3149;  // "SNSEarlyQuitFeatureFlags"
constexpr uint64_t S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION_FULL = 0xd64f4050bed19595;  // "SNSEarlyQuitUpdateNotification"
constexpr uint64_t S_N_S_LEADERBOARD_REQUESTV2_FULL = 0x6b6cd3e9fc9942a3;  // "SNSLeaderboardRequestv2"
constexpr uint64_t S_N_S_LEADERBOARD_RESPONSE_FULL = 0x49c73c643a7c8401;  // "SNSLeaderboardResponse"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2_FULL = 0x695166a3ea7cf080;  // "SNSLobbyAcceptPlayersFailurev2"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2_FULL = 0xc5941c9915cab9bc;  // "SNSLobbyAcceptPlayersSuccessv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2_FULL = 0x7f60c1614ac39d15;  // "SNSLobbyAddEntrantAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2_FULL = 0x186b68bd0e9d40a9;  // "SNSLobbyAddEntrantRejectedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4_FULL = 0xe3db8573a4849019;  // "SNSLobbyAddEntrantRequestv4"
constexpr uint64_t S_N_S_LOBBY_CHAT_ENTRY_FULL = 0xa822cc9f8cda5849;  // "SNSLobbyChatEntry"
constexpr uint64_t S_N_S_LOBBY_DIRECTORY_JSON_FULL = 0xe350817164be1c39;  // "SNSLobbyDirectoryJson"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ACCEPTED_FULL = 0x79706cadf13e3e0a;  // "SNSLobbyEntrantAccepted"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ADDED_FULL = 0xc2e06f883e37eda6;  // "SNSLobbyEntrantAdded"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_DATA_CHANGED_FULL = 0xd8269ff956ad6307;  // "SNSLobbyEntrantDataChanged"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_REMOVED_FULL = 0xb07bc92b08e7a63e;  // "SNSLobbyEntrantRemoved"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_STATEV2_FULL = 0x01915401cf1ceda2;  // "SNSLobbyEntrantStatev2"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_TEAM_CHANGED_FULL = 0x1b3b5db2c5ec0567;  // "SNSLobbyEntrantTeamChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECT_FAILED_FULL = 0xe61d5cd983dedab0;  // "SNSLobbyHostConnectFailed"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECTED_FULL = 0x2bc2e42be9d0d421;  // "SNSLobbyHostConnected"
constexpr uint64_t S_N_S_LOBBY_HOST_DATA_CHANGED_FULL = 0xfab46b6c0c0b99fc;  // "SNSLobbyHostDataChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_DISCONNECTED_FULL = 0x4730971841504640;  // "SNSLobbyHostDisconnected"
constexpr uint64_t S_N_S_LOBBY_HOST_STATE_FULL = 0x186b1f6ae1f3d15b;  // "SNSLobbyHostState"
constexpr uint64_t S_N_S_LOBBY_JOIN_ACCEPTEDV2_FULL = 0x0bfb1a6affab367e;  // "SNSLobbyJoinAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_JOIN_COMPLETE_FULL = 0x79d7494719f8184b;  // "SNSLobbyJoinComplete"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_FULL = 0x5aec9beeea34ff39;  // "SNSLobbyJoinRejected"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_INTERNAL_FULL = 0x8505d08467ae76ad;  // "SNSLobbyJoinRejectedInternal"
constexpr uint64_t S_N_S_LOBBY_JOIN_REQUESTV4_FULL = 0xc21f17359451970c;  // "SNSLobbyJoinRequestv4"
constexpr uint64_t S_N_S_LOBBY_JOIN_SETUP_FULL = 0x97aa956fecf91c6e;  // "SNSLobbyJoinSetup"
constexpr uint64_t S_N_S_LOBBY_LEFT_FULL = 0xd3dd16787339b204;  // "SNSLobbyLeft"
constexpr uint64_t S_N_S_LOBBY_LOCK_ENTRANTS_FULL = 0xb7dacad57136e02f;  // "SNSLobbyLockEntrants"
constexpr uint64_t S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED_FULL = 0x225b9e700032a28a;  // "SNSLobbyMatchmakerStatusReceived"
constexpr uint64_t S_N_S_LOBBY_OWNER_DATA_CHANGED_FULL = 0x37b37459e67d16bd;  // "SNSLobbyOwnerDataChanged"
constexpr uint64_t S_N_S_LOBBY_OWNER_STATE_FULL = 0xbf37b4c83d2f05ef;  // "SNSLobbyOwnerState"
constexpr uint64_t S_N_S_LOBBY_PASS_OWNERSHIP_FULL = 0xef2fe1c1662958e4;  // "SNSLobbyPassOwnership"
constexpr uint64_t S_N_S_LOBBY_PING_REQUESTV3_FULL = 0xc2719fc62353967d;  // "SNSLobbyPingRequestv3"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_FAILURE_FULL = 0x5fc7f27253cb7bc7;  // "SNSLobbyRegistrationFailure"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_SUCCESS_FULL = 0x02b0f443274bad6c;  // "SNSLobbyRegistrationSuccess"
constexpr uint64_t S_N_S_LOBBY_REMOVING_ENTRANT_FULL = 0x191ad4f7c60ce289;  // "SNSLobbyRemovingEntrant"
constexpr uint64_t S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS_FULL = 0x7da79d43426171d6;  // "SNSLobbySendClientLobbySettings"
constexpr uint64_t S_N_S_LOBBY_SESSION_ERROR_FULL = 0xa23f482c91048497;  // "SNSLobbySessionError"
constexpr uint64_t S_N_S_LOBBY_SESSION_FAILUREV4_FULL = 0x15aa1d930a8b39ad;  // "SNSLobbySessionFailurev4"
constexpr uint64_t S_N_S_LOBBY_SESSION_STARTING_FULL = 0x526944350a1fb09b;  // "SNSLobbySessionStarting"
constexpr uint64_t S_N_S_LOBBY_SESSION_SUCCESSV5_FULL = 0x40456577a7df20dd;  // "SNSLobbySessionSuccessv5"
constexpr uint64_t S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER_FULL = 0x44e715eac757284a;  // "SNSLobbySetSpawnBotOnServer"
constexpr uint64_t S_N_S_LOBBY_SETTINGS_RESPONSE_FULL = 0x018edad9cc9de36d;  // "SNSLobbySettingsResponse"
constexpr uint64_t S_N_S_LOBBY_SMITE_ENTRANT_FULL = 0x408ce40cb1901c1c;  // "SNSLobbySmiteEntrant"
constexpr uint64_t S_N_S_LOBBY_START_SESSIONV4_FULL = 0xb4a49cac3c7e9ee6;  // "SNSLobbyStartSessionv4"
constexpr uint64_t S_N_S_LOBBY_STATUS_NOTIFYV2_FULL = 0x5f8fc5e9bd575d69;  // "SNSLobbyStatusNotifyv2"
constexpr uint64_t S_N_S_LOBBY_TERMINATE_PROCESS_FULL = 0x7133fd3a9d2d6411;  // "SNSLobbyTerminateProcess"
constexpr uint64_t S_N_S_LOBBY_UNLOCK_ENTRANTS_FULL = 0xd44db7fe16170e45;  // "SNSLobbyUnlockEntrants"
constexpr uint64_t S_N_S_LOBBY_UPDATE_PINGS_FULL = 0xab43523979e29ccd;  // "SNSLobbyUpdatePings"
constexpr uint64_t S_N_S_LOBBY_VOICE_ENTRY_FULL = 0x80d230b9c0e59eac;  // "SNSLobbyVoiceEntry"
constexpr uint64_t S_N_S_LOGGED_IN_USER_PROFILE_REQUEST_FULL = 0x8bd3cc648a79e56a;  // "SNSLoggedInUserProfileRequest"
constexpr uint64_t S_N_S_LOGIN_PROFILE_RESULT_FULL = 0xdeca5559da877271;  // "SNSLoginProfileResult"
constexpr uint64_t S_N_S_LOGIN_SETTINGS_FULL = 0xbf69696a4145d21b;  // "SNSLoginSettings"
constexpr uint64_t S_N_S_MATCH_ENDEDV5_FULL = 0xfdfb13641074dd49;  // "SNSMatchEndedv5"
constexpr uint64_t S_N_S_NEW_UNLOCKS_NOTIFICATION_FULL = 0xc60bff675dd7dfc8;  // "SNSNewUnlocksNotification"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_FAILURE_FULL = 0xae51bee113b77c6a;  // "SNSOtherUserProfileFailure"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_REQUEST_FULL = 0xd61dd0e693da3053;  // "SNSOtherUserProfileRequest"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_SUCCESS_FULL = 0xce67a614dc8678fc;  // "SNSOtherUserProfileSuccess"
constexpr uint64_t S_N_S_PROFILE_RESPONSE_FULL = 0xaab1fef66e9e7689;  // "SNSProfileResponse"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_FULL = 0x0b80ac142ec32fd6;  // "SNSPurchaseItems"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_RESULT_FULL = 0x15d1399fcd0829a6;  // "SNSPurchaseItemsResult"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_FULL = 0x88612b1051b899db;  // "SNSReconcileIAP"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_RESULT_FULL = 0xb1b17a9a8c608629;  // "SNSReconcileIAPResult"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FOR_USER_FULL = 0x61fcc944826594c2;  // "SNSRefreshProfileForUser"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FROM_SERVER_FULL = 0x2e62734d440bcb16;  // "SNSRefreshProfileFromServer"
constexpr uint64_t S_N_S_REFRESH_PROFILE_RESULT_FULL = 0x9ed2710da21e5802;  // "SNSRefreshProfileResult"
constexpr uint64_t S_N_S_REMOTE_LOG_SETV3_FULL = 0x866601d6890b9bbc;  // "SNSRemoteLogSetv3"
constexpr uint64_t S_N_S_REMOVE_TOURNAMENT_FULL = 0x24bcc194a23584cf;  // "SNSRemoveTournament"
constexpr uint64_t S_N_S_REWARDS_SETTINGS_FULL = 0xa4b166c5579a8092;  // "SNSRewardsSettings"
constexpr uint64_t S_N_S_SERVER_SETTINGS_RESPONSEV2_FULL = 0x4001bc018125da75;  // "SNSServerSettingsResponsev2"
constexpr uint64_t S_N_S_TELEMETRY_EVENT_FULL = 0x17b618a70744dbb7;  // "SNSTelemetryEvent"
constexpr uint64_t S_N_S_TELEMETRY_NOTIFY_FULL = 0xb2ebdd8e9539de3e;  // "SNSTelemetryNotify"
constexpr uint64_t S_N_S_UPDATE_PROFILE_FULL = 0x429dea0b834c5dda;  // "SNSUpdateProfile"
constexpr uint64_t S_N_S_UPDATE_PROFILE_FAILURE_FULL = 0xa7ba824038921133;  // "SNSUpdateProfileFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE_FULL = 0xc9945d367da100c0;  // "SNSUserServerProfileUpdateFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST_FULL = 0xea26d4c665c02499;  // "SNSUserServerProfileUpdateRequest"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS_FULL = 0x07bd2a7fe4437b49;  // "SNSUserServerProfileUpdateSuccess"

// ============================================================================
// VARIATION 2: Without 'SNS' prefix (e.g., 'LobbyJoinRequestv4')
// ============================================================================

constexpr uint64_t S_N_S_ADD_TOURNAMENT_NO_SNS = 0x24d5ae5ddd4d5f92;  // "AddTournament"
constexpr uint64_t S_N_S_CHANNEL_INFO_REQUEST_NO_SNS = 0x02d8e3603324fa23;  // "ChannelInfoRequest"
constexpr uint64_t S_N_S_CHANNEL_INFO_RESPONSE_NO_SNS = 0x3cc56d32fa1b3a34;  // "ChannelInfoResponse"
constexpr uint64_t S_N_S_CONFIG_FAILUREV2_NO_SNS = 0x7ffbd774d4438c9b;  // "ConfigFailurev2"
constexpr uint64_t S_N_S_CONFIG_REQUESTV2_NO_SNS = 0x34e439c0b6128576;  // "ConfigRequestv2"
constexpr uint64_t S_N_S_CONFIG_SUCCESSV2_NO_SNS = 0x4100d7ddcc8939cd;  // "ConfigSuccessv2"
constexpr uint64_t S_N_S_DOCUMENT_FAILURE_NO_SNS = 0xd0d00cbad2b11434;  // "DocumentFailure"
constexpr uint64_t S_N_S_DOCUMENT_REQUESTV2_NO_SNS = 0x231adc1fda0eb06b;  // "DocumentRequestv2"
constexpr uint64_t S_N_S_DOCUMENT_SUCCESS_NO_SNS = 0x881e6eb1612810a3;  // "DocumentSuccess"
constexpr uint64_t S_N_S_EARLY_QUIT_CONFIG_NO_SNS = 0x3cad319006478ef8;  // "EarlyQuitConfig"
constexpr uint64_t S_N_S_EARLY_QUIT_FEATURE_FLAGS_NO_SNS = 0xcd2c14d9956357c5;  // "EarlyQuitFeatureFlags"
constexpr uint64_t S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION_NO_SNS = 0xe3d34542557b9c5f;  // "EarlyQuitUpdateNotification"
constexpr uint64_t S_N_S_LEADERBOARD_REQUESTV2_NO_SNS = 0x112fec7164d71c2b;  // "LeaderboardRequestv2"
constexpr uint64_t S_N_S_LEADERBOARD_RESPONSE_NO_SNS = 0x2a7361845981e6ef;  // "LeaderboardResponse"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2_NO_SNS = 0x5ad3bb65dc7b2656;  // "LobbyAcceptPlayersFailurev2"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2_NO_SNS = 0x92af3108d96fd0c7;  // "LobbyAcceptPlayersSuccessv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2_NO_SNS = 0x6dac87d576496fe4;  // "LobbyAddEntrantAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2_NO_SNS = 0xbc4f4f8924ea154b;  // "LobbyAddEntrantRejectedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4_NO_SNS = 0x633db3cb7e1197ec;  // "LobbyAddEntrantRequestv4"
constexpr uint64_t S_N_S_LOBBY_CHAT_ENTRY_NO_SNS = 0x05616e937a0393fe;  // "LobbyChatEntry"
constexpr uint64_t S_N_S_LOBBY_DIRECTORY_JSON_NO_SNS = 0x784e37a83f763bde;  // "LobbyDirectoryJson"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ACCEPTED_NO_SNS = 0xf00968d1c5834051;  // "LobbyEntrantAccepted"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ADDED_NO_SNS = 0x5ca475d37773b370;  // "LobbyEntrantAdded"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_DATA_CHANGED_NO_SNS = 0xba99ec4f90995839;  // "LobbyEntrantDataChanged"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_REMOVED_NO_SNS = 0xa9ee1208eb4430b0;  // "LobbyEntrantRemoved"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_STATEV2_NO_SNS = 0xd4ca5287c35c2451;  // "LobbyEntrantStatev2"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_TEAM_CHANGED_NO_SNS = 0xcfdad79f93e19428;  // "LobbyEntrantTeamChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECT_FAILED_NO_SNS = 0x12050ac97a45a1d8;  // "LobbyHostConnectFailed"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECTED_NO_SNS = 0x623ef49107374ec6;  // "LobbyHostConnected"
constexpr uint64_t S_N_S_LOBBY_HOST_DATA_CHANGED_NO_SNS = 0xa4101aa371d28980;  // "LobbyHostDataChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_DISCONNECTED_NO_SNS = 0x47aaafc168bb2329;  // "LobbyHostDisconnected"
constexpr uint64_t S_N_S_LOBBY_HOST_STATE_NO_SNS = 0x5a8ab418540d1ad9;  // "LobbyHostState"
constexpr uint64_t S_N_S_LOBBY_JOIN_ACCEPTEDV2_NO_SNS = 0x702a90c3f6ec830a;  // "LobbyJoinAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_JOIN_COMPLETE_NO_SNS = 0x88e99adcf23b63a1;  // "LobbyJoinComplete"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_NO_SNS = 0xc4fb7249f3aa22f7;  // "LobbyJoinRejected"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_INTERNAL_NO_SNS = 0x41b01ae7ed49d820;  // "LobbyJoinRejectedInternal"
constexpr uint64_t S_N_S_LOBBY_JOIN_REQUESTV4_NO_SNS = 0x414aacab1c8f57f8;  // "LobbyJoinRequestv4"
constexpr uint64_t S_N_S_LOBBY_JOIN_SETUP_NO_SNS = 0x6bd4475ca06fb779;  // "LobbyJoinSetup"
constexpr uint64_t S_N_S_LOBBY_LEFT_NO_SNS = 0xaf70ae9330248a65;  // "LobbyLeft"
constexpr uint64_t S_N_S_LOBBY_LOCK_ENTRANTS_NO_SNS = 0xc60c91e0f5856886;  // "LobbyLockEntrants"
constexpr uint64_t S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED_NO_SNS = 0xd7df2fbac9561800;  // "LobbyMatchmakerStatusReceived"
constexpr uint64_t S_N_S_LOBBY_OWNER_DATA_CHANGED_NO_SNS = 0xe5f3297ab66b9d6d;  // "LobbyOwnerDataChanged"
constexpr uint64_t S_N_S_LOBBY_OWNER_STATE_NO_SNS = 0xea967857e8dea79d;  // "LobbyOwnerState"
constexpr uint64_t S_N_S_LOBBY_PASS_OWNERSHIP_NO_SNS = 0x851f808d2190effb;  // "LobbyPassOwnership"
constexpr uint64_t S_N_S_LOBBY_PING_REQUESTV3_NO_SNS = 0x0142e4610a94aecd;  // "LobbyPingRequestv3"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_FAILURE_NO_SNS = 0xaad18fc915608fcb;  // "LobbyRegistrationFailure"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_SUCCESS_NO_SNS = 0x191e68b2bcb8c176;  // "LobbyRegistrationSuccess"
constexpr uint64_t S_N_S_LOBBY_REMOVING_ENTRANT_NO_SNS = 0x784f520911e04562;  // "LobbyRemovingEntrant"
constexpr uint64_t S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS_NO_SNS = 0xe1871a3206c1b530;  // "LobbySendClientLobbySettings"
constexpr uint64_t S_N_S_LOBBY_SESSION_ERROR_NO_SNS = 0x860e29686285835b;  // "LobbySessionError"
constexpr uint64_t S_N_S_LOBBY_SESSION_FAILUREV4_NO_SNS = 0x19369a42dca27c90;  // "LobbySessionFailurev4"
constexpr uint64_t S_N_S_LOBBY_SESSION_STARTING_NO_SNS = 0xbf0d0d6fee5480d3;  // "LobbySessionStarting"
constexpr uint64_t S_N_S_LOBBY_SESSION_SUCCESSV5_NO_SNS = 0x482d6551d496ee62;  // "LobbySessionSuccessv5"
constexpr uint64_t S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER_NO_SNS = 0x395194c4d5c8b751;  // "LobbySetSpawnBotOnServer"
constexpr uint64_t S_N_S_LOBBY_SETTINGS_RESPONSE_NO_SNS = 0x50429ba0163be3a0;  // "LobbySettingsResponse"
constexpr uint64_t S_N_S_LOBBY_SMITE_ENTRANT_NO_SNS = 0xfe613d199293eb55;  // "LobbySmiteEntrant"
constexpr uint64_t S_N_S_LOBBY_START_SESSIONV4_NO_SNS = 0x3d100c9666b9d6b1;  // "LobbyStartSessionv4"
constexpr uint64_t S_N_S_LOBBY_STATUS_NOTIFYV2_NO_SNS = 0x59083454f21ed268;  // "LobbyStatusNotifyv2"
constexpr uint64_t S_N_S_LOBBY_TERMINATE_PROCESS_NO_SNS = 0x876d1d90ae3d69f6;  // "LobbyTerminateProcess"
constexpr uint64_t S_N_S_LOBBY_UNLOCK_ENTRANTS_NO_SNS = 0x41b46d203e473e77;  // "LobbyUnlockEntrants"
constexpr uint64_t S_N_S_LOBBY_UPDATE_PINGS_NO_SNS = 0xdb0ecfc7d231237c;  // "LobbyUpdatePings"
constexpr uint64_t S_N_S_LOBBY_VOICE_ENTRY_NO_SNS = 0xe80da5f89c4120c9;  // "LobbyVoiceEntry"
constexpr uint64_t S_N_S_LOGGED_IN_USER_PROFILE_REQUEST_NO_SNS = 0xba9277f6cc80729e;  // "LoggedInUserProfileRequest"
constexpr uint64_t S_N_S_LOGIN_PROFILE_RESULT_NO_SNS = 0xdc99adf6b66827db;  // "LoginProfileResult"
constexpr uint64_t S_N_S_LOGIN_SETTINGS_NO_SNS = 0x2abe9a930ac4089b;  // "LoginSettings"
constexpr uint64_t S_N_S_MATCH_ENDEDV5_NO_SNS = 0x29647e40edc4ff10;  // "MatchEndedv5"
constexpr uint64_t S_N_S_NEW_UNLOCKS_NOTIFICATION_NO_SNS = 0x2f9de0bda90c06b3;  // "NewUnlocksNotification"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_FAILURE_NO_SNS = 0x8055c61951e8e0b0;  // "OtherUserProfileFailure"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_REQUEST_NO_SNS = 0x20e093717e840bb2;  // "OtherUserProfileRequest"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_SUCCESS_NO_SNS = 0xa431bf990ac4d293;  // "OtherUserProfileSuccess"
constexpr uint64_t S_N_S_PROFILE_RESPONSE_NO_SNS = 0xf5ea4e088602ea1f;  // "ProfileResponse"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_NO_SNS = 0xab62ef7bd8ce6a43;  // "PurchaseItems"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_RESULT_NO_SNS = 0xab61719f78132809;  // "PurchaseItemsResult"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_NO_SNS = 0x130190f9eba58824;  // "ReconcileIAP"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_RESULT_NO_SNS = 0xc37caba1d810f5cc;  // "ReconcileIAPResult"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FOR_USER_NO_SNS = 0x949ae2ea12bbb1e3;  // "RefreshProfileForUser"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FROM_SERVER_NO_SNS = 0x7f8af4d053572ff8;  // "RefreshProfileFromServer"
constexpr uint64_t S_N_S_REFRESH_PROFILE_RESULT_NO_SNS = 0x554fe8f4a7b486cc;  // "RefreshProfileResult"
constexpr uint64_t S_N_S_REMOTE_LOG_SETV3_NO_SNS = 0x250f5a6f74578be7;  // "RemoteLogSetv3"
constexpr uint64_t S_N_S_REMOVE_TOURNAMENT_NO_SNS = 0xe22ff69241f4b5d4;  // "RemoveTournament"
constexpr uint64_t S_N_S_REWARDS_SETTINGS_NO_SNS = 0xc53d69195bc358f3;  // "RewardsSettings"
constexpr uint64_t S_N_S_SERVER_SETTINGS_RESPONSEV2_NO_SNS = 0x8c1abad6efa7adbc;  // "ServerSettingsResponsev2"
constexpr uint64_t S_N_S_TELEMETRY_EVENT_NO_SNS = 0xaa5f0c60cdb96ee8;  // "TelemetryEvent"
constexpr uint64_t S_N_S_TELEMETRY_NOTIFY_NO_SNS = 0xbb27d41144a98c8a;  // "TelemetryNotify"
constexpr uint64_t S_N_S_UPDATE_PROFILE_NO_SNS = 0xd6c92ebb8beee50b;  // "UpdateProfile"
constexpr uint64_t S_N_S_UPDATE_PROFILE_FAILURE_NO_SNS = 0x02d11fcf5ff613ca;  // "UpdateProfileFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE_NO_SNS = 0x93faf2a28ed2b488;  // "UserServerProfileUpdateFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST_NO_SNS = 0xbbf99b0a6f6ea8bc;  // "UserServerProfileUpdateRequest"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS_NO_SNS = 0x13e2985eceb8be0f;  // "UserServerProfileUpdateSuccess"

// ============================================================================
// VARIATION 3: With 'S' prefix only (e.g., 'SLobbyJoinRequestv4')
// ============================================================================

constexpr uint64_t S_N_S_ADD_TOURNAMENT_S_PREFIX = 0x5a659c47e46fcbae;  // "SAddTournament"
constexpr uint64_t S_N_S_CHANNEL_INFO_REQUEST_S_PREFIX = 0x3022883f7c0b9865;  // "SChannelInfoRequest"
constexpr uint64_t S_N_S_CHANNEL_INFO_RESPONSE_S_PREFIX = 0x485c59f093849421;  // "SChannelInfoResponse"
constexpr uint64_t S_N_S_CONFIG_FAILUREV2_S_PREFIX = 0xc7a9dce6cdb2ee27;  // "SConfigFailurev2"
constexpr uint64_t S_N_S_CONFIG_REQUESTV2_S_PREFIX = 0xd8973e690ffcb7c9;  // "SConfigRequestv2"
constexpr uint64_t S_N_S_CONFIG_SUCCESSV2_S_PREFIX = 0x71b3bb1761950d60;  // "SConfigSuccessv2"
constexpr uint64_t S_N_S_DOCUMENT_FAILURE_S_PREFIX = 0xc696e4e67f71ee5b;  // "SDocumentFailure"
constexpr uint64_t S_N_S_DOCUMENT_REQUESTV2_S_PREFIX = 0x3356b41623300994;  // "SDocumentRequestv2"
constexpr uint64_t S_N_S_DOCUMENT_SUCCESS_S_PREFIX = 0x0007d0856f926084;  // "SDocumentSuccess"
constexpr uint64_t S_N_S_EARLY_QUIT_CONFIG_S_PREFIX = 0x9a3eae6c94158df1;  // "SEarlyQuitConfig"
constexpr uint64_t S_N_S_EARLY_QUIT_FEATURE_FLAGS_S_PREFIX = 0xa2c38e6ac8546db4;  // "SEarlyQuitFeatureFlags"
constexpr uint64_t S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION_S_PREFIX = 0x03e65388f9dab39f;  // "SEarlyQuitUpdateNotification"
constexpr uint64_t S_N_S_LEADERBOARD_REQUESTV2_S_PREFIX = 0x5489a2c6cd358544;  // "SLeaderboardRequestv2"
constexpr uint64_t S_N_S_LEADERBOARD_RESPONSE_S_PREFIX = 0xcac7d967399d6884;  // "SLeaderboardResponse"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2_S_PREFIX = 0xa6d8b0517541271c;  // "SLobbyAcceptPlayersFailurev2"
constexpr uint64_t S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2_S_PREFIX = 0xabb426984d4bcd17;  // "SLobbyAcceptPlayersSuccessv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2_S_PREFIX = 0xe0f1a71c97e8753a;  // "SLobbyAddEntrantAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2_S_PREFIX = 0x3ecab8ae89e6a6cd;  // "SLobbyAddEntrantRejectedv2"
constexpr uint64_t S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4_S_PREFIX = 0x9f0531c1f197cbea;  // "SLobbyAddEntrantRequestv4"
constexpr uint64_t S_N_S_LOBBY_CHAT_ENTRY_S_PREFIX = 0x449fc80a1d474faf;  // "SLobbyChatEntry"
constexpr uint64_t S_N_S_LOBBY_DIRECTORY_JSON_S_PREFIX = 0x58de8420cd18447c;  // "SLobbyDirectoryJson"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ACCEPTED_S_PREFIX = 0x66976861ecc55587;  // "SLobbyEntrantAccepted"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_ADDED_S_PREFIX = 0xf5ab7c0f5c76ad25;  // "SLobbyEntrantAdded"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_DATA_CHANGED_S_PREFIX = 0xa285dc7687aca1e4;  // "SLobbyEntrantDataChanged"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_REMOVED_S_PREFIX = 0xf88973855288a3cc;  // "SLobbyEntrantRemoved"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_STATEV2_S_PREFIX = 0x65733fba5870e3f5;  // "SLobbyEntrantStatev2"
constexpr uint64_t S_N_S_LOBBY_ENTRANT_TEAM_CHANGED_S_PREFIX = 0x1181854b6144d05d;  // "SLobbyEntrantTeamChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECT_FAILED_S_PREFIX = 0x630b33567bdd2dda;  // "SLobbyHostConnectFailed"
constexpr uint64_t S_N_S_LOBBY_HOST_CONNECTED_S_PREFIX = 0x03eedee3653cb66e;  // "SLobbyHostConnected"
constexpr uint64_t S_N_S_LOBBY_HOST_DATA_CHANGED_S_PREFIX = 0x1dc8b543ac03638c;  // "SLobbyHostDataChanged"
constexpr uint64_t S_N_S_LOBBY_HOST_DISCONNECTED_S_PREFIX = 0xb5f149d5ba5495f5;  // "SLobbyHostDisconnected"
constexpr uint64_t S_N_S_LOBBY_HOST_STATE_S_PREFIX = 0x3d27c5cb302bcaa3;  // "SLobbyHostState"
constexpr uint64_t S_N_S_LOBBY_JOIN_ACCEPTEDV2_S_PREFIX = 0xca61d6e76a610ba9;  // "SLobbyJoinAcceptedv2"
constexpr uint64_t S_N_S_LOBBY_JOIN_COMPLETE_S_PREFIX = 0x2092dc99730b747e;  // "SLobbyJoinComplete"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_S_PREFIX = 0xadee28c2bc3f01f7;  // "SLobbyJoinRejected"
constexpr uint64_t S_N_S_LOBBY_JOIN_REJECTED_INTERNAL_S_PREFIX = 0x8246817c1f0c7509;  // "SLobbyJoinRejectedInternal"
constexpr uint64_t S_N_S_LOBBY_JOIN_REQUESTV4_S_PREFIX = 0x06ec5a995ded3139;  // "SLobbyJoinRequestv4"
constexpr uint64_t S_N_S_LOBBY_JOIN_SETUP_S_PREFIX = 0xd8788eac9f5fde8e;  // "SLobbyJoinSetup"
constexpr uint64_t S_N_S_LOBBY_LEFT_S_PREFIX = 0x93e081ece4407a15;  // "SLobbyLeft"
constexpr uint64_t S_N_S_LOBBY_LOCK_ENTRANTS_S_PREFIX = 0xe9a34c61f468c7f7;  // "SLobbyLockEntrants"
constexpr uint64_t S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED_S_PREFIX = 0x749f36b25c9f23c2;  // "SLobbyMatchmakerStatusReceived"
constexpr uint64_t S_N_S_LOBBY_OWNER_DATA_CHANGED_S_PREFIX = 0x220ff4758c95fa77;  // "SLobbyOwnerDataChanged"
constexpr uint64_t S_N_S_LOBBY_OWNER_STATE_S_PREFIX = 0x20e32880c1c8796d;  // "SLobbyOwnerState"
constexpr uint64_t S_N_S_LOBBY_PASS_OWNERSHIP_S_PREFIX = 0xa936f1e23674d170;  // "SLobbyPassOwnership"
constexpr uint64_t S_N_S_LOBBY_PING_REQUESTV3_S_PREFIX = 0x255836241778ac20;  // "SLobbyPingRequestv3"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_FAILURE_S_PREFIX = 0x8207d3075242c371;  // "SLobbyRegistrationFailure"
constexpr uint64_t S_N_S_LOBBY_REGISTRATION_SUCCESS_S_PREFIX = 0x5ab68924da1f9a78;  // "SLobbyRegistrationSuccess"
constexpr uint64_t S_N_S_LOBBY_REMOVING_ENTRANT_S_PREFIX = 0x694e488fe5739d6e;  // "SLobbyRemovingEntrant"
constexpr uint64_t S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS_S_PREFIX = 0x9296f0d055adeeb3;  // "SLobbySendClientLobbySettings"
constexpr uint64_t S_N_S_LOBBY_SESSION_ERROR_S_PREFIX = 0x060e70a195d77364;  // "SLobbySessionError"
constexpr uint64_t S_N_S_LOBBY_SESSION_FAILUREV4_S_PREFIX = 0x2dc79398d371cf3c;  // "SLobbySessionFailurev4"
constexpr uint64_t S_N_S_LOBBY_SESSION_STARTING_S_PREFIX = 0x204b910ae7b891ec;  // "SLobbySessionStarting"
constexpr uint64_t S_N_S_LOBBY_SESSION_SUCCESSV5_S_PREFIX = 0x671da745986d305f;  // "SLobbySessionSuccessv5"
constexpr uint64_t S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER_S_PREFIX = 0x1c1523d9a5a627f0;  // "SLobbySetSpawnBotOnServer"
constexpr uint64_t S_N_S_LOBBY_SETTINGS_RESPONSE_S_PREFIX = 0x0711421dc2936d42;  // "SLobbySettingsResponse"
constexpr uint64_t S_N_S_LOBBY_SMITE_ENTRANT_S_PREFIX = 0x2b9b0a8c14ea912b;  // "SLobbySmiteEntrant"
constexpr uint64_t S_N_S_LOBBY_START_SESSIONV4_S_PREFIX = 0xf93a50e43188e557;  // "SLobbyStartSessionv4"
constexpr uint64_t S_N_S_LOBBY_STATUS_NOTIFYV2_S_PREFIX = 0x7ca825a336e1b9d3;  // "SLobbyStatusNotifyv2"
constexpr uint64_t S_N_S_LOBBY_TERMINATE_PROCESS_S_PREFIX = 0x63c9996513725441;  // "SLobbyTerminateProcess"
constexpr uint64_t S_N_S_LOBBY_UNLOCK_ENTRANTS_S_PREFIX = 0x162c95b0e9a16112;  // "SLobbyUnlockEntrants"
constexpr uint64_t S_N_S_LOBBY_UPDATE_PINGS_S_PREFIX = 0x86540192d0a77f9a;  // "SLobbyUpdatePings"
constexpr uint64_t S_N_S_LOBBY_VOICE_ENTRY_S_PREFIX = 0x4209c81b9db7807a;  // "SLobbyVoiceEntry"
constexpr uint64_t S_N_S_LOGGED_IN_USER_PROFILE_REQUEST_S_PREFIX = 0xc743cfe4a3ab2d45;  // "SLoggedInUserProfileRequest"
constexpr uint64_t S_N_S_LOGIN_PROFILE_RESULT_S_PREFIX = 0x089b1dfff665b96f;  // "SLoginProfileResult"
constexpr uint64_t S_N_S_LOGIN_SETTINGS_S_PREFIX = 0xdb74d86452454d37;  // "SLoginSettings"
constexpr uint64_t S_N_S_MATCH_ENDEDV5_S_PREFIX = 0xb07ae69dc99ba9e8;  // "SMatchEndedv5"
constexpr uint64_t S_N_S_NEW_UNLOCKS_NOTIFICATION_S_PREFIX = 0x2b12ec76642da2c6;  // "SNewUnlocksNotification"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_FAILURE_S_PREFIX = 0x7978d8ace853ff00;  // "SOtherUserProfileFailure"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_REQUEST_S_PREFIX = 0x13cc756522e3933e;  // "SOtherUserProfileRequest"
constexpr uint64_t S_N_S_OTHER_USER_PROFILE_SUCCESS_S_PREFIX = 0xf194743715082813;  // "SOtherUserProfileSuccess"
constexpr uint64_t S_N_S_PROFILE_RESPONSE_S_PREFIX = 0x7a7873c4193ef5ea;  // "SProfileResponse"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_S_PREFIX = 0x560e6c709ff36c5f;  // "SPurchaseItems"
constexpr uint64_t S_N_S_PURCHASE_ITEMS_RESULT_S_PREFIX = 0xb281d4a821b53f9a;  // "SPurchaseItemsResult"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_S_PREFIX = 0xe441686a5b65d3bd;  // "SReconcileIAP"
constexpr uint64_t S_N_S_RECONCILE_I_A_P_RESULT_S_PREFIX = 0x0ba62c66f2b90779;  // "SReconcileIAPResult"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FOR_USER_S_PREFIX = 0x10cb3f2bae604db9;  // "SRefreshProfileForUser"
constexpr uint64_t S_N_S_REFRESH_PROFILE_FROM_SERVER_S_PREFIX = 0x8c57c8e296cbb0f1;  // "SRefreshProfileFromServer"
constexpr uint64_t S_N_S_REFRESH_PROFILE_RESULT_S_PREFIX = 0x0d2ad02e643bc59c;  // "SRefreshProfileResult"
constexpr uint64_t S_N_S_REMOTE_LOG_SETV3_S_PREFIX = 0x30d1e8803a477694;  // "SRemoteLogSetv3"
constexpr uint64_t S_N_S_REMOVE_TOURNAMENT_S_PREFIX = 0x0ee47624a965bf93;  // "SRemoveTournament"
constexpr uint64_t S_N_S_REWARDS_SETTINGS_S_PREFIX = 0x3cc908acaf6fe387;  // "SRewardsSettings"
constexpr uint64_t S_N_S_SERVER_SETTINGS_RESPONSEV2_S_PREFIX = 0xaa93a953dfbb7f80;  // "SServerSettingsResponsev2"
constexpr uint64_t S_N_S_TELEMETRY_EVENT_S_PREFIX = 0xccd506a11e0cafc7;  // "STelemetryEvent"
constexpr uint64_t S_N_S_TELEMETRY_NOTIFY_S_PREFIX = 0x0016b4b4b21329e5;  // "STelemetryNotify"
constexpr uint64_t S_N_S_UPDATE_PROFILE_S_PREFIX = 0x40115afd31a2b4dd;  // "SUpdateProfile"
constexpr uint64_t S_N_S_UPDATE_PROFILE_FAILURE_S_PREFIX = 0x4e5dc15513bffa53;  // "SUpdateProfileFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE_S_PREFIX = 0x5bb5ad49b3d3d455;  // "SUserServerProfileUpdateFailure"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST_S_PREFIX = 0x8887c069328bb0fd;  // "SUserServerProfileUpdateRequest"
constexpr uint64_t S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS_S_PREFIX = 0x52484868585a1b2d;  // "SUserServerProfileUpdateSuccess"

// ============================================================================
// REVERSE LOOKUP MAPS (for debugging)
// ============================================================================

inline const std::unordered_map<uint64_t, std::string_view> FULL_HASH_TO_NAME = {
    {S_N_S_ADD_TOURNAMENT_FULL, "SNSAddTournament"},
    {S_N_S_CHANNEL_INFO_REQUEST_FULL, "SNSChannelInfoRequest"},
    {S_N_S_CHANNEL_INFO_RESPONSE_FULL, "SNSChannelInfoResponse"},
    {S_N_S_CONFIG_FAILUREV2_FULL, "SNSConfigFailurev2"},
    {S_N_S_CONFIG_REQUESTV2_FULL, "SNSConfigRequestv2"},
    {S_N_S_CONFIG_SUCCESSV2_FULL, "SNSConfigSuccessv2"},
    {S_N_S_DOCUMENT_FAILURE_FULL, "SNSDocumentFailure"},
    {S_N_S_DOCUMENT_REQUESTV2_FULL, "SNSDocumentRequestv2"},
    {S_N_S_DOCUMENT_SUCCESS_FULL, "SNSDocumentSuccess"},
    {S_N_S_EARLY_QUIT_CONFIG_FULL, "SNSEarlyQuitConfig"},
    {S_N_S_EARLY_QUIT_FEATURE_FLAGS_FULL, "SNSEarlyQuitFeatureFlags"},
    {S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION_FULL, "SNSEarlyQuitUpdateNotification"},
    {S_N_S_LEADERBOARD_REQUESTV2_FULL, "SNSLeaderboardRequestv2"},
    {S_N_S_LEADERBOARD_RESPONSE_FULL, "SNSLeaderboardResponse"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2_FULL, "SNSLobbyAcceptPlayersFailurev2"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2_FULL, "SNSLobbyAcceptPlayersSuccessv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2_FULL, "SNSLobbyAddEntrantAcceptedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2_FULL, "SNSLobbyAddEntrantRejectedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4_FULL, "SNSLobbyAddEntrantRequestv4"},
    {S_N_S_LOBBY_CHAT_ENTRY_FULL, "SNSLobbyChatEntry"},
    {S_N_S_LOBBY_DIRECTORY_JSON_FULL, "SNSLobbyDirectoryJson"},
    {S_N_S_LOBBY_ENTRANT_ACCEPTED_FULL, "SNSLobbyEntrantAccepted"},
    {S_N_S_LOBBY_ENTRANT_ADDED_FULL, "SNSLobbyEntrantAdded"},
    {S_N_S_LOBBY_ENTRANT_DATA_CHANGED_FULL, "SNSLobbyEntrantDataChanged"},
    {S_N_S_LOBBY_ENTRANT_REMOVED_FULL, "SNSLobbyEntrantRemoved"},
    {S_N_S_LOBBY_ENTRANT_STATEV2_FULL, "SNSLobbyEntrantStatev2"},
    {S_N_S_LOBBY_ENTRANT_TEAM_CHANGED_FULL, "SNSLobbyEntrantTeamChanged"},
    {S_N_S_LOBBY_HOST_CONNECT_FAILED_FULL, "SNSLobbyHostConnectFailed"},
    {S_N_S_LOBBY_HOST_CONNECTED_FULL, "SNSLobbyHostConnected"},
    {S_N_S_LOBBY_HOST_DATA_CHANGED_FULL, "SNSLobbyHostDataChanged"},
    {S_N_S_LOBBY_HOST_DISCONNECTED_FULL, "SNSLobbyHostDisconnected"},
    {S_N_S_LOBBY_HOST_STATE_FULL, "SNSLobbyHostState"},
    {S_N_S_LOBBY_JOIN_ACCEPTEDV2_FULL, "SNSLobbyJoinAcceptedv2"},
    {S_N_S_LOBBY_JOIN_COMPLETE_FULL, "SNSLobbyJoinComplete"},
    {S_N_S_LOBBY_JOIN_REJECTED_FULL, "SNSLobbyJoinRejected"},
    {S_N_S_LOBBY_JOIN_REJECTED_INTERNAL_FULL, "SNSLobbyJoinRejectedInternal"},
    {S_N_S_LOBBY_JOIN_REQUESTV4_FULL, "SNSLobbyJoinRequestv4"},
    {S_N_S_LOBBY_JOIN_SETUP_FULL, "SNSLobbyJoinSetup"},
    {S_N_S_LOBBY_LEFT_FULL, "SNSLobbyLeft"},
    {S_N_S_LOBBY_LOCK_ENTRANTS_FULL, "SNSLobbyLockEntrants"},
    {S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED_FULL, "SNSLobbyMatchmakerStatusReceived"},
    {S_N_S_LOBBY_OWNER_DATA_CHANGED_FULL, "SNSLobbyOwnerDataChanged"},
    {S_N_S_LOBBY_OWNER_STATE_FULL, "SNSLobbyOwnerState"},
    {S_N_S_LOBBY_PASS_OWNERSHIP_FULL, "SNSLobbyPassOwnership"},
    {S_N_S_LOBBY_PING_REQUESTV3_FULL, "SNSLobbyPingRequestv3"},
    {S_N_S_LOBBY_REGISTRATION_FAILURE_FULL, "SNSLobbyRegistrationFailure"},
    {S_N_S_LOBBY_REGISTRATION_SUCCESS_FULL, "SNSLobbyRegistrationSuccess"},
    {S_N_S_LOBBY_REMOVING_ENTRANT_FULL, "SNSLobbyRemovingEntrant"},
    {S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS_FULL, "SNSLobbySendClientLobbySettings"},
    {S_N_S_LOBBY_SESSION_ERROR_FULL, "SNSLobbySessionError"},
    {S_N_S_LOBBY_SESSION_FAILUREV4_FULL, "SNSLobbySessionFailurev4"},
    {S_N_S_LOBBY_SESSION_STARTING_FULL, "SNSLobbySessionStarting"},
    {S_N_S_LOBBY_SESSION_SUCCESSV5_FULL, "SNSLobbySessionSuccessv5"},
    {S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER_FULL, "SNSLobbySetSpawnBotOnServer"},
    {S_N_S_LOBBY_SETTINGS_RESPONSE_FULL, "SNSLobbySettingsResponse"},
    {S_N_S_LOBBY_SMITE_ENTRANT_FULL, "SNSLobbySmiteEntrant"},
    {S_N_S_LOBBY_START_SESSIONV4_FULL, "SNSLobbyStartSessionv4"},
    {S_N_S_LOBBY_STATUS_NOTIFYV2_FULL, "SNSLobbyStatusNotifyv2"},
    {S_N_S_LOBBY_TERMINATE_PROCESS_FULL, "SNSLobbyTerminateProcess"},
    {S_N_S_LOBBY_UNLOCK_ENTRANTS_FULL, "SNSLobbyUnlockEntrants"},
    {S_N_S_LOBBY_UPDATE_PINGS_FULL, "SNSLobbyUpdatePings"},
    {S_N_S_LOBBY_VOICE_ENTRY_FULL, "SNSLobbyVoiceEntry"},
    {S_N_S_LOGGED_IN_USER_PROFILE_REQUEST_FULL, "SNSLoggedInUserProfileRequest"},
    {S_N_S_LOGIN_PROFILE_RESULT_FULL, "SNSLoginProfileResult"},
    {S_N_S_LOGIN_SETTINGS_FULL, "SNSLoginSettings"},
    {S_N_S_MATCH_ENDEDV5_FULL, "SNSMatchEndedv5"},
    {S_N_S_NEW_UNLOCKS_NOTIFICATION_FULL, "SNSNewUnlocksNotification"},
    {S_N_S_OTHER_USER_PROFILE_FAILURE_FULL, "SNSOtherUserProfileFailure"},
    {S_N_S_OTHER_USER_PROFILE_REQUEST_FULL, "SNSOtherUserProfileRequest"},
    {S_N_S_OTHER_USER_PROFILE_SUCCESS_FULL, "SNSOtherUserProfileSuccess"},
    {S_N_S_PROFILE_RESPONSE_FULL, "SNSProfileResponse"},
    {S_N_S_PURCHASE_ITEMS_FULL, "SNSPurchaseItems"},
    {S_N_S_PURCHASE_ITEMS_RESULT_FULL, "SNSPurchaseItemsResult"},
    {S_N_S_RECONCILE_I_A_P_FULL, "SNSReconcileIAP"},
    {S_N_S_RECONCILE_I_A_P_RESULT_FULL, "SNSReconcileIAPResult"},
    {S_N_S_REFRESH_PROFILE_FOR_USER_FULL, "SNSRefreshProfileForUser"},
    {S_N_S_REFRESH_PROFILE_FROM_SERVER_FULL, "SNSRefreshProfileFromServer"},
    {S_N_S_REFRESH_PROFILE_RESULT_FULL, "SNSRefreshProfileResult"},
    {S_N_S_REMOTE_LOG_SETV3_FULL, "SNSRemoteLogSetv3"},
    {S_N_S_REMOVE_TOURNAMENT_FULL, "SNSRemoveTournament"},
    {S_N_S_REWARDS_SETTINGS_FULL, "SNSRewardsSettings"},
    {S_N_S_SERVER_SETTINGS_RESPONSEV2_FULL, "SNSServerSettingsResponsev2"},
    {S_N_S_TELEMETRY_EVENT_FULL, "SNSTelemetryEvent"},
    {S_N_S_TELEMETRY_NOTIFY_FULL, "SNSTelemetryNotify"},
    {S_N_S_UPDATE_PROFILE_FULL, "SNSUpdateProfile"},
    {S_N_S_UPDATE_PROFILE_FAILURE_FULL, "SNSUpdateProfileFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE_FULL, "SNSUserServerProfileUpdateFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST_FULL, "SNSUserServerProfileUpdateRequest"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS_FULL, "SNSUserServerProfileUpdateSuccess"},
};

inline const std::unordered_map<uint64_t, std::string_view> NO_SNS_HASH_TO_NAME = {
    {S_N_S_ADD_TOURNAMENT_NO_SNS, "AddTournament"},
    {S_N_S_CHANNEL_INFO_REQUEST_NO_SNS, "ChannelInfoRequest"},
    {S_N_S_CHANNEL_INFO_RESPONSE_NO_SNS, "ChannelInfoResponse"},
    {S_N_S_CONFIG_FAILUREV2_NO_SNS, "ConfigFailurev2"},
    {S_N_S_CONFIG_REQUESTV2_NO_SNS, "ConfigRequestv2"},
    {S_N_S_CONFIG_SUCCESSV2_NO_SNS, "ConfigSuccessv2"},
    {S_N_S_DOCUMENT_FAILURE_NO_SNS, "DocumentFailure"},
    {S_N_S_DOCUMENT_REQUESTV2_NO_SNS, "DocumentRequestv2"},
    {S_N_S_DOCUMENT_SUCCESS_NO_SNS, "DocumentSuccess"},
    {S_N_S_EARLY_QUIT_CONFIG_NO_SNS, "EarlyQuitConfig"},
    {S_N_S_EARLY_QUIT_FEATURE_FLAGS_NO_SNS, "EarlyQuitFeatureFlags"},
    {S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION_NO_SNS, "EarlyQuitUpdateNotification"},
    {S_N_S_LEADERBOARD_REQUESTV2_NO_SNS, "LeaderboardRequestv2"},
    {S_N_S_LEADERBOARD_RESPONSE_NO_SNS, "LeaderboardResponse"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2_NO_SNS, "LobbyAcceptPlayersFailurev2"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2_NO_SNS, "LobbyAcceptPlayersSuccessv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2_NO_SNS, "LobbyAddEntrantAcceptedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2_NO_SNS, "LobbyAddEntrantRejectedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4_NO_SNS, "LobbyAddEntrantRequestv4"},
    {S_N_S_LOBBY_CHAT_ENTRY_NO_SNS, "LobbyChatEntry"},
    {S_N_S_LOBBY_DIRECTORY_JSON_NO_SNS, "LobbyDirectoryJson"},
    {S_N_S_LOBBY_ENTRANT_ACCEPTED_NO_SNS, "LobbyEntrantAccepted"},
    {S_N_S_LOBBY_ENTRANT_ADDED_NO_SNS, "LobbyEntrantAdded"},
    {S_N_S_LOBBY_ENTRANT_DATA_CHANGED_NO_SNS, "LobbyEntrantDataChanged"},
    {S_N_S_LOBBY_ENTRANT_REMOVED_NO_SNS, "LobbyEntrantRemoved"},
    {S_N_S_LOBBY_ENTRANT_STATEV2_NO_SNS, "LobbyEntrantStatev2"},
    {S_N_S_LOBBY_ENTRANT_TEAM_CHANGED_NO_SNS, "LobbyEntrantTeamChanged"},
    {S_N_S_LOBBY_HOST_CONNECT_FAILED_NO_SNS, "LobbyHostConnectFailed"},
    {S_N_S_LOBBY_HOST_CONNECTED_NO_SNS, "LobbyHostConnected"},
    {S_N_S_LOBBY_HOST_DATA_CHANGED_NO_SNS, "LobbyHostDataChanged"},
    {S_N_S_LOBBY_HOST_DISCONNECTED_NO_SNS, "LobbyHostDisconnected"},
    {S_N_S_LOBBY_HOST_STATE_NO_SNS, "LobbyHostState"},
    {S_N_S_LOBBY_JOIN_ACCEPTEDV2_NO_SNS, "LobbyJoinAcceptedv2"},
    {S_N_S_LOBBY_JOIN_COMPLETE_NO_SNS, "LobbyJoinComplete"},
    {S_N_S_LOBBY_JOIN_REJECTED_NO_SNS, "LobbyJoinRejected"},
    {S_N_S_LOBBY_JOIN_REJECTED_INTERNAL_NO_SNS, "LobbyJoinRejectedInternal"},
    {S_N_S_LOBBY_JOIN_REQUESTV4_NO_SNS, "LobbyJoinRequestv4"},
    {S_N_S_LOBBY_JOIN_SETUP_NO_SNS, "LobbyJoinSetup"},
    {S_N_S_LOBBY_LEFT_NO_SNS, "LobbyLeft"},
    {S_N_S_LOBBY_LOCK_ENTRANTS_NO_SNS, "LobbyLockEntrants"},
    {S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED_NO_SNS, "LobbyMatchmakerStatusReceived"},
    {S_N_S_LOBBY_OWNER_DATA_CHANGED_NO_SNS, "LobbyOwnerDataChanged"},
    {S_N_S_LOBBY_OWNER_STATE_NO_SNS, "LobbyOwnerState"},
    {S_N_S_LOBBY_PASS_OWNERSHIP_NO_SNS, "LobbyPassOwnership"},
    {S_N_S_LOBBY_PING_REQUESTV3_NO_SNS, "LobbyPingRequestv3"},
    {S_N_S_LOBBY_REGISTRATION_FAILURE_NO_SNS, "LobbyRegistrationFailure"},
    {S_N_S_LOBBY_REGISTRATION_SUCCESS_NO_SNS, "LobbyRegistrationSuccess"},
    {S_N_S_LOBBY_REMOVING_ENTRANT_NO_SNS, "LobbyRemovingEntrant"},
    {S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS_NO_SNS, "LobbySendClientLobbySettings"},
    {S_N_S_LOBBY_SESSION_ERROR_NO_SNS, "LobbySessionError"},
    {S_N_S_LOBBY_SESSION_FAILUREV4_NO_SNS, "LobbySessionFailurev4"},
    {S_N_S_LOBBY_SESSION_STARTING_NO_SNS, "LobbySessionStarting"},
    {S_N_S_LOBBY_SESSION_SUCCESSV5_NO_SNS, "LobbySessionSuccessv5"},
    {S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER_NO_SNS, "LobbySetSpawnBotOnServer"},
    {S_N_S_LOBBY_SETTINGS_RESPONSE_NO_SNS, "LobbySettingsResponse"},
    {S_N_S_LOBBY_SMITE_ENTRANT_NO_SNS, "LobbySmiteEntrant"},
    {S_N_S_LOBBY_START_SESSIONV4_NO_SNS, "LobbyStartSessionv4"},
    {S_N_S_LOBBY_STATUS_NOTIFYV2_NO_SNS, "LobbyStatusNotifyv2"},
    {S_N_S_LOBBY_TERMINATE_PROCESS_NO_SNS, "LobbyTerminateProcess"},
    {S_N_S_LOBBY_UNLOCK_ENTRANTS_NO_SNS, "LobbyUnlockEntrants"},
    {S_N_S_LOBBY_UPDATE_PINGS_NO_SNS, "LobbyUpdatePings"},
    {S_N_S_LOBBY_VOICE_ENTRY_NO_SNS, "LobbyVoiceEntry"},
    {S_N_S_LOGGED_IN_USER_PROFILE_REQUEST_NO_SNS, "LoggedInUserProfileRequest"},
    {S_N_S_LOGIN_PROFILE_RESULT_NO_SNS, "LoginProfileResult"},
    {S_N_S_LOGIN_SETTINGS_NO_SNS, "LoginSettings"},
    {S_N_S_MATCH_ENDEDV5_NO_SNS, "MatchEndedv5"},
    {S_N_S_NEW_UNLOCKS_NOTIFICATION_NO_SNS, "NewUnlocksNotification"},
    {S_N_S_OTHER_USER_PROFILE_FAILURE_NO_SNS, "OtherUserProfileFailure"},
    {S_N_S_OTHER_USER_PROFILE_REQUEST_NO_SNS, "OtherUserProfileRequest"},
    {S_N_S_OTHER_USER_PROFILE_SUCCESS_NO_SNS, "OtherUserProfileSuccess"},
    {S_N_S_PROFILE_RESPONSE_NO_SNS, "ProfileResponse"},
    {S_N_S_PURCHASE_ITEMS_NO_SNS, "PurchaseItems"},
    {S_N_S_PURCHASE_ITEMS_RESULT_NO_SNS, "PurchaseItemsResult"},
    {S_N_S_RECONCILE_I_A_P_NO_SNS, "ReconcileIAP"},
    {S_N_S_RECONCILE_I_A_P_RESULT_NO_SNS, "ReconcileIAPResult"},
    {S_N_S_REFRESH_PROFILE_FOR_USER_NO_SNS, "RefreshProfileForUser"},
    {S_N_S_REFRESH_PROFILE_FROM_SERVER_NO_SNS, "RefreshProfileFromServer"},
    {S_N_S_REFRESH_PROFILE_RESULT_NO_SNS, "RefreshProfileResult"},
    {S_N_S_REMOTE_LOG_SETV3_NO_SNS, "RemoteLogSetv3"},
    {S_N_S_REMOVE_TOURNAMENT_NO_SNS, "RemoveTournament"},
    {S_N_S_REWARDS_SETTINGS_NO_SNS, "RewardsSettings"},
    {S_N_S_SERVER_SETTINGS_RESPONSEV2_NO_SNS, "ServerSettingsResponsev2"},
    {S_N_S_TELEMETRY_EVENT_NO_SNS, "TelemetryEvent"},
    {S_N_S_TELEMETRY_NOTIFY_NO_SNS, "TelemetryNotify"},
    {S_N_S_UPDATE_PROFILE_NO_SNS, "UpdateProfile"},
    {S_N_S_UPDATE_PROFILE_FAILURE_NO_SNS, "UpdateProfileFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE_NO_SNS, "UserServerProfileUpdateFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST_NO_SNS, "UserServerProfileUpdateRequest"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS_NO_SNS, "UserServerProfileUpdateSuccess"},
};

inline const std::unordered_map<uint64_t, std::string_view> S_PREFIX_HASH_TO_NAME = {
    {S_N_S_ADD_TOURNAMENT_S_PREFIX, "SAddTournament"},
    {S_N_S_CHANNEL_INFO_REQUEST_S_PREFIX, "SChannelInfoRequest"},
    {S_N_S_CHANNEL_INFO_RESPONSE_S_PREFIX, "SChannelInfoResponse"},
    {S_N_S_CONFIG_FAILUREV2_S_PREFIX, "SConfigFailurev2"},
    {S_N_S_CONFIG_REQUESTV2_S_PREFIX, "SConfigRequestv2"},
    {S_N_S_CONFIG_SUCCESSV2_S_PREFIX, "SConfigSuccessv2"},
    {S_N_S_DOCUMENT_FAILURE_S_PREFIX, "SDocumentFailure"},
    {S_N_S_DOCUMENT_REQUESTV2_S_PREFIX, "SDocumentRequestv2"},
    {S_N_S_DOCUMENT_SUCCESS_S_PREFIX, "SDocumentSuccess"},
    {S_N_S_EARLY_QUIT_CONFIG_S_PREFIX, "SEarlyQuitConfig"},
    {S_N_S_EARLY_QUIT_FEATURE_FLAGS_S_PREFIX, "SEarlyQuitFeatureFlags"},
    {S_N_S_EARLY_QUIT_UPDATE_NOTIFICATION_S_PREFIX, "SEarlyQuitUpdateNotification"},
    {S_N_S_LEADERBOARD_REQUESTV2_S_PREFIX, "SLeaderboardRequestv2"},
    {S_N_S_LEADERBOARD_RESPONSE_S_PREFIX, "SLeaderboardResponse"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_FAILUREV2_S_PREFIX, "SLobbyAcceptPlayersFailurev2"},
    {S_N_S_LOBBY_ACCEPT_PLAYERS_SUCCESSV2_S_PREFIX, "SLobbyAcceptPlayersSuccessv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_ACCEPTEDV2_S_PREFIX, "SLobbyAddEntrantAcceptedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REJECTEDV2_S_PREFIX, "SLobbyAddEntrantRejectedv2"},
    {S_N_S_LOBBY_ADD_ENTRANT_REQUESTV4_S_PREFIX, "SLobbyAddEntrantRequestv4"},
    {S_N_S_LOBBY_CHAT_ENTRY_S_PREFIX, "SLobbyChatEntry"},
    {S_N_S_LOBBY_DIRECTORY_JSON_S_PREFIX, "SLobbyDirectoryJson"},
    {S_N_S_LOBBY_ENTRANT_ACCEPTED_S_PREFIX, "SLobbyEntrantAccepted"},
    {S_N_S_LOBBY_ENTRANT_ADDED_S_PREFIX, "SLobbyEntrantAdded"},
    {S_N_S_LOBBY_ENTRANT_DATA_CHANGED_S_PREFIX, "SLobbyEntrantDataChanged"},
    {S_N_S_LOBBY_ENTRANT_REMOVED_S_PREFIX, "SLobbyEntrantRemoved"},
    {S_N_S_LOBBY_ENTRANT_STATEV2_S_PREFIX, "SLobbyEntrantStatev2"},
    {S_N_S_LOBBY_ENTRANT_TEAM_CHANGED_S_PREFIX, "SLobbyEntrantTeamChanged"},
    {S_N_S_LOBBY_HOST_CONNECT_FAILED_S_PREFIX, "SLobbyHostConnectFailed"},
    {S_N_S_LOBBY_HOST_CONNECTED_S_PREFIX, "SLobbyHostConnected"},
    {S_N_S_LOBBY_HOST_DATA_CHANGED_S_PREFIX, "SLobbyHostDataChanged"},
    {S_N_S_LOBBY_HOST_DISCONNECTED_S_PREFIX, "SLobbyHostDisconnected"},
    {S_N_S_LOBBY_HOST_STATE_S_PREFIX, "SLobbyHostState"},
    {S_N_S_LOBBY_JOIN_ACCEPTEDV2_S_PREFIX, "SLobbyJoinAcceptedv2"},
    {S_N_S_LOBBY_JOIN_COMPLETE_S_PREFIX, "SLobbyJoinComplete"},
    {S_N_S_LOBBY_JOIN_REJECTED_S_PREFIX, "SLobbyJoinRejected"},
    {S_N_S_LOBBY_JOIN_REJECTED_INTERNAL_S_PREFIX, "SLobbyJoinRejectedInternal"},
    {S_N_S_LOBBY_JOIN_REQUESTV4_S_PREFIX, "SLobbyJoinRequestv4"},
    {S_N_S_LOBBY_JOIN_SETUP_S_PREFIX, "SLobbyJoinSetup"},
    {S_N_S_LOBBY_LEFT_S_PREFIX, "SLobbyLeft"},
    {S_N_S_LOBBY_LOCK_ENTRANTS_S_PREFIX, "SLobbyLockEntrants"},
    {S_N_S_LOBBY_MATCHMAKER_STATUS_RECEIVED_S_PREFIX, "SLobbyMatchmakerStatusReceived"},
    {S_N_S_LOBBY_OWNER_DATA_CHANGED_S_PREFIX, "SLobbyOwnerDataChanged"},
    {S_N_S_LOBBY_OWNER_STATE_S_PREFIX, "SLobbyOwnerState"},
    {S_N_S_LOBBY_PASS_OWNERSHIP_S_PREFIX, "SLobbyPassOwnership"},
    {S_N_S_LOBBY_PING_REQUESTV3_S_PREFIX, "SLobbyPingRequestv3"},
    {S_N_S_LOBBY_REGISTRATION_FAILURE_S_PREFIX, "SLobbyRegistrationFailure"},
    {S_N_S_LOBBY_REGISTRATION_SUCCESS_S_PREFIX, "SLobbyRegistrationSuccess"},
    {S_N_S_LOBBY_REMOVING_ENTRANT_S_PREFIX, "SLobbyRemovingEntrant"},
    {S_N_S_LOBBY_SEND_CLIENT_LOBBY_SETTINGS_S_PREFIX, "SLobbySendClientLobbySettings"},
    {S_N_S_LOBBY_SESSION_ERROR_S_PREFIX, "SLobbySessionError"},
    {S_N_S_LOBBY_SESSION_FAILUREV4_S_PREFIX, "SLobbySessionFailurev4"},
    {S_N_S_LOBBY_SESSION_STARTING_S_PREFIX, "SLobbySessionStarting"},
    {S_N_S_LOBBY_SESSION_SUCCESSV5_S_PREFIX, "SLobbySessionSuccessv5"},
    {S_N_S_LOBBY_SET_SPAWN_BOT_ON_SERVER_S_PREFIX, "SLobbySetSpawnBotOnServer"},
    {S_N_S_LOBBY_SETTINGS_RESPONSE_S_PREFIX, "SLobbySettingsResponse"},
    {S_N_S_LOBBY_SMITE_ENTRANT_S_PREFIX, "SLobbySmiteEntrant"},
    {S_N_S_LOBBY_START_SESSIONV4_S_PREFIX, "SLobbyStartSessionv4"},
    {S_N_S_LOBBY_STATUS_NOTIFYV2_S_PREFIX, "SLobbyStatusNotifyv2"},
    {S_N_S_LOBBY_TERMINATE_PROCESS_S_PREFIX, "SLobbyTerminateProcess"},
    {S_N_S_LOBBY_UNLOCK_ENTRANTS_S_PREFIX, "SLobbyUnlockEntrants"},
    {S_N_S_LOBBY_UPDATE_PINGS_S_PREFIX, "SLobbyUpdatePings"},
    {S_N_S_LOBBY_VOICE_ENTRY_S_PREFIX, "SLobbyVoiceEntry"},
    {S_N_S_LOGGED_IN_USER_PROFILE_REQUEST_S_PREFIX, "SLoggedInUserProfileRequest"},
    {S_N_S_LOGIN_PROFILE_RESULT_S_PREFIX, "SLoginProfileResult"},
    {S_N_S_LOGIN_SETTINGS_S_PREFIX, "SLoginSettings"},
    {S_N_S_MATCH_ENDEDV5_S_PREFIX, "SMatchEndedv5"},
    {S_N_S_NEW_UNLOCKS_NOTIFICATION_S_PREFIX, "SNewUnlocksNotification"},
    {S_N_S_OTHER_USER_PROFILE_FAILURE_S_PREFIX, "SOtherUserProfileFailure"},
    {S_N_S_OTHER_USER_PROFILE_REQUEST_S_PREFIX, "SOtherUserProfileRequest"},
    {S_N_S_OTHER_USER_PROFILE_SUCCESS_S_PREFIX, "SOtherUserProfileSuccess"},
    {S_N_S_PROFILE_RESPONSE_S_PREFIX, "SProfileResponse"},
    {S_N_S_PURCHASE_ITEMS_S_PREFIX, "SPurchaseItems"},
    {S_N_S_PURCHASE_ITEMS_RESULT_S_PREFIX, "SPurchaseItemsResult"},
    {S_N_S_RECONCILE_I_A_P_S_PREFIX, "SReconcileIAP"},
    {S_N_S_RECONCILE_I_A_P_RESULT_S_PREFIX, "SReconcileIAPResult"},
    {S_N_S_REFRESH_PROFILE_FOR_USER_S_PREFIX, "SRefreshProfileForUser"},
    {S_N_S_REFRESH_PROFILE_FROM_SERVER_S_PREFIX, "SRefreshProfileFromServer"},
    {S_N_S_REFRESH_PROFILE_RESULT_S_PREFIX, "SRefreshProfileResult"},
    {S_N_S_REMOTE_LOG_SETV3_S_PREFIX, "SRemoteLogSetv3"},
    {S_N_S_REMOVE_TOURNAMENT_S_PREFIX, "SRemoveTournament"},
    {S_N_S_REWARDS_SETTINGS_S_PREFIX, "SRewardsSettings"},
    {S_N_S_SERVER_SETTINGS_RESPONSEV2_S_PREFIX, "SServerSettingsResponsev2"},
    {S_N_S_TELEMETRY_EVENT_S_PREFIX, "STelemetryEvent"},
    {S_N_S_TELEMETRY_NOTIFY_S_PREFIX, "STelemetryNotify"},
    {S_N_S_UPDATE_PROFILE_S_PREFIX, "SUpdateProfile"},
    {S_N_S_UPDATE_PROFILE_FAILURE_S_PREFIX, "SUpdateProfileFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_FAILURE_S_PREFIX, "SUserServerProfileUpdateFailure"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_REQUEST_S_PREFIX, "SUserServerProfileUpdateRequest"},
    {S_N_S_USER_SERVER_PROFILE_UPDATE_SUCCESS_S_PREFIX, "SUserServerProfileUpdateSuccess"},
};

}  // namespace SNS
}  // namespace EchoVR