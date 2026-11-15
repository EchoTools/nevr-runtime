// The custom-map channel's PC start (docs/adr/0048-custom-maps-channel.md): the core's channel (core/map_channel.h) over the PC's env,
// map file system, stock source, script provider and resource-set registry. Thin: every decision is the core's; this file reads the
// config, names the Win32 seams and installs the hooks the core's decision allows.
#pragma once

#include <cstdint>

namespace nevr_map_channel::pc {

// The boot thread, once (a second call returns at once). With maps.base_url unset the core writes its one Info record and nothing
// else is touched: no hook, no thread, no directory. A dedicated server runs the channel for the group of the session it is allocated for (map_hold.cpp).
void Boot(std::uintptr_t game_base, bool is_server);

// The boot thread, after Boot: the background fetch. A no-op unless the channel runs and the cache root's lock is taken.
void StartFetch();

// Stops the background fetch with a bounded wait. Safe when the channel never ran. Not yet called from the shutdown path (report).
void StopFetch();

}  // namespace nevr_map_channel::pc
