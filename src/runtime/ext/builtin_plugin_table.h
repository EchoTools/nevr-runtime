#pragma once

// The external plugin DLLs the loader refuses by name: those whose function nevr-runtime.dll now has built in (with the
// feature group that replaces each) and those it refuses until they are integrated. The loader refuses an entry that names one of these files before LoadLibrary (plugin_loader.cpp,
// SkipBuiltIn): the built-in combat module (runtime/combat, the port of he_is_the_cat's EchoCombat DroneFix,
// PowerBoost and VeilNet at nevr-pack c3ff4b54, see runtime/combat/combat.h) hooks the same game functions, so the
// external copy's prologue check fails ("prologue mismatch") and its second set of hooks fights ours.
//
// PCH-safe on purpose (std only, like plugin_load_plan.h): the loader is compiled with the core PCH.
//
// Identity: the file name is the only identity the loader can read before LoadLibrary. The DLL's own plugin name
// is known only after it is loaded, which is the step this refusal exists to prevent.

#include <array>
#include <cstddef>
#include <string_view>

namespace nevr_builtin_plugin {

/// The reason tokens in the refusal's record and in the login's plugin report.
/// built_in: the runtime already does this plugin's work (the groups in `replaced_by`).
/// superseded: the runtime refuses the plugin until what is wanted from it is integrated; no group replaces it yet.
inline constexpr std::string_view kReasonBuiltIn = "built_in";
inline constexpr std::string_view kReasonSuperseded = "superseded";

struct Entry {
  std::string_view file;         // the external DLL's file name
  std::string_view plugin;       // what the external plugin is called
  std::string_view reason;       // kReasonBuiltIn or kReasonSuperseded
  std::string_view replaced_by;  // the config.yaml `combat` keys of the built-in groups that do its work; empty if none
};

inline constexpr std::size_t kCount = 6;

// Group keys are combat_settings.h's kGroups (config.yaml `combat.<key>`).
inline constexpr std::array<Entry, kCount> kTable = {{
    {"DroneFix.dll", "DroneFix", kReasonBuiltIn, "combat.drone"},
    {"PowerBoost.dll", "PowerBoost", kReasonBuiltIn, "combat.overcharge,combat.medigun"},
    {"VeilNet.dll", "VeilNet", kReasonBuiltIn, "combat.veil,combat.match_control,combat.clean_exit"},
    // The three plugins the launcher marks required: refused the same way until what is wanted from them is built in.
    {"NvrAssetPatches.dll", "NvrAssetPatches", kReasonSuperseded, ""},
    {"NvrXmlHttpFix.dll", "NvrXmlHttpFix", kReasonSuperseded, ""},
    {"NvrGpuRating.dll", "NvrGpuRating", kReasonSuperseded, ""},
}};

inline bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

/// The entry for `file` (a bare file name, compared without case as Windows compares them), or nullptr.
inline const Entry* Find(std::string_view file) {
  for (const Entry& e : kTable) {
    if (EqualNoCase(e.file, file)) return &e;
  }
  return nullptr;
}

}  // namespace nevr_builtin_plugin
