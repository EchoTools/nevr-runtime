// The production half of script_provider.h: the run's facts read from this process, the resource-set script waiver and
// the "map-scripts" load rewriter.
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "core/log_text.h"
#include "core/logging.h"
#include "core/resource_set_registry.h"
#include "runtime/content/content_pack.h"
#include "runtime/content/script_provider.h"
#include "runtime/hook/dll_load_hook.h"

namespace nevr_map_scripts {

namespace {

constexpr std::size_t kExePathChars = MAX_PATH * 2;  // the executable's path: a Win32 path can be longer than MAX_PATH
constexpr std::size_t kServeLogTextBytes = 300;      // the most of a path one served line carries; the rest is cut and counted

// Loads served under the loader lock (a DllMain's load) are counted, not logged: a line there is not safe (the logger's rule).
std::atomic<unsigned> g_servedUnderLoaderLock{0};

bool RewriteMapScript(const wchar_t* lower_path, std::wstring* load_path) {
  std::string owner;
  std::uint64_t hash = 0;
  bool first = false;
  if (!Rewrite(lower_path, load_path, &owner, &hash, &first)) return false;
  if (nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
    g_servedUnderLoaderLock.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  // The first serve of a script is a summary at Info; the game loads it again after each free, and those are Debug.
  const std::string file = nevr_content::Narrow(*load_path);
  const EchoVR::LogLevel level = first ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Debug;
  Log(level,
      "[NEVR.MAPS] map script served owner=%s hash=%016llx file=\"%s\" file_len=%zu served_under_loader_lock_total=%u",
      nevr_log_text::Printable(owner.c_str(), kServeLogTextBytes).c_str(), static_cast<unsigned long long>(hash),
      nevr_log_text::Printable(file.c_str(), kServeLogTextBytes).c_str(), file.size(),
      g_servedUnderLoaderLock.load(std::memory_order_relaxed));
  return true;
}

}  // namespace

void SetPolicy(bool signed_only, std::uint64_t byte_limit) {
  Inputs in;
  wchar_t exe[kExePathChars];
  const DWORD n = GetModuleFileNameW(nullptr, exe, kExePathChars);
  const std::wstring exe_path = (n > 0 && n < kExePathChars) ? std::wstring(exe, n) : std::wstring();
  in.exe_dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
  in.signed_only = signed_only;
  in.byte_limit = byte_limit;
  in.pack_carries = &nevr_content::CarriesScript;
  Configure(in);
  nevr_resource_sets::SetScriptWaiver(&Holds);
  const std::string dir = nevr_content::Narrow(in.exe_dir);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] map script policy exe_dir=\"%s\" signed_only=%d byte_limit=%llu (a map's script is served once the maps "
      "channel registers it)",
      nevr_log_text::Printable(dir.c_str(), kServeLogTextBytes).c_str(), in.signed_only ? 1 : 0,
      static_cast<unsigned long long>(in.byte_limit));
}

bool InstallMapScriptsRewriter() {
  static std::once_flag s_once;
  std::call_once(s_once, [] { nevr_dll_load_hook::AddLoadRewriter("map-scripts", &RewriteMapScript); });
  return true;
}

}  // namespace nevr_map_scripts
