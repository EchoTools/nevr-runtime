#pragma once
// Forwarding shim. NOT for new code. See hooking.h beside this file.
//
// src/legacy/gamepatches/plugin_loader.h:3 spells
// `#include "common/nevr_plugin_interface.h"`. The header is now
// src/extension/plugin_interface.h — the `nevr_` prefix was redundant once the
// directory said `extension`.
#include "extension/plugin_interface.h"
