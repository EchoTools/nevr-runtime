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
