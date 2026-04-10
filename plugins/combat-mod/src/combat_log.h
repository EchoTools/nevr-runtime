/*
 * combat_log.h — Logging for the combat-mod plugin.
 *
 * Wraps the shared NEVR_DEFINE_PLUGIN_LOG macro in the combat_mod
 * namespace so all existing combat_mod::PluginLog() call sites work
 * without modification.
 */
#pragma once

#include "plugin_logger.h"

namespace combat_mod {
NEVR_DEFINE_PLUGIN_LOG("[NEVR.COMBAT]")
} // namespace combat_mod
