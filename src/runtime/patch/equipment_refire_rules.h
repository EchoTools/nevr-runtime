#pragma once
// When the equipment change the game dropped is applied again (#2121), apart from the hooks that apply it
// (patch/equipment_refire.cpp on PC, quest/plugins/q15t/src/equiprefire.cpp on Quest) so the rule is unit-tested without
// the game and is one definition for both builds.
//
// Evidence (ReVault, project echovr; PC VAs are echovr.exe, Quest VAs are libr15.so as the disassembly shows them):
//   - CR15NetPlayerEquipmentCS keeps one 0x58-byte record per player. Three setters write a slot and, only when the value
//     changed, call the slot's change handler:
//       weapon   PC 0x140d498e0 writes record+0x18, calls 0x140cec740    Quest SetWeaponSlot  0x227ca84, HandleWeaponChange  0x227afb4
//       tac mod  PC 0x140d43f00 writes record+0x20, calls 0x140ceaaa0    Quest SetAbilitySlot 0x227cde4, HandleAbilityChange 0x227b124
//       ordnance PC 0x140d45990 writes record+0x28, calls 0x140ceb720    Quest SetGrenadeSlot 0x227cf90, HandleGrenadeChange 0x227b1dc
//     The setters take (component, handle, value). The handler takes (component, handle), reads the value from the record
//     and sends the equipment-changed event (0xefe2dea238cf2e32) to the scripts that arm the slot.
//   - The scripts drop an event that arrives while they are still handling the previous one (#2121: the ability core's change
//     handler, "the earlier tac mod is armed"); the record already holds the new value, so the HUD shows the last choice and
//     the game uses an earlier one. A tester reported the same for weapons and ordnance. The only information that is lost is the
//     event, so sending it once more when nothing is in flight restores the state the record already has.
//   - The chain a change starts takes about 2 s (the reference build's own measurement in the community EchoCombat tool that
//     found the drop, credited in the README thanks), so the slot must be still for longer than that before the event is sent
//     again; a re-fire that lands inside a chain would be dropped like the change it repairs.
//
// The re-fire calls the handler, never the setter, so it cannot start another debounce. It is sent once per quiet period: after
// it, the slot is idle until the next change.
//
// The component's layout is the same on both builds (the Quest setters read +0xc8, +0xf8, the 0x58 stride and the actor key at
// ctx+0x58 and +0x100 exactly as the PC setters do; the net game's player slots are at +0xdc, +0x178, +0x13a on both: Quest
// CR15NetGame::UserId 0x135a9b8 and PC 0x1401c92d0 build the same handle), so the reads that find a record, its actor and the
// local player's slot are written once below, over a reader the build supplies.

#include <cstddef>
#include <cstdint>

namespace nevr_equipment_refire {

enum class Slot : std::uint8_t { kWeapon = 0, kTacMod = 1, kOrdnance = 2 };
inline constexpr std::size_t kSlotCount = 3;

constexpr const char* SlotName(Slot slot) {
  switch (slot) {
    case Slot::kWeapon: return "weapon";
    case Slot::kTacMod: return "tac_mod";
    case Slot::kOrdnance: return "ordnance";
  }
  return "unknown";
}

// How long a slot has to be still after its last change before the handler runs again: the ~2 s chain plus a margin. The
// first live run is the measurement of this number (the log line carries the time since the last change).
inline constexpr std::uint64_t kSettleMs = 2500;

// CR15NetPlayerEquipmentCS, from the setters' and handlers' own reads. A record handle is (generation << 16) | sparse index; the
// sparse index selects an entry of the index table, whose u16 is the dense index of the record in the record array and whose next
// u16 is the generation (PC 0x140cda940 builds the handle the same way).
inline constexpr std::size_t kCtxOffset = 0x80;          // the component context
inline constexpr std::size_t kIndexTableOffset = 0xc8;   // 4 bytes per sparse index: u16 dense index, u16 generation
inline constexpr std::size_t kRecordsOffset = 0xf8;      // kRecordStride bytes per dense index
inline constexpr std::size_t kLinkTableOffset = 0x100;   // 0x10 bytes per dense index: the record's actor link
inline constexpr std::size_t kCountOffset = 0xf2;        // u16: the number of records (PC 0x140cda940 loops to it)
inline constexpr std::size_t kRecordStride = 0x58;
inline constexpr std::size_t kLinkStride = 0x10;
inline constexpr std::size_t kCtxNetGameArgOffset = 0x8;   // *(ctx + 8) is what the net game getter takes
inline constexpr std::size_t kCtxActorKeyOffset = 0x58;    // *(ctx + 0x58) is the first half of the actor key; the link is the second
inline constexpr std::size_t kComponentSpan = 0x108;       // the bytes of the component the reads above reach
inline constexpr std::size_t kRecordValueOffset[3] = {0x18, 0x20, 0x28};  // weapon, tac mod, ordnance

// The net game's player slots: +0xdc is the count of slots in use, slot i's field is at +0x178 + i * 0x250, and the generation of
// the slot a field names is at +0x13a + (field & 0x1f) * 4. A slot's handle is (generation << 16) | (field & 0x1f). The game keeps
// the local player in slot 0 (PC 0x140c46bd0 SetTier reads it so to find the local player's customization; the lookup of a slot by
// actor, PC 0x1401c92d0 and Quest 0x135a9b8, returns a handle built the same way).
inline constexpr std::size_t kGameSlotCountOffset = 0xdc;
inline constexpr std::size_t kGameLocalFieldOffset = 0x178;
inline constexpr std::size_t kGameSlotGenerationOffset = 0x13a;
inline constexpr std::uint32_t kSlotMask = 0x1f;

constexpr std::uint32_t SlotHandle(std::uint16_t field, std::uint16_t generation) {
  return (static_cast<std::uint32_t>(generation) << 16) | (field & kSlotMask);
}

// `read(address, out, size)` copies `size` readable bytes and returns false when it cannot. Every function below fails closed: an
// unreadable or inconsistent read is "no".
template <typename Read, typename T>
bool ReadValue(Read& read, std::uintptr_t address, T* out) {
  return read(address, out, sizeof(T));
}

// The record's dense index, or -1 when the handle does not name a live record of the component (stale generation, index out of range).
template <typename Read>
long long DenseIndex(Read& read, std::uintptr_t component, std::uint32_t handle) {
  std::uintptr_t table = 0;
  std::uint16_t count = 0;
  if (!ReadValue(read, component + kIndexTableOffset, &table) || !ReadValue(read, component + kCountOffset, &count)) return -1;
  const std::uintptr_t entry = table + static_cast<std::uintptr_t>(handle & 0xffffU) * 4;
  std::uint16_t dense = 0;
  std::uint16_t generation = 0;
  if (!ReadValue(read, entry, &dense) || !ReadValue(read, entry + 2, &generation)) return -1;
  if (generation != (handle >> 16) || dense >= count) return -1;
  return dense;
}

// The value a slot holds in the record the handle names.
template <typename Read>
bool RecordValue(Read& read, std::uintptr_t component, std::uint32_t handle, Slot slot, std::uint64_t* out) {
  const long long dense = DenseIndex(read, component, handle);
  std::uintptr_t records = 0;
  if (dense < 0 || !ReadValue(read, component + kRecordsOffset, &records)) return false;
  return ReadValue(read, records + static_cast<std::uintptr_t>(dense) * kRecordStride + kRecordValueOffset[static_cast<std::size_t>(slot)], out);
}

// What the net game is asked about a record: the argument its getter takes and the record's actor key.
struct ActorKey {
  std::uintptr_t netGameArg = 0;
  std::uint64_t key[2] = {0, 0};
};

template <typename Read>
bool ReadActorKey(Read& read, std::uintptr_t component, std::uint32_t handle, ActorKey* out) {
  const long long dense = DenseIndex(read, component, handle);
  std::uintptr_t ctx = 0;
  std::uintptr_t links = 0;
  if (dense < 0 || !ReadValue(read, component + kCtxOffset, &ctx) || !ReadValue(read, ctx + kCtxNetGameArgOffset, &out->netGameArg) ||
      !ReadValue(read, ctx + kCtxActorKeyOffset, &out->key[0]) || !ReadValue(read, component + kLinkTableOffset, &links) ||
      !ReadValue(read, links + static_cast<std::uintptr_t>(dense) * kLinkStride, &out->key[1])) {
    return false;
  }
  return true;
}

// The handle of the net game's slot 0, the local player's. False when the game has no slots in use or a read fails.
template <typename Read>
bool LocalSlotHandle(Read& read, std::uintptr_t game, std::uint32_t* out) {
  std::uint16_t slots = 0;
  std::uint16_t field = 0;
  std::uint16_t generation = 0;
  if (game == 0 || !ReadValue(read, game + kGameSlotCountOffset, &slots) || slots == 0 ||
      !ReadValue(read, game + kGameLocalFieldOffset, &field) ||
      !ReadValue(read, game + kGameSlotGenerationOffset + static_cast<std::uintptr_t>(field & kSlotMask) * 4, &generation)) {
    return false;
  }
  *out = SlotHandle(field, generation);
  return true;
}

// What a setter call became.
enum class Verdict : std::uint8_t {
  kTracked,    // a change of the local player's record: re-fire after the slot is still
  kUnchanged,  // the setter was given the value the record already held: the game ran no handler, nothing to repair
  kNotLocal,   // another player's record: its owner repairs it, never this process
  kServer,     // this process is a dedicated server: it re-fires for no one
};

constexpr const char* VerdictName(Verdict verdict) {
  switch (verdict) {
    case Verdict::kTracked: return "tracked";
    case Verdict::kUnchanged: return "unchanged";
    case Verdict::kNotLocal: return "not_local";
    case Verdict::kServer: return "server";
  }
  return "unknown";
}

// One handler call to make.
struct Refire {
  Slot slot = Slot::kWeapon;
  std::uintptr_t owner = 0;  // the equipment component the setter ran on
  std::uint32_t handle = 0;  // the record's handle
  std::uint64_t value = 0;   // the last value set, for the log (the handler reads the record, not this)
  std::uint64_t changes = 0;  // how many changes the burst held
  std::uint64_t quietMs = 0;  // how long the slot had been still
};

// The state across calls. Not thread-safe: the hook and the frame step lock around it. One slot is tracked at a time per kind;
// there is one local player, so a later change of another record of the same slot replaces the earlier one.
class Debouncer {
 public:
  // A setter ran. `changed` is false when the record already held `value` before the call.
  Verdict Changed(Slot slot, std::uintptr_t owner, std::uint32_t handle, std::uint64_t value, bool local, bool server, bool changed,
                  std::uint64_t nowMs) {
    if (server) return Verdict::kServer;
    if (!local) return Verdict::kNotLocal;
    if (!changed) return Verdict::kUnchanged;
    Entry& e = entries_[static_cast<std::size_t>(slot)];
    const bool sameRecord = e.pending && e.owner == owner && e.handle == handle;
    e.changes = sameRecord ? e.changes + 1 : 1;
    e.pending = true;
    e.owner = owner;
    e.handle = handle;
    e.value = value;
    e.lastMs = nowMs;
    return Verdict::kTracked;
  }

  // The next slot that has been still for kSettleMs, cleared as it is returned; false when none is due. Call until false.
  // A clock that went backwards moves the slot's last change to now, so it waits a full period instead of firing early.
  bool Due(std::uint64_t nowMs, Refire* out) {
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      Entry& e = entries_[i];
      if (!e.pending) continue;
      if (nowMs < e.lastMs) {
        e.lastMs = nowMs;
        continue;
      }
      if (nowMs - e.lastMs < kSettleMs) continue;
      out->slot = static_cast<Slot>(i);
      out->owner = e.owner;
      out->handle = e.handle;
      out->value = e.value;
      out->changes = e.changes;
      out->quietMs = nowMs - e.lastMs;
      e = Entry{};
      return true;
    }
    return false;
  }

  // Drops everything tracked, for a component that is gone (the level unloaded under a pending change).
  void Clear() {
    for (Entry& e : entries_) e = Entry{};
  }

  bool Pending(Slot slot) const { return entries_[static_cast<std::size_t>(slot)].pending; }

 private:
  struct Entry {
    bool pending = false;
    std::uintptr_t owner = 0;
    std::uint32_t handle = 0;
    std::uint64_t value = 0;
    std::uint64_t changes = 0;
    std::uint64_t lastMs = 0;
  };
  Entry entries_[kSlotCount];
};

}  // namespace nevr_equipment_refire
