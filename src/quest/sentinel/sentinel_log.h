// Sentinel logging: every line goes to logcat (tag NEVR-Sentinel) and is appended to the on-disk
// log `nevr-sentinel.log` in the app's external files directory. Callers pass key and feature
// names only, never configured values.
#pragma once

#include "quest/sentinel/quest_config.h"

#include <string>

namespace sentinel {

// The app's external files directory (compile-time default, overridable at build time).
const char* FilesDir();

// Writes `message` to logcat and the on-disk log. If the disk log cannot be opened or written the
// failure is reported once to logcat at error level and logcat keeps receiving every line.
void Emit(nevr_quest::LogLevel level, const std::string& message);

}  // namespace sentinel
