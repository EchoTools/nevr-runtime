// What a loader-test fixture plugin (plugin_fixture.cpp) records about the context it was given. The fixture exports one
// `fx_seen`; the test finds it with dlsym on the plugin's own handle.
#pragma once

#include <cstdint>

struct FxSeen {
  std::uint32_t ctor_ran;
  std::uint32_t init_count;
  std::uint32_t shutdown_count;
  std::uint32_t ctx_size;
  std::uint32_t flags;
  std::uintptr_t base_addr;
  const void* net_game;
  std::uint32_t game_state;
  std::uint64_t level_symbol;
  const char* level_name;
  std::int32_t plugin_count_at_init;
  char first_plugin_name[32];
  char plugin_id[64];
  char args[256];
  char game_build_id[64];
  std::int32_t covers_game_build_id;
  const void* quest;
  std::int32_t covers_quest;
};
