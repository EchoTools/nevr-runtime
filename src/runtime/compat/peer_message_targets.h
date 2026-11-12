// peer_message_targets.h: the game's addresses and layouts that nEVR's control messages on the game's broadcaster use
// (#2365), one table per binary of the one codebase (echovr.exe on PC, libr15.so on Quest; AGENTS.md "Project").
//
// Each value says where it was read (ReVault project echovr, the function and address) and its grade:
//   FACT: read directly in that function. INFERRED: reasoned from reads (the basis is named). NOT ESTABLISHED: not read;
//   the Quest field holds 0 and nothing may use it until it is read.
// PC VAs are image VAs (base 0x140000000). Quest VAs are ReVault's (ELF vaddr + 0x100000).
// The Quest half is not built in this slice: the Quest values are recorded so the Quest port uses this table, not a copy.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nevr_peer_message {

struct Targets {
  // CBroadcaster::Send(bc, CMatSym type, int, const void* item, u64 itemSize, const void* buf, u64 bufLen, SPeer peer,
  // u64 dest, float priority, CSymbol64). PC 0x140f89af0: FACT (0x140129f00 calls it so, size 8). Quest 0x25e7e94: FACT
  // (demangled symbol; CR15NetGame::Broadcast<SR15NetAbilityActivate> 0x13df000 calls it).
  std::uint64_t broadcasterSend;
  // CBroadcaster::Listen(bc, CMatSym type, u32 reliable, const CDelegate&, u32 prepend). PC 0x140f80ed0: FACT
  // (BeginMultiplayer's listener block 0x140157fb0 calls it with reliable 1, prepend 0). Quest 0x25e8a70: FACT (symbol).
  std::uint64_t broadcasterListen;
  // SBroadcasterData::InvokeListeners: calls each listener's proxy as (instance, &method, payload copy, size, a3,
  // SENDER peer); a symbol with no listener is dropped. PC 0x140f80860: FACT (call site 0x140f80c82; the [NETDATA]
  // "Receive ... from Peer %llu" line logs the same register as the sender). Quest 0x260bca8: FACT (symbol).
  std::uint64_t invokeListeners;
  // The server's peer as a client sees it. PC GetAuthorityPeer 0x1401ab5c0 is `SPeer* (CR15NetGame*, SPeer* out)`: FACT.
  // Quest CR15NetGame::Server() const 0x133ca70 returns the SPeer by value: FACT. The two calling conventions differ.
  std::uint64_t authorityPeer;
  // CR15NetGame::AbilityActivateCB(const SMsg<SR15NetAbilityActivate>&). PC 0x140150f50: FACT. Quest 0x1362f54: FACT.
  std::uint64_t abilityActivateCallback;
  // CR15NetGame::Broadcast<SR15NetAbilityActivate>(msg, buf, bufLen, SPeer, dest): what the cooldown node sends the use
  // with. PC 0x14012a010: FACT (6 arguments, the net game first). Quest 0x13df000: FACT (symbol).
  std::uint64_t abilityActivateSend;
  // CR15NetAbilityCooldownNode::Enter: logs "Cooldown node activate" and sends Activate to the authority peer. PC
  // 0x140dd8f40: FACT. Quest 0x241a13c: FACT.
  std::uint64_t cooldownNodeEnter;
  // CNetPlayerEquipmentCS::ReceivePlayerEquipmentUpdate(component, const SMsg*): the server's receiver of a player's
  // equipment. PC 0x140d37300: FACT. Quest: NOT ESTABLISHED (twin not looked up).
  std::uint64_t receiveEquipmentUpdate;
  // The net game's "slot handle of the player whose actor key this is": PC 0x1401c92d0 `(game, u32* out, u64 key[2])`
  // FACT (runtime/patch/equipment_refire.cpp kFindSlotVa). Quest CR15NetGame::UserId 0x135a9b8: FACT per
  // runtime/patch/equipment_refire_rules.h.
  std::uint64_t slotOfActor;

  // Offsets.
  std::size_t contextNetGame;      // the game context's CR15NetGame*: PC 0x8518 FACT (GetNetGameFromContext 0x140113a90);
                                   // Quest 0x8510 FACT per equipment_refire_rules.h (NetGameMT 0x13027d4)
  std::size_t netGameBroadcaster;  // CR15NetGame's peer CBroadcaster*: 0x28c8 on both. PC FACT (ConfigureEngine 0x140145e50
                                   // stores it at 0x1401460dd; 0x14012a010 sends on it); Quest FACT (0x13df000, ctor 0x1344d58)
  std::size_t netGameFlags;        // pointer to the role flags word: 0x2da0 on both, FACT (0x1401ab5c0; 0x1362f54)
  std::size_t netGameLobby;        // CR15NetLobby*: 0x40 on both. PC FACT (0x1401ab5c0); Quest FACT (Server() 0x133ca70)
  std::size_t lobbyNsLobby;        // the CNSLobby inside CR15NetLobby: 0x18. Quest FACT (ctor 0x1344d58); PC INFERRED
                                   // (0x1401ab5c0 adds 0x18 before HostPeer's +0x138 read)
  std::size_t nsLobbyHostPeer;     // CNSLobby::HostPeer's field: 0x138 on both. PC FACT (0x140609a00), Quest FACT
  std::size_t nsLobbySessions;     // the player-session slots: 0xc8 (array), count at +0x8. PC INFERRED: the repo's layout
                                   // (src/abi/echovr.h Lobby::playerSessions, used by ServerContext), not read in echovr.exe
                                   // for this table; Quest FACT (CNSLobby::EntrantPeer 0x1a2b4b4)
  std::size_t sessionStride;       // 0x28 on both: PC INFERRED (echovr.h static_assert, the repo's layout); Quest FACT
                                   // (EntrantPeer madd #0x28)
  std::size_t sessionJoinState;    // u64 at +0x18, 4 = accepted. PC INFERRED (echovr.h, the repo's layout); Quest NOT
                                   // ESTABLISHED
  std::size_t msgPayload;          // SMsg+0x10 is the message struct, SMsg+0 the sender peer: FACT on both (ListenProxy
                                   // PC 0x14013cd50, Quest 0x13e55cc)
  std::size_t slotIndexTable;      // the handle's {u16 index, u16 generation} table: index at +0x138, generation at +0x13a,
                                   // 4 bytes per slot, slot < 16: FACT on both (AbilityActivateCB)
};

inline constexpr Targets kPcTargets = {
    0x140f89af0, 0x140f80ed0, 0x140f80860, 0x1401ab5c0, 0x140150f50, 0x14012a010, 0x140dd8f40, 0x140d37300, 0x1401c92d0,
    0x8518, 0x28c8, 0x2da0, 0x40, 0x18, 0x138, 0xc8, 0x28, 0x18, 0x10, 0x138,
};

inline constexpr Targets kQuestTargets = {
    0x25e7e94, 0x25e8a70, 0x260bca8, 0x133ca70, 0x1362f54, 0x13df000, 0x241a13c, /*receiveEquipmentUpdate: NOT ESTABLISHED*/ 0,
    0x135a9b8, 0x8510, 0x28c8, 0x2da0, 0x40, 0x18, 0x138, 0xc8, 0x28, /*sessionJoinState: NOT ESTABLISHED*/ 0, 0x10, 0x138,
};

// The first bytes of each PC function this code calls or hooks, read from echovr.exe (the build this table is for). A
// call or a hook is made only after its bytes match (AGENTS.md "Guardrails": never blind).
inline constexpr std::array<std::uint8_t, 16> kPcAuthorityPeerPrologue = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                                                          0x81, 0xa0, 0x2d, 0x00, 0x00, 0x48, 0x8b, 0xda};
inline constexpr std::array<std::uint8_t, 16> kPcAbilityActivateCallbackPrologue = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x81, 0xa0, 0x2d, 0x00};
inline constexpr std::array<std::uint8_t, 16> kPcAbilityActivateSendPrologue = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                                                                0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57};
inline constexpr std::array<std::uint8_t, 16> kPcReceiveEquipmentUpdatePrologue = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83};
inline constexpr std::array<std::uint8_t, 16> kPcSlotOfActorPrologue = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
                                                                        0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};

}  // namespace nevr_peer_message
