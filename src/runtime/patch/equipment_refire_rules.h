#pragma once
// When the equipment change the game dropped is applied again (#2121). This header is the whole feature above the hooks: the
// debounce, the reads that find a record and the local player, how a setter call becomes a verdict, when a due re-fire is stale,
// the loop that fires what is due, and the words of the log lines. The hooks that feed it are patch/equipment_refire.cpp on PC and
// quest/combat/equip_refire.cpp on Quest (each is an Env, below); the PC GTest drives this one definition with a fake Env.
//
// Evidence (ReVault, project echovr; PC VAs are echovr.exe, Quest VAs are libr15.so as the disassembly shows them):
//   - CR15NetPlayerEquipmentCS keeps one 0x58-byte record per player. Three setters write a slot and, only when the value
//     changed, call the slot's change handler:
//       weapon   PC 0x140d498e0 writes record+0x18, calls 0x140cec740    Quest SetWeaponSlot  0x227ca84, HandleWeaponChange  0x227afb4
//       tac mod  PC 0x140d43f00 writes record+0x20, calls 0x140ceaaa0    Quest SetAbilitySlot 0x227cde4, HandleAbilityChange 0x227b124
//       ordnance PC 0x140d45990 writes record+0x28, calls 0x140ceb720    Quest SetGrenadeSlot 0x227cf90, HandleGrenadeChange 0x227b1dc
//     The setters take (component, handle, value). The handler takes (component, handle), reads the value from the record
//     and sends the slot's equipment-changed event to the scripts that arm the slot (Quest: the ability handler sends
//     0xefe2dea238cf2e32, the weapon handler 0x141da9bee941e9bc).
//   - The scripts drop an event that arrives while they are still handling the previous one (#2121: the ability core's change
//     handler, "the earlier tac mod is armed"); the record already holds the new value, so the HUD shows the last choice and
//     the game uses an earlier one. A tester reported the same for weapons and ordnance. The only information that is lost is the
//     event, so sending it once more when nothing is in flight restores the state the record already has.
//   - The chain a change starts takes about 2 s (measured by he_is_the_cat's equip_guard script in the EchoCombat repository, which
//     found the drop; credited in config/credits.json), so the slot must be still for longer than that before the event is sent
//     again; a re-fire that lands inside a chain would be dropped like the change it repairs.
//
// Frequency (ADR 0038). The setters run once per loadout event (a station button press, a bot cycling its gun, the game restoring a
// loadout), reached from script nodes and the deferred queue, never per frame (callers read in ReVault; not measured). The per-frame
// cost is FireDue: one lock, one clock read and an empty-table test.
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
inline constexpr std::size_t kGameSlotCount2Offset = 0xde;  // SetTier (PC 0x140c46bd0) and ApplyItemFromLoadout (Quest 0x135a6dc) test both counts
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

// The handle of the net game's slot 0, the local player's. False when the game has no slots in use (either count is zero, as
// SetTier requires) or a read fails.
template <typename Read>
bool LocalSlotHandle(Read& read, std::uintptr_t game, std::uint32_t* out) {
  std::uint16_t slots = 0;
  std::uint16_t slots2 = 0;
  std::uint16_t field = 0;
  std::uint16_t generation = 0;
  if (game == 0 || !ReadValue(read, game + kGameSlotCountOffset, &slots) || slots == 0 ||
      !ReadValue(read, game + kGameSlotCount2Offset, &slots2) || slots2 == 0 ||
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
  kNotLocal,   // another player's record: not tracked; the re-fire is the local player's own and sends nothing to other clients
  kServer,     // this process is a dedicated server: it re-fires for no one
  kUnreadable, // the record could not be read before the call (stale handle, unreadable component): nothing can be said about it
};

constexpr const char* VerdictName(Verdict verdict) {
  switch (verdict) {
    case Verdict::kTracked: return "tracked";
    case Verdict::kUnchanged: return "unchanged";
    case Verdict::kNotLocal: return "not_local";
    case Verdict::kServer: return "server";
    case Verdict::kUnreadable: return "unreadable";
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

  // Drops everything tracked. Production never calls it: a component that is gone is caught by StaleReason when its re-fire is due.
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

// ---- The feature above the hooks, once. -----------------------------------------------------------------------------------------
//
// An Env is the build's side of it (PC glue, Quest glue, the GTest's fake). It is also the reader the layout templates above take.
//   bool operator()(std::uintptr_t address, void* out, std::size_t size)   read that never faults; false when unreadable
//   static constexpr std::size_t kNetGameField                             offset the net game getter reads at its argument (PC 0x8518, Quest 0x8510)
//   std::uintptr_t NetGame(std::uintptr_t arg)                             the game's getter (PC GetNetGameFromContext 0x140113a90, Quest NetGameMT 0x13027d4)
//   std::uint32_t SlotOf(std::uintptr_t game, const std::uint64_t key[2])  the game's slot-handle lookup by actor key (PC 0x1401c92d0, Quest UserId 0x135a9b8)
//   void CallHandler(Slot, std::uintptr_t owner, std::uint32_t handle)     the slot's change handler
//   std::uint64_t NowMs();  void Lock();  void Unlock();                   a monotonic millisecond clock; one non-recursive lock around the Tracker
//   void LogChange(Slot, const SetterResult&, std::uint32_t handle, std::uint64_t value)
//   void LogRefired(const Refire&);  void LogSkipped(const Refire&, const char* reason)   with the words below

// The words of the log lines (the PC line is `[NEVR.EQUIP] <message> slot=.. value=.. handle=.. changes=.. quiet_ms=..`, the Quest line
// the same message and keys in a sentinel record).
inline constexpr const char* kMsgChange = "equipment change";
inline constexpr const char* kMsgRefired =
    "equipment re-fired (the game's change handler ran once more so the slot's scripts see the last choice)";
inline constexpr const char* kMsgSkipped = "equipment re-fire skipped (the change is left as the game made it)";

// What is remembered between a setter call and its re-fire. Guarded by Env::Lock.
struct Tracker {
  Debouncer debouncer;
  std::uintptr_t vtable[kSlotCount] = {};  // the component's vtable when the change was tracked: a different one later is another component
};

// True when the record the handle names in the component belongs to the net game's slot 0, the local player. Fails closed: anything
// unreadable or unmatched is "not local" and the change is not tracked.
template <typename Env>
bool IsLocalRecord(Env& env, std::uintptr_t component, std::uint32_t handle) {
  ActorKey actor;
  if (!ReadActorKey(env, component, handle, &actor)) return false;
  // The getter loads [arg + kNetGameField] without a check, so the argument is probed before the game's code reads it.
  std::uintptr_t probe = 0;
  if (!ReadValue(env, actor.netGameArg + Env::kNetGameField, &probe)) return false;
  const std::uintptr_t game = env.NetGame(actor.netGameArg);
  std::uint32_t local = 0;
  if (!LocalSlotHandle(env, game, &local)) return false;
  return env.SlotOf(game, actor.key) == local;
}

// What a setter call became, and the facts behind it (for the log).
struct SetterResult {
  Verdict verdict = Verdict::kServer;
  bool read = false;     // the record's value could be read before the call
  bool changed = false;  // it differed from the value set, so the game ran the slot's handler
  bool local = false;    // the record is the local player's (asked only for a change)
};

// A setter ran. `callOriginal` runs the game's setter. The record's value is read BEFORE it (afterwards both sides are equal), the
// change is judged after, and a dedicated server (`server`) reads nothing and tracks nothing. A call that changed nothing was never
// asked who owns the record, so it reports unchanged, not not_local.
template <typename Env, typename Original>
SetterResult OnSetter(Env& env, Tracker& tracker, Slot slot, std::uintptr_t component, std::uint32_t handle, std::uint64_t value,
                      bool server, Original&& callOriginal) {
  SetterResult r;
  std::uint64_t before = 0;
  r.read = !server && RecordValue(env, component, handle, slot, &before);
  callOriginal();
  if (server) {
    r.verdict = Verdict::kServer;
  } else if (!r.read) {
    r.verdict = Verdict::kUnreadable;
  } else {
    r.changed = before != value;
    r.local = r.changed && IsLocalRecord(env, component, handle);
    env.Lock();
    r.verdict = tracker.debouncer.Changed(slot, component, handle, value, r.local || !r.changed, false, r.changed, env.NowMs());
    if (r.verdict == Verdict::kTracked) {
      std::uintptr_t vtable = 0;
      env(component, &vtable, sizeof vtable);  // unreadable stays 0: the re-fire then reads as component_replaced
      tracker.vtable[static_cast<std::size_t>(slot)] = vtable;
    }
    env.Unlock();
  }
  env.LogChange(slot, r, handle, value);
  return r;
}

// Why a due re-fire is not made, or nullptr when it is. Everything is read again: the component may have been freed, the level
// unloaded, the record reused or the choice changed by something other than the setter.
template <typename Env>
const char* StaleReason(Env& env, Tracker& tracker, const Refire& r) {
  std::uint8_t span[kComponentSpan];
  if (!env(r.owner, span, sizeof span)) return "component_unreadable";
  std::uintptr_t vtable = 0;
  for (std::size_t i = 0; i < sizeof vtable; ++i) vtable |= static_cast<std::uintptr_t>(span[i]) << (8 * i);
  env.Lock();
  const std::uintptr_t tracked = tracker.vtable[static_cast<std::size_t>(r.slot)];
  env.Unlock();
  if (vtable == 0 || vtable != tracked) return "component_replaced";
  std::uint64_t current = 0;
  if (!RecordValue(env, r.owner, r.handle, r.slot, &current)) return "record_gone";
  if (current != r.value) return "record_changed_elsewhere";
  if (!IsLocalRecord(env, r.owner, r.handle)) return "not_local";
  return nullptr;
}

// Fires what is due: for each slot still for kSettleMs, the handler runs once more unless the record changed under it. Returns how
// many handlers ran. Call it from the thread the game's scripts run on (the handlers send events into them).
template <typename Env>
std::size_t FireDue(Env& env, Tracker& tracker) {
  Refire due[kSlotCount];
  std::size_t count = 0;
  env.Lock();
  const std::uint64_t now = env.NowMs();
  while (count < kSlotCount && tracker.debouncer.Due(now, &due[count])) ++count;
  env.Unlock();
  std::size_t fired = 0;
  for (std::size_t n = 0; n < count; ++n) {
    if (const char* why = StaleReason(env, tracker, due[n])) {
      env.LogSkipped(due[n], why);
      continue;
    }
    env.CallHandler(due[n].slot, due[n].owner, due[n].handle);
    env.LogRefired(due[n]);
    ++fired;
  }
  return fired;
}

}  // namespace nevr_equipment_refire
