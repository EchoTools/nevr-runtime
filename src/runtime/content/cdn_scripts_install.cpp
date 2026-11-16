// The PC half of the CDN's script registrar (cdn/cdn_scripts.h): each label's `scripts` copy is claimed through the script provider
// (script_provider.h) under the owner "cargo:<label>", and the outcome is logged once per label. The policy is configure-once, so this
// registrar sets it first when the maps channel has not (its call is then kept, or refused when the signed-only facts differ).
#include <winsock2.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "cdn/cdn_env.h"
#include "cdn/cdn_scripts.h"
#include "cdn/env_windows.h"
#include "core/log_text.h"
#include "core/logging.h"
#include "core/map_manifest.h"
#include "core/map_script_check.h"
#include "runtime/content/cdn_scripts_install.h"
#include "runtime/content/content_pack.h"
#include "runtime/content/script_provider.h"

namespace nevr_cdn_scripts {

namespace {

constexpr const char* kEnvTag = "[NEVR.CDN]";
constexpr std::size_t kPackTextBytes = 120;  // a pack id in a line: cut at this many bytes
constexpr const char* kConsequenceServed = "the copies are served from this device; no script byte travels";
constexpr const char* kConsequencePack = "the installed content pack serves this script; remove the pack to get this label";

// The owner a label's copies are held under (map_script_check.h): "cargo:" and the label, which the manifest checked as a label name.
std::string OwnerOf(const std::string& label) { return std::string(nevr_map_script::kCargoOwnerPrefix) + label; }

// The cache folder as UTF-16 for the provider (its copy path is built from it); empty when the text does not convert, which the provider
// refuses as bad_copy_dir.
std::wstring WidenUtf8(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (chars <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], chars) != chars) return std::wstring();
  return out;
}

// One Info line per label the manifest lists (served), or one Warning (refused, with its reason and the pack when a pack holds the hash).
void LogLabels(const nevr_cdn::Manifest& manifest, const nevr_cdn::Platform& platform, const nevr_cdn::ScriptRegistration& result) {
  for (const std::string& label : nevr_cdn::ScriptLabels(manifest, platform)) {
    std::size_t listed = 0;
    for (const nevr_cdn::ScriptEntry& s : manifest.scripts) {
      if (s.label == label && s.platform == platform.name) ++listed;
    }
    const nevr_cdn::ScriptRefusal* refusal = nullptr;
    for (const nevr_cdn::ScriptRefusal& r : result.refused) {
      if (r.label == label) {
        refusal = &r;
        break;
      }
    }
    if (refusal == nullptr) {
      Log(EchoVR::LogLevel::Info, "[NEVR.CDN] cdn_scripts label=%s result=served scripts=%zu reason=ok consequence=\"%s\"", label.c_str(),
          listed, kConsequenceServed);
    } else if (refusal->pack.empty()) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.CDN] cdn_scripts label=%s result=refused scripts=%zu reason=%s consequence=\"%s\"",
          label.c_str(), listed, refusal->reason.c_str(), nevr_cdn::RefusalConsequence(refusal->reason).c_str());
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.CDN] cdn_scripts label=%s result=refused scripts=%zu reason=%s pack=%s consequence=\"%s\"",
          label.c_str(), listed, refusal->reason.c_str(), nevr_log_text::Printable(refusal->pack.c_str(), kPackTextBytes).c_str(),
          kConsequencePack);
    }
  }
}

}  // namespace

nevr_cdn::ScriptRegistrar PcRegistrar(const std::string& cache_dir, bool signed_only) {
  return [cache_dir, signed_only](const nevr_cdn::Manifest& manifest, const nevr_cdn::Platform& platform) {
    nevr_cdn::WindowsEnv env(kEnvTag);
    nevr_cdn::DeviceFacts facts;
    env.QueryDevice(cache_dir, &facts);
    nevr_map_scripts::SetPolicy(nevr_map_scripts::PolicyCaller::kCargo, signed_only, nevr_maps::MapDocumentBytes(facts));

    const std::wstring copyDir = WidenUtf8(cache_dir);
    const nevr_cdn::HeldScriptFn held = [](const std::string& label, std::uint64_t name) {
      return nevr_map_scripts::Holds(OwnerOf(label), name);
    };
    const nevr_cdn::ClaimScriptFn claim = [copyDir](const std::string& label, const nevr_cdn::ScriptEntry& s, bool official) {
      const nevr_map_scripts::Outcome o =
          nevr_map_scripts::RegisterRekey(OwnerOf(label), s.name, s.stock_source, s.stock_sha256, official, copyDir);
      nevr_cdn::ScriptClaim c;
      c.ok = o.ok();
      c.reason = o.reason;
      if (c.ok) {
        nevr_map_scripts::InstallMapScriptsRewriter();  // after the first accepted copy, so a run with none adds no rewriter (5.3 item 3)
      } else if (o.reason == "pack_hash") {
        c.pack = nevr_content::ServedPackId();
      }
      return c;
    };
    const nevr_cdn::WithdrawLabelFn withdraw = [](const std::string& label) { nevr_map_scripts::Withdraw(OwnerOf(label)); };

    const nevr_cdn::ScriptRegistration result =
        nevr_cdn::RegisterScriptLabels(manifest, platform, held, claim, withdraw, nevr_cdn::ManifestVerified(manifest));
    LogLabels(manifest, platform, result);
    return result;
  };
}

}  // namespace nevr_cdn_scripts
