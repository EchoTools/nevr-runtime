#include "runtime/patch/equipment_refire.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "abi/echovr.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/lifecycle/cli.h"  // g_isServer
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/equipment_refire_rules.h"

namespace nevr_equipment_refire {
namespace {

using nevr_crash_recovery::IsReadableMemory;

struct SetterRow {
  Slot slot;
  const char* name;
  std::uint64_t setterVa;
  std::uint64_t handlerVa;
  std::array<std::uint8_t, 100> setterBody;
  std::array<std::uint8_t, 32> handlerPrologue;
};

// The three setters are 100 bytes each and identical but for the field offset they compare and write, the handler they call and the
// rel32 of that call. The whole body is pinned, so each row proves which field it writes and which handler it calls.
constexpr SetterRow kRows[kSlotCount] = {
    {Slot::kWeapon, "CR15NetPlayerEquipmentCS::SetWeaponSlot", 0x140D498E0, 0x140CEC740,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00,
      0x00, 0x00, 0x48, 0x8b, 0xf1, 0x8b, 0xda, 0x0f, 0xb7, 0xd2, 0x0f, 0xb7, 0x14, 0x90, 0x48, 0x6b, 0xfa, 0x58, 0x48, 0x03,
      0xb9, 0xf8, 0x00, 0x00, 0x00, 0x4c, 0x39, 0x47, 0x18, 0x74, 0x21, 0x8b, 0xd3, 0x4c, 0x89, 0x47, 0x18, 0xe8, 0x22, 0x2e,
      0xfa, 0xff, 0x83, 0x7f, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7f, 0x14, 0x00, 0x74, 0x0a, 0x8b, 0xd3, 0x48, 0x8b, 0xce, 0xe8,
      0x5c, 0xa4, 0xff, 0xff, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3},
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70,
      0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xf1, 0x8b}},
    {Slot::kTacMod, "CR15NetPlayerEquipmentCS::SetAbilitySlot", 0x140D43F00, 0x140CEAAA0,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00,
      0x00, 0x00, 0x48, 0x8b, 0xf1, 0x8b, 0xda, 0x0f, 0xb7, 0xd2, 0x0f, 0xb7, 0x14, 0x90, 0x48, 0x6b, 0xfa, 0x58, 0x48, 0x03,
      0xb9, 0xf8, 0x00, 0x00, 0x00, 0x4c, 0x39, 0x47, 0x20, 0x74, 0x21, 0x8b, 0xd3, 0x4c, 0x89, 0x47, 0x20, 0xe8, 0x62, 0x6b,
      0xfa, 0xff, 0x83, 0x7f, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7f, 0x14, 0x00, 0x74, 0x0a, 0x8b, 0xd3, 0x48, 0x8b, 0xce, 0xe8,
      0x3c, 0xfe, 0xff, 0xff, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3},
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70,
      0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xf1, 0x8b}},
    {Slot::kOrdnance, "CR15NetPlayerEquipmentCS::SetGrenadeSlot", 0x140D45990, 0x140CEB720,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00,
      0x00, 0x00, 0x48, 0x8b, 0xf1, 0x8b, 0xda, 0x0f, 0xb7, 0xd2, 0x0f, 0xb7, 0x14, 0x90, 0x48, 0x6b, 0xfa, 0x58, 0x48, 0x03,
      0xb9, 0xf8, 0x00, 0x00, 0x00, 0x4c, 0x39, 0x47, 0x28, 0x74, 0x21, 0x8b, 0xd3, 0x4c, 0x89, 0x47, 0x28, 0xe8, 0x52, 0x5d,
      0xfa, 0xff, 0x83, 0x7f, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7f, 0x14, 0x00, 0x74, 0x0a, 0x8b, 0xd3, 0x48, 0x8b, 0xce, 0xe8,
      0xac, 0xe3, 0xff, 0xff, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3},
     {0x4c, 0x8b, 0xdc, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x4c,
      0x8b, 0xf9, 0x8b, 0xf2, 0x0f, 0xb7, 0xd2, 0x0f, 0xb7, 0x14, 0x90, 0x48}},
};

// GetNetGameFromContext (0x140113a90): MOV RAX,[RCX+0x8518]; RET. The argument is *(ctx + 8).
constexpr std::uint64_t kNetGameVa = 0x140113A90;
constexpr std::size_t kNetGameFieldOffset = 0x8518;  // the displacement in kNetGamePrologue
constexpr std::array<std::uint8_t, 8> kNetGamePrologue = {0x48, 0x8b, 0x81, 0x18, 0x85, 0x00, 0x00, 0xc3};
// The net game's "slot handle of the player whose actor key this is" (0x1401c92d0): (game, u32* out, u64 key[2]) -> out.
constexpr std::uint64_t kFindSlotVa = 0x1401C92D0;
constexpr std::array<std::uint8_t, 32> kFindSlotPrologue = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48,
                                                            0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                            0xec, 0x30, 0x66, 0x83, 0xb9, 0xdc, 0x00, 0x00, 0x00, 0x00};

using SetterFn = void (*)(void* component, std::uint32_t handle, std::uint64_t value);
using HandlerFn = void (*)(void* component, std::uint32_t handle);
using NetGameFn = void* (*)(void* contextArg);
using FindSlotFn = std::uint32_t* (*)(void* game, std::uint32_t* out, std::uint64_t* key);

SetterFn g_original[kSlotCount] = {};
HandlerFn g_handler[kSlotCount] = {};
NetGameFn g_netGame = nullptr;
FindSlotFn g_findSlot = nullptr;
bool g_installed = false;

std::mutex g_lock;
Debouncer g_debouncer;                          // guarded by g_lock
std::uintptr_t g_vtable[kSlotCount] = {};       // guarded by g_lock: the component's vtable when the change was tracked

// Reads the game's memory after checking it is committed and readable (VirtualQuery only; no fault to catch).
struct ProcessRead {
  bool operator()(std::uintptr_t address, void* out, std::size_t size) const {
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), size)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
  }
};

// True when the record belongs to the player slot the net game calls slot 0, which is the local player. Fails closed: anything
// unreadable or unmatched is "not local" and the change is not tracked.
bool IsLocalRecord(std::uintptr_t component, std::uint32_t handle) {
  ProcessRead read;
  ActorKey actor;
  std::uint32_t local = 0;
  if (!ReadActorKey(read, component, handle, &actor)) return false;
  // GetNetGameFromContext only loads [arg + 0x8518] (the pinned body); the argument is checked before the game's code reads it.
  std::uintptr_t netGameField = 0;
  if (!read(actor.netGameArg + kNetGameFieldOffset, &netGameField, sizeof netGameField)) return false;
  const auto game = reinterpret_cast<std::uintptr_t>(g_netGame(reinterpret_cast<void*>(actor.netGameArg)));
  if (!LocalSlotHandle(read, game, &local)) return false;
  std::uint32_t found = 0xffffffffU;
  g_findSlot(reinterpret_cast<void*>(game), &found, actor.key);
  return found == local;
}

void ValueText(std::uint64_t value, char* out, int size) { EchoVR::FormatSymbolId(out, size, value); }

template <std::size_t I>
void SetterHook(void* component, std::uint32_t handle, std::uint64_t value) {
  const SetterRow& row = kRows[I];
  const auto cs = reinterpret_cast<std::uintptr_t>(component);
  const bool server = g_isServer != FALSE;
  // The setter runs the handler only when the value differs from the record's; the record is read before it is overwritten.
  std::uint64_t before = 0;
  ProcessRead reader;
  const bool read = !server && RecordValue(reader, cs, handle, row.slot, &before);
  g_original[I](component, handle, value);
  const bool changed = read && before != value;
  const bool local = changed && IsLocalRecord(cs, handle);
  Verdict verdict;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    verdict = g_debouncer.Changed(row.slot, cs, handle, value, local, server, changed, GetTickCount64());
    if (verdict == Verdict::kTracked) reader(cs, &g_vtable[I], sizeof g_vtable[I]);
  }
  char text[96];
  ValueText(value, text, sizeof text);
  Log(EchoVR::LogLevel::Debug, "[NEVR.EQUIP] equipment change slot=%s verdict=%s handle=0x%08x value=%s read=%s changed=%s local=%s",
      SlotName(row.slot), VerdictName(verdict), handle, text, read ? "yes" : "no", changed ? "yes" : "no", local ? "yes" : "no");
}

// Why a due re-fire is not made, or nullptr when it is made.
const char* StaleReason(std::size_t slotIndex, const Refire& r) {
  const SetterRow& row = kRows[slotIndex];
  ProcessRead reader;
  if (!IsReadableMemory(reinterpret_cast<const void*>(r.owner), kComponentSpan)) return "component_unreadable";
  std::uintptr_t vtable = 0;
  reader(r.owner, &vtable, sizeof vtable);
  std::uintptr_t tracked = 0;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    tracked = g_vtable[slotIndex];
  }
  if (vtable == 0 || vtable != tracked) return "component_replaced";
  std::uint64_t current = 0;
  if (!RecordValue(reader, r.owner, r.handle, row.slot, &current)) return "record_gone";
  if (current != r.value) return "record_changed_elsewhere";
  if (!IsLocalRecord(r.owner, r.handle)) return "not_local";
  return nullptr;
}

bool Matches(std::uintptr_t gameBase, std::uint64_t va, const std::uint8_t* expected, std::size_t size, const char* name) {
  void* target = nevr::ResolveVA_Checked(gameBase, va);
  if (target != nullptr && nevr::ValidatePrologue(target, expected, size)) return true;
  nevr_boot_log_tee::TeeFprintf("[NEVR.EQUIP] warn; equipment re-fire not installed: %s va=0x%llx reason=bytes_mismatch\n", name,
                                static_cast<unsigned long long>(va));
  return false;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  bool ok = Matches(gameBase, kNetGameVa, kNetGamePrologue.data(), kNetGamePrologue.size(), "GetNetGameFromContext");
  ok = Matches(gameBase, kFindSlotVa, kFindSlotPrologue.data(), kFindSlotPrologue.size(), "FindPlayerSlotByActor") && ok;
  for (const SetterRow& row : kRows) {
    ok = Matches(gameBase, row.setterVa, row.setterBody.data(), row.setterBody.size(), row.name) && ok;
    ok = Matches(gameBase, row.handlerVa, row.handlerPrologue.data(), row.handlerPrologue.size(), "equipment change handler") && ok;
  }
  if (!ok) return;  // nothing is hooked on a build this was not written against, and the lines above say which check failed
  g_netGame = reinterpret_cast<NetGameFn>(nevr::ResolveVA_Checked(gameBase, kNetGameVa));
  g_findSlot = reinterpret_cast<FindSlotFn>(nevr::ResolveVA_Checked(gameBase, kFindSlotVa));
  for (std::size_t i = 0; i < kSlotCount; ++i) g_handler[i] = reinterpret_cast<HandlerFn>(nevr::ResolveVA_Checked(gameBase, kRows[i].handlerVa));
  g_original[0] = reinterpret_cast<SetterFn>(nevr::ResolveVA_Checked(gameBase, kRows[0].setterVa));
  g_original[1] = reinterpret_cast<SetterFn>(nevr::ResolveVA_Checked(gameBase, kRows[1].setterVa));
  g_original[2] = reinterpret_cast<SetterFn>(nevr::ResolveVA_Checked(gameBase, kRows[2].setterVa));
  int installed = 0;
  const auto hook = [&](std::size_t i, void* detour) {
    if (PatchDetour(&g_original[i], detour, kRows[i].name)) {
      ++installed;
      nevr_boot_log_tee::TeeFprintf("[NEVR.EQUIP] info; equipment setter hook installed slot=%s va=0x%llx handler=0x%llx\n",
                                    SlotName(kRows[i].slot), static_cast<unsigned long long>(kRows[i].setterVa),
                                    static_cast<unsigned long long>(kRows[i].handlerVa));
    }
  };
  hook(0, reinterpret_cast<void*>(&SetterHook<0>));
  hook(1, reinterpret_cast<void*>(&SetterHook<1>));
  hook(2, reinterpret_cast<void*>(&SetterHook<2>));
  g_installed = installed > 0;
  nevr_boot_log_tee::TeeFprintf(
      "[NEVR.EQUIP] %s; equipment re-fire hooks: %d of %zu installed (a slot changed by the local player is applied again after %llu ms of "
      "stillness)\n",
      installed == static_cast<int>(kSlotCount) ? "info" : "warn", installed, kSlotCount,
      static_cast<unsigned long long>(kSettleMs));
}

void OnFrame() {
  if (!g_installed) return;
  Refire due[kSlotCount];
  std::size_t count = 0;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    const std::uint64_t now = GetTickCount64();
    while (count < kSlotCount && g_debouncer.Due(now, &due[count])) ++count;
  }
  for (std::size_t n = 0; n < count; ++n) {
    const Refire& r = due[n];
    const auto slotIndex = static_cast<std::size_t>(r.slot);
    char text[96];
    ValueText(r.value, text, sizeof text);
    if (const char* why = StaleReason(slotIndex, r)) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.EQUIP] equipment re-fire skipped slot=%s value=%s handle=0x%08x reason=%s (the change is left as the game made it)",
          SlotName(r.slot), text, r.handle, why);
      continue;
    }
    g_handler[slotIndex](reinterpret_cast<void*>(r.owner), r.handle);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.EQUIP] equipment re-fired slot=%s value=%s handle=0x%08x changes=%llu quiet_ms=%llu (the game's change handler ran once more "
        "so the slot's scripts see the last choice)",
        SlotName(r.slot), text, r.handle, static_cast<unsigned long long>(r.changes), static_cast<unsigned long long>(r.quietMs));
  }
}

}  // namespace nevr_equipment_refire
