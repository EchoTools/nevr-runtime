#include "equiprefire.h"

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "module_switch.h"
#include "pbhud_log.h"
#include "plugin_log.h"
#include "runtime/patch/equipment_refire_rules.h"
#include "safe_read.h"

namespace q15t {
namespace {

namespace rules = nevr_equipment_refire;

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000); the table is EquipRefireAddrs in the header.
//
// The stock functions (disassembly libr15, read this session; the PC twins and the rules are in equipment_refire_rules.h):
//   void CR15NetPlayerEquipmentCS::SetWeaponSlot(this = x0, handle = x1, value = x2) {   // 0x227ca84; SetAbilitySlot 0x227cde4 (+0x20);
//     rec = records[ indexTable[handle & 0xffff] ];     // ldr x8,[x0,#0xc8]; ldr x26,[x0,#0xf8]; stride 0x58   SetGrenadeSlot 0x227cf90 (+0x28)
//     if (rec.weapon == value) return;                  // ldr x9,[x8,#0x18]!; cmp x9,x2; b.eq
//     rec.weapon = value;                                // str x2,[x8]
//     ... then, when rec+0x10 or rec+0x14 is set, HandleWeaponChange's work (inline in the first two; SetGrenadeSlot calls
//     HandleGrenadeChange through its PLT stub 0xfd7640)
//   }
//   void HandleXChange(this = x0, handle = x1)           // 0x227afb4 / 0x227b124 / 0x227b1dc: reads the value from the record, sends the
//                                                        // equipment-changed event to the scripts that arm the slot
// The HOOK points are the setters. The three handler addresses are what a re-fire CALLS (handler(component, handle), never the setter,
// so a re-fire cannot start another debounce).
//
// The local player's record: CR15NetGame::UserId(game, key0, key1) at 0x135a9b8 (key0 = *(ctx+0x58), key1 = the record's link, game =
// NetGameMT(*(ctx+8))) returns the slot handle of the player with that actor key; the local player is the net game's slot 0
// (rules::LocalSlotHandle). Both lookups are the game's own functions, called by their pinned addresses.

constexpr int kSlots = 3;
constexpr int kWords = 2;  // 0 = JUMP_SLOT (the PLT stub's GOT slot), 1 = GLOB_DAT
constexpr const char* kWordName[kWords] = {"jump_slot", "glob_dat"};
// NetGameMT is `mov w8,#0x8510; ldr x0,[x0,x8]; ret` (pinned): its one read, checked here before the call so a wild argument is a refusal.
constexpr std::uintptr_t kNetGameField = 0x8510;
// The setter wrappers log one Debug record per call; this many per process, then one WARNING and silence (the sentinel log has no level gate).
constexpr unsigned kSetterLines = 256;

using SetterFn = void (*)(void* component, std::uint64_t handle, std::uint64_t value);
using HandlerFn = void (*)(void* component, std::uint64_t handle);
using NetGameFn = std::uintptr_t (*)(std::uintptr_t arg);
using UserIdFn = std::uint32_t (*)(void* game, std::uint64_t key0, std::uint64_t key1);

// Each of the six words has its own stored original. InstallImportHook clears the word it stored when a word is refused, so a shared one
// would unhook the words that installed before it.
std::atomic<std::uintptr_t> g_next[kSlots][kWords]{};
EquipRefireFns g_fns{};  // written before any word is swapped
std::atomic<bool> g_on{false};
std::uint64_t (*g_clock)() = nullptr;

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // a plain POD (no constructor at load)
rules::Debouncer g_deb;                            // guarded by g_mu (constant-initialised: every member has an initializer)
std::uintptr_t g_vtable[kSlots] = {};              // guarded by g_mu: the component's vtable when the change was tracked

std::atomic<unsigned> g_tracked{0}, g_unchanged{0}, g_not_local{0}, g_refired{0}, g_skipped{0}, g_setter_lines{0};

void Lock() { pthread_mutex_lock(&g_mu); }
void Unlock() { pthread_mutex_unlock(&g_mu); }

std::uint64_t Now() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000 + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

// The rules' reader: a read that never faults (an unmapped range is false, never a signal).
struct Reader {
  bool operator()(std::uintptr_t address, void* out, std::size_t size) const { return SafeRead(address, out, size); }
};

rules::Slot SlotOf(int i) { return static_cast<rules::Slot>(i); }

void HexText(unsigned long long v, int digits, char* out, std::size_t n) { std::snprintf(out, n, "0x%0*llx", digits, v); }

// True when the record the handle names in the component belongs to the net game's slot 0, the local player. Fails closed: anything
// unreadable or unmatched is "not local" and the change is not tracked.
bool IsLocalRecord(std::uintptr_t component, std::uint32_t handle) {
  Reader read;
  rules::ActorKey actor;
  std::uint32_t local = 0;
  if (!rules::ReadActorKey(read, component, handle, &actor)) return false;
  std::uintptr_t probe = 0;
  if (!read(actor.netGameArg + kNetGameField, &probe, sizeof probe)) return false;
  const auto net_game = reinterpret_cast<NetGameFn>(g_fns.net_game);
  const auto user_id = reinterpret_cast<UserIdFn>(g_fns.user_id);
  if (net_game == nullptr || user_id == nullptr) return false;
  const std::uintptr_t game = net_game(actor.netGameArg);
  if (!rules::LocalSlotHandle(read, game, &local)) return false;
  return user_id(reinterpret_cast<void*>(game), actor.key[0], actor.key[1]) == local;
}

void Setter(int slot, int word, void* component, std::uint64_t handle, std::uint64_t value) {
  // Stored before the word was swapped (InstallImportHook), so the original is set whenever this runs.
  const auto next = reinterpret_cast<SetterFn>(g_next[slot][word].load(std::memory_order_acquire));
  const auto cs = reinterpret_cast<std::uintptr_t>(component);
  const auto h32 = static_cast<std::uint32_t>(handle);  // the setters use the low 16 bits to index and pass the low 32 on
  // The setter runs the handler only when the value differs from the record's; the record is read before it is overwritten.
  Reader reader;
  std::uint64_t before = 0;
  const bool read = rules::RecordValue(reader, cs, h32, SlotOf(slot), &before);
  next(component, handle, value);
  const bool changed = read && before != value;
  const bool local = changed && IsLocalRecord(cs, h32);
  rules::Verdict verdict;
  {
    Lock();
    // `local` is asked of the game only for a change, so an unchanged call is told "local" to reach the rule's own kUnchanged verdict
    // (otherwise it would read as another player's change in the log and in the counters).
    verdict = g_deb.Changed(SlotOf(slot), cs, h32, value, local || !changed, false, changed, Now());
    if (verdict == rules::Verdict::kTracked) reader(cs, &g_vtable[slot], sizeof g_vtable[slot]);
    Unlock();
  }
  if (verdict == rules::Verdict::kTracked) g_tracked.fetch_add(1, std::memory_order_relaxed);
  if (verdict == rules::Verdict::kUnchanged) g_unchanged.fetch_add(1, std::memory_order_relaxed);
  if (verdict == rules::Verdict::kNotLocal) g_not_local.fetch_add(1, std::memory_order_relaxed);
  const unsigned n = g_setter_lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= kSetterLines) {
    char hbuf[16], vbuf[24];
    HexText(h32, 8, hbuf, sizeof hbuf);
    HexText(value, 16, vbuf, sizeof vbuf);
    Rec().S("slot", rules::SlotName(SlotOf(slot))).S("word", kWordName[word]).S("verdict", rules::VerdictName(verdict)).S("handle", hbuf)
        .S("value", vbuf).S("read", read ? "yes" : "no").S("changed", changed ? "yes" : "no").S("local", local ? "yes" : "no")
        .Emit(NEVR_LOG_DEBUG, "equiprefire: equipment change");
  } else if (n == kSetterLines + 1) {
    Rec().U("cap", kSetterLines).Emit(NEVR_LOG_WARNING, "equiprefire: per-call records capped, further setter calls are counted, not logged");
  }
}

template <int S, int W>
void Wrapper(void* component, std::uint64_t handle, std::uint64_t value) {
  Setter(S, W, component, handle, value);
}

using WrapperFn = void (*)(void*, std::uint64_t, std::uint64_t);
constexpr WrapperFn kWrappers[kSlots][kWords] = {
    {&Wrapper<0, 0>, &Wrapper<0, 1>}, {&Wrapper<1, 0>, &Wrapper<1, 1>}, {&Wrapper<2, 0>, &Wrapper<2, 1>}};

// Why a due re-fire is not made, or nullptr when it is made. Everything is read again: the component may have been freed, the level
// unloaded, the record reused or the choice changed by something other than the setter.
const char* StaleReason(int slot, const rules::Refire& r) {
  Reader reader;
  std::uint8_t span[rules::kComponentSpan];
  if (!SafeRead(r.owner, span, sizeof span)) return "component_unreadable";
  std::uintptr_t vtable = 0;
  std::memcpy(&vtable, span, sizeof vtable);
  std::uintptr_t tracked = 0;
  Lock();
  tracked = g_vtable[slot];
  Unlock();
  if (vtable == 0 || vtable != tracked) return "component_replaced";
  std::uint64_t current = 0;
  if (!rules::RecordValue(reader, r.owner, r.handle, r.slot, &current)) return "record_gone";
  if (current != r.value) return "record_changed_elsewhere";
  if (!IsLocalRecord(r.owner, r.handle)) return "not_local";
  return nullptr;
}

}  // namespace

EquipRefireFns EquipRefireFnsAt(std::uintptr_t base, const EquipRefireAddrs& addrs) {
  EquipRefireFns f{};
  for (int i = 0; i < kSlots; ++i) f.handler[i] = base + addrs.handler[i];
  f.net_game = base + addrs.net_game;
  f.user_id = base + addrs.user_id;
  return f;
}

bool EquipRefireWordPinsMatch(std::uintptr_t base) {
  return CheckWordPins("equiprefire", base, kEquipRefireWordPins, kEquipRefireWordPinCount);
}

EquipRefireStats GetEquipRefireStats() {
  EquipRefireStats s;
  s.tracked = g_tracked.load();
  s.unchanged = g_unchanged.load();
  s.not_local = g_not_local.load();
  s.refired = g_refired.load();
  s.skipped = g_skipped.load();
  s.on = g_on.load();
  return s;
}

void SetEquipRefireClockForTest(std::uint64_t (*clock_ms)()) { g_clock = clock_ms; }

void ResetEquipRefireForTest() {
  for (auto& row : g_next)
    for (auto& w : row) w = 0;
  g_fns = EquipRefireFns{};
  g_on = false;
  g_clock = nullptr;
  Lock();
  g_deb.Clear();
  for (auto& v : g_vtable) v = 0;
  Unlock();
  g_tracked = 0;
  g_unchanged = 0;
  g_not_local = 0;
  g_refired = 0;
  g_skipped = 0;
  g_setter_lines = 0;
}

void EquipRefireTick(std::uintptr_t /*game*/) {
  if (!g_on.load(std::memory_order_acquire)) return;
  rules::Refire due[kSlots];
  std::size_t count = 0;
  Lock();
  const std::uint64_t now = Now();
  while (count < kSlots && g_deb.Due(now, &due[count])) ++count;
  Unlock();
  for (std::size_t n = 0; n < count; ++n) {
    const rules::Refire& r = due[n];
    const int slot = static_cast<int>(r.slot);
    char hbuf[16], vbuf[24];
    HexText(r.handle, 8, hbuf, sizeof hbuf);
    HexText(r.value, 16, vbuf, sizeof vbuf);
    if (const char* why = StaleReason(slot, r)) {
      g_skipped.fetch_add(1, std::memory_order_relaxed);
      Rec().S("slot", rules::SlotName(r.slot)).S("value", vbuf).S("handle", hbuf).S("reason", why)
          .Emit(NEVR_LOG_INFO, "equiprefire: equipment re-fire skipped (the change is left as the game made it)");
      continue;
    }
    reinterpret_cast<HandlerFn>(g_fns.handler[slot])(reinterpret_cast<void*>(r.owner), r.handle);
    g_refired.fetch_add(1, std::memory_order_relaxed);
    Rec().S("slot", rules::SlotName(r.slot)).S("value", vbuf).S("handle", hbuf).U("changes", r.changes).U("quiet_ms", r.quietMs)
        .Emit(NEVR_LOG_INFO, "equiprefire: equipment re-fired (the game's change handler ran once more so the slot's scripts see the last choice)");
  }
}

InstallResult InstallEquipRefireAt(std::uintptr_t base, const EquipRefireAddrs& addrs, const EquipRefireFns& fns) {
  InstallResult result;
  if (base == 0) {
    result.reason = "no_base";
    Log(NEVR_LOG_ERROR, "equiprefire not installed: no game base");
    return result;
  }
  g_fns = fns;  // before any word is swapped: a wrapper reads it
  int installed = 0;
  SwapStatus first_failure = SwapStatus::kOk;
  const char* first_reason = "";
  for (int s = 0; s < kSlots; ++s) {
    const std::uintptr_t words[kWords] = {addrs.jump_slot[s], addrs.glob_dat[s]};
    for (int w = 0; w < kWords; ++w) {
      const InstallResult r = InstallImportHook("equiprefire", base, words[w], reinterpret_cast<std::uintptr_t>(kWrappers[s][w]),
                                                &g_next[s][w], addrs.setter[s]);
      Rec rec;
      rec.S("slot", rules::SlotName(SlotOf(s))).S("word", kWordName[w]).X("slot_elf_vaddr", words[w]).X("fn_elf_vaddr", addrs.setter[s])
          .S("status", StatusName(r.status)).S("reason", r.reason);
      if (r.status == SwapStatus::kOk) {
        ++installed;
        rec.Emit(NEVR_LOG_INFO, "equiprefire: slot swapped");
      } else if (r.status == SwapStatus::kRestoreFailed) {
        ++installed;  // the word was written; only the page's protection could not be put back (the caller logged it)
        rec.Emit(NEVR_LOG_ERROR, "equiprefire: slot swapped, but the page's old protection could not be restored");
      } else {
        if (first_failure == SwapStatus::kOk) {
          first_failure = r.status;
          first_reason = r.reason;
        }
        rec.Emit(NEVR_LOG_WARNING, "equiprefire: slot refused, its calls stay stock");
      }
    }
  }

  if (installed == 0) {
    result.status = first_failure;
    result.reason = first_reason;
    Log(NEVR_LOG_ERROR, "equiprefire: OFF; no slot installed, nothing changed");
    return result;
  }
  g_on.store(true, std::memory_order_release);
  char names[kSlots][48];
  for (int s = 0; s < kSlots; ++s)
    std::snprintf(names[s], sizeof names[s], "0x%llx,0x%llx", static_cast<unsigned long long>(addrs.jump_slot[s]),
                  static_cast<unsigned long long>(addrs.glob_dat[s]));
  Rec().U("slots", static_cast<unsigned long long>(installed)).U("of", static_cast<unsigned long long>(kSlots * kWords)).U("settle_ms", rules::kSettleMs)
      .S("weapon", names[0]).S("tac_mod", names[1]).S("ordnance", names[2])
      .Emit(NEVR_LOG_INFO, "equiprefire: on (a slot the local player changed is applied again after it has been still for settle_ms)");
  result.status = SwapStatus::kOk;
  result.real_resolved = true;
  return result;
}

InstallResult InstallEquipRefire(std::uintptr_t base) {
  InstallResult result;
  if (base == 0) {
    result.reason = "no_base";
    Log(NEVR_LOG_ERROR, "equiprefire not installed: no game base");
    return result;
  }
  if (ModuleSwitchPresent("equiprefire.off")) {
    const NevrLogField f[] = {{"why", "equiprefire.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "equiprefire off", f, 1);
    result.reason = "off_file";
    return result;
  }
  if (!EquipRefireWordPinsMatch(base)) {
    result.status = SwapStatus::kUnexpectedOriginal;
    result.reason = "word_pin_mismatch";
    return result;
  }
  return InstallEquipRefireAt(base, kEquipRefirePinned, EquipRefireFnsAt(base, kEquipRefirePinned));
}

}  // namespace q15t
