#pragma once

// The plugin DLLs the loader refuses by name: those whose function nevr-runtime.dll now has built in (with the
// feature group that replaces each), the log filter (N89) and those it refuses until they are integrated. The loader
// refuses an entry that names one of these files before LoadLibrary (plugin_loader.cpp, SkipRefusedByName): the built-in combat module (runtime/combat, the port of he_is_the_cat's EchoCombat DroneFix,
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
/// superseded: built-in code replaces this plugin and loading it too would fight that code (the log filter).
/// blocked: the runtime refuses the plugin until what is wanted from it is integrated; no code replaces it yet.
inline constexpr std::string_view kReasonBuiltIn = "built_in";
inline constexpr std::string_view kReasonSuperseded = "superseded";
inline constexpr std::string_view kReasonBlocked = "blocked";

// The log filter's refusal keeps the text the smoke checks and the N89 tests pin (tests/smoke/score-log.sh P05).
inline constexpr std::string_view kLogFilterSkipped =
    "superseded by the built-in log filter. Loading it would install a second MinHook on CLog::PrintfImpl and "
    "silently disable both file logging and max_line_length truncation. Remove it from config.yaml.";
inline constexpr std::string_view kLogFilterError = "superseded by the built-in log filter";

struct Entry {
  std::string_view file;            // the external DLL's file name
  std::string_view plugin;          // what the external plugin is called
  std::string_view reason;          // kReasonBuiltIn, kReasonSuperseded or kReasonBlocked
  std::string_view replaced_by;     // the config.yaml `combat` keys of the built-in groups that do its work; empty if none
  std::string_view skipped_text;    // non-empty: the record is the older "SKIPPED <file> - <text>" line with this text
  std::string_view manifest_error;  // non-empty: the login report's error for the entry; empty: reason[: replaced by ..]
};

inline constexpr std::size_t kCount = 8;

// Group keys are combat_settings.h's kGroups (config.yaml `combat.<key>`).
inline constexpr std::array<Entry, kCount> kTable = {{
    {"DroneFix.dll", "DroneFix", kReasonBuiltIn, "combat.drone", "", ""},
    {"PowerBoost.dll", "PowerBoost", kReasonBuiltIn, "combat.overcharge,combat.medigun", "", ""},
    {"VeilNet.dll", "VeilNet", kReasonBuiltIn, "combat.veil,combat.match_control,combat.clean_exit", "", ""},
    // The three plugins the launcher marks required: refused until what is wanted from them is built in.
    {"NvrAssetPatches.dll", "NvrAssetPatches", kReasonBlocked, "", "", ""},
    {"NvrXmlHttpFix.dll", "NvrXmlHttpFix", kReasonBlocked, "", "", ""},
    {"NvrGpuRating.dll", "NvrGpuRating", kReasonBlocked, "", "", ""},
    // N89: the log filter plugin duplicates the built-in filter.
    {"log_filter.dll", "log_filter", kReasonSuperseded, "", kLogFilterSkipped, kLogFilterError},
    {"log-filter.dll", "log-filter", kReasonSuperseded, "", kLogFilterSkipped, kLogFilterError},
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
