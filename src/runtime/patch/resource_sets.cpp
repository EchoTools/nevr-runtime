/*
 * resource_sets.cpp - plugin resource sets on PC (plugin API v9, #2161; docs/reference/plugin-resources.md).
 *
 * A plugin hands register_resources the bytes of a resource set as its own image maps them. This file checks that
 * those bytes are file bytes of a read-only section of that plugin's image (image_file_range.h), opens the plugin's
 * file read-only and deny-write for the life of the process, checks the file holds the set's first and last MiB as
 * mapped (the bytes between are not compared), and hands the set
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
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/resource_set.h"
#include "runtime/patch/image_file_range.h"
#include "runtime/patch/plugin_assets.h"
#include "runtime/patch/resource_buffer_key.h"
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
  std::atomic<bool> firstWinReady{false};  // stored after the three above, so a reader that sees it sees them
};

// When a mount is still waiting this long after the plugin pass, one Warning says why.
constexpr std::uint64_t kLateMs = 60000;
// A label: what a plugin calls its set, written into log lines and the login's JSON.
constexpr std::size_t kMaxLabel = 64;
// A map's label is "map:<id>@<version>" (CDN_DESIGN F3), and an id alone may be 55 bytes.
constexpr std::size_t kMaxMapLabel = 128;

std::mutex g_mutex;  // the registry, the init state and the mount
// A set's check of the plugin assets and its Add; a plugin asset's check of the sets and its Register (LockKeyClaims).
// Taken before g_mutex and before the asset registry's own lock, never after either.
std::mutex g_keyMutex;
Registry g_registry;
// The mapped views of the map sets the registry holds, by their file's handle; unmapped when the handle is closed.
// Taken after g_mutex, never before it.
std::mutex g_viewMutex;
std::vector<std::pair<std::int64_t, const void*>> g_fileViews;
std::atomic<Published*> g_published{nullptr};
std::atomic<bool> g_mountDeferred{false};
bool g_deferSaid = false;
// Read without g_mutex by TryMount's check before it takes the lock.
std::atomic<bool> g_lateSaid{false};
std::atomic<std::uint64_t> g_armedAtMs{0};
// True once TryMount has run on the game-loop thread: the late warning says whether the game's thread reaches it.
std::atomic<bool> g_gameThreadSeen{false};
// Set by the resolver hook on the game's first request. The game adds its boot package sets in one function
// (echovr.exe 0x141548da0 calls CResourceManager::AddPackage at 0x14154973c and 0x1415497a7) and only then initialises
// the streaming reader that serves requests (CStreamingIOProcessor::Initialize, call at 0x141549902), so by the first
// request the game's manifest holds both sets. A tick can run between the two AddPackage calls.
std::atomic<bool> g_requestSeen{false};
std::atomic<std::uint64_t> g_lastSummaryMs{0};
std::atomic<bool> g_stockWinSaid{false};
std::atomic<bool> g_mountFailedSaid{false};
std::atomic<bool> g_summaryFailedSaid{false};
std::atomic<void (*)(const MountReport&)> g_mountObserver{nullptr};
std::atomic<bool> g_mountObserved{false};

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
MountThrowAt g_testMountThrow = MountThrowAt::kNowhere;
void (*g_testBeforeAdd)() = nullptr;
void (*g_testAfterPublish)() = nullptr;
bool g_testNoResolverHook = false;
bool g_testThrowAfterOpen = false;
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

void UnmapFileView(std::int64_t handle) {
  const void* view = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    const auto it = std::find_if(g_fileViews.begin(), g_fileViews.end(),
                                 [handle](const std::pair<std::int64_t, const void*>& v) { return v.first == handle; });
    if (it == g_fileViews.end()) return;
    view = it->second;
    g_fileViews.erase(it);
  }
  UnmapViewOfFile(view);
}

void CloseHandles(const std::vector<std::int64_t>& handles) {
  for (std::int64_t h : handles) {
    if (h == -1 || h == 0) continue;
    UnmapFileView(h);
    CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(h)));
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
bool LabelOk(const char* label, std::size_t maxLength = kMaxLabel) {
  if (label == nullptr) return false;
  const std::size_t n = std::strlen(label);
  if (n == 0 || n > maxLength) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(label[i]);
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}

// Text from outside (a plugin id, a label not yet checked, another registry's label) made safe for a quoted log
// field: printable ASCII but '"' and '\\', every other byte as '?', at most kMaxLabel bytes. It is written into `out`,
// so a line on an out-of-memory path, where nothing may allocate, shows the text as every other line does.
const char* PrintableInto(const char* text, char (&out)[kMaxLabel + 1]) {
  std::size_t n = 0;
  for (; text != nullptr && text[n] != '\0' && n < kMaxLabel; ++n) {
    const unsigned char c = static_cast<unsigned char>(text[n]);
    out[n] = (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? '?' : static_cast<char>(c);
  }
  out[n] = '\0';
  return out;
}

}  // namespace

std::string Printable(const char* text) {
  char out[kMaxLabel + 1];
  return PrintableInto(text, out);
}

namespace {

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
    if (got == 0) {  // a positioned read of a file fails at its end instead; without this a read of 0 would loop
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

// The first record of `view` whose key a plugin asset holds, as kCollides names it; empty when none. Under g_keyMutex.
std::string AssetClaimOn(const nevr_resource_set::View& view) {
  const std::vector<nevr::assets::AssetManifestKey> assetKeys = nevr::assets::AssetManifestKeys();
  for (std::uint32_t i = 0; i < view.record_count() && !assetKeys.empty(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    for (const nevr::assets::AssetManifestKey& key : assetKeys) {
      if (key.type == r.type && key.name == r.name) {
        return KeyText(r.type, r.name) + " owner=asset:" + Printable(key.label.c_str());
      }
    }
  }
  return {};
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
#ifdef NEVR_TEST_HOOKS
  if (g_testThrowAfterOpen) throw std::bad_alloc();
#endif
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
    // The check of the plugin assets and the Add hold one lock, which a plugin asset's registration holds over its
    // check of the sets and its Register: a set and an asset never both take one resource (the asset hook would serve
    // one and the resolver the other).
    const std::lock_guard<std::mutex> claim(g_keyMutex);
    const std::string claimed = AssetClaimOn(view);
    if (!claimed.empty()) {
      return Refused(pluginId, label, NEVR_RESOURCES_COLLIDES, RefusalName(Refusal::kCollides), claimed);
    }
#ifdef NEVR_TEST_HOOKS
    if (g_testBeforeAdd != nullptr) g_testBeforeAdd();
#endif
    const std::lock_guard<std::mutex> lock(g_mutex);
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
      Printable(pluginId).c_str(), label.c_str(), view.record_count(), view.chunk_count(), levels,
      view.override_count(), static_cast<unsigned long long>(desc->size),
      static_cast<unsigned long long>(range.file_offset));
  return NEVR_RESOURCES_OK;
}

bool IsSha256Hex(const std::string& text) {
  return text.size() == 64 &&
         std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The SHA-256 of the first `size` bytes of `file`, read through it 1 MiB at a time, as 64 lowercase hex. Empty when a
// read or the hash fails; `error` is then the failing call's GetLastError (ERROR_HANDLE_EOF: the file ended first).
std::string FileSha256(HANDLE file, std::uint64_t size, DWORD* error) {
  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
    *error = GetLastError();
    return {};
  }
  HCRYPTHASH hash = 0;
  std::string hex;
  if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    *error = GetLastError();
  } else {
    std::vector<unsigned char> buffer(std::size_t{1} << 20);
    bool ok = true;
    for (std::uint64_t done = 0; ok && done < size;) {
      OVERLAPPED at = {};
      at.Offset = static_cast<DWORD>(done & 0xFFFFFFFFu);
      at.OffsetHigh = static_cast<DWORD>(done >> 32);
      const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(size - done, buffer.size()));
      DWORD got = 0;
      if (!ReadFile(file, buffer.data(), want, &got, &at)) {
        *error = GetLastError();
        ok = false;
      } else if (got == 0) {
        *error = ERROR_HANDLE_EOF;
        ok = false;
      } else if (!CryptHashData(hash, buffer.data(), got, 0)) {
        *error = GetLastError();
        ok = false;
      } else {
        done += got;
      }
    }
    BYTE digest[32] = {};
    DWORD length = sizeof(digest);
    if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest)) {
      constexpr char kDigits[] = "0123456789abcdef";
      for (const BYTE b : digest) {
        hex.push_back(kDigits[b >> 4]);
        hex.push_back(kDigits[b & 0x0f]);
      }
    } else if (ok) {
      *error = GetLastError();
    }
    CryptDestroyHash(hash);
  }
  CryptReleaseContext(provider, 0);
  return hex;
}

// A read-only view of a whole file; unmapped unless released to the registry.
class FileView {
 public:
  FileView() = default;
  FileView(const FileView&) = delete;
  FileView& operator=(const FileView&) = delete;
  ~FileView() {
    if (data_ != nullptr) UnmapViewOfFile(data_);
  }
  // False with `error` set when the file cannot be mapped.
  bool Map(HANDLE file, DWORD* error) {
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
      *error = GetLastError();
      return false;
    }
    data_ = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (data_ == nullptr) *error = GetLastError();
    CloseHandle(mapping);  // the view keeps the section alive
    return data_ != nullptr;
  }
  const void* get() const { return data_; }
  void release() { data_ = nullptr; }

 private:
  void* data_ = nullptr;
};

Outcome MapRefused(const std::string& owner, const std::string& label, Refusal refusal, std::string detail) {
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.MAPS] map resource set refused owner=%s label=\"%s\" reason=%s detail=\"%s\" (nothing was registered, "
      "so none of the map's levels or resources is served; the caller keeps the file)",
      Printable(owner.c_str()).c_str(), Printable(label.c_str()).c_str(), RefusalName(refusal), detail.c_str());
  return Outcome{refusal, std::move(detail)};
}

bool MountedNow() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.mounted();
}

// The mount observer, once a run, with no lock held.
void NotifyMountObserver(const MountReport& report) {
  const auto observer = g_mountObserver.load();
  if (observer == nullptr || g_mountObserved.exchange(true)) return;
  try {
    observer(report);
  } catch (...) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map mount observer threw (the mount goes on: every set it mounted is served)");
  }
}

// The mount itself, under g_mutex, once TryMount found the manifest ready. Fills `report` and `mounted` for the lines
// written after the lock is released.
void MountLocked(std::uint64_t stockRecords, MountReport* report, std::vector<const Set*>* mounted) {
#ifdef NEVR_TEST_HOOKS
  if (g_testMountThrow == MountThrowAt::kBeforeMount) throw std::bad_alloc();
#endif
  *report = g_registry.Mount(StockView{&StockHasThunk, nullptr, stockRecords});
  *mounted = g_registry.MountedSets();
}

// After MountLocked, under g_mutex: the shadow goes out to the resolver hook.
void PublishLocked(const std::vector<const Set*>& mounted) {
  if (const nevr_shadow::Shadow* shadow = g_registry.shadow()) {
    // The first read window starts here, before the publish: a read the resolver serves on another thread between
    // the publish and this frame's summary is then counted in a window that has a start.
    g_lastSummaryMs.store(NowMs());
    g_published.store(new Published(shadow, mounted.size()), std::memory_order_release);
#ifdef NEVR_TEST_HOOKS
    if (g_testAfterPublish != nullptr) g_testAfterPublish();
#endif
  }
  g_mountDeferred.store(false);
}

// `waitedMs`: from the plugin pass to the mount.
void WriteMountLines(std::uint64_t startedMs, const MountReport& report, const std::vector<const Set*>& mounted,
                     const std::vector<std::int64_t>& abandoned, const std::vector<std::string>& abandonedPlugins,
                     std::uint64_t waitedMs) {
#ifdef NEVR_TEST_HOOKS
  if (g_testMountThrow == MountThrowAt::kMountLines) throw std::bad_alloc();
#endif
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
        Printable(refused.plugin.c_str()).c_str(), refused.label.c_str(), RefusalName(refused.outcome.refusal),
        refused.outcome.detail.c_str());
  }
  CloseHandles(report.handles_to_close);
  for (const Set* set : mounted) {
    // A set's overrides replace resources the game has; one whose key the game does not hold would replace nothing.
    std::uint32_t registered = 0;
    std::uint32_t unmatched = 0;
    std::uint32_t skipped = 0;
    for (const OverrideRef& o : set->overrides) {
      if (!StockHasNow(o.type, o.name)) {
        ++unmatched;
        continue;
      }
      // An earlier fixed entry for the key (the CDN cache, a plugin's own NEVR_RegisterResourceOverride) serves its
      // loads, so the set's would never be the one chosen: it is not registered, and the line says whose wins.
      const std::string owner = FirstFixedOverrideLabelFor(o.type, o.name);
      if (!owner.empty()) {
        ++skipped;
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] resource set override skipped plugin=%s label=\"%s\" key=%s owner=\"%s\" (an earlier "
            "override of this resource serves it, so this set's bytes are not used)",
            Printable(set->plugin.c_str()).c_str(), set->label.c_str(), KeyText(o.type, o.name).c_str(),
            Printable(owner.c_str()).c_str());
        continue;
      }
      RegisterResourceOverride(o.type, o.name, o.data, o.size, set->label.c_str());
      ++registered;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] resource set mounted plugin=%s label=\"%s\" records=%zu levels=%zu level_names=%s "
        "overrides=%u overrides_unmatched=%u overrides_skipped=%u",
        Printable(set->plugin.c_str()).c_str(), set->label.c_str(), set->records.size(), set->levels.size(),
        LevelList(*set).c_str(),
        registered, unmatched, skipped);
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] resource sets mounted sets=%zu refused=%zu records=%zu levels=%zu chunk_base=0x%08x "
      "stock_records=%llu waited_ms=%llu duration_ms=%llu",
      report.sets, report.refused, report.records, report.levels, report.chunk_base,
      static_cast<unsigned long long>(report.stock_records), static_cast<unsigned long long>(waitedMs),
      static_cast<unsigned long long>(NowMs() - startedMs));
}

// A mount that threw (it runs inside the game's frame, or from the plugin loader): it is not tried again, and one
// line says whether the sets were published before the throw. It builds no string of its own; g_mutex is not held (the
// throw left the scope that held it).
void MountFailed(const char* reason) {
  g_mountDeferred.store(false);
  NotifyMountObserver(MountReport{});
  if (g_mountFailedSaid.exchange(true)) return;
  const bool served = g_published.load(std::memory_order_acquire) != nullptr;
  Log(EchoVR::LogLevel::Error,
      "[NEVR.RESOURCE] resource sets mount failed reason=%s served=%s (%s)", reason, served ? "true" : "false",
      served ? "the sets are served, but the mount's lines and its overrides may be incomplete"
             : "no plugin level or resource is served, and the mount is not tried again");
}

// A deferred mount's attempt: on the game's own thread, once the game has asked for its first resource and its
// manifest holds records.
void TryMount(bool onLoopThread) {
  const std::uint64_t now = NowMs();
  if (onLoopThread && !g_gameThreadSeen.load(std::memory_order_relaxed)) g_gameThreadSeen.store(true);
  // Every tick while the mount waits: with no lock, nothing to do until the game's thread runs this after the first
  // request, except the one late warning.
  const bool ready = onLoopThread && g_requestSeen.load(std::memory_order_relaxed);
  const std::uint64_t armed = g_armedAtMs.load(std::memory_order_relaxed);
  if (!ready && (g_lateSaid.load(std::memory_order_relaxed) || now - armed < kLateMs)) return;
  MountReport report;
  std::vector<const Set*> mounted;
  bool observed = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_mountDeferred.load() || g_registry.mounted()) return;
    const std::uint64_t count = StockRecordsNow();
    if (!g_lateSaid.load() && now - armed >= kLateMs) {
      g_lateSaid.store(true);
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.RESOURCE] resource sets still not mounted after %llu s sets=%zu stock_records=%llu "
          "first_request_seen=%s game_thread_seen=%s (no plugin level or resource is served until the mount; it waits "
          "for the game's first resource request, on the game's own thread)",
          static_cast<unsigned long long>((now - armed) / 1000), g_registry.held(),
          static_cast<unsigned long long>(count), g_requestSeen.load() ? "true" : "false",
          g_gameThreadSeen.load() ? "true" : "false");
    }
    if (!ready || count == 0) return;
    MountLocked(count, &report, &mounted);
    observed = g_mountObserver.load() != nullptr;
    if (!observed) PublishLocked(mounted);
  }
  if (observed) {
    NotifyMountObserver(report);
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishLocked(mounted);
  }
  WriteMountLines(now, report, mounted, {}, {}, now - armed);
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
  char id[kMaxLabel + 1];  // the catches below allocate nothing
  try {
    return RegisterOne(pluginId, desc);
  } catch (const std::bad_alloc&) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.RESOURCE] resource set refused plugin=%s reason=out_of_memory (registration ran out of memory; nothing "
        "was registered)",
        PrintableInto(pluginId, id));
  } catch (const std::exception&) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.RESOURCE] resource set refused plugin=%s reason=exception (registration threw; nothing was registered)",
        PrintableInto(pluginId, id));
  }
  return NEVR_RESOURCES_BAD_FORMAT;
}

Outcome RegisterFileResourceSet(const std::string& owner, std::int64_t held_handle, const std::string& label,
                                const std::string& sha256, bool rehash) {
  if (!IsMapOwner(owner) || !LabelOk(owner.c_str(), kMaxMapLabel)) {
    return MapRefused(owner, label, Refusal::kFormat,
                      "bad_owner: \"map:<id>\", at most 128 bytes of printable ASCII with no double quote or "
                      "backslash");
  }
  if (!LabelOk(label.c_str(), kMaxMapLabel)) {
    return MapRefused(owner, label, Refusal::kFormat,
                      "bad_label: 1 to 128 bytes of printable ASCII with no double quote or backslash");
  }
  if (rehash && !IsSha256Hex(sha256)) {
    return MapRefused(owner, label, Refusal::kFormat, "bad_pin: the pinned sha256 is not 64 lowercase hex digits");
  }
  const HANDLE file = reinterpret_cast<HANDLE>(static_cast<intptr_t>(held_handle));
  if (held_handle == 0 || file == INVALID_HANDLE_VALUE) {
    return MapRefused(owner, label, Refusal::kFormat, "bad_handle: no file handle");
  }
  // Before the read and the map, which are wasted once the sets are mounted; Add checks again under the lock.
  if (MountedNow()) return MapRefused(owner, label, Refusal::kTooLate, "the run's resource sets are already mounted");

  LARGE_INTEGER fileSize = {};
  if (!GetFileSizeEx(file, &fileSize)) {
    return MapRefused(owner, label, Refusal::kFormat, "GetFileSizeEx error " + std::to_string(GetLastError()));
  }
  const auto size = static_cast<std::uint64_t>(fileSize.QuadPart);
  if (size == 0) return MapRefused(owner, label, Refusal::kFormat, "the file is empty");
  if (rehash) {
    DWORD error = 0;
    const std::string actual = FileSha256(file, size, &error);
    if (actual.empty()) {
      return MapRefused(owner, label, Refusal::kFormat,
                        error == ERROR_HANDLE_EOF ? std::string("the file ended before its size while it was hashed")
                                                  : "hash read error " + std::to_string(error));
    }
    if (actual != sha256) return MapRefused(owner, label, Refusal::kHash, "sha256 " + actual + ", pinned " + sha256);
  }
  FileView view;
  DWORD mapError = 0;
  if (!view.Map(file, &mapError)) {
    return MapRefused(owner, label, Refusal::kFormat, "the file maps with error " + std::to_string(mapError));
  }
  nevr_resource_set::View parsed;
  const nevr_resource_set::Result format = nevr_resource_set::Parse(view.get(), size, &parsed);
  if (!format.ok()) {
    return MapRefused(owner, label, Refusal::kFormat,
                      std::string(nevr_resource_set::ProblemName(format.problem)) + " at index " +
                          std::to_string(format.index));
  }
  std::size_t levels = 0;
  for (std::uint32_t i = 0; i < parsed.record_count(); ++i) {
    if (parsed.RecordAt(i).type == kLevelRootType) ++levels;
  }

  Outcome outcome;
  {
    // The same claim as a plugin's set: a set and a plugin asset never both take one resource.
    const std::lock_guard<std::mutex> claim(g_keyMutex);
    const std::string claimed = AssetClaimOn(parsed);
    if (!claimed.empty()) return MapRefused(owner, label, Refusal::kCollides, claimed);
    const std::lock_guard<std::mutex> lock(g_mutex);
    {
      // Listed before the Add, whose success must leave nothing that can throw: the registry owns the handle then.
      const std::lock_guard<std::mutex> views(g_viewMutex);
      g_fileViews.emplace_back(held_handle, view.get());
    }
    outcome = g_registry.AddMapFile(owner, label, view.get(), size, held_handle);
    if (outcome.ok()) {
      view.release();
    } else {
      const std::lock_guard<std::mutex> views(g_viewMutex);
      g_fileViews.pop_back();
    }
  }
  if (!outcome.ok()) return MapRefused(owner, label, outcome.refusal, outcome.detail);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] map resource set registered owner=%s label=\"%s\" records=%u chunks=%u levels=%zu overrides=%u "
      "bytes=%llu rehashed=%d (mapped from the file, held for the mount)",
      owner.c_str(), label.c_str(), parsed.record_count(), parsed.chunk_count(), levels, parsed.override_count(),
      static_cast<unsigned long long>(size), rehash ? 1 : 0);
  return outcome;
}

void WithdrawFileResourceSet(const std::string& owner) {
  std::vector<std::int64_t> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    dropped = g_registry.DropMap(owner);
  }
  if (dropped.empty()) return;
  CloseHandles(dropped);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] resource set withdrawn owner=%s count=%zu (the map did not mount whole, so its set is not "
      "served)",
      Printable(owner.c_str()).c_str(), dropped.size());
}

void SetMountObserver(void (*observer)(const MountReport& report)) { g_mountObserver.store(observer); }

namespace {
void MountWhenReadyUnguarded() {
  std::vector<std::int64_t> abandoned;
  std::vector<std::string> abandonedPlugins;
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_registry.mounted()) return;
    if (g_registry.held() == 0) {
      // Nothing to mount: close registration for the run without a line (no plugin carries resources).
      const MountReport none = g_registry.Mount(StockView{&StockHasThunk, nullptr, 0});
      lock.unlock();
      NotifyMountObserver(none);
      return;
    }
    // The resolver hook goes in now, so it sees the game's first request (which the mount waits for).
    bool hooked = true;
#ifdef NEVR_TEST_HOOKS
    if (g_testNoResolverHook) {
      hooked = false;
    } else if (g_testStockHas == nullptr) {
      hooked = nevr::assets::EnsureResolverHook();
    }
#else
    hooked = nevr::assets::EnsureResolverHook();
#endif
    if (!hooked) {
      for (const Set* set : g_registry.HeldSets()) abandonedPlugins.push_back(Printable(set->plugin.c_str()));
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
  if (!abandoned.empty()) {
    NotifyMountObserver(MountReport{});
    WriteMountLines(NowMs(), MountReport{}, {}, abandoned, abandonedPlugins, 0);
  }
}
}  // namespace

void MountWhenReady() {
  try {
    MountWhenReadyUnguarded();
  } catch (const std::bad_alloc&) {
    MountFailed("out_of_memory");
  } catch (const std::exception&) {
    MountFailed("exception");
  }
}

void NoteRequest() {
  if (!g_requestSeen.load(std::memory_order_relaxed)) g_requestSeen.store(true, std::memory_order_relaxed);
}

namespace {
// The reads served in the last window, one line per set that served any, every 10 s; and the first key the game's
// manifest answered for a set.
void SummarizeReads() {
#ifdef NEVR_TEST_HOOKS
  if (g_testMountThrow == MountThrowAt::kReadSummary) throw std::bad_alloc();
#endif
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
        Printable(sets[i]->plugin.c_str()).c_str(), sets[i]->label.c_str(), static_cast<unsigned long long>(served),
        static_cast<unsigned long long>(now - last));
  }
  if (published->firstWinReady.load(std::memory_order_acquire) && !g_stockWinSaid.exchange(true)) {
    const std::uint64_t stockWins = published->stockWins.load(std::memory_order_relaxed);
    const std::uint32_t user = published->firstWinUser.load();
    const std::string plugin =
        user != 0 && user - 1 < sets.size() ? Printable(sets[user - 1]->plugin.c_str()) : std::string("unknown");
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource set keys answered by the game's manifest count=%llu first=%s plugin=%s (a package "
        "set added after the mount holds keys a plugin's set holds; the game's bytes are served for them)",
        static_cast<unsigned long long>(stockWins),
        KeyText(published->firstWinType.load(), published->firstWinName.load()).c_str(), plugin.c_str());
  }
}
}  // namespace

// Per frame, inside the game's own hook (DispatchPerFrameWork): nothing here may throw into the game.
void OnFrame(bool onLoopThread) {
  if (g_mountDeferred.load(std::memory_order_relaxed)) {
    try {
      TryMount(onLoopThread);
    } catch (const std::bad_alloc&) {
      MountFailed("out_of_memory");
    } catch (const std::exception&) {
      MountFailed("exception");
    }
  }
  const char* summaryFailed = nullptr;
  try {
    SummarizeReads();
  } catch (const std::bad_alloc&) {
    summaryFailed = "out_of_memory";
  } catch (const std::exception&) {
    summaryFailed = "exception";
  }
  if (summaryFailed != nullptr && !g_summaryFailedSaid.exchange(true)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] resource set read summary skipped reason=%s (the sets are still served; the summary is tried "
        "again next window, and this line is written once a run)",
        summaryFailed);
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
      published->firstWinReady.store(true, std::memory_order_release);
    }
    return nullptr;
  }
  if (user < published->served.size()) published->served[user].fetch_add(1, std::memory_order_relaxed);
  return published->shadow->manager();
}

std::unique_lock<std::mutex> LockKeyClaims() { return std::unique_lock<std::mutex>(g_keyMutex); }

bool HoldsKey(std::uint64_t type, std::uint64_t name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.Holds(type, name) || g_registry.Holds(nevr_resource_key::OnDiskType(type, 0), name) ||
         g_registry.Holds(nevr_resource_key::OnDiskType(type, 1), name);
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
  std::vector<std::int64_t> handles;
  for (const Set* set : g_registry.HeldSets()) handles.push_back(set->handle);
  CloseHandles(handles);  // the files the registry holds; the callers closed the refused and dropped sets' files
  g_registry = Registry();
  g_published.store(nullptr);
  g_mountDeferred.store(false);
  g_deferSaid = false;
  g_lateSaid = false;
  g_armedAtMs = 0;
  g_gameThreadSeen.store(false);
  g_requestSeen.store(false);
  g_lastSummaryMs.store(0);
  g_stockWinSaid.store(false);
  g_mountFailedSaid.store(false);
  g_summaryFailedSaid.store(false);
  g_mountObserver.store(nullptr);
  g_mountObserved.store(false);
  g_init = InitState{};
  g_testStockHas = nullptr;
  g_testStockRecords = 0;
  g_testNow = nullptr;
  g_testThrow = false;
  g_testMountThrow = MountThrowAt::kNowhere;
  g_testBeforeAdd = nullptr;
  g_testAfterPublish = nullptr;
  g_testNoResolverHook = false;
  g_testThrowAfterOpen = false;
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

void SetMountThrowForTest(MountThrowAt where) { g_testMountThrow = where; }

void SetNoResolverHookForTest(bool on) { g_testNoResolverHook = on; }

void SetThrowAfterOpenForTest(bool on) { g_testThrowAfterOpen = on; }

void SetBeforeAddForTest(void (*hook)()) { g_testBeforeAdd = hook; }

void SetAfterPublishForTest(void (*hook)()) { g_testAfterPublish = hook; }

void SetFilePathForTest(const wchar_t* path) { g_testFilePath = path != nullptr ? path : L""; }

int FileCheckForTest(const wchar_t* path, std::uint64_t fileOffset, const unsigned char* mapped, std::uint64_t size) {
  FileHandle file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) return 1;
  DWORD error = 0;
  switch (FileHoldsMappedBytes(file.get(), fileOffset, mapped, size, &error)) {
    case FileCheck::kSame: return 0;
    case FileCheck::kReadFailed: return 1;
    case FileCheck::kDiffers: return 2;
  }
  return 1;
}
#endif

}  // namespace nevr_resource_sets
