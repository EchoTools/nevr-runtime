#pragma once

// The per-install fields of a GameServerRegistration (#990): the levels this install can load, read from its
// package manifests, and the regions and guilds the nevr_regions / nevr_guilds config keys set (the same values
// the ServerDB URL params carry).

#include "runtime/server/registration_envelope.h"

namespace nevr_game_server {

// Fills params.installedLevels, params.regions and params.guilds. Reads the manifests of the install this
// process runs from (the directory of the game's exe), so it runs on every registration, including a reconnect.
// A manifest that cannot be read is logged and skipped; the other fields are still filled.
void FillRegistrationMetadata(RegistrationParams& params);

}  // namespace nevr_game_server
