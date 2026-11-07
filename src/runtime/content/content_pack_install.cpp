// The production half of content_pack.h: the facts Decide needs, read from this process, the one hook, and the decision's line.
#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <string>

#include "core/logging.h"
#include "core/nevr_root.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/content/content_pack.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/game_image_guard.h"

namespace nevr_content {

namespace {

// echovr.exe 631547: CFile::Open(const char* utf8_path, uint32_t flags, HANDLE* out), and the first 16 bytes of its prologue
// (push rbx; push rdi; push r12; push r13; push r14; sub rsp, 0xa40).
constexpr std::uint64_t kCFileOpenVa = 0x1400e15e0;
constexpr std::uint8_t kCFileOpenPrologue[] = {0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                                               0x56, 0x48, 0x81, 0xec, 0x40, 0x0a, 0x00, 0x00};
// The resource manager InitGlobals creates (stored at 0x14154920b) before it adds the boot package sets (0x14154973c): set
// means the game has read its manifests already.
constexpr std::uint64_t kResourceManagerVa = 0x1420a2ab8;

CFileOpenFn g_cfileOpen = nullptr;

std::uint64_t CFileOpenHook(const char* path, std::uint32_t flags, HANDLE* out) {
  return OpenThrough(g_cfileOpen, path, flags, out);
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// Text from content.json or a path, for a quoted log field: at most 300 bytes of printable ASCII; a quote or any other byte is '?'.
std::string Printable(const std::string& text) {
  std::string out;
  for (const char c : text.substr(0, 300)) out += (c >= 0x20 && c <= 0x7e && c != '"') ? c : '?';
  return out;
}

Decision Refuse(Decision d, const char* reason, const char* detail) {
  d.outcome = Outcome::kRefused;
  d.reason = reason;
  d.detail = detail;
  return d;
}

}  // namespace

void Install(std::uintptr_t game_base) {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;

  const std::wstring root = nevr_root::RootW();
  if (root.empty()) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CONTENT] content packs not read: no nevr root (LOCALAPPDATA unset or too long); the game runs on its stock data");
    return;
  }
  Inputs in;
  in.content_dir = nevr_root::ContentDir(root);
  wchar_t exe[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  const std::wstring exe_path = (n > 0 && n < MAX_PATH * 2) ? std::wstring(exe, n) : std::wstring();
  in.exe_dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
  // The game folder, which holds _data: the exe folder's grandparent (bin\win10), the game's own project root.
  in.game_dir = in.exe_dir + L"\\..\\..";
  in.signed_only = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;
  in.game_supported = nevr_game_image_guard::IsSupportedGameModule(reinterpret_cast<const void*>(game_base));
  void* const* manager = static_cast<void* const*>(nevr::ResolveVA_Checked(game_base, kResourceManagerVa));
  in.data_read_already = manager != nullptr && *manager != nullptr;

  Decision d = Decide(in);
  if (d.outcome == Outcome::kServed) {
    void* target = nevr::ResolveVA_Checked(game_base, kCFileOpenVa);
    if (target == nullptr || !nevr::ValidatePrologue(target, kCFileOpenPrologue, sizeof(kCFileOpenPrologue))) {
      d = Refuse(d, "hook_target_changed", "CFile::Open va=0x1400e15e0 is not the bytes this hook was written against");
    } else {
      g_cfileOpen = reinterpret_cast<CFileOpenFn>(target);
      if (!PatchDetour(&g_cfileOpen, reinterpret_cast<PVOID>(&CFileOpenHook), "CFile::Open (content pack)")) {
        d = Refuse(d, "hook_failed", "CFile::Open va=0x1400e15e0 (the hook line above names the cause)");
      }
    }
    if (d.outcome == Outcome::kServed && d.scripts > 0) nevr_dll_load_hook::AddLoadRewriter("content-pack", &RewriteScript);
    if (d.outcome != Outcome::kServed) Forget();
  }

  const std::string dir = Narrow(in.content_dir);
  switch (d.outcome) {
    case Outcome::kNone:
      Log(EchoVR::LogLevel::Info, "[NEVR.CONTENT] no content pack installed dir=%s (the game runs on its stock data)", dir.c_str());
      break;
    case Outcome::kServed:
      Log(EchoVR::LogLevel::Info,
          "[NEVR.CONTENT] content pack served id=%s version=%s manifests=%zu packages=%zu scripts=%zu files_checked=%zu dir=%s "
          "(the game reads these files in place of its stock ones; removing the pack's folder runs the game stock)",
          d.id.c_str(), d.version.c_str(), d.manifests, d.packages, d.scripts, d.files_checked, dir.c_str());
      break;
    case Outcome::kRefused:
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.CONTENT] content pack refused id=%s reason=%s detail=\"%s\" dir=%s (nothing of the pack is served; the game runs "
          "on its stock data)",
          d.id.empty() ? "none" : Printable(d.id).c_str(), d.reason.c_str(), Printable(d.detail).c_str(), dir.c_str());
      break;
  }
}

}  // namespace nevr_content
