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
// Social / Friends (pnsrad SNS messages)
// Source: echovr-reconstruction/src/pnsrad/Social/CNSRADFriends_protocol.h
// ============================================================================

namespace Social {

// --- Friends outgoing (pnsrad → server, 3 unique hashes) ---
constexpr SymbolId FriendSendInviteRequest   = 0x7f0d7a28de3c6f70;  // add_friend_by_id/name
constexpr SymbolId FriendRemoveRequest       = 0x1bbcb7e810af4620;  // remove_friend (shared w/ Party Join)
constexpr SymbolId FriendActionRequest       = 0x78908988b7fe6db4;  // accept/reject/block (shared w/ Party Leave)

// --- Friends inbound (server → pnsrad, 12 callback hashes) ---
constexpr SymbolId FriendListResponse        = 0xa78aeb2a4e89b10b;  // SNSFriendsListResponse (0x20)
constexpr SymbolId FriendStatusNotify        = 0x26a19dc4d2d5579d;  // SNSFriendNotifyPayload (0x18)
constexpr SymbolId FriendInviteSuccess       = 0x7f0c6a3ac83c6f77;  // SNSFriendIdPayload (0x10)
constexpr SymbolId FriendInviteFailure       = 0x7f197e30c72c6e61;  // SNSFriendNotifyPayload (0x18)
constexpr SymbolId FriendInviteNotify        = 0xca09b0b36bd981b7;  // SNSFriendIdPayload (0x10)
constexpr SymbolId FriendAcceptSuccess       = 0x1bbda7fa06af4627;  // SNSFriendNotifyPayload (0x18)
constexpr SymbolId FriendAddFailure          = 0x1ba8b3f009bf4731;  // SNSFriendNotifyPayload (0x18)
constexpr SymbolId FriendAcceptNotify        = 0xc237c84c31d3ae05;  // SNSFriendNotifyPayload (0x18)
constexpr SymbolId FriendBlockSuccess        = 0xc2bf83a08ea3a955;  // SNSFriendIdPayload (0x10)
constexpr SymbolId FriendRemoveNotify        = 0xe06972f49cd72265;  // SNSFriendIdPayload (0x10)
constexpr SymbolId FriendWithdrawnNotify     = 0x191aa30801ec6d03;  // SNSFriendIdPayload (0x10)
constexpr SymbolId FriendRejectNotify        = 0xb9b86c0ce8e8d0c1;  // SNSFriendIdPayload (0x10)

// --- Party outgoing (pnsrad → server) ---
constexpr SymbolId PartyKickRequest          = 0xff02bf488e77bcba;  // SNSPartyTargetPayload (0x30)
constexpr SymbolId PartyPassOwnershipRequest = 0x352f9d0e16001420;  // SNSPartyTargetPayload (0x30)
constexpr SymbolId PartyRespondToInvite      = 0xeaf428c8a8a5cd2a;  // SNSPartyTargetPayload (0x30)
constexpr SymbolId PartyMemberUpdate         = 0x0b7bd21332523994;  // variable

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
