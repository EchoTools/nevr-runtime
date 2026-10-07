// Process-wide Quest configuration and feature activation, resolved once from the embedded
// defaults and the optional `nevr-quest.json`. Every resolved state is logged.
#pragma once

#include "quest/sentinel/quest_config.h"

namespace sentinel {

// Resolves and logs the configuration. Idempotent and thread-safe.
void InitActivation();

// The resolved configuration (initialises on first use).
const nevr_quest::ResolvedConfig& ActiveConfig();

// True only when the feature is on in the file and its prerequisites hold.
bool FeatureEnabled(nevr_quest::Feature feature);

}  // namespace sentinel
