// The Quest plugin loader as the sentinel runs it (docs/adr/0019-quest-plugin-loader.md, sections 2 and 3): one loader for
// the process, planned once after the configuration is read, run at its two install moments, and ended when the game goes
// first. Everything here is a thin binding of Loader to the device (SentinelPlatform); the decisions are the loader's and
// are tested on the host (src/quest/tests/plugin_loader_test.cpp).
//
// None of these functions throws and none blocks on anything but the plugin's own code.
#pragma once

#include <cstddef>
#include <vector>

#include "core/nevr_config.h"

namespace nevr_quest::plugin {

struct PlanCounts {
  std::size_t early = 0;   // entries the early pass would load
  std::size_t normal = 0;  // pending entries that wait for the first post-load call
};

// Builds the plan from the file's `plugins:` list and writes the records the plan alone settled (disabled, refused) and the
// plan line. Runs no plugin code. `devBuild`: a development build (a plugin's DEBUG lines are logged). The first call plans;
// a later one changes nothing and returns the counts as they stand. `configRejected`: config.yaml was rejected as a whole, so
// `specs` is empty and every packaged plugin loads with its manifest values; each plugin_load record says so.
PlanCounts PlanPlugins(const std::vector<nevr::PluginSpec>& specs, bool devBuild, bool configRejected = false) noexcept;

// Moment (a): loads the `early` entries. Runs once, after the reporter has started.
void RunEarlyPlugins() noexcept;

// Moment (b): loads every entry still pending. Runs once, at the first post-load call.
void RunNormalPlugins() noexcept;

// Writes the game's main level into the contexts the loaders already handed out (Loader::RefreshMainLevel, #1106). Called once
// per NetGame::Update from the frame dispatcher, so it never waits: when a pass holds the loader this frame does nothing.
void RefreshMainLevel() noexcept;

// Ends every entry still pending as `not_started`, with `reasonToken` (a plugin_plan.h reason token). Safe to call more than
// once and with nothing planned.
void FinishPlugins(const char* reasonToken) noexcept;

// Forgets the loader (the host test starts each case clean). Not for production: a loaded plugin's code stays mapped.
void ResetPluginsForTest() noexcept;

}  // namespace nevr_quest::plugin
