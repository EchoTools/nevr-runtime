#pragma once

#include <cstdint>

#include "runtime/server/telemetry_player_pose.h"
#include "runtime/server/telemetry_snapshot.h"

// A player's stunned, invulnerable, blocking and emote state, read the way the game's own /session writers
// read them (echovr.exe 34.4.631547.1). The game functions it calls come in as pointers (StateFunctions,
// resolved from nevr_game_func_addr on the game thread), so a test drives it with fakes.
//
// What /session does for entrant i, after it has resolved the entrant's entity as the pose read does
// (ResolvePlayerEntity; no entity, nothing is written):
//   - stunned, invulnerable, blocking: the game-state writer 0x140155c80 (0x1401572b8-0x1401575b0) finds the
//     entity's R14NetPunchable component, GetComponent<R14NetPunchable> (0x140167fc0, the fallback it calls at
//     0x14015738e after its inlined first level), and reads it only when the component is not null and its
//     check (vfunc +0x1C8) accepts the index (0x140157393-0x1401573b0). The row is
//     *(component + 0xF8) + table[index] * 0x48, the table at *(component + 0xC8) with a u16 every 4 bytes
//     (0x1401573b6-0x1401573d2), and the row's dword at +0x04 holds the flags (0x1401573d6):
//       stunned      = bit 1 (0x1401573dc, 0x1401573e5; written at 0x14015740c-0x140157490),
//       invulnerable = bit 2 (0x1401573e1, 0x1401573e8; written at 0x140157499-0x140157520),
//       blocking     = any of bits 5, 6, 7 (0x1401573ec-0x140157401; written at 0x140157528-0x1401575b0).
//   - is_emote_playing: the player writer 0x1401b2ea0 (0x1401b3ee7-0x1401b4070) finds the entity's
//     R15NetCustomization component, GetComponent<R15NetCustomization> (0x1401679a0, its fallback at
//     0x1401b3fb2), checks it the same way (0x1401b3fb7-0x1401b3fd2), and writes what 0x140d08d00(component,
//     index) returns (0x1401b3fdf). That function takes the row *(component + 0x100) + table[index] * 0x1C0,
//     table *(component + 0xD0), and returns 1 when the reference at row + 0x118 is live (0x140128b20: it
//     resolves the reference, 0x14011c8a0, and asks the referenced component's check), else 0.
namespace nevr_telemetry_state {

// FUN_140d08d00(R15NetCustomization component, index) → 1 while the player's emote plays, else 0.
using EmotePlayingFn = int(__fastcall*)(void* component, uint32_t index);

struct StateFunctions {
  nevr_telemetry_pose::ResolveEntityHandleFn resolveHandle = nullptr;
  nevr_telemetry_pose::EntityLookupFn lookupEntity = nullptr;
  nevr_telemetry_pose::GetComponentFn punchableComponent = nullptr;
  nevr_telemetry_pose::GetComponentFn customizationComponent = nullptr;
  EmotePlayingFn emotePlaying = nullptr;

  bool Complete() const {
    return resolveHandle && lookupEntity && punchableComponent && customizationComponent && emotePlaying;
  }
};

// R14NetPunchable instance data, as the game-state writer reads it (0x1401573b6-0x140157401).
constexpr uint64_t kPunchableRowTable = 0xC8;  // u16 row per instance, 4 bytes apart
constexpr uint64_t kPunchableRows = 0xF8;
constexpr uint64_t kPunchableRowStride = 0x48;
constexpr uint64_t kPunchableRowFlags = 0x04;
constexpr uint32_t kPunchableStunned = 1u << 1;
constexpr uint32_t kPunchableInvulnerable = 1u << 2;
constexpr uint32_t kPunchableBlocking = 0xE0;  // bit 5, 6 or 7

// What ReadPlayerState found for one entrant.
struct StateResult {
  bool entity = false;
  bool punchable = false;
  bool customization = false;
};

// Reads entrant `entrantIndex`'s state into *out, on the game thread. A state not read keeps its value (false
// in a cleared snapshot). Null punchable tables are this reader's own check; /session reads them unchecked.
inline StateResult ReadPlayerState(const uint8_t* netGame, uint16_t entrantIndex, const StateFunctions& fn,
                                   TelemetrySnapshot::PlayerInfo* out) {
  using nevr_telemetry_pose::ReadAt;
  StateResult result;
  if (!fn.Complete()) return result;
  const nevr_telemetry_pose::PlayerEntity player =
      nevr_telemetry_pose::ResolvePlayerEntity(netGame, entrantIndex, fn.resolveHandle, fn.lookupEntity);
  if (player.entity == nullptr) return result;
  result.entity = true;

  const nevr_telemetry_pose::ComponentRef punchable =
      nevr_telemetry_pose::FindComponent(fn.punchableComponent, player.entity, player.handleExtra);
  if (punchable.component) {
    const uint8_t* component = static_cast<const uint8_t*>(punchable.component);
    const uint8_t* rowTable = ReadAt<const uint8_t*>(component, kPunchableRowTable);
    const uint8_t* rows = ReadAt<const uint8_t*>(component, kPunchableRows);
    if (rowTable && rows) {
      const uint16_t row = ReadAt<uint16_t>(rowTable, static_cast<uint64_t>(punchable.index & 0xFFFF) * 4);
      const uint32_t flags = ReadAt<uint32_t>(rows + static_cast<uint64_t>(row) * kPunchableRowStride,
                                              kPunchableRowFlags);
      out->stunned = (flags & kPunchableStunned) != 0;
      out->invulnerable = (flags & kPunchableInvulnerable) != 0;
      out->blocking = (flags & kPunchableBlocking) != 0;
      result.punchable = true;
    }
  }

  const nevr_telemetry_pose::ComponentRef customization =
      nevr_telemetry_pose::FindComponent(fn.customizationComponent, player.entity, player.handleExtra);
  if (customization.component) {
    out->emotePlaying = fn.emotePlaying(customization.component, customization.index) != 0;
    result.customization = true;
  }
  return result;
}

// What ReadPlayerStates found over a snapshot's players.
struct StatesResult {
  uint16_t withEntity = 0;
  uint16_t noEntity = 0;
};

// Reads the state of every present player into its playerInfo, from the entrant ReadPlayers read for that slot
// (PlayerInfo::entrantIndex).
inline StatesResult ReadPlayerStates(const uint8_t* netGame, TelemetrySnapshot* snap, const StateFunctions& fn) {
  StatesResult result;
  for (uint8_t slot = 0; slot < 16; slot++) {
    auto& info = snap->playerInfo[slot];
    if (!info.present) continue;
    if (ReadPlayerState(netGame, info.entrantIndex, fn, &info).entity) {
      result.withEntity++;
    } else {
      result.noEntity++;
    }
  }
  return result;
}

}  // namespace nevr_telemetry_state
