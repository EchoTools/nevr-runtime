#pragma once
// Forwarding shim. NOT for new code. See hooking.h beside this file.
//
// src/legacy/gamepatches/plugin_loader.h:3 spells
// `#include "common/nevr_plugin_interface.h"`. The header is now
// sdk/include/nevr/plugin_interface.h, the plugin SDK. The `nevr_` prefix was redundant once the
// directory said `nevr`.
#include "nevr/plugin_interface.h"
