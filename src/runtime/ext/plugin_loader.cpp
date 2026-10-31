#include "runtime/ext/plugin_loader.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "abi/game_level.h"           // #1094: where the game keeps its main level
#include "core/globals.h"
#include "core/logging.h"
#include "core/nevr_root.h"

#include "runtime/log/symcache.h"             // #1094: the symbol cache, which names the lobby and match levels
#include "runtime/patch/ui_texture_mips_rules.h"  // #1094: NameForHash, the search of the table sorted by hash
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"    // g_pGame
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal
#include "runtime/hook/hook_guard.h"
#include "runtime/ext/plugin_load_plan.h"       // N134 S6: config-driven load plan
#include "runtime/ext/plugin_log.h"             // #598: a plugin's log line, as the host writes it
#include "runtime/ext/plugin_manifest.h"        // #60: the login's plugin report
#include "runtime/ext/plugin_refusal_notice.h"   // #741: the player's notice, once per run
#include "runtime/ext/plugin_signature.h"       // #741: who signed the file
#include "runtime/ext/plugin_tree.h"            // #741: and every DLL it names from its own directory
#include "runtime/hook/dll_load_hook.h"         // #741: loads a plugin makes after init
#include "runtime/ext/plugin_trust.h"           // #741: what the runtime pins, and whether this build enforces it
#include "runtime/patch/asset_registry.h"      // #432: assets a plugin carries leave with its image

struct LoadedPlugin {
  HMODULE                     hModule;
  NvrPluginInfo               info;
  uint32_t                    api_version;
  uint32_t                    capabilities;  /* NvrPluginCapabilities bitmask; 0 = UNDECLARED */
  /* What get_plugin_info hands out: its own struct, not a cast of `info` (whose
   * padding sits where NvrLoadedPluginInfo has api_version). */
  NvrLoadedPluginInfo         loaded_info;
  NvrPluginInit_fn            init;
  NvrPluginOnFrame_fn         on_frame;
  NvrPluginOnGameStateChange_fn on_state_change;
  NvrPluginShutdown_fn        shutdown;
  std::string                 path;
  const char*                 id;  /* the config.yaml entry's name; see InternPluginId */
};

// A deque: push_back never moves an element already stored, so the address GetLoadedPluginInfo hands
// out for a plugin stays valid across every later load, whatever it adds.
static std::deque<LoadedPlugin> g_plugins;

// #60: what happened to each configured plugin at the last LoadPlugins(), in
// config.yaml order. Written once at the end of LoadPlugins (boot thread), read
// by the login builder on the bridge's websocket thread, hence the mutex.
static std::mutex g_pluginReportMutex;
static std::vector<PluginManifestEntry> g_pluginReport;

static void PublishPluginReport(std::vector<PluginManifestEntry> report) {
  std::lock_guard<std::mutex> lock(g_pluginReportMutex);
  g_pluginReport = std::move(report);
}

// N135 / #598: the host's name for a plugin (its config.yaml entry's `name`) as a string a plugin may keep
// for the life of the process: ctx->plugin_id. A deque never moves a stored string, and the store is
// never cleared, so the pointer outlives UnloadPlugins and stays valid for a plugin's Shutdown.
static std::deque<std::string> g_pluginIds;

static const char* InternPluginId(const std::string& name) {
  for (const std::string& known : g_pluginIds) {
    if (known == name) return known.c_str();
  }
  g_pluginIds.push_back(name);
  return g_pluginIds.back().c_str();
}

// N135: ctx->log. The line goes through Log(), which is boot-aware: while the runtime boots under the
// loader lock it reaches the boot tee, afterwards the game's log. The message is plain text and is
// passed as an argument, never as the format.
void HostPluginLog(const char* pluginId, uint32_t level, const char* message,
                   const NevrLogField* fields, uint32_t fieldCount) {
  const nevr_plugin_log::Formatted out = nevr_plugin_log::Format(pluginId, level, message, fields, fieldCount);
  EchoVR::LogLevel hostLevel = EchoVR::LogLevel::Info;
  switch (out.level) {
    case NEVR_LOG_DEBUG:   hostLevel = EchoVR::LogLevel::Debug;   break;
    case NEVR_LOG_INFO:    hostLevel = EchoVR::LogLevel::Info;    break;
    case NEVR_LOG_WARNING: hostLevel = EchoVR::LogLevel::Warning; break;
    case NEVR_LOG_ERROR:   hostLevel = EchoVR::LogLevel::Error;   break;
  }
  Log(hostLevel, "%s", out.line.c_str());
}

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the generated symbol cache, sorted by hash (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

// #1094: a level symbol's name from the symbol cache (the generated table of the game's PC and Quest names, which
// holds the lobby and match levels), or nullptr. A level the cache does not know, such as a custom one, has none.
static const char* LevelNameOf(uint64_t symbol) {
  return nevr_ui_texture_mips::NameForHash(evr::kSymCacheData, static_cast<size_t>(evr::kSymCacheCount), symbol);
}

// #1094: a level symbol as a log reads it: "none", the name when the cache knows it, else the hex value.
static std::string LevelLabel(uint64_t symbol) {
  if (symbol == 0) return "none";
  if (const char* name = LevelNameOf(symbol)) return name;
  char label[24];
  snprintf(label, sizeof(label), "0x%016llx", static_cast<unsigned long long>(symbol));
  return label;
}

// #1094: the level the game holds as its main level, read from the game context (the runtime's g_pGame) on every
// context the loader builds, so a plugin sees the level at init, on every frame and at every state change.
// A level can load with no state transition, so the frame context carries it too. The host logs each change
// once, whichever plugin or frame sees it first.
static uint64_t ObserveMainLevel() {
  const uint64_t level = EchoVR::ReadMainLevelSymbol(g_pGame, EchoVR::kPcMainLevelSymbolOffset);
  static std::atomic<uint64_t> s_lastLogged{0};
  const uint64_t before = s_lastLogged.exchange(level);
  if (before != level) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] main level changed: from=%s to=%s symbol=0x%016llx",
        LevelLabel(before).c_str(), LevelLabel(level).c_str(), static_cast<unsigned long long>(level));
  }
  return level;
}

// The context one plugin sees: the caller's, with that plugin's id, the host log and the current level filled in.
static NvrGameContext ContextFor(const NvrGameContext& base, const char* id) {
  NvrGameContext ctx = base;
  ctx.plugin_id = id != nullptr ? id : "";
  ctx.log = HostPluginLog;
  ctx.level_symbol = ObserveMainLevel();
  ctx.level_name = LevelNameOf(ctx.level_symbol);
  return ctx;
}

// N134 S8: query API — lets a plugin discover its neighbours at runtime.
// Called through ctx->get_plugin_count / ctx->get_plugin_info (function
// pointers filled into NvrGameContext by LoadPlugins and every per-frame/
// per-state-change ctx construction). Returns the count of successfully
// loaded plugins; a plugin that failed init or was refused (N89) is not in
// g_plugins and therefore not counted.

int GetLoadedPluginCount(void) {
  return static_cast<int>(g_plugins.size());
}

const NvrLoadedPluginInfo* GetLoadedPluginInfo(int index) {
  if (index < 0 || static_cast<size_t>(index) >= g_plugins.size()) return nullptr;
  // The plugin's own NvrLoadedPluginInfo. Not a cast of p.info: NvrPluginInfo is
  // padded to 32 bytes, so through it api_version read the padding and
  // capabilities read api_version. Stable for the process lifetime: g_plugins is a deque, so a
  // later push_back never moves an entry whose pointer a plugin kept.
  return &g_plugins[index].loaded_info;
}

// N112 / #60 — the login's `nevr_plugins` array: one entry per configured
// plugin (loaded, failed, or disabled). Called from ws_bridge.cpp's login
// builder. The JSON is built by the pure BuildPluginManifest (nlohmann-json).
std::string BuildPluginManifestJson() {
  std::lock_guard<std::mutex> lock(g_pluginReportMutex);
  return BuildPluginManifest(g_pluginReport);
}


// N134 S6: required-aware load failure. A plugin the config declared
// `required: true` that fails to load or init is FATAL on a dedicated server
// (ServerFatal -> ForceFatalExit; the same N120 rule modules follow) and a hard
// warning on a client (ServerFatal warns and returns in client mode). An optional
// plugin (`required: false`, the default) warns and the loader continues. Caller
// `continue`s after this returns. `reason` is a fully-formed clause (no values
// that could be secrets — plugin args are NOT interpolated into it). The same
// clause goes into the login's plugin report (#60) as that entry's `error`.
static void FailPluginLoad(const PluginLoadItem& item, const std::string& reason,
                           PluginManifestEntry& report) {
  report.loaded = false;
  report.error = reason;
  if (item.required) {
    ServerFatal("Required plugin %s (%s) failed: %s — a server must not run without a "
                "plugin it declared required",
                item.name.c_str(), item.file.c_str(), reason.c_str());
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] %s (%s): %s — optional, continuing",
        item.name.c_str(), item.file.c_str(), reason.c_str());
  }
}

namespace {

// The legacy `.\name.dll` spelling names the same file as `name.dll`.
std::string_view PluginFilename(const std::string& file) {
  std::string_view name(file);
  if (name.substr(0, 2) == ".\\") name.remove_prefix(2);
  return name;
}

// ADR 0010 section 1: when the OS rejects the restricted search flags, a run that allows unsigned plugins
// retries with the default search order (N75's old behaviour, logged); a signed-only run does not, because a
// load the check did not shape is a load it did not cover.
bool RestrictedSearchFallbackAllowed(bool enforcing, DWORD error) {
  return !enforcing && error == ERROR_INVALID_PARAMETER;
}

// `file` names a DLL inside plugins/. No other path component belongs in a
// configured filename.
bool PluginFileStaysInDirectory(const std::string& file) {
  const std::string_view name = PluginFilename(file);
  if (name.empty() || name == "." || name == "..") return false;
  return name.find_first_of("\\/:") == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

// #768 (ADR 0010): a library the plugin names that only System32 supplies. The loader searches the plugin's own
// directory before System32 (N75) for a name that is not already loaded, so a file put in that directory after the
// check would map in its place, unchecked. The check cannot hold a file that is not there; instead the System32
// library is loaded by its full path first, and the loader takes a module already loaded under a name before it
// searches for one. Whoever holds this object keeps the libraries loaded.
thread_local bool t_pinning = false;  // PluginLoadFilter ignores the loads made while this thread pins
#ifdef NEVR_TEST_HOOKS
bool g_failPinning = false;  // a test makes every pin fail with ERROR_ACCESS_DENIED
#endif

class PinnedModules {
 public:
  PinnedModules() = default;
  PinnedModules(const PinnedModules&) = delete;
  PinnedModules& operator=(const PinnedModules&) = delete;
  ~PinnedModules() {
    for (HMODULE m : modules_) FreeLibrary(m);
  }

  // Loads each lowercase name from System32. False when one cannot be: `failed` names it, `error` is why.
  bool Pin(const std::vector<std::string>& names, std::string* failed, DWORD* error) {
    if (names.empty()) return true;
    char system[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryA(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
      *failed = "the system directory";
      *error = GetLastError();
      return false;
    }
    struct Pinning {
      Pinning() { t_pinning = true; }
      ~Pinning() { t_pinning = false; }
    } pinning;
    for (const std::string& name : names) {
      HMODULE m = LoadLibraryExA((std::string(system) + "\\" + name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#ifdef NEVR_TEST_HOOKS
      if (g_failPinning) {
        if (m != nullptr) FreeLibrary(m);
        m = nullptr;
        SetLastError(ERROR_ACCESS_DENIED);
      }
#endif
      if (m == nullptr) {
        *failed = name;
        *error = GetLastError();
        return false;
      }
      modules_.push_back(m);
    }
    return true;
  }

  // The libraries stay loaded for the life of the process (a filter's verified plugin is loaded later, at a time
  // this code cannot see, and unloading them at exit would run under the loader lock).
  void KeepForTheLifeOfTheProcess() { modules_.clear(); }

 private:
  std::vector<HMODULE> modules_;
};

// #741 (ADR 0010): the plugin file, open for reading with FILE_SHARE_READ only, so no Windows process can
// write to it or replace it while it is held. StageOne verifies the signature on it and keeps it open
// across LoadModule: the bytes that were checked are the bytes that load.
class HeldFile {
 public:
  HeldFile() = default;
  HeldFile(const HeldFile&) = delete;
  HeldFile& operator=(const HeldFile&) = delete;
  ~HeldFile() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  HANDLE get() const { return handle_; }
  void Adopt(HANDLE h) { handle_ = h; }

  // The plugin's dependencies from its own directory, held the same way across the load.
  nevr_plugin_tree::HeldFiles deps;
  // The System32 libraries the plugin names that nothing in its directory does, loaded before the plugin (#768).
  PinnedModules pins;

  // #767: where the held file is, from the handle: no link, no 8.3 name, no prefix. This is what the load names, so
  // the load is of the file that was checked, whatever the configured path leads to by then. Empty: nothing is held.
  std::wstring loadPath;

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// ---- the refusal notice (ADR 0010 section 6) -------------------------------------------------------------------

std::mutex g_refusalMutex;
nevr_plugin_refusal::Tracker g_refusals;
void (*g_refusalDialogSink)(const std::string& text) = nullptr;  // tests replace the dialog

DWORD WINAPI RefusalDialogThread(LPVOID text) {
  MessageBoxA(nullptr, static_cast<const char*>(text), "Echo VR - plugin not loaded",
              MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
  return 0;
}

void RecordRefusal(const std::string& file) {
  std::lock_guard<std::mutex> lock(g_refusalMutex);
  g_refusals.Record(file);
}

// Set once LoadPlugins has shown its own notice. From then on a refusal made by a later load shows the notice
// itself (once per run, the tracker caps it): nothing else would, because LoadPlugins is over.
std::atomic<bool> g_noticeArmed{false};

// The record (log line, manifest entry) is already written when this runs. One dialog per run, on its own thread
// so boot never waits on a modal box; never on a server or a headless run.
void ShowRefusalNoticeIfDue() {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(g_refusalMutex);
    text = g_refusals.TakeDialog(g_isServer != FALSE, g_isHeadless != FALSE);
  }
  if (text.empty()) return;
  if (g_refusalDialogSink != nullptr) {
    g_refusalDialogSink(text);
    return;
  }
  // The text must outlive this call (the thread reads it later); leaked once per process on purpose, as #553's is.
  char* copy = _strdup(text.c_str());
  if (copy == nullptr) return;
  HANDLE thread = CreateThread(nullptr, 0, RefusalDialogThread, copy, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);
}

void ShowRefusalNoticeIfDue();

// A load refused after LoadPlugins finished: record it, and tell the player now. A load refused while LoadPlugins
// still runs (a staged plugin's DllMain, say) is shown by LoadPlugins's own notice instead.
void RecordLateRefusal(const std::string& file) {
  RecordRefusal(file);
  if (g_noticeArmed.load()) ShowRefusalNoticeIfDue();
}

// ---- loads a plugin makes after its init (ADR 0010 section 1) --------------------------------------------------
//
// DllLoadHook runs this predicate before every LoadLibrary* in the process. A path inside the plugins directory
// has to pass the same check as a staged plugin (and so does everything it names). The loader's own staged loads
// were checked before they were made and the OS resolves their imports without the hook, so the path of the load
// in progress is kept per thread and that one path is not checked twice, while the loader is the one loading it.
// A load the plugin itself makes from its DllMain, by that path or another, is checked.

thread_local std::wstring t_stagedPath;

// The file a late load the filter accepted has to map (#1177): the text the filter was asked about (lowercase, as the
// hook gave it) and the exact path of the file it checked and holds. Set by the filter as it accepts, taken by the
// rewriter the hook asks next on the same thread, and cleared whenever the filter runs again.
thread_local std::wstring t_checkedAsked;
thread_local std::wstring t_checkedFile;

struct FilterState {
  std::mutex mutex;
  std::wstring dirLower;  // plugins directory, lowercase, backslashes, trailing backslash; empty: filter inactive
  std::wstring appDir;    // the directory that holds the plugins directory (a relative path is tried against it), lowercase, trailing backslash
  std::string dirNarrow;  // the same directory as the loader spells it, for the dependency walk
  std::wstring dirFinal;  // the directory's final path (long names, no prefix, no reparse points), lowercase, trailing backslash
  // Verified files stay open (FILE_SHARE_READ only) for the life of the process: the bytes that were checked
  // are the bytes the real load maps, and nobody can swap them in between or afterwards.
  struct Verified {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring loadPath;  // the held file's final path, case kept: what a load of it has to name (#1177)
    nevr_plugin_tree::HeldFiles deps;
    PinnedModules pins;
    Verified() = default;
    Verified(const Verified&) = delete;
    Verified& operator=(const Verified&) = delete;
    ~Verified() {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
  };
  std::map<std::wstring, std::unique_ptr<Verified>> verified;
};
FilterState& Filter() {
  static FilterState state;
  return state;
}

std::wstring NormalizeWide(const wchar_t* path) {
  std::wstring out(path);
  for (wchar_t& c : out) {
    if (c == L'/') c = L'\\';
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
  }
  return out;
}

std::string NarrowName(const std::wstring& wide) {
  const size_t slash = wide.find_last_of(L'\\');
  const std::wstring name = slash == std::wstring::npos ? wide : wide.substr(slash + 1);
  std::string out;
  for (wchar_t c : name) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return out;
}

// The final path of an open file or directory: long names resolved, the extended-length prefix removed, the case the
// file system reports. Windows resolves symbolic links and junctions in it; Wine does not resolve a unix symbolic link
// to a directory, so a link to the plugins directory is not recognised there (#788).
bool FinalPathOfHandleExact(HANDLE handle, std::wstring* out) {
  std::wstring buf(512, L'\0');
  for (;;) {
    const DWORD n = GetFinalPathNameByHandleW(handle, buf.data(), static_cast<DWORD>(buf.size()),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) return false;
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(static_cast<size_t>(n) + 1);
  }
  if (buf.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
    buf = L"\\" + buf.substr(7);
  } else if (buf.compare(0, 4, L"\\\\?\\") == 0) {
    buf.erase(0, 4);
  }
  // Wine hands back the spelling the handle was opened with, an 8.3 name included; expand it. Native Windows
  // has already done so and this changes nothing there.
  DWORD longLen = GetLongPathNameW(buf.c_str(), nullptr, 0);
  if (longLen != 0) {
    std::wstring expanded(longLen, L'\0');
    const DWORD got = GetLongPathNameW(buf.c_str(), expanded.data(), longLen);
    if (got != 0 && got < longLen) {
      expanded.resize(got);
      buf = expanded;
    }
  }
  *out = buf;
  return true;
}

// The same, lowercase: what the plugins directory is compared with. The text of the path a caller passes cannot be: an
// 8.3 short name, an extended-length prefix, a trailing dot or space and a junction all name a file in the plugins
// directory without spelling it.
bool FinalPathOfHandle(HANDLE handle, std::wstring* out) {
  if (!FinalPathOfHandleExact(handle, out)) return false;
  if (!out->empty()) CharLowerBuffW(out->data(), static_cast<DWORD>(out->size()));
  return true;
}

// Opens `path` for its attributes only, sharing everything, so looking at a file never stops anyone using it.
bool FinalPathOf(const std::wstring& path, std::wstring* out) {
  HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const bool ok = FinalPathOfHandle(h, out);
  CloseHandle(h);
  return ok;
}

#ifdef NEVR_TEST_HOOKS
// Runs between resolving a late load's path and opening it: where a test points the name somewhere else.
void (*g_betweenResolveAndOpen)(const std::wstring& resolved) = nullptr;
// Runs between a staged plugin's check and its load: where a test points the path somewhere else.
void (*g_betweenCheckAndLoad)(const std::wstring& staged) = nullptr;
// Makes the query for a held file's path fail, as the OS can (a handle to a file on a volume that cannot name it).
bool g_failNamingHeldFile = false;
// Runs after a late load's check has passed and before the filter returns: where a test points the name somewhere else.
void (*g_afterLateLoadAccepted)(const std::wstring& checked) = nullptr;
#endif

#ifdef NEVR_TEST_HOOKS
// Runs after a plugin has been checked and before the load that maps it: where a test plants a file in the
// plugins directory.
void (*g_afterDependenciesChecked)() = nullptr;
#endif

// Whether `candidate` names a file in the plugins directory, and which one. False: it names nothing there.
bool FileInPluginsDirectory(const FilterState& f, const std::wstring& candidate, std::wstring* full) {
  if (FinalPathOf(candidate, full)) {
    return full->compare(0, f.dirFinal.size(), f.dirFinal) == 0 && full->size() > f.dirFinal.size();
  }
  if (candidate.compare(0, f.dirLower.size(), f.dirLower) == 0 && candidate.size() > f.dirLower.size()) {
    *full = candidate;  // cannot be resolved (denied, say) but is spelled as a file in the directory: the open decides
    return true;
  }
  return false;  // not a file in the plugins directory, or no file at all (the real load reports that)
}

// Notes the file a late load that passed the check must map, when its text does not already name it (#1177). A bare
// name is left to the OS search (the plugins directory may not be on it); a run that does not require signatures
// loads what the caller named.
void NoteCheckedFile(const wchar_t* lowerPath, const std::wstring& asked, const std::wstring& loadPath, bool bare,
                     bool enforcing) {
  if (!enforcing || bare || loadPath.empty() || NormalizeWide(loadPath.c_str()) == asked) return;
  t_checkedAsked = lowerPath;
  t_checkedFile = loadPath;
  if (!nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
    std::string path;
    for (wchar_t c : loadPath) path.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] a later load of %s loads the file it checked, %s",
        NarrowName(asked).c_str(), path.c_str());
  }
}

bool PluginLoadFilter(const wchar_t* lowerPath) {
  t_checkedAsked.clear();
  t_checkedFile.clear();
  if (lowerPath == nullptr) return false;
  if (t_pinning) return false;  // this thread is loading a System32 library for the filter's own pin, holding f.mutex
  FilterState& f = Filter();
  std::lock_guard<std::mutex> lock(f.mutex);
  if (f.dirLower.empty()) return false;
  // LoadLibrary appends ".dll" to a name with no extension: the file it maps is the one to check.
  const std::wstring p = nevr_plugin_tree::WithDefaultExtension(NormalizeWide(lowerPath));
  // The loader's own load of a file it checked. It is exempt only until the loader has taken its lock: the plugin's
  // DllMain runs inside the same LoadLibrary call with the lock held, and a load made there is the plugin's own, so it
  // is checked even when it names the path being loaded.
  if (!t_stagedPath.empty() && p == t_stagedPath && !nevr_dll_load_hook::LoaderLockHeldByThisThread()) return false;
  const bool enforcing = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;
  // Where the OS can find the name. A bare name searches the usual places; if the plugins directory holds that
  // name, it is the one at risk. A relative path with a directory part is tried against the current directory and
  // against the application directory, because which one the loader takes is not something this code can see.
  const bool bare = p.find_first_of(L"\\:") == std::wstring::npos;
  const bool relative = !bare && p.compare(0, 1, L"\\") != 0 && p.compare(1, 1, L":") != 0;
  std::vector<std::wstring> candidates;
  if (bare) {
    candidates.push_back(f.dirLower + p);
  } else if (!relative) {
    candidates.push_back(p);
  } else {
    // The loader appends a relative path to every directory it searches (System32, the Windows directory, PATH, the
    // directories set with SetDllDirectory, and each of those through links), so no list of candidates is complete:
    // a "..", an ancestor of plugins\\ or a junction in one of them each reach it. In a signed-only run a relative
    // path with a directory part is refused whatever it leads to; a plugin that needs such a load passes a full path.
    if (enforcing) {
      const std::string name = NarrowName(p);
      if (!nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PLUGIN] refused a later load of %s: a relative path with a directory part cannot be checked; "
            "load it by its full path",
            name.c_str());
      }
      RecordLateRefusal(name);
      return true;
    }
    wchar_t cwdBased[MAX_PATH * 4] = {};
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(cwdBased)), cwdBased, nullptr);
    if (n == 0 || n >= std::size(cwdBased) || f.appDir.empty()) {
      // Cannot tell where this relative path leads: refuse (fail closed) rather than let it through unchecked.
      const std::string name = NarrowName(p);
      if (enforcing) {
        if (!nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
          Log(EchoVR::LogLevel::Warning,
              "[NEVR.PLUGIN] refused a later load of %s: its relative path cannot be resolved to a file", name.c_str());
        }
        RecordLateRefusal(name);
        return true;
      }
      return false;
    }
    candidates.push_back(NormalizeWide(cwdBased));
    candidates.push_back(f.appDir + p);
  }
  std::wstring full;
  bool inDirectory = false;
  for (const std::wstring& candidate : candidates) {
    if (FileInPluginsDirectory(f, candidate, &full)) {
      inDirectory = true;
      break;
    }
  }
  if (!inDirectory) return false;
  const auto remembered = f.verified.find(full);
  if (remembered != f.verified.end()) {
    NoteCheckedFile(lowerPath, p, remembered->second->loadPath, bare, enforcing);
#ifdef NEVR_TEST_HOOKS
    if (g_afterLateLoadAccepted != nullptr) g_afterLateLoadAccepted(full);
#endif
    return false;
  }

  const std::string file = NarrowName(full);
#ifdef NEVR_TEST_HOOKS
  if (g_betweenResolveAndOpen != nullptr) g_betweenResolveAndOpen(full);
#endif
  HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;  // the real load reports it
    if (!enforcing) return false;
    if (!nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] refused a later load of %s: cannot open the file to check its signature: error %lu",
          file.c_str(), static_cast<unsigned long>(error));
    }
    RecordLateRefusal(file);
    return true;
  }
  auto entry = std::make_unique<FilterState::Verified>();
  entry->handle = h;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  std::string refusal;
  // The file that was resolved and the file now held must be the same one: between the two opens a name can be
  // pointed somewhere else. What is verified is what this handle holds.
  std::wstring heldPath;
  const nevr_plugin_signature::Verdict v =
      nevr_plugin_signature::VerifyPluginFile(h, nevr_plugin_trust::ActivePolicy(), now);
  bool named = FinalPathOfHandleExact(h, &entry->loadPath);
#ifdef NEVR_TEST_HOOKS
  if (g_failNamingHeldFile) named = false;
#endif
  if (named) {
    heldPath = entry->loadPath;
    CharLowerBuffW(heldPath.data(), static_cast<DWORD>(heldPath.size()));
  }
  if (!named) {
    refusal = "cannot name the file it checked";
  } else if (heldPath != full) {
    refusal = "the file changed between resolving its path and opening it";
  } else if (!v.accepted()) {
    refusal = v.Describe();
  } else {
    std::vector<std::string> system32Only;
    const nevr_plugin_tree::TreeVerdict t = nevr_plugin_tree::VerifyPluginTree(
        f.dirNarrow, h, nevr_plugin_trust::ActivePolicy(), now, &entry->deps, &system32Only);
    if (!t.accepted) {
      refusal = "dependency " + t.Describe();
      RecordRefusal(t.file);
    } else if (enforcing) {
      std::string failed;
      DWORD error = 0;
      if (!entry->pins.Pin(system32Only, &failed, &error)) {
        refusal = "dependency " + failed + " cannot be loaded from the system directory to pin it: error " +
                  std::to_string(error);
        RecordRefusal(failed);
      } else if (!system32Only.empty() && !nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] later load of %s: %lu system dependencies loaded from the system directory first",
            file.c_str(), static_cast<unsigned long>(system32Only.size()));
      }
    }
  }
  if (refusal.empty()) {
#ifdef NEVR_TEST_HOOKS
    if (g_afterDependenciesChecked != nullptr) g_afterDependenciesChecked();
#endif
    entry->pins.KeepForTheLifeOfTheProcess();
    NoteCheckedFile(lowerPath, p, entry->loadPath, bare, enforcing);
    f.verified[full] = std::move(entry);
#ifdef NEVR_TEST_HOOKS
    if (g_afterLateLoadAccepted != nullptr) g_afterLateLoadAccepted(full);
#endif
    return false;
  }
  CloseHandle(h);
  if (!enforcing) {
    if (!nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] a plugin loads %s after its init: %s, allowed because plugin signatures are not required in "
          "this run (plugin trust mode %s)",
          file.c_str(), refusal.c_str(), nevr_plugin_trust::ModeWord(nevr_plugin_trust::ActiveMode()));
    }
    return false;
  }
  if (!nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] refused a later load of %s: %s", file.c_str(), refusal.c_str());
  }
  RecordLateRefusal(file);
  return true;
}

// The hook asks this right after the filters, for a load none of them refused: the file the filter just checked, if
// the text it was given is not already that file's path (#1177).
bool PluginLoadRewriter(const wchar_t* lowerPath, std::wstring* loadPath) {
  if (lowerPath == nullptr || t_checkedFile.empty() || t_checkedAsked != lowerPath) return false;
  *loadPath = t_checkedFile;
  t_checkedFile.clear();
  t_checkedAsked.clear();
  return true;
}

bool g_filterRegistered = false;  // the hook's own Shutdown() clears every filter; a test that ran it forgets this

// Registers the filter once. The plugins directory is the loader's spelling (a trailing backslash).
void InstallPluginLoadFilter(const std::string& pluginDir) {
  FilterState& f = Filter();
  bool& registered = g_filterRegistered;
  {
    std::lock_guard<std::mutex> lock(f.mutex);
    f.dirNarrow = pluginDir;
    // The directory is spelled in the ANSI code page; a byte above 0x7F is the code page's character, not the
    // character with that number. An empty result leaves the filter inactive and says so.
    const std::wstring wide = nevr_dll_load_hook::AnsiPathToLowerWide(pluginDir.c_str());
    if (wide.empty() && !pluginDir.empty()) {
      Log(EchoVR::LogLevel::Error,
          "[NEVR.PLUGIN] the plugins directory %s cannot be converted from the ANSI code page: later loads are NOT checked",
          pluginDir.c_str());
    }
    f.dirLower = NormalizeWide(wide.c_str());
    if (!f.dirLower.empty() && f.dirLower.back() != L'\\') f.dirLower.push_back(L'\\');
    f.appDir.clear();
    if (!f.dirLower.empty()) {
      const size_t last = f.dirLower.find_last_of(L'\\', f.dirLower.size() - 2);
      if (last != std::wstring::npos) f.appDir = f.dirLower.substr(0, last + 1);
    }
    f.dirFinal.clear();
    if (!f.dirLower.empty() && !FinalPathOf(f.dirLower, &f.dirFinal)) f.dirFinal = f.dirLower;
    if (!f.dirFinal.empty() && f.dirFinal.back() != L'\\') f.dirFinal.push_back(L'\\');
  }
  if (!registered) {
    registered = true;
    nevr_dll_load_hook::AddLoadFilter("plugin-signature", &PluginLoadFilter);
    nevr_dll_load_hook::AddLoadRewriter("plugin-signature", &PluginLoadRewriter);
  }
}

struct StagedPlugin {
  HMODULE                  hModule;
  PluginLoadItem            item;
  NvrPluginInfo             info;
  uint32_t                  apiVersion;
  uint32_t                  caps;      // NvrPluginCapabilities, for pass-2 sorting
  NvrPluginInitEx_fn        initExFn;
  NvrPluginInit_fn          initFn;
  NvrPluginOnFrame_fn       onFrameFn;
  NvrPluginOnGameStateChange_fn onStateChangeFn;
  NvrPluginShutdown_fn      shutdownFn;
  PluginInitKind            initKind;
  std::string               path;       // full path to DLL on disk
  size_t                    planIndex;  // #60: this plugin's entry in `report`
};

// One LoadPlugins() run: pass 1 stages every enabled plugin that loads, pass 2 inits the staged ones in
// capability-priority order. Split out of LoadPlugins() so each step is short enough to read (#100).
class PluginLoadRun {
 public:
  PluginLoadRun(std::vector<PluginLoadItem> planIn, std::string pluginDirIn)
      : plan(std::move(planIn)), pluginDir(std::move(pluginDirIn)), report(plan.size()), heldBy(plan.size()) {
    for (size_t i = 0; i < plan.size(); ++i) {
      heldBy[i] = i;
      // #60: one report entry per configured plugin, in config order. Each starts
      // "not loaded, no error" and is filled in as the loader decides its fate.
      report[i].name = plan[i].name;
      report[i].file = plan[i].file;
      report[i].enabled = plan[i].enabled;
      report[i].required = plan[i].required;
      if (plan[i].enabled) ++enabledCount;
    }
  }

  size_t PlanSize() const { return plan.size(); }
  size_t EnabledCount() const { return enabledCount; }

  // The enabled entries `phase` loads (#496).
  size_t PhaseCount(PluginPhase phase) const {
    size_t n = 0;
    for (const PluginLoadItem& item : plan) {
      if (item.enabled && PluginInPhase(item, phase, earlyPassRan)) ++n;
    }
    return n;
  }

  // Pass 1: LoadLibrary + resolve exports for every plugin `phase` takes, collecting into a staging
  // list. Init is deferred to pass 2 so we can stable-sort by capability priority (N134 S8): observers
  // before modifiers, modifiers before engine-hookers. Within a priority band the config.yaml order is
  // preserved. A run that did the early pass is kept for the normal one (#496), so that pass sees what
  // the early one loaded: a repeat of its entries, the same module under another spelling, the report.
  void Stage(PluginPhase phase) {
    staged.clear();
    for (size_t planIndex = 0; planIndex < plan.size(); ++planIndex) {
      if (PluginInPhase(plan[planIndex], phase, earlyPassRan)) StageOne(planIndex);
    }
    if (phase == PluginPhase::Early) earlyPassRan = true;
  }

  // Pass 2: order the staged plugins and init each one.
  void InitAll(const NvrGameContext& ctx) {
    SortAndLogOrder();
    InitStaged(ctx);
  }

  std::vector<PluginManifestEntry> TakeReport() { return std::move(report); }
  std::vector<PluginManifestEntry> Report() const { return report; }

 private:
  // A module an earlier pass of this run loaded and initialised (#496).
  struct LoadedModule {
    HMODULE hModule;
    size_t  planIndex;
  };

  std::vector<PluginLoadItem> plan;
  std::string pluginDir;
  std::vector<PluginManifestEntry> report;
  std::vector<size_t> heldBy;
  std::vector<StagedPlugin> staged;
  std::vector<LoadedModule> loaded;
  size_t enabledCount = 0;
  bool earlyPassRan = false;

  // A repeat of an entry is skipped (see below), but if the repeat is `required`
  // the plugin it names must still be fatal to lose: the requirement moves onto the
  // entry that is actually loaded. When that entry already failed non-fatally
  // (it was optional then), the failure is raised now, as fatal. Returns true when
  // the requirement was carried over.
  //
  // A skipped entry is recorded in `heldBy` against the entry that actually holds its
  // module, and `carryRequired` follows that chain, so a repeat of a skipped entry
  // reaches the entry that holds the module rather than the skipped one.
  size_t resolveHolder(size_t index) const {
    while (heldBy[index] != index) index = heldBy[index];
    return index;
  }

  bool carryRequired(size_t namedIndex, const PluginLoadItem& repeat) {
    const size_t firstIndex = resolveHolder(namedIndex);
    if (!repeat.required || plan[firstIndex].required) return false;
    plan[firstIndex].required = true;
    report[firstIndex].required = true;
    for (StagedPlugin& s : staged) {
      if (s.planIndex == firstIndex) s.item.required = true;
    }
    const PluginManifestEntry& first = report[firstIndex];
    if (!first.loaded && !first.error.empty()) {
      const std::string reason = first.error;
      FailPluginLoad(plan[firstIndex], reason, report[firstIndex]);
    }
    return true;
  }

  // Stages one plan entry, or records why it was skipped or failed.
  void StageOne(size_t planIndex) {
    const PluginLoadItem& item = plan[planIndex];
    PluginManifestEntry& itemReport = report[planIndex];
    const std::string path = pluginDir + item.file;
    const char* filename = item.file.c_str();

    if (!item.enabled) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PLUGIN] %s (%s): disabled in config.yaml — not loading",
          item.name.c_str(), filename);
      return;
    }
    if (SkipSuperseded(item, itemReport)) return;
    if (SkipDuplicateEntry(planIndex)) return;
    if (pluginDir.empty()) {
      FailPluginLoad(item, "the plugin directory cannot be resolved (no LOCALAPPDATA)", itemReport);
      return;
    }
    if (!PluginFileStaysInDirectory(item.file)) {
      FailPluginLoad(item, "file is outside plugins directory", itemReport);
      return;
    }
    HeldFile held;
    if (!CheckSignature(item, path, itemReport, &held)) return;
#ifdef NEVR_TEST_HOOKS
    if (g_betweenCheckAndLoad != nullptr) g_betweenCheckAndLoad(nevr_dll_load_hook::AnsiPathToLowerWide(path.c_str()));
#endif
    const bool enforcing = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;
#ifdef NEVR_TEST_HOOKS
    if (g_afterDependenciesChecked != nullptr) g_afterDependenciesChecked();
#endif
    HMODULE hPlugin = LoadModule(path, held.loadPath, filename, enforcing);
    if (!hPlugin) {
      const DWORD error = GetLastError();
      if (enforcing && error == ERROR_INVALID_PARAMETER) {
        // ADR 0010 section 1: a load the check did not shape is a load the check did not cover.
        FailPluginLoad(item, "refused: restricted search unsupported, no fallback load in a signed-only run",
                       itemReport);
        return;
      }
      FailPluginLoad(item, "LoadLibrary failed: error " + std::to_string(error), itemReport);
      return;
    }
    if (SkipSameModule(planIndex, hPlugin)) return;
    ResolveAndStage(planIndex, hPlugin, path);
  }

  // #741 (ADR 0010): opens the file, decides whether the pipeline signed it, and leaves it held. False when
  // the plugin must not load (the failure is recorded through FailPluginLoad); in a build that allows
  // unsigned plugins a refusal is logged at Warning and the load goes on.
  bool CheckSignature(const PluginLoadItem& item, const std::string& path, PluginManifestEntry& itemReport,
                      HeldFile* held) {
    const char* filename = item.file.c_str();
    const bool enforcing = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (!enforcing) return true;  // nothing to check; the load reports a missing file as it always did
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        FailPluginLoad(item, "LoadLibrary failed: error " + std::to_string(error), itemReport);
        return false;
      }
      const std::string reason =
          "refused: cannot open the file to check its signature: error " + std::to_string(error);
      Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] plugin=%s file=%s %s", item.name.c_str(), filename,
          reason.c_str());
      FailPluginLoad(item, reason, itemReport);
      return false;
    }
    held->Adopt(h);
    if (!NameHeldFile(item, path, held, itemReport)) return false;
    const nevr_plugin_signature::Verdict verdict = nevr_plugin_signature::VerifyPluginFile(
        held->get(), nevr_plugin_trust::ActivePolicy(), static_cast<int64_t>(std::time(nullptr)));
    if (verdict.accepted()) return enforcing ? CheckDependencies(item, held, itemReport) : true;
    if (!enforcing) {
      const std::string what = verdict.reason == nevr_plugin_signature::Reason::Unsigned
                                   ? std::string("UNSIGNED")
                                   : "NOT ACCEPTED: " + verdict.Describe();
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] plugin=%s file=%s %s, loaded because plugin signatures are not required in this run "
          "(plugin trust mode %s; start with -require-signed-plugins to refuse it)",
          item.name.c_str(), filename, what.c_str(), nevr_plugin_trust::ModeWord(nevr_plugin_trust::ActiveMode()));
      return true;
    }
    const std::string reason = "refused: " + verdict.Describe();
    Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] plugin=%s file=%s %s", item.name.c_str(), filename,
        reason.c_str());
    RecordRefusal(item.file);
    FailPluginLoad(item, reason, itemReport);
    return false;
  }

  // #767: the held file's own path, which LoadModule loads. The configured path is a name, and a name that goes
  // through a link, a junction or a directory someone can rename leads to another file by the time of the load; the
  // handle does not. A signed-only run refuses a plugin whose held file cannot be named; any other run loads the
  // configured path, as it did before, and says so.
  bool NameHeldFile(const PluginLoadItem& item, const std::string& path, HeldFile* held, PluginManifestEntry& itemReport) {
    const bool enforcing = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;
    bool named = FinalPathOfHandleExact(held->get(), &held->loadPath);
#ifdef NEVR_TEST_HOOKS
    if (g_failNamingHeldFile) {
      named = false;
      SetLastError(ERROR_ACCESS_DENIED);
    }
#endif
    if (!named) {
      const DWORD error = GetLastError();
      held->loadPath.clear();
      const std::string reason = "cannot name the file it checked: error " + std::to_string(error);
      if (enforcing) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] plugin=%s file=%s refused: %s", item.name.c_str(),
            item.file.c_str(), reason.c_str());
        FailPluginLoad(item, "refused: " + reason, itemReport);
        return false;
      }
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] plugin=%s file=%s %s; loading the configured path, which is not tied to the file checked "
          "(plugin trust mode %s)",
          item.name.c_str(), item.file.c_str(), reason.c_str(), nevr_plugin_trust::ModeWord(nevr_plugin_trust::ActiveMode()));
      return true;
    }
    std::wstring configured = nevr_dll_load_hook::AnsiPathToLowerWide(path.c_str());
    std::wstring heldLower = held->loadPath;
    CharLowerBuffW(heldLower.data(), static_cast<DWORD>(heldLower.size()));
    if (NormalizeWide(configured.c_str()) != heldLower) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] plugin=%s file=%s is loaded from %s, the file that was checked",
          item.name.c_str(), item.file.c_str(), NarrowName(held->loadPath).c_str());
    }
    return true;
  }

  // ADR 0010 section 1: the DLLs this plugin names (imports, delay-load imports, forwarded exports) that sit in
  // the plugins directory pass the same check, or the plugin does not load. They stay held across the load.
  bool CheckDependencies(const PluginLoadItem& item, HeldFile* held, PluginManifestEntry& itemReport) {
    std::vector<std::string> system32Only;
    const nevr_plugin_tree::TreeVerdict tree = nevr_plugin_tree::VerifyPluginTree(
        pluginDir, held->get(), nevr_plugin_trust::ActivePolicy(), static_cast<int64_t>(std::time(nullptr)),
        &held->deps, &system32Only);
    if (tree.accepted) return PinSystemDependencies(item, system32Only, held, itemReport);
    const std::string reason = "refused: dependency " + tree.Describe();
    Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] plugin=%s file=%s %s", item.name.c_str(), item.file.c_str(),
        reason.c_str());
    RecordRefusal(item.file);  // the dialog names the plugin the player asked for and the file that stopped it
    RecordRefusal(tree.file);
    FailPluginLoad(item, reason, itemReport);
    return false;
  }

  // #768 (ADR 0010): the System32 libraries the plugin names are loaded before it, so a file put in the plugins
  // directory after the check cannot take their place. Held until the plugin is loaded and staged.
  bool PinSystemDependencies(const PluginLoadItem& item, const std::vector<std::string>& names, HeldFile* held,
                             PluginManifestEntry& itemReport) {
    std::string failed;
    DWORD error = 0;
    if (held->pins.Pin(names, &failed, &error)) {
      if (!names.empty()) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] plugin=%s file=%s %lu system dependencies loaded from the system directory first",
            item.name.c_str(), item.file.c_str(), static_cast<unsigned long>(names.size()));
      }
      return true;
    }
    const std::string reason = "refused: dependency " + failed +
                               " cannot be loaded from the system directory to pin it: error " + std::to_string(error);
    Log(EchoVR::LogLevel::Warning, "[NEVR.PLUGIN] plugin=%s file=%s %s", item.name.c_str(), item.file.c_str(),
        reason.c_str());
    RecordRefusal(item.file);
    RecordRefusal(failed);
    FailPluginLoad(item, reason, itemReport);
    return false;
  }

  // True when the entry is skipped.
  bool SkipSuperseded(const PluginLoadItem& item, PluginManifestEntry& itemReport) {
    const char* filename = item.file.c_str();
    const std::string_view name = PluginFilename(item.file);
  // N89: refuse plugins whose function is now BUILT IN to gamepatches. Loading
  // one makes two MinHook instances (gamepatches links extern/minhook, plugins
  // link the vcpkg port — separate static copies, separate hook tables) fight
  // over the same target. This survives the cutover to config-driven loading:
  // an operator naming log_filter.dll explicitly is still an error we correct,
  // because the built-in filter already provides it. Refused even if the entry
  // is marked required — the function is present (built in), so it is not a
  // missing dependency; warn and continue rather than fatal.
  {
    static const char* kSupersededByBuiltin[] = {"log_filter.dll", "log-filter.dll"};
    bool superseded = false;
    for (const char* s : kSupersededByBuiltin) {
      if (name.size() == strlen(s) && _strnicmp(name.data(), s, name.size()) == 0) {
        superseded = true;
        break;
      }
    }
    if (superseded) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] SKIPPED %s — superseded by the built-in log filter. Loading it "
          "would install a second MinHook on CLog::PrintfImpl and silently disable both "
          "file logging and max_line_length truncation. Remove it from config.yaml.",
          filename);
      itemReport.error = "superseded by the built-in log filter";
      return true;
    }
  }
    return false;
  }

  // True when the entry is skipped.
  bool SkipDuplicateEntry(size_t planIndex) {
    const PluginLoadItem& item = plan[planIndex];
    PluginManifestEntry& itemReport = report[planIndex];
    const char* filename = item.file.c_str();
  // The same file listed twice: LoadLibrary would return the module already
  // loaded, and its init would run again with every callback doubled. Refused
  // even if this entry is marked required: the plugin itself is loaded, and the
  // requirement is carried onto the earlier entry.
  {
    const long first = DuplicatePluginEntry(plan, planIndex);
    if (first >= 0) {
      const bool carried = carryRequired(static_cast<size_t>(first), item);
      heldBy[planIndex] = resolveHolder(static_cast<size_t>(first));
      const std::string carriedNote = carried
          ? " This entry is required, so entry " + std::to_string(first + 1) +
                " is now required too."
          : std::string();
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] SKIPPED %s (%s) — the same file as entry %ld (%s), listed earlier "
          "in config.yaml. Loading it again would run its init twice and double every "
          "callback. This entry's args were ignored; entry %ld's args apply.%s "
          "Remove one of the entries.",
          item.name.c_str(), filename, first + 1, plan[first].name.c_str(), first + 1,
          carriedNote.c_str());
      itemReport.error = "listed twice in config.yaml";
      return true;
    }
  }
    return false;
  }

  // LoadLibrary with the restricted search; null on failure (the caller reports it). `heldPath` is the path of the
  // file the check held (#767): the load names it, not `path`. Empty: nothing was held (a run that allows unsigned
  // plugins and could not open the file), and `path` is loaded as it always was.
  static HMODULE LoadModule(const std::string& path, const std::wstring& heldPath, const char* filename, bool enforcing) {
  /* N75: LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32.
   *
   * The risk was never loading OUR dll — we pass a full path. It is how ITS
   * dependencies resolve: with dwFlags=0 the search order starts at the
   * application directory, so a dll dropped next to echovr.exe can satisfy a
   * dependency ahead of the real one. These flags restrict the search to the
   * loaded dll's own directory plus System32.
   *
   * Not hypothetical: N89 was a stale dll sitting in plugins/ silently taking
   * over the log filter for entire runs. That was an accident; the same directory
   * and the same loader are what an attacker would use deliberately.
   *
   * The flags require an absolute path (we have one). If the OS rejects them we
   * log and fall back rather than failing to load — but we say so, because a
   * silent fallback would defeat the whole point. */
  struct StagedLoadScope {  // this path is the loader's own load of a file it checked (PluginLoadFilter)
    explicit StagedLoadScope(const std::wstring& staged) {
      t_stagedPath = nevr_plugin_tree::WithDefaultExtension(NormalizeWide(staged.c_str()));
    }
    ~StagedLoadScope() { t_stagedPath.clear(); }
  } stagedScope(heldPath.empty() ? nevr_dll_load_hook::AnsiPathToLowerWide(path.c_str()) : heldPath);
  const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
  HMODULE hPlugin = heldPath.empty() ? LoadLibraryExA(path.c_str(), nullptr, flags)
                                     : LoadLibraryExW(heldPath.c_str(), nullptr, flags);
  // A signed-only run has no fallback (ADR 0010 section 1): the caller refuses on ERROR_INVALID_PARAMETER.
  if (!hPlugin && RestrictedSearchFallbackAllowed(enforcing, GetLastError())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PLUGIN] %s: restricted search flags unsupported — falling back to "
        "default search order (hardening inactive for this load)", filename);
    hPlugin = heldPath.empty() ? LoadLibraryA(path.c_str()) : LoadLibraryW(heldPath.c_str());
  }
    return hPlugin;
  }

  // True when this module is already staged under another entry.
  bool SkipSameModule(size_t planIndex, HMODULE hPlugin) {
    const PluginLoadItem& item = plan[planIndex];
    PluginManifestEntry& itemReport = report[planIndex];
    const char* filename = item.file.c_str();
  // The backstop for what the file-name check can't see: another spelling of the
  // same path, staged in this pass or loaded by an earlier one (#496). LoadLibrary
  // counted a reference for this call; give it back.
  {
    long holder = -1;
    for (const StagedPlugin& s : staged) {
      if (s.hModule == hPlugin) { holder = static_cast<long>(s.planIndex); break; }
    }
    for (const LoadedModule& m : loaded) {
      if (holder < 0 && m.hModule == hPlugin) holder = static_cast<long>(m.planIndex);
    }
    if (holder >= 0) {
      const PluginLoadItem& already = plan[static_cast<size_t>(holder)];
      FreeLibrary(hPlugin);
      const bool carried = carryRequired(static_cast<size_t>(holder), item);
      heldBy[planIndex] = resolveHolder(static_cast<size_t>(holder));
      const std::string carriedNote = carried
          ? " This entry is required, so " + already.name + " is now required too."
          : std::string();
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PLUGIN] SKIPPED %s (%s) — the same module as %s (%s), already loaded. "
          "This entry's args were ignored; %s's args apply.%s "
          "Remove one of the entries from config.yaml.",
          item.name.c_str(), filename, already.name.c_str(), already.file.c_str(),
          already.name.c_str(), carriedNote.c_str());
      itemReport.error = "the same module as " + already.name;
      return true;
    }
  }
    return false;
  }

  // Resolve the plugin's exports and stage it for pass 2, or free it and record why not.
  void ResolveAndStage(size_t planIndex, HMODULE hPlugin, const std::string& path) {
    const PluginLoadItem& item = plan[planIndex];
    PluginManifestEntry& itemReport = report[planIndex];
    const char* filename = item.file.c_str();
  auto getInfoFn = reinterpret_cast<NvrPluginGetInfo_fn>(GetProcAddress(hPlugin, "NvrPluginGetInfo"));
  if (!getInfoFn) {
    FreeLibrary(hPlugin);
    FailPluginLoad(item, "missing NvrPluginGetInfo export", itemReport);
    return;
  }

  NvrPluginInfo info = getInfoFn();
  if (!info.name) {
    FreeLibrary(hPlugin);
    FailPluginLoad(item, "NvrPluginGetInfo returned NULL name", itemReport);
    return;
  }

  // Check API version (v1 plugins lack this export). POLICY (unchanged, N134
  // S6): a plugin declaring a version ABOVE the host WARNS and loads anyway —
  // NOT a fatal, and NOT gated on `required`. The module loader fatals on an
  // unsupported version; the plugin loader deliberately does not, and this
  // asymmetry is deliberate (see N134). `required`
  // governs load/init FAILURE, not version skew.
  auto apiVersionFn = reinterpret_cast<NvrPluginGetApiVersion_fn>(GetProcAddress(hPlugin, "NvrPluginGetApiVersion"));
  uint32_t apiVersion = apiVersionFn ? apiVersionFn() : 1;
  if (apiVersion > NEVR_PLUGIN_API_VERSION) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PLUGIN] %s requires API v%u, host supports v%u — loading anyway",
        filename, apiVersion, NEVR_PLUGIN_API_VERSION);
  }

  // What does this plugin DO? (API v3, optional export — absent => UNDECLARED.)
  // A declaration, not an enforcement: the host cannot verify these bits and a
  // plugin that lies is believed. It exists so honest plugins are legible, so a
  // server can require a declaration before accepting a session, and so an
  // operator can see the answer without reading the plugin's source.
  auto capsFn = reinterpret_cast<NvrPluginGetCapabilities_fn>(GetProcAddress(hPlugin, "NvrPluginGetCapabilities"));
  const uint32_t caps = capsFn ? capsFn() : NEVR_PLUGIN_CAP_UNDECLARED;

  // Resolve init exports: v4 args-aware NvrPluginInitEx (preferred) and v3
  // NvrPluginInit (fallback), plus the tick/state/shutdown lifecycle exports.
  auto initExFn = reinterpret_cast<NvrPluginInitEx_fn>(GetProcAddress(hPlugin, "NvrPluginInitEx"));
  auto initFn = reinterpret_cast<NvrPluginInit_fn>(GetProcAddress(hPlugin, "NvrPluginInit"));
  auto onFrameFn = reinterpret_cast<NvrPluginOnFrame_fn>(GetProcAddress(hPlugin, "NvrPluginOnFrame"));
  auto onStateChangeFn = reinterpret_cast<NvrPluginOnGameStateChange_fn>(GetProcAddress(hPlugin, "NvrPluginOnGameStateChange"));
  auto shutdownFn = reinterpret_cast<NvrPluginShutdown_fn>(GetProcAddress(hPlugin, "NvrPluginShutdown"));

  const PluginInitKind initKind =
      ChoosePluginInit(initExFn != nullptr, initFn != nullptr);

  // Invalid UTF-8 in an arg (a ${VAR} from the ANSI environment, a mistyped
  // config.yaml) was serialized as U+FFFD. Name the plugin and the arg KEYS,
  // never the values (they may hold secrets).
  for (const std::string& key : item.args_replaced_keys) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PLUGIN] %s: arg '%s' held invalid UTF-8; bytes replaced with "
        "U+FFFD in args_json", item.name.c_str(), key.c_str());
  }

  // A v3 plugin (only NvrPluginInit) has no args channel — if the operator
  // configured args for it, say so rather than dropping them silently. Do NOT
  // log the args_json itself (it may hold secrets); log only that args exist.
  if (initKind == PluginInitKind::Legacy && item.args_json != "{}") {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PLUGIN] %s: args configured but the plugin exports only the v3 "
        "NvrPluginInit (no args channel) — args ignored", filename);
  }

  // Stage for pass-2 caps-ordered init (N134 S8).
  staged.push_back({hPlugin, item, info, apiVersion, caps,
                    initExFn, initFn, onFrameFn, onStateChangeFn, shutdownFn,
                    initKind, path, planIndex});
  }

  // Stable-sort by capability priority and log the resulting load order.
  void SortAndLogOrder() {
  // Pass 2: stable-sort by capability priority (config order preserved within
  // a band), then init each plugin in that order.
  std::stable_sort(staged.begin(), staged.end(), [](const StagedPlugin& a, const StagedPlugin& b) {
    // Lower `priority` loads first; the stable sort keeps config order within a band.
    return CapsLoadPriority(a.caps) < CapsLoadPriority(b.caps);
  });

  // Log the sorted order so an operator can see the load sequence: one INFO
  // summary naming the order, with the full per-item detail at DEBUG — an
  // INFO per-item line would near-duplicate the
  // per-plugin "Loaded: ..." INFO confirmation below once each plugin's init
  // succeeds (Rule 12: INFO is summary, DEBUG is narrative).
  {
    std::string order;
    for (size_t i = 0; i < staged.size(); i++) {
      if (i > 0) order += ", ";
      order += staged[i].item.name;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] load order (priority-sorted): %s", order.c_str());
  }
  for (size_t i = 0; i < staged.size(); i++) {
    const StagedPlugin& s = staged[i];
    const char* initVia = s.initKind == PluginInitKind::Ex ? "InitEx" :
                          s.initKind == PluginInitKind::Legacy ? "Init" : "no-init";
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.PLUGIN] [%zu/%zu] %s v%u.%u.%u (API v%u) caps=0x%02X via %s (priority=%d)",
        i + 1, staged.size(),
        s.item.name.c_str(),
        s.info.version_major, s.info.version_minor, s.info.version_patch,
        s.apiVersion, s.caps, initVia, CapsLoadPriority(s.caps));
  }
  }

  // Init each staged plugin in order; record the outcome in `report` and `g_plugins`.
  void InitStaged(const NvrGameContext& ctx) {
  for (const StagedPlugin& s : staged) {
    const char* initVia = s.initKind == PluginInitKind::Ex ? "InitEx" :
                          s.initKind == PluginInitKind::Legacy ? "Init" : "no-init";
    const char* pluginId = InternPluginId(s.item.name);
    const NvrGameContext pluginCtx = ContextFor(ctx, pluginId);
    int result = 0;
    switch (s.initKind) {
      case PluginInitKind::Ex:     result = s.initExFn(&pluginCtx, s.item.args_json.c_str()); break;
      case PluginInitKind::Legacy: result = s.initFn(&pluginCtx); break;
      case PluginInitKind::None:   break;  // no init export — plugin still loads
    }
    if (result != 0) {
      // An unsuccessful init may already have installed detours or acquired
      // resources. Give the plugin its shutdown callback before unmapping
      // its code. Without that export there is no safe way to prove cleanup;
      // retain the module reference until process exit.
      if (s.shutdownFn) {
        s.shutdownFn();
        FreeLibrary(s.hModule);
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PLUGIN] %s: shutdown after failed init code=%d completed; module released",
            s.item.file.c_str(), result);
      } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PLUGIN] %s: init failed without shutdown export — module retained until process exit",
            s.item.file.c_str());
      }
    }

    // N84: re-verify hooked addresses after this plugin's init may have installed its own detours.
    // A plugin that re-hooked an address this runtime already owns undoes our patch there, so that
    // is fatal on a server.
    if (s.initKind != PluginInitKind::None) {
      const int collisions = nevr_hook_guard::VerifyAll(s.item.file.c_str());
      if (collisions > 0) {
        ServerFatal("Plugin %s re-hooked %d address(es) this runtime already owns "
                    "— our patches at those addresses are no longer applied",
                    s.item.file.c_str(), collisions);
      }
    }

    if (result != 0) {
      FailPluginLoad(s.item, std::string(initVia) + " returned code " + std::to_string(result),
                     report[s.planIndex]);
      continue;
    }

    const NvrLoadedPluginInfo loadedInfo = {
        s.info.name, s.info.description,
        s.info.version_major, s.info.version_minor, s.info.version_patch,
        s.apiVersion, s.caps};
    g_plugins.push_back({s.hModule, s.info, s.apiVersion, s.caps, loadedInfo,
                         s.initFn, s.onFrameFn,
                         s.onStateChangeFn, s.shutdownFn, s.path, pluginId});
    loaded.push_back({s.hModule, s.planIndex});
    {
      PluginManifestEntry& r = report[s.planIndex];
      r.loaded = true;
      r.error.clear();
      r.version_major = s.info.version_major;
      r.version_minor = s.info.version_minor;
      r.version_patch = s.info.version_patch;
      r.api_version = s.apiVersion;
      r.capabilities = s.caps;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] Loaded: %s v%u.%u.%u (API v%u) caps=0x%02X%s via %s",
        s.info.name,
        s.info.version_major, s.info.version_minor, s.info.version_patch,
        s.apiVersion, s.caps,
        s.caps == NEVR_PLUGIN_CAP_UNDECLARED ? " UNDECLARED" : "",
        initVia);
    if (s.caps == NEVR_PLUGIN_CAP_UNDECLARED) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PLUGIN] %s declares no capabilities (pre-v3 or omitted). It is not "
          "known whether it affects gameplay; treat as unknown, not as harmless.",
          s.info.name);
    }
  }
  }
};

}  // namespace

// #496: the run an early pass did, kept for the normal pass.
static std::unique_ptr<PluginLoadRun> g_earlyRun;

// #924: states the plugin trust mode of this run, once: whichever pass comes first says it, and a run with no plugin
// configured says it too, so a player's log always shows whether signatures were enforced. The mode is the one
// PreflightRuntimeBootstrap set from the command line before the first pass; the literal is the build's.
static bool g_trustModeStated = false;

static void StatePluginTrustMode() {
  if (g_trustModeStated) return;
  g_trustModeStated = true;
  const nevr_plugin_trust::Mode mode = nevr_plugin_trust::ActiveMode();
  const nevr_plugin_trust::ModeSource source = nevr_plugin_trust::ActiveModeSource();
  // A run that is signed-only because its command line could not be read is a degraded start, not the operator's choice.
  const bool failedRead = source == nevr_plugin_trust::ModeSource::CommandLineUnreadable ||
                          source == nevr_plugin_trust::ModeSource::CommandLineReadFailed;
  const char* consequence =
      failedRead ? "the command line could not be read, so whether -require-signed-plugins was given is unknown: a plugin "
                   "not signed through the release pipeline does not load"
      : mode == nevr_plugin_trust::Mode::SignedOnly ? "a plugin not signed through the release pipeline does not load"
                                                     : "plugins load whether or not they are signed; "
                                                       "-require-signed-plugins makes a run signed-only";
  Log(failedRead ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.PLUGIN] plugin trust: mode=%s source=%s build=%s literal=\"%s\" (%s)", nevr_plugin_trust::ModeWord(mode),
      nevr_plugin_trust::ModeSourceWord(source), nevr_plugin_trust::BuildWord(), nevr_plugin_trust::ModeLiteral(),
      consequence);
}

void LoadPlugins(PluginPhase phase) {
  const bool early = phase == PluginPhase::Early;
  StatePluginTrustMode();

  // ADR 0012: plugins load from the nevr root's plugins\ folder, never from beside the game executable. An
  // unresolved root (no %LOCALAPPDATA%) leaves the directory empty: every configured plugin then fails with the
  // reason below, and nothing is searched in the game folder.
  const std::string rootPlugins = nevr_root::PluginsDir(nevr_root::RootA());
  const std::string pluginDir = rootPlugins.empty() ? std::string() : rootPlugins + "\\";
  if (pluginDir.empty()) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.PLUGIN] the plugin directory cannot be resolved: %%LOCALAPPDATA%% is not set, so no plugin loads "
        "(plugins load from <LOCALAPPDATA>\\nevr\\echovr\\plugins\\, never from the game folder)");
  }

  std::unique_ptr<PluginLoadRun> run = std::move(g_earlyRun);
  if (!run) {
    // N134 S6: the plugin set is the ORDERED `plugins:` list in config.yaml, NOT a
    // glob of the deployed directory. Config is authoritative — an absent/empty
    // list loads NOTHING, and an orphan DLL left in plugins/ is ignored unless the
    // config names it. This is a deliberate hardening over the old "load every
    // *.dll found" discovery, whose failure mode was N89: a stale DLL silently
    // taking over for an entire run because it happened to sit in the directory.
    std::vector<PluginLoadItem> plan = NevrCfgPluginLoadPlan();
    if (plan.empty()) {
      if (early) return;  // the normal pass reports it
      PublishPluginReport({});
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PLUGIN] no plugins configured in config.yaml — loading none");
      return;
    }
    run = std::make_unique<PluginLoadRun>(std::move(plan), pluginDir);
  }

  const size_t phaseCount = run->PhaseCount(phase);
  if (early) {
    if (phaseCount == 0) return;  // no entry is marked early: the normal pass loads them all
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] early pass: %zu plugin(s) marked early; loading them before the game "
        "reads its data, from %s",
        phaseCount, pluginDir.c_str());
  } else {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PLUGIN] %zu plugin(s) configured, %zu enabled; loading in list order from %s",
        run->PlanSize(), run->EnabledCount(), pluginDir.c_str());
  }
  const size_t loadedBefore = g_plugins.size();

  // Build init context (shared; the per-plugin channel is args_json via InitEx).
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.net_game = nullptr;
  ctx.game_state = 0;
  ctx.flags = 0;
  if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
  else ctx.flags |= NEVR_HOST_IS_CLIENT;
  if (g_isHeadless) ctx.flags |= NEVR_HOST_IS_HEADLESS;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = GetLoadedPluginCount;
  ctx.get_plugin_info = GetLoadedPluginInfo;

  InstallPluginLoadFilter(pluginDir);  // loads a plugin makes after its init face the same check

  run->Stage(phase);
  run->InitAll(ctx);

  if (early) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] early pass complete: %zu/%zu loaded",
        g_plugins.size() - loadedBefore, phaseCount);
    PublishPluginReport(run->Report());  // a copy: the normal pass fills in its entries
    g_earlyRun = std::move(run);
    return;
  }

  // Denominator (enabled entries) lets an operator tell "5 enabled, 5 loaded"
  // (healthy) from "5 enabled, 3 loaded" (2 silently missing) without
  // scrolling back through Warning lines to count. Disabled entries are not
  // missing, so they are not in the denominator.
  Log(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] plugin load complete: %zu/%zu loaded",
      g_plugins.size(), run->EnabledCount());

  PublishPluginReport(run->TakeReport());
  g_noticeArmed.store(true);  // a refusal from here on shows the notice itself (RecordLateRefusal), so none falls in a gap
  ShowRefusalNoticeIfDue();  // after the record; once per run, client with graphics only
}

// Assets a plugin registered point into its image; drop them before the image goes so the engine can never be
// handed a pointer into freed memory. A well-behaved plugin deregisters its own in shutdown; this is the host's
// guarantee for one that does not.
static void ReleaseAssetsOf(HMODULE module) {
  if (module == nullptr) return;
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;
  const size_t released =
      nevr::assets::Global().Deregister(base, base + nt->OptionalHeader.SizeOfImage);
  if (released != 0) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.ASSET] released %zu asset(s) still registered by a plugin at unload", released);
  }
}

void UnloadPlugins() {
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    if (it->shutdown) {
      it->shutdown();
    }
    ReleaseAssetsOf(it->hModule);
    FreeLibrary(it->hModule);
  }
  g_plugins.clear();
  g_earlyRun.reset();
  g_trustModeStated = false;
  PublishPluginReport({});  // nothing is loaded any more; do not report stale state
}

void TickPlugins(const NvrGameContext* ctx) {
  for (auto& p : g_plugins) {
    if (p.on_frame) {
      const NvrGameContext pluginCtx = ContextFor(*ctx, p.id);
      p.on_frame(&pluginCtx);
    }
  }
}

void NotifyPluginsStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state) {
  for (auto& p : g_plugins) {
    if (p.on_state_change) {
      const NvrGameContext pluginCtx = ContextFor(*ctx, p.id);
      p.on_state_change(&pluginCtx, old_state, new_state);
    }
  }
}

// ============================================================================
// Test hooks — NEVR_TEST_HOOKS enables unit-test injection of mock callbacks
// into the static plugin registry, so behavioral tests can verify that
// TickPlugins / NotifyPluginsStateChange actually fire registered callbacks.
// These are NEVER compiled into production builds (gamepatches.dll).
// ============================================================================

#ifdef NEVR_TEST_HOOKS

void TestHook_RegisterPluginOnFrame(NvrPluginOnFrame_fn fn) {
  LoadedPlugin p = {};
  p.hModule = nullptr;
  p.on_frame = fn;
  p.on_state_change = nullptr;
  p.shutdown = nullptr;
  g_plugins.push_back(p);
}

void TestHook_RegisterPluginOnStateChange(NvrPluginOnGameStateChange_fn fn) {
  LoadedPlugin p = {};
  p.hModule = nullptr;
  p.on_frame = nullptr;
  p.on_state_change = fn;
  p.shutdown = nullptr;
  g_plugins.push_back(p);
}

bool TestHook_RestrictedSearchFallbackAllowed(bool enforcing, unsigned long error) {
  return RestrictedSearchFallbackAllowed(enforcing, static_cast<DWORD>(error));
}

void TestHook_SetRefusalDialogSink(void (*sink)(const std::string& text)) { g_refusalDialogSink = sink; }

void TestHook_ResetRefusals() {
  std::lock_guard<std::mutex> lock(g_refusalMutex);
  g_refusals = nevr_plugin_refusal::Tracker();
}

void TestHook_ShowRefusalNoticeIfDue() { ShowRefusalNoticeIfDue(); }

void TestHook_RecordRefusal(const std::string& file) { RecordRefusal(file); }

void TestHook_InstallPluginLoadFilter(const std::string& pluginDir) { InstallPluginLoadFilter(pluginDir); }

void TestHook_SetBetweenResolveAndOpen(void (*hook)(const std::wstring& resolved)) { g_betweenResolveAndOpen = hook; }
void TestHook_SetAfterDependenciesChecked(void (*hook)()) { g_afterDependenciesChecked = hook; }
void TestHook_FailPinning(bool fail) { g_failPinning = fail; }

void TestHook_SetBetweenCheckAndLoad(void (*hook)(const std::wstring& staged)) { g_betweenCheckAndLoad = hook; }

void TestHook_FailNamingTheHeldFile(bool fail) { g_failNamingHeldFile = fail; }
void TestHook_SetAfterLateLoadAccepted(void (*hook)(const std::wstring& checked)) { g_afterLateLoadAccepted = hook; }

void TestHook_ForgetLoadFilterRegistration() { g_filterRegistered = false; }

size_t TestHook_LoadFilterVerifiedCount() {
  FilterState& f = Filter();
  std::lock_guard<std::mutex> lock(f.mutex);
  return f.verified.size();
}

void TestHook_ResetPluginLoadFilter() {
  FilterState& f = Filter();
  std::lock_guard<std::mutex> lock(f.mutex);
  g_noticeArmed.store(false);
  f.dirLower.clear();
  f.appDir.clear();
  f.dirFinal.clear();
  f.dirNarrow.clear();
  f.verified.clear();  // closes the held handles, so a test can delete or rewrite its files
}

void TestHook_ClearPlugins() {
  g_plugins.clear();
  g_earlyRun.reset();
  PublishPluginReport({});
}

#endif  // NEVR_TEST_HOOKS
