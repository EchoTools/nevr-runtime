#pragma once
#ifndef GAMESERVER_CONSTANTS_H
#define GAMESERVER_CONSTANTS_H

#include <cstdint>

namespace GameServer {

// Player slot limits
constexpr uint32_t MAX_PLAYER_SLOTS = 8;

// DelegateProxy marker for invalid/unused method slot
constexpr uint64_t DELEGATE_PROXY_INVALID_METHOD = 0xFFFFFFFFFFFFFFFF;

// Slot index extraction masks
constexpr uint32_t SLOT_INDEX_MASK = 0xFFFF;
constexpr uint32_t SLOT_GEN_SHIFT = 16;

// Minimum message sizes for validation
constexpr uint64_t MIN_LOADOUT_MSG_SIZE = 4;

// ServerDB symbol for websocket service
constexpr int64_t SYMBOL_SERVERDB = 0x25E886012CED8064;

}  // namespace GameServer

#endif  // GAMESERVER_CONSTANTS_H
