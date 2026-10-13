#pragma once

#include "echovr.h"

namespace EchoVR::Symbols {

// Internal broadcaster message symbols (UDP local events)

// Registration/session lifecycle
constexpr SymbolId LobbyRegistrationSuccess = 0xFEF8EFEC97A3B98;
constexpr SymbolId LobbyRegistrationFailure = 0xCC3A40870CDBC852;
constexpr SymbolId LobbySessionStarting = 0x233E6E7E3A13BABC;
constexpr SymbolId LobbySessionError = 0x425393736F0CDB8B;
constexpr SymbolId LobbyTerminateProcess = 0xF0FA52B6F8A33A49;

// Session management
constexpr SymbolId LobbyStartSessionV4 = 0x96101C684E7F325;
constexpr SymbolId LobbyJoinRequestedV4 = 0xB4D724E8564BFE88;
constexpr SymbolId LobbyAddEntrantRequestV4 = 0x5A44AB3E283D136D;
constexpr SymbolId LobbySessionSuccessV5 = 0x83E96504A9FC81C6;
constexpr SymbolId LobbyAcceptPlayersSuccessV2 = 0xFCBA8F2834F8DE40;
constexpr SymbolId LobbyAcceptPlayersFailureV2 = 0xED9A4B86F8F3640A;
constexpr SymbolId LobbySmiteEntrant = 0xCCBC52F97F2E0EF1;
constexpr SymbolId LobbyChatEntry = 0xDCB7130D1BEB9AC4;
constexpr SymbolId LobbyVoiceEntry = 0x27504F14881C1A43;

// Loadout/cosmetics (R15Net* messages)
constexpr SymbolId SaveLoadoutRequest = 0xa3749626d6d20ef7;
constexpr SymbolId SaveLoadoutSuccess = 0xdb64d31f58e19f74;
constexpr SymbolId SaveLoadoutPartial = 0x0d12f3b7fa1d20a0;
constexpr SymbolId CurrentLoadoutRequest = 0xc66ca34bb55fdc28;
constexpr SymbolId CurrentLoadoutResponse = 0xa52712ec66b2db48;

// Profile refresh (NS* messages)
constexpr SymbolId RefreshProfileForUser = 0x4aa419e5505280e9;
constexpr SymbolId RefreshProfileFromServer = 0x06877b856e8d785f;

// Lobby settings
constexpr SymbolId LobbySendClientLobbySettings = 0xd26c901b8d4c56a6;

// Rewards and progression
constexpr SymbolId TierRewardMsg = 0xb3acdcc23a081c92;
constexpr SymbolId TopAwardsMsg = 0xf28582c141e30960;
constexpr SymbolId NewUnlocks = 0x187008fe3940a327;

// Statistics
constexpr SymbolId ReliableStatUpdate = 0x5d84b0c2d4bb6cfc;
constexpr SymbolId ReliableTeamStatUpdate = 0x7ba444f7d419b1a2;

// Protobuf transport
constexpr SymbolId ProtobufMsg = 0xf62cb25bb5b8a839;
constexpr SymbolId ProtobufJson = 0x59f3af95e7d44e72;

// NEVR-specific protobuf JSON transport (for Nakama integration)
constexpr SymbolId NEVRProtobufJSONMessageV1 = 0xc6b3710cd9c4ef47;

// ============================================================================
// Remaining message types of the game's CMatSym registry.
// Each value is CMatSym::Hash of the string in its trailing comment (the
// registry's own spelling, which the identifier does not always repeat).
// tools/tests/test_abi_symbols.py recomputes every constant in this file and refuses a
// SymbolId declaration in a form it cannot read.
// ============================================================================

// Broadcaster events (SBroadcaster*)
constexpr SymbolId BroadcasterConnectEvent = 0x5069b99c8b15cbd8;  // SBroadcasterConnectEvent
constexpr SymbolId BroadcasterConnectFailedEvent = 0x0fed6de7c416da1f;  // SBroadcasterConnectFailedEvent
constexpr SymbolId BroadcasterDisconnectEvent = 0xe24e16d850752b9a;  // SBroadcasterDisconnectEvent
constexpr SymbolId BroadcasterEnableCompressionEvent_1 = 0x4f513db80cc3e10a;  // SBroadcasterEnableCompressionEvent_1
constexpr SymbolId BroadcasterIntroduceAckEvent = 0x8ecbe7fe6b311ceb;  // SBroadcasterIntroduceAckEvent
constexpr SymbolId BroadcasterIntroduceFailEvent = 0x7cb6d548e883fa5d;  // SBroadcasterIntroduceFailEvent
constexpr SymbolId BroadcasterIntroduceFinEvent = 0x4e66b43b6d2ebb5c;  // SBroadcasterIntroduceFinEvent
constexpr SymbolId BroadcasterIntroduceReqEvent = 0x9f28ab06bf7e1fce;  // SBroadcasterIntroduceReqEvent
constexpr SymbolId BroadcasterMailboxBatchEvent = 0xc951b3fbe4b83d76;  // SBroadcasterMailboxBatchEvent
constexpr SymbolId BroadcasterMsgEncodingFailEvent = 0x59e4c5ea6e01083b;  // SBroadcasterMsgEncodingFailEvent
constexpr SymbolId BroadcasterMsgEncodingSupportEvent_1 = 0x1175ae5766f221aa;  // SBroadcasterMsgEncodingSupportEvent_1
constexpr SymbolId BroadcasterPingUpdate = 0x014c925361833bf6;  // SBroadcasterPingUpdate
constexpr SymbolId BroadcasterReliableBatchEvent = 0x2cd38a848b5b9c73;  // SBroadcasterReliableBatchEvent
constexpr SymbolId BroadcasterRequestEncodingResetEvent = 0x74c962b8ad22c433;  // SBroadcasterRequestEncodingResetEvent
constexpr SymbolId BroadcasterResetEncodingEvent = 0x2a4106dcd57edeca;  // SBroadcasterResetEncodingEvent

// Lobby and session (SNS*)
constexpr SymbolId LobbyAddEntrantAcceptedV2 = 0x3b54f90b60ef1710;  // SNSLobbyAddEntrantAcceptedv2
constexpr SymbolId LobbyAddEntrantRejectedV2 = 0xb83f5f68da7ff684;  // SNSLobbyAddEntrantRejectedv2
constexpr SymbolId LobbyDirectoryJson = 0x317e584e465f65f4;  // SNSLobbyDirectoryJson
constexpr SymbolId LobbyEntrantAccepted = 0xb960a3492fe646b7;  // SNSLobbyEntrantAccepted
constexpr SymbolId LobbyEntrantAdded = 0x299283a7141dbce2;  // SNSLobbyEntrantAdded
constexpr SymbolId LobbyEntrantDataChanged = 0x8f05b6735e916658;  // SNSLobbyEntrantDataChanged
constexpr SymbolId LobbyEntrantRemoved = 0x39156563932117d6;  // SNSLobbyEntrantRemoved
constexpr SymbolId LobbyEntrantStateV2 = 0xeb6218834b3fa648;  // SNSLobbyEntrantStatev2
constexpr SymbolId LobbyEntrantTeamChanged = 0xceb8759c36184b82;  // SNSLobbyEntrantTeamChanged
constexpr SymbolId LobbyHostConnectFailed = 0x0ba12ca642569be5;  // SNSLobbyHostConnectFailed
constexpr SymbolId LobbyHostConnected = 0x8475c9362c5b6a1e;  // SNSLobbyHostConnected
constexpr SymbolId LobbyHostDataChanged = 0xe0f632824ebdf11d;  // SNSLobbyHostDataChanged
constexpr SymbolId LobbyHostDisconnected = 0x9d73acf6ad59040d;  // SNSLobbyHostDisconnected
constexpr SymbolId LobbyHostState = 0x174ea9b31846fe3a;  // SNSLobbyHostState
constexpr SymbolId LobbyJoinAcceptedV2 = 0xb23e3c1167ec7a79;  // SNSLobbyJoinAcceptedv2
constexpr SymbolId LobbyJoinComplete = 0x65b2feba429e1b02;  // SNSLobbyJoinComplete
constexpr SymbolId LobbyJoinRejected = 0x22409acd8ac6cbad;  // SNSLobbyJoinRejected
constexpr SymbolId LobbyJoinRejectedInternal = 0x370623474348a8db;  // SNSLobbyJoinRejectedInternal
constexpr SymbolId LobbyJoinSetup = 0xb3e2b4258ff860e4;  // SNSLobbyJoinSetup
constexpr SymbolId LobbyLeft = 0x5ab932231c71660a;  // SNSLobbyLeft
constexpr SymbolId LobbyLockEntrants = 0x2dd64ecd5b5f421c;  // SNSLobbyLockEntrants
constexpr SymbolId LobbyMatchmakerStatusReceived = 0xce66e4786f0b118a;  // SNSLobbyMatchmakerStatusReceived
constexpr SymbolId LobbyOwnerDataChanged = 0xadc58c2466ed01b6;  // SNSLobbyOwnerDataChanged
constexpr SymbolId LobbyOwnerState = 0x2068c0f03b4b8f25;  // SNSLobbyOwnerState
constexpr SymbolId LobbyPassOwnership = 0xf7ca39f0e61baa95;  // SNSLobbyPassOwnership
constexpr SymbolId LobbyRemovingEntrant = 0xf75c4972e6267c4e;  // SNSLobbyRemovingEntrant
constexpr SymbolId LobbySessionFailureV4 = 0x4554f516a438c623;  // SNSLobbySessionFailurev4
constexpr SymbolId LobbySetSpawnBotOnServer = 0xa0687d9799640878;  // SNSLobbySetSpawnBotOnServer
constexpr SymbolId LobbySettingsResponse = 0x4cd5b97b2b97d9a3;  // SNSLobbySettingsResponse
constexpr SymbolId LobbyUnlockEntrants = 0xfc250b496eddbfa7;  // SNSLobbyUnlockEntrants
constexpr SymbolId LobbyUpdatePings = 0x03e01d2bd46a86c0;  // SNSLobbyUpdatePings

// Gameplay and replication (SR15Net*)
constexpr SymbolId AbilityActivate = 0x06604436ae89aae0;  // SR15NetAbilityActivate
constexpr SymbolId AbilityCooldown = 0xc4ad1c91b858ef08;  // SR15NetAbilityCooldown
constexpr SymbolId ActivateExplosion = 0x36ad7f161cfba5a2;  // SR15NetActivateExplosion
constexpr SymbolId AutoTargetFireBullet = 0x95bcc1e3fec575b2;  // SR15NetAutoTargetFireBullet
constexpr SymbolId BalanceSettingsClientReady = 0x10ec6147d112e1e4;  // SR15NetBalanceSettingsClientReady
constexpr SymbolId BalanceSettingsMassUpdate = 0xe9a4b52cba8b27e7;  // SR15NetBalanceSettingsMassUpdate
constexpr SymbolId BalanceSettingsUpdate = 0xf81ca8ddf9c39fec;  // SR15NetBalanceSettingsUpdate
constexpr SymbolId BitFieldRequest = 0x06bc9290c472273a;  // SR15NetBitFieldRequest
constexpr SymbolId BitFieldResponse = 0x13f6ad878f60cb05;  // SR15NetBitFieldResponse
constexpr SymbolId BotActivateScenario = 0x0fdaad6014282fa7;  // SR15NetBotActivateScenario
constexpr SymbolId BotDisableScenario = 0xdc9c4af990c331b7;  // SR15NetBotDisableScenario
constexpr SymbolId BotRPC = 0xd29e69cbb5e9f53a;  // SR15NetBotRPC
constexpr SymbolId BotSetGear = 0x8d838b6e4df79b4e;  // SR15NetBotSetGear
constexpr SymbolId CancelGoal = 0x6af7784293e2edab;  // SR15NetCancelGoal
constexpr SymbolId CaptureVolumeReset = 0x3125c641e7b157bd;  // SR15NetCaptureVolumeReset
constexpr SymbolId CaptureVolumeSpoofClaim = 0x95206ad1af54d6dd;  // SR15NetCaptureVolumeSpoofClaim
constexpr SymbolId CaptureVolumeUpdate = 0x9fd63bf21c96ef8e;  // SR15NetCaptureVolumeUpdate
constexpr SymbolId ClientPingResponseMsg = 0x67232ca3535df81c;  // SR15NetClientPingResponseMsg
constexpr SymbolId ComponentEvent = 0x6e54d0d298bffb17;  // SR15NetComponentEvent
constexpr SymbolId CooldownActive = 0x39a7e87c23af7ac6;  // SR15NetCooldownActive
constexpr SymbolId CooldownRefund = 0x795fa17c1eb9ec3a;  // SR15NetCooldownRefund
constexpr SymbolId CooldownRefundRequest = 0x440b3c21955733f8;  // SR15NetCooldownRefundRequest
constexpr SymbolId DamageableBulletCollision = 0xcf5e411193b929f3;  // SR15NetDamageableBulletCollision
constexpr SymbolId DamageableDamageDealt = 0x534e3b3f0f17e56b;  // SR15NetDamageableDamageDealt
constexpr SymbolId DamageableDealDamage = 0xe669be764c8a1db6;  // SR15NetDamageableDealDamage
constexpr SymbolId DamageableHealthBroadcast = 0x97bbbd245d467354;  // SR15NetDamageableHealthBroadcast
constexpr SymbolId DamageableLocallyTrackedDamage = 0x690bb5171245edf2;  // SR15NetDamageableLocallyTrackedDamage
constexpr SymbolId DamageablePlayerKilled = 0xba9db3f79fa8a511;  // SR15NetDamageablePlayerKilled
constexpr SymbolId DamageablePlayerRespawn = 0xd2b758303e284785;  // SR15NetDamageablePlayerRespawn
constexpr SymbolId DamageablePlayerRespawnComplete = 0xcad5776e4afc09d2;  // SR15NetDamageablePlayerRespawnComplete
constexpr SymbolId DamageableShieldRepaired = 0x87e6b4fa40a91118;  // SR15NetDamageableShieldRepaired
constexpr SymbolId ExplosionNothingHit = 0x7da3136ff17537ed;  // SR15NetExplosionNothingHit
constexpr SymbolId ExplosionPlayerDamage = 0xc9bf5b1dbb888def;  // SR15NetExplosionPlayerDamage
constexpr SymbolId FrisbeeServerGoaliePingWindowReliable = 0xc5f625b6c1903b63;  // SR15NetFrisbeeServerGoaliePingWindowReliable
constexpr SymbolId GameActivateHealBubbleMsg = 0xc472a1be2ea5776e;  // SR15NetGameActivateHealBubbleMsg
constexpr SymbolId GameCancelFireGunMsg = 0x9ff4df8105525589;  // SR15NetGameCancelFireGunMsg
constexpr SymbolId GameDepositMagazine = 0x56e20c9ef59659ef;  // SR15NetGameDepositMagazine
constexpr SymbolId GameDestroyMagazine = 0x94ff8d9ddf80936d;  // SR15NetGameDestroyMagazine
constexpr SymbolId GameFireGun = 0xe22470ae4641ee83;  // SR15NetGameFireGun
constexpr SymbolId GameInputFireGunMsg = 0x58dfab8175b5a7c9;  // SR15NetGameInputFireGunMsg
constexpr SymbolId GameInputPullPinMsg = 0x2adef00270bb598d;  // SR15NetGameInputPullPinMsg
constexpr SymbolId GameReplicateFireGunMsg = 0x36db7e68743f5ffc;  // SR15NetGameReplicateFireGunMsg
constexpr SymbolId GameReplicatePullPinMsg = 0xfe6f71222eb8826a;  // SR15NetGameReplicatePullPinMsg
constexpr SymbolId GameSetMagazineState = 0x5186cb60ac8ef1a2;  // SR15NetGameSetMagazineState
constexpr SymbolId GameSpawnMagazine = 0x9cd4195be16305e5;  // SR15NetGameSpawnMagazine
constexpr SymbolId GameSynchronizeCreateMagazine = 0xaa728fdd4683c11b;  // SR15NetGameSynchronizeCreateMagazine
constexpr SymbolId GameplaySensorTriggered = 0x889bac7502b461f5;  // SR15NetGameplaySensorTriggered
constexpr SymbolId GameplayTeleportPlayer = 0x4d7fcb464e548b84;  // SR15NetGameplayTeleportPlayer
constexpr SymbolId GameplayTeleportPlayerRequest = 0x0fbac5d994dde2e8;  // SR15NetGameplayTeleportPlayerRequest
constexpr SymbolId HealBubbleParamUpdate = 0x40b2dd75fc72e38f;  // SR15NetHealBubbleParamUpdate
constexpr SymbolId IdsRequest = 0xc4fe7b57caab27e2;  // SR15NetIdsRequest
constexpr SymbolId IdsResponse = 0x8f8af081af21cbe7;  // SR15NetIdsResponse
constexpr SymbolId ItemEquippedNotification = 0xed0fba849badc7bf;  // SR15NetItemEquippedNotification
constexpr SymbolId LeaderboardMsg = 0x2ae390e83284b8aa;  // SR15NetLeaderboardMsg
constexpr SymbolId LoadoutNumberNotification = 0x80af66bf1e67b474;  // SR15NetLoadoutNumberNotification
constexpr SymbolId PayloadPosStateUpdateReliable = 0x3ef8a408b935f46b;  // SR15NetPayloadPosStateUpdateReliable
constexpr SymbolId PayloadRequestAutodriveChangeReliable = 0xa9936ede72920da7;  // SR15NetPayloadRequestAutodriveChangeReliable
constexpr SymbolId PayloadRequestSpoofedClaimChangeReliable = 0xb5d773ce1f4be6cb;  // SR15NetPayloadRequestSpoofedClaimChangeReliable
constexpr SymbolId PayloadTrackpointReachedReliable = 0x2b5e8480b5fbd1e3;  // SR15NetPayloadTrackpointReachedReliable
constexpr SymbolId PayloadUpdateMiscStateReliable = 0x17d4cde80c164a9a;  // SR15NetPayloadUpdateMiscStateReliable
constexpr SymbolId PayloadUpdatedAutodriveReliable = 0x66fac59368547aee;  // SR15NetPayloadUpdatedAutodriveReliable
constexpr SymbolId PayloadUpdatedSpoofedClaimersReliable = 0xd172a6bbb6077925;  // SR15NetPayloadUpdatedSpoofedClaimersReliable
constexpr SymbolId PhysicsSimStateEvent = 0x8f62adba23f15fe0;  // SR15NetPhysicsSimStateEvent
constexpr SymbolId PlayEmote = 0x7c63c5877b0eee97;  // SR15NetPlayEmote
constexpr SymbolId PlayerEquipmentUpdate = 0x71c59625b07711b9;  // SR15NetPlayerEquipmentUpdate
constexpr SymbolId PlayerPausedMsg = 0xe9426cf2f6cfb73c;  // SR15NetPlayerPausedMsg
constexpr SymbolId PlayerStateMsg = 0xc6bb60e8d9e9a3d3;  // SR15NetPlayerStateMsg
constexpr SymbolId PooledActorCreate = 0xe82df95823db043b;  // SR15NetPooledActorCreate
constexpr SymbolId PooledActorDestroy = 0xf477a3ea01bf0489;  // SR15NetPooledActorDestroy
constexpr SymbolId PropertyModifiersAdd = 0xc7227611c153e9de;  // SR15NetPropertyModifiersAdd
constexpr SymbolId ReliablePostScoreUpdate = 0xb9f8831ab6e068a9;  // SR15NetReliablePostScoreUpdate
constexpr SymbolId ReplicatedClientReady = 0xa026b3042add4e14;  // SR15NetReplicatedClientReady
constexpr SymbolId ReplicatedDeltaAck = 0xd0aead470b6648b8;  // SR15NetReplicatedDeltaAck
constexpr SymbolId ReplicatedDeltaUnreliableVars = 0x2ded551babc46dbf;  // SR15NetReplicatedDeltaUnreliableVars
constexpr SymbolId RequestUserApplyEntrant = 0xe4c3e33e6d85a049;  // SR15NetRequestUserApplyEntrant
constexpr SymbolId SaveLoadoutFailure = 0x3eaa5d81cd06fde7;  // SR15NetSaveLoadoutFailure
constexpr SymbolId SensorPing = 0x2050928ad5a3b7d4;  // SR15NetSensorPing
constexpr SymbolId ServerFrameMsg = 0xc57ec38913be8582;  // SR15NetServerFrameMsg
constexpr SymbolId ServerInfo = 0xfa563ae00ebff2ba;  // SR15NetServerInfo
constexpr SymbolId SetBits = 0xb043bde1197c11f1;  // SR15NetSetBits
constexpr SymbolId SetOwner = 0x4a2953dc4d5fbaed;  // SR15NetSetOwner
constexpr SymbolId SocketLink = 0x7b018e4c8a2badcd;  // SR15NetSocketLink
constexpr SymbolId SocketUnlink = 0xca7ebf198d10a8a7;  // SR15NetSocketUnlink
constexpr SymbolId TetherActivate = 0x2ee0c0db2ce55ada;  // SR15NetTetherActivate
constexpr SymbolId TetherConnected = 0x8886462c18d50130;  // SR15NetTetherConnected
constexpr SymbolId TetherConnectingUpdate = 0x54a9c1f67ca0a769;  // SR15NetTetherConnectingUpdate
constexpr SymbolId TetherConnectionBroken = 0x05fc60a690faebbf;  // SR15NetTetherConnectionBroken
constexpr SymbolId TetherFoundTarget = 0xd6bfbb958e7ba892;  // SR15NetTetherFoundTarget
constexpr SymbolId TetherParamUpdate = 0x8608d39bb1844776;  // SR15NetTetherParamUpdate
constexpr SymbolId TouchOwnershipDeclined = 0x8255a3b48e69fd70;  // SR15NetTouchOwnershipDeclined
constexpr SymbolId TouchOwnershipGranted = 0x84745d47a4b31f27;  // SR15NetTouchOwnershipGranted
constexpr SymbolId TouchOwnershipTransferMailbox = 0x60fd8780fe4017f5;  // SR15NetTouchOwnershipTransferMailbox
constexpr SymbolId TouchOwnershipTransferReliable = 0x6f248c06a631d7cb;  // SR15NetTouchOwnershipTransferReliable
constexpr SymbolId TouchStateEvent = 0x6d9a367ee21a5642;  // SR15NetTouchStateEvent
constexpr SymbolId UIDecSettingMsg = 0x1ef9c84f7285f216;  // SR15NetUIDecSettingMsg
constexpr SymbolId UIIncSettingMsg = 0xe7ece98b6a0ac2b6;  // SR15NetUIIncSettingMsg
constexpr SymbolId UIReplJSONSettings = 0xd69fb5820373ee18;  // SR15NetUIReplJSONSettings
constexpr SymbolId UIUserChangedSettings = 0x032d1569cbfd89bd;  // SR15NetUIUserChangedSettings
constexpr SymbolId UnreliableStatUpdate = 0x3fe43afc2a73c712;  // SR15NetUnreliableStatUpdate
constexpr SymbolId UserAssignTeamRequest = 0xab60f5197bf1f96b;  // SR15NetUserAssignTeamRequest
constexpr SymbolId UserAssignTeamResponse = 0x212d30f7f0093a97;  // SR15NetUserAssignTeamResponse
constexpr SymbolId UserPingEstMsg = 0xf85fd7296f268b1b;  // SR15NetUserPingEstMsg
constexpr SymbolId UserVoipEvent = 0x599b9fd3de2fa94f;  // SR15NetUserVoipEvent

// Gameplay (SR15*)
constexpr SymbolId ForcePushPlayer = 0x1e7cda39027bb6cc;  // SR15ForcePushPlayer
constexpr SymbolId RemoveStun = 0x0d82d66c7ebdc8f5;  // SR15RemoveStun
constexpr SymbolId RequestDebugInputPlayback = 0xacb4eac11508be1b;  // SR15RequestDebugInputPlayback
constexpr SymbolId RequestDebugInputRecording = 0x0bd85a5dd54bacd6;  // SR15RequestDebugInputRecording
constexpr SymbolId StartDebugInputPlayback = 0xd22e545ca6894824;  // SR15StartDebugInputPlayback
constexpr SymbolId StartDebugInputRecording = 0xc04af6bd7f0f523e;  // SR15StartDebugInputRecording
constexpr SymbolId StunMsg = 0x1feb495968559bc3;  // SR15StunMsg
constexpr SymbolId StunPlayerMsg = 0xccd9ed2d3125ff67;  // SR15StunPlayerMsg

// ============================================================================
// Social / Friends (pnsrad SNS messages)
// Source: echovr-reconstruction/src/pnsrad/Social/CNSRADFriends_protocol.h
// ============================================================================

namespace Social {

// --- Friends outgoing (pnsrad → server, 3 unique hashes) ---
constexpr SymbolId FriendSendInviteRequest   = 0x7f0d7a28de3c6f70;  // add_friend_by_id/name; hashes CSymbol64("SNSFriendInviteRequest")
constexpr SymbolId FriendRemoveRequest       = 0x1bbcb7e810af4620;  // remove_friend (shared w/ Party Join); hashes CSymbol64("SNSFriendAcceptRequest")
constexpr SymbolId FriendActionRequest       = 0x78908988b7fe6db4;  // accept/reject/block (shared w/ Party Leave); hashes CSymbol64("SNSFriendRemoveRequest")

// --- Friends inbound (server → pnsrad, 12 callback hashes) ---
constexpr SymbolId FriendListResponse        = 0xa78aeb2a4e89b10b;  // SNSFriendsListResponse (0x20); hashes CSymbol64("SNSFriendListResponse")
constexpr SymbolId FriendStatusNotify        = 0x26a19dc4d2d5579d;  // SNSFriendNotifyPayload (0x18); hashes CSymbol64("SNSFriendStatusNotify")
constexpr SymbolId FriendInviteSuccess       = 0x7f0c6a3ac83c6f77;  // SNSFriendIdPayload (0x10); hashes CSymbol64("SNSFriendInviteSuccess")
constexpr SymbolId FriendInviteFailure       = 0x7f197e30c72c6e61;  // SNSFriendNotifyPayload (0x18); hashes CSymbol64("SNSFriendInviteFailure")
constexpr SymbolId FriendInviteNotify        = 0xca09b0b36bd981b7;  // SNSFriendIdPayload (0x10); hashes CSymbol64("SNSFriendInviteNotify")
constexpr SymbolId FriendAcceptSuccess       = 0x1bbda7fa06af4627;  // SNSFriendNotifyPayload (0x18); hashes CSymbol64("SNSFriendAcceptSuccess")
constexpr SymbolId FriendAddFailure          = 0x1ba8b3f009bf4731;  // SNSFriendNotifyPayload (0x18); hashes CSymbol64("SNSFriendAcceptFailure")
constexpr SymbolId FriendAcceptNotify        = 0xc237c84c31d3ae05;  // SNSFriendNotifyPayload (0x18); hashes CSymbol64("SNSFriendAcceptNotify")
constexpr SymbolId FriendRemoveResponse      = 0xc2bf83a08ea3a955;  // SNSFriendIdPayload (0x10); CSymbol64("SNSFriendRemoveResponse")
constexpr SymbolId FriendRemoveNotify        = 0xe06972f49cd72265;  // SNSFriendIdPayload (0x10); hashes CSymbol64("SNSFriendRemoveNotify")
constexpr SymbolId FriendWithdrawnNotify     = 0x191aa30801ec6d03;  // SNSFriendIdPayload (0x10); hashes CSymbol64("SNSFriendWithdrawnNotify")
constexpr SymbolId FriendRejectNotify        = 0xb9b86c0ce8e8d0c1;  // SNSFriendIdPayload (0x10); hashes CSymbol64("SNSFriendRejectNotify")

// --- Party outgoing (pnsrad → server) ---
constexpr SymbolId PartyKickRequest          = 0xff02bf488e77bcba;  // SNSPartyTargetPayload (0x30); hashes CSymbol64("SNSActivityDailyRewardRequest")
constexpr SymbolId PartyPassOwnershipRequest = 0x352f9d0e16001420;  // SNSPartyTargetPayload (0x30); hashes CSymbol64("SNSActivityEventRewardRequest")
constexpr SymbolId PartyRespondToInvite      = 0xeaf428c8a8a5cd2a;  // SNSPartyTargetPayload (0x30); hashes CSymbol64("SNSActivityWeeklyRewardRequest")
constexpr SymbolId PartyMemberUpdate         = 0x0b7bd21332523994;  // variable; hashes CSymbol64("SNSPartyCreateRequest")

}  // namespace Social

namespace Tcp {

// Registration (official symbols from EchoVR)
constexpr SymbolId LobbyRegistrationSuccess = -5369924845641990433;
constexpr SymbolId LobbyRegistrationFailure = -5373034290044534839;
constexpr SymbolId LobbySessionSuccessV5 = 0x6d4de3650ee3110f;

// Game client messages (received from game in client mode)
constexpr SymbolId GameClientMsg1 = 0x5b71b22a4483bda5;
constexpr SymbolId GameClientMsg2 = 0xa88cb5d166cc2ca;
constexpr SymbolId GameClientMsg3 = 0xa7a9e5a70b2429db;

}  // namespace Tcp

}  // namespace EchoVR::Symbols
