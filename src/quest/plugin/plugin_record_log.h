// How the loader's records reach the sentinel's log (docs/adr/0019-quest-plugin-loader.md, section 10): one JSON line per
// plan entry, event `plugin_load`, and one `plugin_plan` line for the plan as a whole. Argument keys ride along, argument
// values never do.
#pragma once

#include <cstddef>

#include "quest/plugin/plugin_loader.h"

namespace nevr_quest::plugin {

// Fields: name, file, sha256, plugin_build_id, game_build_id, phase, outcome, reason (`ok` for a loaded plugin), required
// (0 or 1), arg_keys. Info for loaded and disabled, Warn for everything that did not load.
void LogLoadRecord(const LoadRecord& record);

// Fields: entries, early, normal, refused, disabled.
void LogPlanSummary(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                    std::size_t disabled);

}  // namespace nevr_quest::plugin
