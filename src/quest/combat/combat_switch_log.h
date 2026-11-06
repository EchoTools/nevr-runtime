/* The record of one module switch the install step read (docs/adr/0046 rule 4): `{"event":"combat_config_switch","module":"<m>","key":"combat.<key>",
 * "on":0|1,"source":"default|config"}`, one per switch, written by the module's install function with the value it was handed. Header only.
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat {

inline void LogSwitch(const char* module, const char* key, bool on, bool fromConfig) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config_switch",
                      {{"module", module}, {"key", key}, {"on", on ? 1 : 0}, {"source", fromConfig ? "config" : "default"}});
}

}  // namespace nevr_quest::combat
