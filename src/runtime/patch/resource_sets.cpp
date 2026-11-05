/*
 * resource_sets.cpp - plugin resource sets on PC (plugin API v9, #2161; docs/reference/plugin-resources.md).
 *
 * A plugin hands register_resources the bytes of a resource set as its own image maps them. This file checks that
 * those bytes are file bytes of that plugin's image (image_file_range.h), opens the plugin's file read-only and
 * deny-write for the life of the process, checks the file holds the same bytes, and hands the set to the registry
 * (resource_set_registry.h). After the plugin pass it mounts the sets against the game's live manifest, read with
 * the game's own FindEntry (plugin_assets.cpp), and publishes the shadow the resolver hook asks about.
 *
 * Containment: nothing here writes a file. The plugin's file is opened GENERIC_READ with FILE_SHARE_READ only, and
 * the game's packages and manifests are never touched.
 */

#include "runtime/patch/resource_sets.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <winsock2.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/resource_set.h"
#include "runtime/patch/image_file_range.h"
#include "runtime/patch/plugin_assets.h"
#include "runtime/patch/resource_set_registry.h"

namespace nevr_resource_sets {

namespace {

// What the resolver hook reads with no lock: the shadow and one serve counter per mounted set. Published once with
// a release store after the mount and never freed (a request in flight may still be filled from it).
struct Published {
  explicit Published(const nevr_shadow::Shadow* s, std::size_t sets) : shadow(s), served(sets) {}
  const nevr_shadow::Shadow* shadow;
  std::vector<std::atomic<std::uint64_t>> served;
  std::atomic<std::uint64_t> stockWins{0};
};

std::mutex g_mutex;  // the registry, the init state and the mount
Registry g_registry;
std::atomic<Published*> g_published{nullptr};
std::atomic<bool> g_mountDeferred{false};
bool g_deferSaid = false;
std::atomic<std::uint64_t> g_lastSummaryMs{0};
std::atomic<bool> g_stockWinSaid{false};

struct InitState {
  std::string id;
  HMODULE module = nullptr;
  DWORD thread = 0;
  bool active = false;
};
InitState g_init;

#ifdef NEVR_TEST_HOOKS
StockHasFn g_testStockHas = nullptr;
std::uint64_t g_testStockRecords = 0;
#endif

bool StockHasNow(std::uint64_t type, std::uint64_t name) {
#ifdef NEVR_TEST_HOOKS
  if (g_testStockHas != nullptr) return g_testStockHas(type, name);
#endif
  return nevr::assets::StockHas(type, name);
}

bool StockHasThunk(void*, std::uint64_t type, std::uint64_t name) { return StockHasNow(type, name); }

std::uint64_t StockRecordsNow() {
#ifdef NEVR_TEST_HOOKS
  if (g_testStockHas != nullptr) return g_testStockRecords;
#endif
  return nevr::assets::StockRecordCount();
}

void CloseHandles(const std::vector<std::int64_t>& handles) {
  for (std::int64_t h : handles) {
    if (h != -1 && h != 0) CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(h)));
  }
}

std::string LevelList(const Set& set) {
  std::string list;
  char hex[24];
  for (const Level& level : set.levels) {
    std::snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(level.name));
    if (!list.empty()) list += ',';
    list += hex;
  }
  return list.empty() ? std::string("none") : list;
}

// Reads `length` bytes at `offset` of `file` into `out`; false when the read comes up short.
bool ReadFileAt(HANDLE file, std::uint64_t offset, std::size_t length, std::vector<unsigned char>* out) {
  out->assign(length, 0);
  std::size_t done = 0;
  while (done < length) {
    OVERLAPPED at = {};
    at.Offset = static_cast<DWORD>((offset + done) & 0xFFFFFFFFu);
    at.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(length - done, 1u << 20));
    if (!ReadFile(file, out->data() + done, want, &got, &at) || got == 0) return false;
    done += got;
  }
  return true;
}

// The set's first and last MiB, read from the file at the offset the image's headers name, equal the mapped bytes.
bool FileHoldsMappedBytes(HANDLE file, std::uint64_t fileOffset, const unsigned char* mapped, std::uint64_t size) {
  const std::size_t span = static_cast<std::size_t>(std::min<std::uint64_t>(size, 1u << 20));
  std::vector<unsigned char> read;
  if (!ReadFileAt(file, fileOffset, span, &read) || std::memcmp(read.data(), mapped, span) != 0) return false;
  const std::uint64_t tail = size - span;
  if (!ReadFileAt(file, fileOffset + tail, span, &read) || std::memcmp(read.data(), mapped + tail, span) != 0) return false;
  return true;
}

int32_t Refused(const char* pluginId, const char* label, int32_t status, const char* reason, const std::string& detail,
                const char* consequence) {
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.RESOURCE] resource set refused plugin=%s label=%s status=%d reason=%s detail=\"%s\" (%s)",
      pluginId != nullptr ? pluginId : "", label != nullptr ? label : "", status, reason, detail.c_str(), consequence);
  return status;
}

int32_t StatusFor(Refusal refusal) {
  switch (refusal) {
    case Refusal::kCollides: return NEVR_RESOURCES_COLLIDES;
    case Refusal::kTooLate: return NEVR_RESOURCES_TOO_LATE;
    default: return NEVR_RESOURCES_BAD_FORMAT;
  }
}

}  // namespace

void BeginPluginInit(const char* pluginId, void* module) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_init.id = pluginId != nullptr ? pluginId : "";
  g_init.module = static_cast<HMODULE>(module);
  g_init.thread = GetCurrentThreadId();
  g_init.active = true;
}

void EndPluginInit(const char* pluginId, bool succeeded) {
  std::vector<std::int64_t> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_init = InitState{};
    const std::string id = pluginId != nullptr ? pluginId : "";
    if (succeeded) {
      g_registry.Commit(id);
    } else {
      dropped = g_registry.Drop(id);
    }
  }
  if (!dropped.empty()) {
    CloseHandles(dropped);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] resource sets dropped plugin=%s count=%zu (its init failed, so nothing it registered is served)",
        pluginId != nullptr ? pluginId : "", dropped.size());
  }
}

int32_t HostRegisterResources(const char* pluginId, const NevrResourcesDesc* desc) {
  const char* label = desc != nullptr && desc->struct_size >= sizeof(NevrResourcesDesc) && desc->label != nullptr
                          ? desc->label
                          : "";
  if (pluginId == nullptr || desc == nullptr || desc->struct_size < sizeof(NevrResourcesDesc) || desc->flags != 0 ||
      desc->data == nullptr || desc->size == 0) {
    return Refused(pluginId, label, NEVR_RESOURCES_BAD_ARGUMENT, "bad_argument",
                   "a null pointer, a short struct_size, nonzero flags or an empty set", "nothing was registered");
  }
  HMODULE module = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_init.active || g_init.thread != GetCurrentThreadId() || g_init.id != pluginId) {
      return Refused(pluginId, label, NEVR_RESOURCES_BAD_ARGUMENT, "not_in_init",
                     "register_resources is called from the plugin's own init, with ctx->plugin_id",
                     "nothing was registered");
    }
    module = g_init.module;
  }

  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(desc->data), &owner) ||
      owner != module) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "not_in_image",
                   "the bytes are not in the image of the plugin whose init is running", "nothing was registered");
  }
  const auto* image = reinterpret_cast<const unsigned char*>(module);
  const auto* bytes = static_cast<const unsigned char*>(desc->data);
  const std::uint64_t rva = static_cast<std::uint64_t>(bytes - image);
  const nevr_image_range::Result range = nevr_image_range::FileOffsetOf(image, rva, desc->size);
  if (!range.ok()) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, nevr_image_range::ProblemName(range.problem),
                   "the set must lie in one section's raw data with no base relocation in it (an .incbin into a "
                   "read-only section)",
                   "nothing was registered");
  }

  std::wstring path(32768, L'\0');
  const DWORD pathLength = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (pathLength == 0 || pathLength >= path.size()) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "no_module_path",
                   "GetModuleFileNameW error " + std::to_string(GetLastError()), "nothing was registered");
  }
  path.resize(pathLength);
  // Read-only, and deny-write: the file the game's reader reads cannot change under the running game.
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "file_open",
                   "CreateFileW error " + std::to_string(GetLastError()), "nothing was registered");
  }
  if (!FileHoldsMappedBytes(file, range.file_offset, bytes, desc->size)) {
    CloseHandle(file);
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "file_differs",
                   "the plugin's file does not hold the mapped bytes at the offset its headers name",
                   "nothing was registered");
  }

  nevr_resource_set::View view;
  nevr_resource_set::Parse(bytes, desc->size, &view);
  Outcome outcome;
  std::size_t levels = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    outcome = g_registry.Add(pluginId, label, bytes, desc->size,
                             static_cast<std::int64_t>(reinterpret_cast<intptr_t>(file)), range.file_offset);
  }
  if (!outcome.ok()) {
    CloseHandle(file);
    return Refused(pluginId, label, StatusFor(outcome.refusal), RefusalName(outcome.refusal), outcome.detail,
                   "nothing was registered");
  }
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    if (view.RecordAt(i).type == kLevelRootType) ++levels;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] resource set registered plugin=%s label=%s records=%u chunks=%u levels=%zu bytes=%llu "
      "file_offset=0x%llx (held until the plugin's init returns, then mounted after the plugin pass)",
      pluginId, label, view.record_count(), view.chunk_count(), levels, static_cast<unsigned long long>(desc->size),
      static_cast<unsigned long long>(range.file_offset));
  return NEVR_RESOURCES_OK;
}

void MountWhenReady() {
  MountReport report;
  std::vector<std::int64_t> abandoned;
  std::vector<const Set*> mounted;
  const ULONGLONG started = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_registry.mounted()) {
      g_mountDeferred.store(false);
      return;
    }
    if (g_registry.held() == 0) {
      // Nothing to mount: close registration for the run without a line (no plugin carries resources).
      report = g_registry.Mount(StockView{&StockHasThunk, nullptr, 0});
      g_mountDeferred.store(false);
      return;
    }
    const std::uint64_t stockRecords = StockRecordsNow();
    if (stockRecords == 0) {
      if (!g_deferSaid) {
        g_deferSaid = true;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] resource sets wait for the game's manifest sets=%zu (the mount retries each frame until "
            "the game has read its package manifests)",
            g_registry.held());
      }
      g_mountDeferred.store(true);
      return;
    }
    bool hooked = true;
#ifdef NEVR_TEST_HOOKS
    if (g_testStockHas == nullptr) hooked = nevr::assets::EnsureResolverHook();
#else
    hooked = nevr::assets::EnsureResolverHook();
#endif
    if (!hooked) {
      abandoned = g_registry.Abandon();
    } else {
      report = g_registry.Mount(StockView{&StockHasThunk, nullptr, stockRecords});
      mounted = g_registry.MountedSets();
      if (const nevr_shadow::Shadow* shadow = g_registry.shadow()) {
        g_published.store(new Published(shadow, mounted.size()), std::memory_order_release);
      }
    }
    g_mountDeferred.store(false);
  }
  if (!abandoned.empty()) {
    CloseHandles(abandoned);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource sets not mounted sets=%zu reason=no_resolver_hook (the resolver hook at 0x140fa1fe0 "
        "is not installed, so no plugin level or resource is served)",
        abandoned.size());
    return;
  }
  for (const MountReport::Refused& refused : report.refusals) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource set refused at mount plugin=%s label=%s reason=%s detail=\"%s\" (its levels are "
        "neither served nor listed as installed)",
        refused.plugin.c_str(), refused.label.c_str(), RefusalName(refused.outcome.refusal),
        refused.outcome.detail.c_str());
  }
  CloseHandles(report.handles_to_close);
  for (const Set* set : mounted) {
    Log(EchoVR::LogLevel::Info, "[NEVR.RESOURCE] resource set mounted plugin=%s label=%s records=%zu levels=%s",
        set->plugin.c_str(), set->label.c_str(), set->records.size(), LevelList(*set).c_str());
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] resource sets mounted sets=%zu refused=%zu records=%zu levels=%zu chunk_base=0x%08x "
      "stock_records=%llu duration_ms=%llu",
      report.sets, report.refused, report.records, report.levels, report.chunk_base,
      static_cast<unsigned long long>(report.stock_records),
      static_cast<unsigned long long>(GetTickCount64() - started));
}

void OnFrame() {
  if (g_mountDeferred.load(std::memory_order_relaxed)) MountWhenReady();
  Published* published = g_published.load(std::memory_order_acquire);
  if (published == nullptr) return;
  const std::uint64_t now = GetTickCount64();
  std::uint64_t last = g_lastSummaryMs.load(std::memory_order_relaxed);
  if (now - last < 10000 || !g_lastSummaryMs.compare_exchange_strong(last, now)) return;
  std::vector<const Set*> sets;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sets = g_registry.MountedSets();
  }
  for (std::size_t i = 0; i < sets.size() && i < published->served.size(); ++i) {
    const std::uint64_t served = published->served[i].exchange(0, std::memory_order_relaxed);
    if (served == 0) continue;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] resource set reads plugin=%s label=%s served=%llu window_s=10 (the game read these "
        "records from the plugin's file)",
        sets[i]->plugin.c_str(), sets[i]->label.c_str(), static_cast<unsigned long long>(served));
  }
  const std::uint64_t stockWins = published->stockWins.load(std::memory_order_relaxed);
  if (stockWins != 0 && !g_stockWinSaid.exchange(true)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource set keys answered by the game's manifest count=%llu (a package set added after the "
        "mount holds keys a plugin's set holds; the game's bytes are served for them)",
        static_cast<unsigned long long>(stockWins));
  }
}

const void* ManagerFor(std::uint64_t type, std::uint64_t name) {
  Published* published = g_published.load(std::memory_order_acquire);
  if (published == nullptr) return nullptr;
  std::uint32_t user = 0;
  if (!published->shadow->Find(type, name, nullptr, &user)) return nullptr;
  if (StockHasNow(type, name)) {
    published->stockWins.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  if (user < published->served.size()) published->served[user].fetch_add(1, std::memory_order_relaxed);
  return published->shadow->manager();
}

std::vector<std::uint64_t> MountedLevelNames() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.MountedLevels();
}

std::string ReportJson() {
  nlohmann::json sets = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Set* set : g_registry.MountedSets()) {
    nlohmann::json levels = nlohmann::json::array();
    char hex[24];
    for (const Level& level : set->levels) {
      std::snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(level.name));
      levels.push_back(hex);
    }
    sets.push_back({{"plugin", set->plugin},
                    {"label", set->label},
                    {"levels", levels},
                    {"records", set->records.size()},
                    {"bytes", set->bytes}});
  }
  return sets.dump();
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_registry = Registry();
  g_published.store(nullptr);
  g_mountDeferred.store(false);
  g_deferSaid = false;
  g_lastSummaryMs.store(0);
  g_stockWinSaid.store(false);
  g_init = InitState{};
  g_testStockHas = nullptr;
  g_testStockRecords = 0;
}

void SetStockForTest(StockHasFn has, std::uint64_t records) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_testStockHas = has;
  g_testStockRecords = records;
}
#endif

}  // namespace nevr_resource_sets
