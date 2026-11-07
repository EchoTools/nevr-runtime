#include "runtime/combat/gear_trace.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/combat/gear_trace_rules.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"

namespace nevr_gear_trace {
namespace {

// The gear table rebuild (echovr.exe 0x140c4eba0, no arguments): MOV RAX,RSP; PUSH RBP; LEA RBP,[RAX-0x108];
// SUB RSP,0x200. It runs at startup and once per SettingsUpdateCB, never per frame.
constexpr std::uint64_t kGearReloadVA = 0x140C4EBA0;
constexpr std::array<std::uint8_t, 18> kGearReloadPrologue = {0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0xA8, 0xF8, 0xFE,
                                                              0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00};
using GearReloadFn = void (*)();

// CR15NetBalanceSettingsCS's init (0x140cfe650, the function that asserts "There can be only one instance of %s",
// makes the table at 0x1420d3a68 and, on a client, listens for R15NetBalanceSettingsUpdate at 0x140cfee4a):
// MOV [RSP+8],RCX; PUSH RBP,RBX,RSI,RDI,R12,R13,R14,R15. Once per component system made.
constexpr std::uint64_t kBalanceInitVA = 0x140CFE650;
constexpr std::array<std::uint8_t, 17> kBalanceInitPrologue = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57,
                                                               0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
using BalanceInitFn = std::uint64_t (*)(void* cs);

// CR15NetBalanceSettingsCS vslot 43 (0x140d4cd70): frees the table and zeroes 0x1420d3a68. MOV [RSP+0x10],RBX;
// MOV [RSP+0x18],RBP; MOV [RSP+0x20],RSI; PUSH RDI,R14,R15; SUB RSP,0x20. Once per component system torn down.
constexpr std::uint64_t kBalanceTeardownVA = 0x140D4CD70;
constexpr std::array<std::uint8_t, 24> kBalanceTeardownPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                                   0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                                                                   0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20};
using BalanceTeardownFn = void (*)(void* cs);

// The client's R15NetBalanceSettingsUpdate listener (0x140d49950, cs, msg): MOV [RSP+8],RBX; MOV [RSP+0x18],RBP;
// MOV [RSP+0x20],RSI; PUSH RDI; SUB RSP,0x20. msg+0x10 points at the resource id, msg+0x18 is the JSON text and
// msg+0x20 its size (the listener hands those three to CJson_LoadFromBuffer 0x1405f0bd0 and CResourceID_GetName).
// Once per push.
constexpr std::uint64_t kBalanceUpdateVA = 0x140D49950;
constexpr std::array<std::uint8_t, 20> kBalanceUpdatePrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89,
                                                                 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                                                 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20};
using BalanceUpdateFn = void (*)(void* cs, void* msg);

// The game's own readers (called, not hooked).
constexpr std::uint64_t kResolveConfigHashVA = 0x140D0B170;  // CJson* (id)
constexpr std::uint64_t kJsonGetFloatVA = 0x1405FCA80;       // float (CJson*, path hash, fallback)
using ResolveConfigHashFn = void* (*)(std::int64_t id);
using JsonGetFloatFn = float (*)(void* json, std::int64_t path, float fallback);

constexpr std::uint64_t kBalanceTableVA = 0x1420D3A68;       // CR15NetBalanceSettingsCS's table, or null
constexpr std::uint64_t kBalanceServerFlagVA = 0x1420D3A70;  // 1: the server's table (ResolveConfigHash skips it)
constexpr std::uint64_t kGearInitVA = 0x1420D3418;           // non-zero once the gear tables exist
constexpr std::uint64_t kOrdnanceListVA = 0x1420D3428;
constexpr std::uint64_t kTacmodListVA = 0x1420D3430;
constexpr std::uint64_t kWeaponListVA = 0x1420D3420;
constexpr std::size_t kGearEntrySize = 0x88;
constexpr std::size_t kGearNameSize = 0x40;
constexpr std::size_t kTableEntrySize = 0x18;
constexpr std::size_t kTableCountOffset = 0x30;

std::uintptr_t g_base = 0;
GearReloadFn g_originalGearReload = nullptr;
BalanceInitFn g_originalBalanceInit = nullptr;
BalanceTeardownFn g_originalBalanceTeardown = nullptr;
BalanceUpdateFn g_originalBalanceUpdate = nullptr;
ResolveConfigHashFn g_resolve = nullptr;
JsonGetFloatFn g_getFloat = nullptr;

std::mutex g_mutex;   // the snapshots below; the hooks run on the game thread, the lock is for safety only
Snapshot g_stock;     // the values at the startup gear load
Snapshot g_last;      // the values of the last record

template <typename T>
T ReadGlobal(std::uint64_t va) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const void*>(g_base + (va - kImageBase)), sizeof v);
  return v;
}

std::uint64_t TableCount(const std::uint8_t* table) {
  if (table == nullptr) return 0;
  std::uint64_t n = 0;
  std::memcpy(&n, table + kTableCountOffset, sizeof n);
  return n;
}

bool TableHolds(const std::uint8_t* table, std::uint64_t id) {
  const std::uint64_t n = TableCount(table);
  if (n == 0 || n > 4096) return false;
  const std::uint8_t* entries = nullptr;
  std::memcpy(&entries, table, sizeof entries);
  if (entries == nullptr) return false;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t key = 0;
    std::memcpy(&key, entries + i * kTableEntrySize, sizeof key);
    if (key == id) return true;
  }
  return false;
}

Snapshot Take() {
  Snapshot s;
  s.taken = true;
  s.values.fill(std::numeric_limits<float>::quiet_NaN());
  if (g_resolve == nullptr || g_getFloat == nullptr) return s;
  const auto* table = ReadGlobal<const std::uint8_t*>(kBalanceTableVA);
  s.fromTable = ReadGlobal<std::uint32_t>(kBalanceServerFlagVA) == 0 && TableHolds(table, kWeaponSettingsId);
  void* json = g_resolve(static_cast<std::int64_t>(kWeaponSettingsId));
  if (json == nullptr) return s;
  s.readable = true;
  for (std::size_t i = 0; i < kKeys.size(); ++i)
    s.values[i] = g_getFloat(json, static_cast<std::int64_t>(kKeys[i].hash), std::numeric_limits<float>::quiet_NaN());
  return s;
}

void Record(const char* at) {
  const Snapshot now = Take();
  std::lock_guard<std::mutex> lock(g_mutex);
  Log(EchoVR::LogLevel::Info, "%s", FormatValues(at, now, g_stock, g_last).c_str());
  g_last = now;
}

std::string TableState() {
  const auto* table = ReadGlobal<const std::uint8_t*>(kBalanceTableVA);
  char b[160];
  std::snprintf(b, sizeof(b), "table=%s files=%llu role=%s holds_mp_weapon_settings=%s",
                table ? "present" : "absent", static_cast<unsigned long long>(TableCount(table)),
                ReadGlobal<std::uint32_t>(kBalanceServerFlagVA) ? "server" : "client",
                TableHolds(table, kWeaponSettingsId) ? "yes" : "no");
  return b;
}

std::string GearNames(std::uint64_t listVa, std::uint64_t* count) {
  *count = 0;
  const auto* list = ReadGlobal<const std::uint8_t*>(listVa);
  if (list == nullptr) return "none";
  const std::uint8_t* data = nullptr;
  std::memcpy(&data, list + 8, sizeof data);
  std::memcpy(count, list + 0x10, sizeof *count);
  if (data == nullptr || *count > 64) return "unreadable";
  std::string out;
  for (std::uint64_t i = 0; i < *count; ++i) {
    char name[kGearNameSize + 1] = {};
    std::memcpy(name, data + i * kGearEntrySize, kGearNameSize);
    if (!out.empty()) out += ',';
    out += name[0] ? name : "(empty)";
  }
  return out.empty() ? "none" : out;
}

void GearReloadHook() {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const Caller caller = ClassifyCaller(static_cast<std::uint64_t>(ret - g_base) + kImageBase);
  g_originalGearReload();
  std::uint64_t weapons = 0, ordnances = 0, tacmods = 0;
  GearNames(kWeaponListVA, &weapons);
  const std::string ordNames = GearNames(kOrdnanceListVA, &ordnances);
  const std::string tacNames = GearNames(kTacmodListVA, &tacmods);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] gear tables loaded caller=%s gear_init=%s weapons=%llu ordnances=%llu (%s) tacmods=%llu (%s) %s",
      CallerName(caller), ReadGlobal<std::uint64_t>(kGearInitVA) ? "yes" : "no",
      static_cast<unsigned long long>(weapons), static_cast<unsigned long long>(ordnances), ordNames.c_str(),
      static_cast<unsigned long long>(tacmods), tacNames.c_str(), TableState().c_str());
  if (caller == Caller::kStartup) {
    const Snapshot now = Take();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_stock.taken) g_stock = now;
    Log(EchoVR::LogLevel::Info, "%s", FormatValues("startup", now, g_stock, g_last).c_str());
    g_last = now;
    return;
  }
  Record(caller == Caller::kSettingsUpdate ? "gear_reload_settings_update" : "gear_reload_other");
}

std::uint64_t BalanceInitHook(void* cs) {
  const std::uint64_t status = g_originalBalanceInit(cs);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] gear balance table made status=%llu %s (CR15NetBalanceSettingsCS init: a session's balance files "
      "start from the stock resources)",
      static_cast<unsigned long long>(status), TableState().c_str());
  Record("balance_table_made");
  return status;
}

void BalanceTeardownHook(void* cs) {
  const std::string before = TableState();
  g_originalBalanceTeardown(cs);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] gear balance table freed before={%s} after={%s} (CR15NetBalanceSettingsCS teardown: what a "
      "server pushed is dropped with it)",
      before.c_str(), TableState().c_str());
  Record("balance_table_freed");
}

// The stock sections of mp_weapon_settings; a pushed file's other section names are counted, not written (Rule 4).
bool IsStockSection(const std::string& k) {
  return k == "damage_table" || k == "weapon_properties" || k == "grenade_properties" || k == "ability_properties" ||
         k == "player_properties";
}

void BalanceUpdateHook(void* cs, void* msg) {
  std::uint64_t id = 0, size = 0;
  const char* text = nullptr;
  if (msg != nullptr) {
    const auto* m = static_cast<const std::uint8_t*>(msg);
    const std::uint64_t* idp = nullptr;
    std::memcpy(&idp, m + 0x10, sizeof idp);
    if (idp != nullptr) id = *idp;
    std::memcpy(&text, m + 0x18, sizeof text);
    std::memcpy(&size, m + 0x20, sizeof size);
  }
  std::string sections = "none", parsed = "not_read";
  std::size_t other = 0;
  if (text != nullptr && size > 0 && size <= (8u << 20)) {
    const nlohmann::json j = nlohmann::json::parse(text, text + size, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      parsed = "unparsed";
    } else {
      parsed = "parsed";
      sections.clear();
      for (auto it = j.begin(); it != j.end(); ++it) {
        if (!IsStockSection(it.key())) {
          ++other;
          continue;
        }
        if (!sections.empty()) sections += ',';
        sections += it.key();
      }
      if (sections.empty()) sections = "none";
    }
  }
  const Snapshot before = Take();
  const std::string tableBefore = TableState();
  g_originalBalanceUpdate(cs, msg);
  const Snapshot after = Take();
  std::size_t changed = 0;
  const std::string changes = FormatChanges(before, after, &changed);
  char idText[24];
  std::snprintf(idText, sizeof(idText), "0x%016llx", static_cast<unsigned long long>(id));
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] gear balance push received resource=%s bytes=%llu json=%s sections=%s other_sections=%zu "
      "sender=unknown (the listener is handed the message, not the peer that sent it) %s changed=%zu (%s)",
      id == kWeaponSettingsId ? "mp_weapon_settings" : idText, static_cast<unsigned long long>(size), parsed.c_str(),
      sections.c_str(), other, tableBefore.c_str(), changed, changes.c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  Log(EchoVR::LogLevel::Info, "%s", FormatValues("balance_push", after, g_stock, g_last).c_str());
  g_last = after;
}

std::string HexBytes(const void* address, std::size_t count) {
  if (address == nullptr) return "unreadable";
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

template <typename Fn, std::size_t Size>
bool HookChecked(std::uint64_t va, const std::array<std::uint8_t, Size>& prologue, Fn& original, PVOID detour,
                 const char* name) {
  void* target = nevr::ResolveVA_Checked(g_base, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] gear hook skipped name=%s va=0x%llX reason=prologue_mismatch expected=%s actual=%s (the record "
        "it writes is not written)",
        name, static_cast<unsigned long long>(va), HexBytes(prologue.data(), prologue.size()).c_str(),
        HexBytes(target, prologue.size()).c_str());
    return false;
  }
  original = reinterpret_cast<Fn>(target);
  return PatchDetour(&original, detour, name) != FALSE;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_base = gameBase;
  g_resolve = reinterpret_cast<ResolveConfigHashFn>(nevr::ResolveVA_Checked(g_base, kResolveConfigHashVA));
  g_getFloat = reinterpret_cast<JsonGetFloatFn>(nevr::ResolveVA_Checked(g_base, kJsonGetFloatVA));
  int ok = 0;
  ok += HookChecked(kGearReloadVA, kGearReloadPrologue, g_originalGearReload, reinterpret_cast<PVOID>(&GearReloadHook),
                    "GearTablesReload");
  ok += HookChecked(kBalanceInitVA, kBalanceInitPrologue, g_originalBalanceInit,
                    reinterpret_cast<PVOID>(&BalanceInitHook), "BalanceSettingsCSInit");
  ok += HookChecked(kBalanceTeardownVA, kBalanceTeardownPrologue, g_originalBalanceTeardown,
                    reinterpret_cast<PVOID>(&BalanceTeardownHook), "BalanceSettingsCSTeardown");
  ok += HookChecked(kBalanceUpdateVA, kBalanceUpdatePrologue, g_originalBalanceUpdate,
                    reinterpret_cast<PVOID>(&BalanceUpdateHook), "BalanceSettingsUpdateListener");
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] gear trace hooks: %d of 4 installed, readers resolve_config_hash=%s json_get_float=%s (measure "
      "only: every call is forwarded unchanged; the gear lines say what settings a client has at startup, at each "
      "server push and at each join and leave)",
      ok, g_resolve ? "ok" : "missing", g_getFloat ? "ok" : "missing");
}

void OnNetGameState(std::int32_t from, std::int32_t to) {
  if (g_base == 0) return;
  const char* event = SessionEvent(from, to);
  char at[64];
  std::snprintf(at, sizeof(at), "%s from=%d to=%d", event, from, to);
  Record(at);
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stock = Snapshot{};
  g_last = Snapshot{};
}

}  // namespace nevr_gear_trace
