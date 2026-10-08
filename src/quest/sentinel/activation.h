// Process-wide Quest configuration and feature activation, resolved once from the embedded
// defaults and the optional `nevr-quest.json`. Every resolved state is logged.
#pragma once

#include "quest/sentinel/quest_config.h"

#include <string>

namespace sentinel {

// Resolves and logs the configuration. Idempotent and thread-safe.
void InitActivation();

// The resolved configuration (initialises on first use).
const nevr_quest::ResolvedConfig& ActiveConfig();

// True only when the feature is on in the file and its prerequisites hold.
bool FeatureEnabled(nevr_quest::Feature feature);

enum class ReadStatus { kRead, kAbsent, kNotRegularFile, kError };

// Reads the config file without blocking: the open is non-blocking and the target must be a
// regular file (a FIFO, directory or device at the path is kNotRegularFile and is never read).
// At most kMaxConfigBytes + 1 bytes are read. `*err` is set for kAbsent and kError.
ReadStatus ReadConfigFile(const std::string& path, std::string* out, int* err);

// Reads `path`, resolves against the embedded defaults and logs every state. Not cached; this
// is what InitActivation runs once.
nevr_quest::ResolvedConfig ResolveFromDisk(const std::string& path);

}  // namespace sentinel
