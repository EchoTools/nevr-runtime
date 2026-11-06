/*
 * resource_sets.cpp - plugin resource sets on PC (plugin API v9, #2161; docs/reference/plugin-resources.md).
 *
 * A plugin hands register_resources the bytes of a resource set as its own image maps them. This file checks that
 * those bytes are file bytes of a read-only section of that plugin's image (image_file_range.h), opens the plugin's
 * file read-only and deny-write for the life of the process, checks the file holds the same bytes, and hands the set
 * to the registry (resource_set_registry.h). After the plugin pass it installs the resolver hook; on the game's own
 * thread, once the game has asked the resolver for its first resource (its boot package sets are all read by then), it
 * mounts the sets against the game's manifest, read with the game's own FindEntry (plugin_assets.cpp), publishes the
 * shadow the resolver hook asks about, and registers the mounted sets' overrides.
 *
 * Containment: nothing here writes a file. The plugin's file is opened GENERIC_READ with FILE_SHARE_READ only, and
 * the game's packages and manifests are never touched.
 */

#include "runtime/patch/resource_sets.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
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
#include "runtime/patch/resource_override.h"
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
  // The first key the game's manifest answered instead of a set, and that set's index (+1; 0 until one).
  std::atomic<std::uint64_t> firstWinType{0};
  std::atomic<std::uint64_t> firstWinName{0};
  std::atomic<std::uint32_t> firstWinUser{0};
};

// When a mount is still waiting this long after the plugin pass, one Warning says why.
constexpr std::uint64_t kLateMs = 60000;
// A label: what a plugin calls its set, written into log lines and the login's JSON.
constexpr std::size_t kMaxLabel = 64;

std::mutex g_mutex;  // the registry, the init state and the mount
Registry g_registry;
std::atomic<Published*> g_published{nullptr};
std::atomic<bool> g_mountDeferred{false};
bool g_deferSaid = false;
bool g_lateSaid = false;
std::uint64_t g_armedAtMs = 0;
// Set by the resolver hook on the game's first request. The game adds its boot package sets in one function
// (echovr.exe 0x141548da0 calls CResourceManager::AddPackage at 0x14154973c and 0x1415497a7) and only then initialises
// the streaming reader that serves requests (CStreamingIOProcessor::Initialize, call at 0x141549902), so by the first
// request the game's manifest holds both sets. A tick can run between the two AddPackage calls.
std::atomic<bool> g_requestSeen{false};
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
std::uint64_t (*g_testNow)() = nullptr;
bool g_testThrow = false;
std::wstring g_testFilePath;  // non-empty: the file registration opens instead of the plugin's own
#endif

std::uint64_t NowMs() {
#ifdef NEVR_TEST_HOOKS
  if (g_testNow != nullptr) return g_testNow();
#endif
  return GetTickCount64();
}

// The game's live manifest (the mount's view of it).
bool StockHasNow(std::uint64_t type, std::uint64_t name) {
#ifdef NEVR_TEST_HOOKS
  if (g_testStockHas != nullptr) return g_testStockHas(type, name);
#endif
  return nevr::assets::StockHas(type, name);
}

// The manifest of the manager a request was made to (the resolver's view of it).
bool ManagerHasNow(const void* manager, std::uint64_t type, std::uint64_t name) {
#ifdef NEVR_TEST_HOOKS
  if (g_testStockHas != nullptr) return g_testStockHas(type, name);
#endif
  return nevr::assets::ManagerHas(manager, type, name);
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

// The plugin's file, open read-only; closed unless released to the registry.
class FileHandle {
 public:
  explicit FileHandle(HANDLE h) : h_(h) {}
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  ~FileHandle() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  HANDLE get() const { return h_; }
  HANDLE release() {
    const HANDLE h = h_;
    h_ = INVALID_HANDLE_VALUE;
    return h;
  }

 private:
  HANDLE h_;
};

// A label is 1 to kMaxLabel bytes of printable ASCII, with no double quote or backslash: it is written into log lines
// (quoted) and into the login's JSON, so it can neither start a line nor break either.
bool LabelOk(const char* label) {
  if (label == nullptr) return false;
  const std::size_t n = std::strlen(label);
  if (n == 0 || n > kMaxLabel) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(label[i]);
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}

// Text from outside (a label not yet checked, another registry's label) made safe for a quoted log field.
std::string Printable(const char* text) {
  std::string out;
  if (text == nullptr) return out;
  for (std::size_t i = 0; text[i] != '\0' && out.size() < kMaxLabel; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    out += (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? '?' : static_cast<char>(c);
  }
  return out;
}

std::string LevelList(const Set& set) {
  std::string list;
  char hex[24];
  for (const std::uint64_t level : set.levels) {
    std::snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(level));
    if (!list.empty()) list += ',';
    list += hex;
  }
  return list.empty() ? std::string("none") : list;
}

// Reads `length` bytes at `offset` of `file` into `out`. False when a read fails or comes up short; `error` is the
// read's GetLastError, and ERROR_HANDLE_EOF when the file ends first: a read that names its offset fails with that
// code at the end of the file, and a read that returns no bytes means the same.
bool ReadFileAt(HANDLE file, std::uint64_t offset, std::size_t length, std::vector<unsigned char>* out, DWORD* error) {
  out->assign(length, 0);
  std::size_t done = 0;
  while (done < length) {
    OVERLAPPED at = {};
    at.Offset = static_cast<DWORD>((offset + done) & 0xFFFFFFFFu);
    at.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(length - done, 1u << 20));
    if (!ReadFile(file, out->data() + done, want, &got, &at)) {
      *error = GetLastError();
      return false;
    }
    if (got == 0) {
      *error = ERROR_HANDLE_EOF;
      return false;
    }
    done += got;
  }
  return true;
}

enum class FileCheck { kSame, kReadFailed, kDiffers };

// The set's first and last MiB, read from the file at the offset the image's headers name, equal the mapped bytes.
FileCheck FileHoldsMappedBytes(HANDLE file, std::uint64_t fileOffset, const unsigned char* mapped, std::uint64_t size,
                               DWORD* error) {
  const std::size_t span = static_cast<std::size_t>(std::min<std::uint64_t>(size, 1u << 20));
  std::vector<unsigned char> read;
  if (!ReadFileAt(file, fileOffset, span, &read, error)) return FileCheck::kReadFailed;
  if (std::memcmp(read.data(), mapped, span) != 0) return FileCheck::kDiffers;
  const std::uint64_t tail = size - span;
  if (!ReadFileAt(file, fileOffset + tail, span, &read, error)) return FileCheck::kReadFailed;
  if (std::memcmp(read.data(), mapped + tail, span) != 0) return FileCheck::kDiffers;
  return FileCheck::kSame;
}

int32_t Refused(const char* pluginId, const std::string& label, int32_t status, const char* reason,
                const std::string& detail) {
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.RESOURCE] resource set refused plugin=%s label=\"%s\" status=%d reason=%s detail=\"%s\" (nothing was "
      "registered, so none of its levels or resources is served)",
      Printable(pluginId).c_str(), label.c_str(), status, reason, detail.c_str());
  return status;
}

int32_t StatusFor(Refusal refusal) {
  switch (refusal) {
    case Refusal::kCollides: return NEVR_RESOURCES_COLLIDES;
    case Refusal::kTooLate: return NEVR_RESOURCES_TOO_LATE;
    case Refusal::kFileOffset: return NEVR_RESOURCES_NOT_IN_FILE;
    default: return NEVR_RESOURCES_BAD_FORMAT;
  }
}

int32_t RegisterOne(const char* pluginId, const NevrResourcesDesc* desc) {
#ifdef NEVR_TEST_HOOKS
  if (g_testThrow) throw std::bad_alloc();
#endif
  const bool sized = desc != nullptr && desc->struct_size >= sizeof(NevrResourcesDesc);
  const std::string shown = sized ? Printable(desc->label) : std::string();
  if (pluginId == nullptr || !sized || desc->flags != 0 || desc->data == nullptr || desc->size == 0) {
    return Refused(pluginId, shown, NEVR_RESOURCES_BAD_ARGUMENT, "bad_argument",
                   "a null pointer, a short struct_size, nonzero flags or an empty set");
  }
  if (!LabelOk(desc->label)) {
    return Refused(pluginId, shown, NEVR_RESOURCES_BAD_ARGUMENT, "bad_label",
                   "a label is 1 to 64 bytes of printable ASCII with no double quote or backslash");
  }
  const std::string label = desc->label;
  HMODULE module = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_init.active || g_init.thread != GetCurrentThreadId() || g_init.id != pluginId) {
      return Refused(pluginId, label, NEVR_RESOURCES_BAD_ARGUMENT, "not_in_init",
                     "register_resources is called from the plugin's own init, with ctx->plugin_id");
    }
    module = g_init.module;
  }

  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(desc->data), &owner) ||
      owner != module) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "not_in_image",
                   "the bytes are not in the image of the plugin whose init is running");
  }
  const auto* image = reinterpret_cast<const unsigned char*>(module);
  const auto* bytes = static_cast<const unsigned char*>(desc->data);
  const std::uint64_t rva = static_cast<std::uint64_t>(bytes - image);
  const nevr_image_range::Result range = nevr_image_range::FileOffsetOf(image, rva, desc->size);
  if (!range.ok()) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, nevr_image_range::ProblemName(range.problem),
                   "the set must lie in one read-only section's raw data with no base relocation in it (#embed into "
                   "NEVR_RESOURCE_SET_SECTION)");
  }

  nevr_resource_set::View view;
  const nevr_resource_set::Result parsed = nevr_resource_set::Parse(bytes, desc->size, &view);
  if (!parsed.ok()) {
    return Refused(pluginId, label, NEVR_RESOURCES_BAD_FORMAT, RefusalName(Refusal::kFormat),
                   std::string(nevr_resource_set::ProblemName(parsed.problem)) + " at index " +
                       std::to_string(parsed.index));
  }
  // A key a plugin asset (NEVR_RegisterAsset) holds: the asset hook would serve one and the resolver the other.
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    std::string assetLabel;
    if (nevr::assets::AssetHeld(r.type, r.name, &assetLabel)) {
      return Refused(pluginId, label, NEVR_RESOURCES_COLLIDES, RefusalName(Refusal::kCollides),
                     KeyText(r.type, r.name) + " owner=asset:" + Printable(assetLabel.c_str()));
    }
  }

  std::wstring path(32768, L'\0');
  const DWORD pathLength = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (pathLength == 0 || pathLength >= path.size()) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "no_module_path",
                   "GetModuleFileNameW error " + std::to_string(GetLastError()));
  }
  path.resize(pathLength);
#ifdef NEVR_TEST_HOOKS
  if (!g_testFilePath.empty()) path = g_testFilePath;
#endif
  // Read-only, and deny-write: the file the game's reader reads cannot change under the running game.
  FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "file_open",
                   "CreateFileW error " + std::to_string(GetLastError()));
  }
  DWORD readError = 0;
  switch (FileHoldsMappedBytes(file.get(), range.file_offset, bytes, desc->size, &readError)) {
    case FileCheck::kSame:
      break;
    case FileCheck::kReadFailed:
      return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "file_read",
                     readError == ERROR_HANDLE_EOF
                         ? std::string("the plugin's file ends before the offset its headers name for the set's bytes")
                         : "ReadFile error " + std::to_string(readError));
    case FileCheck::kDiffers:
      return Refused(pluginId, label, NEVR_RESOURCES_NOT_IN_FILE, "file_differs",
                     "the plugin's file does not hold the mapped bytes at the offset its headers name");
  }

  Outcome outcome;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    outcome = g_registry.Add(pluginId, label, bytes, desc->size,
                             static_cast<std::int64_t>(reinterpret_cast<intptr_t>(file.get())), range.file_offset);
    if (outcome.ok()) file.release();  // the registry holds it now
  }
  if (!outcome.ok()) {
    return Refused(pluginId, label, StatusFor(outcome.refusal), RefusalName(outcome.refusal), outcome.detail);
  }
  std::size_t levels = 0;
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    if (view.RecordAt(i).type == kLevelRootType) ++levels;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] resource set registered plugin=%s label=\"%s\" records=%u chunks=%u levels=%zu overrides=%u "
      "bytes=%llu file_offset=%llu (held until the plugin's init returns, then mounted after the plugin pass)",
      pluginId, label.c_str(), view.record_count(), view.chunk_count(), levels, view.override_count(),
      static_cast<unsigned long long>(desc->size), static_cast<unsigned long long>(range.file_offset));
  return NEVR_RESOURCES_OK;
}

// The mount itself, under g_mutex, once TryMount found the manifest ready. Fills `report`, `mounted` and
// `abandoned` for the lines written after the lock is released.
void MountLocked(std::uint64_t stockRecords, MountReport* report, std::vector<const Set*>* mounted) {
  *report = g_registry.Mount(StockView{&StockHasThunk, nullptr, stockRecords});
  *mounted = g_registry.MountedSets();
  if (const nevr_shadow::Shadow* shadow = g_registry.shadow()) {
    // The first read window starts here, before the publish: a read the resolver serves on another thread between
    // the publish and this frame's summary is then counted in a window that has a start.
    g_lastSummaryMs.store(NowMs());
    g_published.store(new Published(shadow, mounted->size()), std::memory_order_release);
  }
  g_mountDeferred.store(false);
}

void WriteMountLines(std::uint64_t startedMs, const MountReport& report, const std::vector<const Set*>& mounted,
                     const std::vector<std::int64_t>& abandoned, const std::vector<std::string>& abandonedPlugins) {
  if (!abandoned.empty()) {
    CloseHandles(abandoned);
    std::string plugins;
    for (const std::string& p : abandonedPlugins) plugins += (plugins.empty() ? "" : ",") + p;
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource sets not mounted sets=%zu plugins=%s reason=no_resolver_hook (the resolver hook at "
        "0x140fa1fe0 is not installed, so no plugin level or resource is served)",
        abandoned.size(), plugins.c_str());
    return;
  }
  for (const MountReport::Refused& refused : report.refusals) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource set refused at mount plugin=%s label=\"%s\" reason=%s detail=\"%s\" (its levels are "
        "neither served nor listed as installed)",
        refused.plugin.c_str(), refused.label.c_str(), RefusalName(refused.outcome.refusal),
        refused.outcome.detail.c_str());
  }
  CloseHandles(report.handles_to_close);
  for (const Set* set : mounted) {
    // A set's overrides replace resources the game has; one whose key the game does not hold would replace nothing.
    std::uint32_t registered = 0;
    std::uint32_t unmatched = 0;
    for (const OverrideRef& o : set->overrides) {
      if (!StockHasNow(o.type, o.name)) {
        ++unmatched;
        continue;
      }
      RegisterResourceOverride(o.type, o.name, o.data, o.size, set->label.c_str());
      ++registered;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] resource set mounted plugin=%s label=\"%s\" records=%zu levels=%zu level_names=%s "
        "overrides=%u overrides_unmatched=%u",
        set->plugin.c_str(), set->label.c_str(), set->records.size(), set->levels.size(), LevelList(*set).c_str(),
        registered, unmatched);
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] resource sets mounted sets=%zu refused=%zu records=%zu levels=%zu chunk_base=0x%08x "
      "stock_records=%llu duration_ms=%llu",
      report.sets, report.refused, report.records, report.levels, report.chunk_base,
      static_cast<unsigned long long>(report.stock_records),
      static_cast<unsigned long long>(NowMs() - startedMs));
}

// A deferred mount's attempt: on the game's own thread, once the game has asked for its first resource and its
// manifest holds records.
void TryMount(bool onLoopThread) {
  MountReport report;
  std::vector<const Set*> mounted;
  const std::uint64_t now = NowMs();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_mountDeferred.load() || g_registry.mounted()) return;
    const std::uint64_t count = StockRecordsNow();
    if (!g_lateSaid && now - g_armedAtMs >= kLateMs) {
      g_lateSaid = true;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.RESOURCE] resource sets still not mounted after %llu s sets=%zu stock_records=%llu "
          "first_request_seen=%s on_game_thread=%s (no plugin level or resource is served until the mount; it waits "
          "for the game's first resource request, on the game's own thread)",
          static_cast<unsigned long long>((now - g_armedAtMs) / 1000), g_registry.held(),
          static_cast<unsigned long long>(count), g_requestSeen.load() ? "true" : "false",
          onLoopThread ? "true" : "false");
    }
    if (!onLoopThread || count == 0 || !g_requestSeen.load()) return;
    MountLocked(count, &report, &mounted);
  }
  WriteMountLines(now, report, mounted, {}, {});
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
        "[NEVR.RESOURCE] resource sets dropped plugin=%s count=%zu (its init failed, so nothing it registered is "
        "served, and none of its overrides is registered)",
        Printable(pluginId).c_str(), dropped.size());
  }
}

// The plugin ABI entry: nothing it does may throw back into the plugin's init.
int32_t HostRegisterResources(const char* pluginId, const NevrResourcesDesc* desc) {
  try {
    return RegisterOne(pluginId, desc);
  } catch (const std::bad_alloc&) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.RESOURCE] resource set refused plugin=%s reason=out_of_memory (registration ran out of memory; nothing "
        "was registered)",
        Printable(pluginId).c_str());
  } catch (const std::exception&) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.RESOURCE] resource set refused plugin=%s reason=exception (registration threw; nothing was registered)",
        Printable(pluginId).c_str());
  }
  return NEVR_RESOURCES_BAD_FORMAT;
}

void MountWhenReady() {
  std::vector<std::int64_t> abandoned;
  std::vector<std::string> abandonedPlugins;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_registry.mounted()) return;
    if (g_registry.held() == 0) {
      // Nothing to mount: close registration for the run without a line (no plugin carries resources).
      g_registry.Mount(StockView{&StockHasThunk, nullptr, 0});
      return;
    }
    // The resolver hook goes in now, so it sees the game's first request (which the mount waits for).
    bool hooked = true;
#ifdef NEVR_TEST_HOOKS
    if (g_testStockHas == nullptr) hooked = nevr::assets::EnsureResolverHook();
#else
    hooked = nevr::assets::EnsureResolverHook();
#endif
    if (!hooked) {
      for (const Set* set : g_registry.HeldSets()) abandonedPlugins.push_back(set->plugin);
      abandoned = g_registry.Abandon();
    } else {
      g_armedAtMs = NowMs();
      if (!g_deferSaid) {
        g_deferSaid = true;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] resource sets wait for the game's manifest sets=%zu (they are mounted on the game's own "
            "thread once the game has asked for its first resource: its boot package sets are all read by then)",
            g_registry.held());
      }
      g_mountDeferred.store(true);
    }
  }
  if (!abandoned.empty()) WriteMountLines(NowMs(), MountReport{}, {}, abandoned, abandonedPlugins);
}

void NoteRequest() {
  if (!g_requestSeen.load(std::memory_order_relaxed)) g_requestSeen.store(true, std::memory_order_relaxed);
}

void OnFrame(bool onLoopThread) {
  if (g_mountDeferred.load(std::memory_order_relaxed)) TryMount(onLoopThread);
  Published* published = g_published.load(std::memory_order_acquire);
  if (published == nullptr) return;
  const std::uint64_t now = NowMs();
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
        "[NEVR.RESOURCE] resource set reads plugin=%s label=\"%s\" served=%llu window_ms=%llu (the game read these "
        "records from the plugin's file)",
        sets[i]->plugin.c_str(), sets[i]->label.c_str(), static_cast<unsigned long long>(served),
        static_cast<unsigned long long>(now - last));
  }
  const std::uint64_t stockWins = published->stockWins.load(std::memory_order_relaxed);
  if (stockWins != 0 && !g_stockWinSaid.exchange(true)) {
    const std::uint32_t user = published->firstWinUser.load();
    const std::string plugin = user != 0 && user - 1 < sets.size() ? sets[user - 1]->plugin : std::string("unknown");
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource set keys answered by the game's manifest count=%llu first=%s plugin=%s (a package set "
        "added after the mount holds keys a plugin's set holds; the game's bytes are served for them)",
        static_cast<unsigned long long>(stockWins),
        KeyText(published->firstWinType.load(), published->firstWinName.load()).c_str(), plugin.c_str());
  }
}

const void* ManagerFor(const void* manager, std::uint64_t type, std::uint64_t name) {
  Published* published = g_published.load(std::memory_order_acquire);
  if (published == nullptr) return nullptr;
  std::uint32_t user = 0;
  if (!published->shadow->Find(type, name, nullptr, &user)) return nullptr;
  if (ManagerHasNow(manager, type, name)) {
    if (published->stockWins.fetch_add(1, std::memory_order_relaxed) == 0) {
      published->firstWinType.store(type);
      published->firstWinName.store(name);
      published->firstWinUser.store(user + 1);
    }
    return nullptr;
  }
  if (user < published->served.size()) published->served[user].fetch_add(1, std::memory_order_relaxed);
  return published->shadow->manager();
}

bool HoldsKey(std::uint64_t type, std::uint64_t name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.Holds(type, name);
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
    for (const std::uint64_t level : set->levels) {
      std::snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(level));
      levels.push_back(hex);
    }
    sets.push_back({{"plugin", set->plugin},
                    {"label", set->label},
                    {"levels", levels},
                    {"records", set->records.size()},
                    {"bytes", set->bytes}});
  }
  // `replace`: a plugin's id and label come from outside; the login must not fail over them.
  return sets.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_registry = Registry();
  g_published.store(nullptr);
  g_mountDeferred.store(false);
  g_deferSaid = false;
  g_lateSaid = false;
  g_armedAtMs = 0;
  g_requestSeen.store(false);
  g_lastSummaryMs.store(0);
  g_stockWinSaid.store(false);
  g_init = InitState{};
  g_testStockHas = nullptr;
  g_testStockRecords = 0;
  g_testNow = nullptr;
  g_testThrow = false;
  g_testFilePath.clear();
}

void SetStockForTest(StockHasFn has, std::uint64_t records) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_testStockHas = has;
  g_testStockRecords = records;
}

void SetClockForTest(std::uint64_t (*now)()) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_testNow = now;
}

void SetThrowForTest(bool on) { g_testThrow = on; }

void SetFilePathForTest(const wchar_t* path) { g_testFilePath = path != nullptr ? path : L""; }
#endif

}  // namespace nevr_resource_sets
