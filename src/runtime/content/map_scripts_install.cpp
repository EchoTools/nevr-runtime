// The production half of script_provider.h: the run's facts read from this process, the resource-set script waiver and
// the "map-scripts" load rewriter.
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "core/logging.h"
#include "core/resource_set_registry.h"
#include "runtime/content/content_pack.h"
#include "runtime/content/script_provider.h"
#include "runtime/hook/dll_load_hook.h"

namespace nevr_map_scripts {

namespace {

std::atomic<unsigned> g_servedUnderLoaderLock{0};

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

bool RewriteMapScript(const wchar_t* lower_path, std::wstring* load_path) {
  std::string owner;
  std::uint64_t hash = 0;
  if (!Rewrite(lower_path, load_path, &owner, &hash)) return false;
  // A line here is safe outside the loader lock only; a load from a DllMain is served and counted, not logged.
  if (nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
    g_servedUnderLoaderLock.fetch_add(1, std::memory_order_relaxed);
  } else {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map script served owner=%s hash=%016llx game_path=%s file=%s served_in_dllmain=%u",
        owner.c_str(), static_cast<unsigned long long>(hash), Narrow(lower_path).c_str(), Narrow(*load_path).c_str(),
        g_servedUnderLoaderLock.load(std::memory_order_relaxed));
  }
  return true;
}

}  // namespace

void SetPolicy(bool signed_only) {
  Inputs in;
  wchar_t exe[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  const std::wstring exe_path = (n > 0 && n < MAX_PATH * 2) ? std::wstring(exe, n) : std::wstring();
  in.exe_dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
  in.signed_only = signed_only;
  in.pack_carries = &nevr_content::CarriesScript;
  Configure(in);
  nevr_resource_sets::SetScriptWaiver(&Holds);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] map script policy exe_dir=%s signed_only=%d (a map's script is served once the maps channel registers it)",
      Narrow(in.exe_dir).c_str(), in.signed_only ? 1 : 0);
}

bool InstallMapScriptsRewriter() {
  static std::once_flag s_once;
  std::call_once(s_once, [] { nevr_dll_load_hook::AddLoadRewriter("map-scripts", &RewriteMapScript); });
  return true;
}

}  // namespace nevr_map_scripts
