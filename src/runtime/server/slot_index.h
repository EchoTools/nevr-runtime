#pragma once

// The player slot and generation a broadcaster message names in its first four bytes (low 16 bits: slot, high 16 bits:
// generation), read by the SaveLoadout and entrant handlers in gameserver_callbacks.cpp. Header-only and free of the
// game, so a test can hold it to the layout.

#include <cstdint>
#include <cstring>

#include "runtime/server/constants.h"

// Slot index extraction from message payloads
struct SlotInfo {
  uint16_t slot;
  uint16_t genId;
};

// Extract slot index from message payload. A null message, or one shorter than the four bytes, names slot 0 generation 0.
inline SlotInfo ExtractSlotIndex(const void* msg, uint64_t msgSize) {
  SlotInfo info = {0, 0};
  if (!msg || msgSize < sizeof(uint32_t)) return info;

  uint32_t packed = 0;
  std::memcpy(&packed, msg, sizeof(uint32_t));

  info.slot = static_cast<uint16_t>(packed & nevr_game_server::SLOT_INDEX_MASK);
  info.genId = static_cast<uint16_t>((packed >> nevr_game_server::SLOT_GEN_SHIFT) & nevr_game_server::SLOT_INDEX_MASK);
  return info;
}
