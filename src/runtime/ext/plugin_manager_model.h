// plugin_manager_model.h — what the plugin manager lists (#1857).
//
// PURE and PCH-safe on purpose, like plugin_manifest.h: std types and the loader header's constants only, no
// windows.h, no Log, no globals, no I/O. It turns the loader's report (PluginManifestEntry: one per entry of
// config.yaml's `plugins:` list, in list order, disabled and failed entries included) into the rows the manager shows.
// Nothing in the runtime calls it yet; the page that shows the rows is a separate change.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/ext/plugin_loader.h"    // CapsLoadPriority
#include "runtime/ext/plugin_manifest.h"  // PluginManifestEntry

namespace nevr_pluginmgr {

enum class RowState : std::uint8_t {
  Loaded,    // enabled and loaded
  Disabled,  // `enabled: false`: a choice, not a failure
  Failed,    // enabled, did not load; `reason` is what the loader recorded
  Pending,   // enabled, not loaded, no reason recorded: the loader has not got to it
};

// `Row::load_band` of a row whose plugin is not loaded: the band comes from the capabilities the plugin declares,
// which the report carries only for a loaded plugin.
inline constexpr int kLoadBandUnknown = -1;

struct Row {
  std::string name;  // the config.yaml entry's `name`
  std::string file;  // the dll filename the loader looks for in plugins/
  RowState state = RowState::Pending;
  std::string reason;  // the report's error for a Failed row; empty for every other state
  bool required = false;
  // The loader's sort key for a Loaded row (CapsLoadPriority of its declared capabilities; lower loads first, config
  // order is kept inside a band), else kLoadBandUnknown.
  int load_band = kLoadBandUnknown;
};

// One row per entry of `entries`, in the same order. A disabled entry is Disabled whatever its error field holds, and
// a loaded one is Loaded; an enabled entry that did not load is Failed when the report gives a reason and Pending when
// it does not.
inline std::vector<Row> BuildRows(const std::vector<PluginManifestEntry>& entries) {
  std::vector<Row> rows;
  rows.reserve(entries.size());
  for (const PluginManifestEntry& entry : entries) {
    Row row;
    row.name = entry.name;
    row.file = entry.file;
    row.required = entry.required;
    if (!entry.enabled) {
      row.state = RowState::Disabled;
    } else if (entry.loaded) {
      row.state = RowState::Loaded;
      row.load_band = CapsLoadPriority(entry.capabilities);
    } else if (!entry.error.empty()) {
      row.state = RowState::Failed;
      row.reason = entry.error;
    } else {
      row.state = RowState::Pending;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace nevr_pluginmgr
