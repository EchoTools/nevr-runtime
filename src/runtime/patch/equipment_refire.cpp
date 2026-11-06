#include "runtime/patch/equipment_refire.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
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
  const char* handlerName;
  std::array<std::uint8_t, 100> setterBody;
  std::array<std::uint8_t, 32> handlerPrologue;
};

// The three setters are 100 bytes each and identical but for the field offset they compare and write, the handler they call and the
// rel32 of that call. The whole body is pinned, so each row proves which field it writes and which handler it calls.
constexpr SetterRow kRows[kSlotCount] = {
    {Slot::kWeapon, "CR15NetPlayerEquipmentCS::SetWeaponSlot", 0x140D498E0, 0x140CEC740, "HandleWeaponChange",
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00,
      0x00, 0x00, 0x48, 0x8b, 0xf1, 0x8b, 0xda, 0x0f, 0xb7, 0xd2, 0x0f, 0xb7, 0x14, 0x90, 0x48, 0x6b, 0xfa, 0x58, 0x48, 0x03,
      0xb9, 0xf8, 0x00, 0x00, 0x00, 0x4c, 0x39, 0x47, 0x18, 0x74, 0x21, 0x8b, 0xd3, 0x4c, 0x89, 0x47, 0x18, 0xe8, 0x22, 0x2e,
      0xfa, 0xff, 0x83, 0x7f, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7f, 0x14, 0x00, 0x74, 0x0a, 0x8b, 0xd3, 0x48, 0x8b, 0xce, 0xe8,
      0x5c, 0xa4, 0xff, 0xff, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3},
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70,
      0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xf1, 0x8b}},
    {Slot::kTacMod, "CR15NetPlayerEquipmentCS::SetAbilitySlot", 0x140D43F00, 0x140CEAAA0, "HandleAbilityChange",
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xc8, 0x00,
      0x00, 0x00, 0x48, 0x8b, 0xf1, 0x8b, 0xda, 0x0f, 0xb7, 0xd2, 0x0f, 0xb7, 0x14, 0x90, 0x48, 0x6b, 0xfa, 0x58, 0x48, 0x03,
      0xb9, 0xf8, 0x00, 0x00, 0x00, 0x4c, 0x39, 0x47, 0x20, 0x74, 0x21, 0x8b, 0xd3, 0x4c, 0x89, 0x47, 0x20, 0xe8, 0x62, 0x6b,
      0xfa, 0xff, 0x83, 0x7f, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7f, 0x14, 0x00, 0x74, 0x0a, 0x8b, 0xd3, 0x48, 0x8b, 0xce, 0xe8,
      0x3c, 0xfe, 0xff, 0xff, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3},
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70,
      0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xf1, 0x8b}},
    {Slot::kOrdnance, "CR15NetPlayerEquipmentCS::SetGrenadeSlot", 0x140D45990, 0x140CEB720, "HandleGrenadeChange",
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
constexpr std::array<std::uint8_t, 8> kNetGamePrologue = {0x48, 0x8b, 0x81, 0x18, 0x85, 0x00, 0x00, 0xc3};
// The net game's "slot handle of the player whose actor key this is" (0x1401c92d0): (game, u32* out, u64 key[2]) -> out.
constexpr std::uint64_t kFindSlotVa = 0x1401C92D0;
constexpr std::array<std::uint8_t, 32> kFindSlotPrologue = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48,
                                                            0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                            0xec, 0x30, 0x66, 0x83, 0xb9, 0xdc, 0x00, 0x00, 0x00, 0x00};

using HandlerFn = void (*)(void* component, std::uint32_t handle);
using NetGameFn = void* (*)(void* contextArg);
using FindSlotFn = std::uint32_t* (*)(void* game, std::uint32_t* out, std::uint64_t* key);

SetterFn g_original[kSlotCount] = {};
HandlerFn g_handler[kSlotCount] = {};
NetGameFn g_netGame = nullptr;
FindSlotFn g_findSlot = nullptr;
bool g_installed = false;
std::atomic<SlotFilter> g_filter{nullptr};

std::mutex g_lock;
Tracker g_tracker;  // guarded by g_lock

void ValueText(std::uint64_t value, char* out, int size) { EchoVR::FormatSymbolId(out, size, value); }

// The PC side of the shared feature (equipment_refire_rules.h): the game's memory, its two lookups, its handlers, the clock and the
// log. Every decision is the header's.
struct PcEnv {
  // GetNetGameFromContext only loads [arg + 0x8518] (kNetGamePrologue); the header probes that field before the game's code reads it.
  static constexpr std::size_t kNetGameField = 0x8518;

  // Reads the game's memory after checking it is committed and readable (VirtualQuery only; no fault to catch).
  bool operator()(std::uintptr_t address, void* out, std::size_t size) {
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), size)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
  }
  std::uintptr_t NetGame(std::uintptr_t arg) { return reinterpret_cast<std::uintptr_t>(g_netGame(reinterpret_cast<void*>(arg))); }
  std::uint32_t SlotOf(std::uintptr_t game, const std::uint64_t key[2]) {
    std::uint64_t k[2] = {key[0], key[1]};
    std::uint32_t found = 0xffffffffU;
    g_findSlot(reinterpret_cast<void*>(game), &found, k);
    return found;
  }
  void CallHandler(Slot slot, std::uintptr_t owner, std::uint32_t handle) {
    g_handler[static_cast<std::size_t>(slot)](reinterpret_cast<void*>(owner), handle);
  }
  std::uint64_t NowMs() { return GetTickCount64(); }
  void Lock() { g_lock.lock(); }
  void Unlock() { g_lock.unlock(); }

  void LogChange(Slot slot, const SetterResult& r, std::uint32_t handle, std::uint64_t value) {
    char text[96];
    ValueText(value, text, sizeof text);
    Log(EchoVR::LogLevel::Debug, "[NEVR.EQUIP] %s slot=%s verdict=%s handle=0x%08x value=%s read=%s changed=%s local=%s", kMsgChange,
        SlotName(slot), VerdictName(r.verdict), handle, text, r.read ? "yes" : "no", r.changed ? "yes" : "no", r.local ? "yes" : "no");
  }
  void LogRefired(const Refire& r) {
    char text[96];
    ValueText(r.value, text, sizeof text);
    Log(EchoVR::LogLevel::Info, "[NEVR.EQUIP] %s slot=%s value=%s handle=0x%08x changes=%llu quiet_ms=%llu", kMsgRefired, SlotName(r.slot),
        text, r.handle, static_cast<unsigned long long>(r.changes), static_cast<unsigned long long>(r.quietMs));
  }
  void LogSkipped(const Refire& r, const char* reason) {
    char text[96];
    ValueText(r.value, text, sizeof text);
    Log(EchoVR::LogLevel::Info, "[NEVR.EQUIP] %s slot=%s value=%s handle=0x%08x reason=%s", kMsgSkipped, SlotName(r.slot), text, r.handle,
        reason);
  }
};

template <std::size_t I>
void SetterHook(void* component, std::uint32_t handle, std::uint64_t value) {
  if (const SlotFilter filter = g_filter.load(std::memory_order_acquire)) value = filter(static_cast<int>(I), component, handle, value);
  PcEnv env;
  OnSetter(env, g_tracker, kRows[I].slot, reinterpret_cast<std::uintptr_t>(component), handle, value, g_isServer != FALSE,
           [&] { g_original[I](component, handle, value); });
}

// Hex of the first bytes of `bytes` (at most 16), for the mismatch line.
void HexBytes(const std::uint8_t* bytes, std::size_t size, char* out, std::size_t outSize) {
  std::size_t n = size < 16 ? size : 16;
  std::size_t at = 0;
  out[0] = '\0';
  for (std::size_t i = 0; i < n && at + 3 <= outSize; ++i) at += static_cast<std::size_t>(std::snprintf(out + at, outSize - at, "%02x", bytes[i]));
}

// True when `size` bytes at `va` are `expected`. A mismatch is a Warning with the address, the name, the first differing offset and
// the first bytes of each side (logging.md Rule 6), and the caller installs nothing.
bool Matches(std::uintptr_t gameBase, std::uint64_t va, const std::uint8_t* expected, std::size_t size, const char* name) {
  void* target = nevr::ResolveVA_Checked(gameBase, va);
  if (target != nullptr && nevr::ValidatePrologue(target, expected, size)) return true;
  std::size_t differs = 0;
  std::size_t start = 0;  // the bytes shown begin 8 before the first difference (or at 0), so a late difference is still in the window
  char want[40] = "";
  char have[40] = "unreadable";
  if (target != nullptr && IsReadableMemory(target, size)) {
    const auto* actual = static_cast<const std::uint8_t*>(target);
    while (differs < size && actual[differs] == expected[differs]) ++differs;
    start = differs < 8 ? 0 : differs - 8;
    HexBytes(actual + start, size - start, have, sizeof have);
    HexBytes(expected + start, size - start, want, sizeof want);
  } else {
    HexBytes(expected, size, want, sizeof want);
  }
  nevr_boot_log_tee::TeeFprintf(
      "[NEVR.EQUIP] warn; equipment re-fire hook not installed: %s va=0x%llx reason=bytes_mismatch first_difference=%zu window_start=%zu expected=%s actual=%s "
      "(the game build is not the one this was written against, so the equipment change is left as the game makes it)\n",
      name, static_cast<unsigned long long>(va), differs, start, want, have);
  return false;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  bool ok = Matches(gameBase, kNetGameVa, kNetGamePrologue.data(), kNetGamePrologue.size(), "GetNetGameFromContext");
  ok = Matches(gameBase, kFindSlotVa, kFindSlotPrologue.data(), kFindSlotPrologue.size(), "FindPlayerSlotByActor") && ok;
  for (const SetterRow& row : kRows) {
    ok = Matches(gameBase, row.setterVa, row.setterBody.data(), row.setterBody.size(), row.name) && ok;
    ok = Matches(gameBase, row.handlerVa, row.handlerPrologue.data(), row.handlerPrologue.size(), row.handlerName) && ok;
  }
  if (!ok) {  // nothing is hooked on a build this was not written against, and the lines above say which check failed
    nevr_boot_log_tee::TeeFprintf("[NEVR.EQUIP] warn; equipment re-fire hooks: 0 of %zu installed (a pinned check failed, see above)\n",
                                  kSlotCount);
    return;
  }
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
      nevr_boot_log_tee::TeeFprintf("[NEVR.EQUIP] debug; equipment setter hook installed slot=%s va=0x%llx handler=0x%llx\n",
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

bool SetSlotFilter(SlotFilter filter) {
  if (!g_installed) return false;
  g_filter.store(filter, std::memory_order_release);
  return true;
}

SetterFn OriginalSetter(int slot) {
  if (!g_installed || slot < 0 || slot >= static_cast<int>(kSlotCount)) return nullptr;
  return g_original[slot];
}

void OnFrame() {
  if (!g_installed) return;
  PcEnv env;
  FireDue(env, g_tracker);
}

}  // namespace nevr_equipment_refire
