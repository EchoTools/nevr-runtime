#pragma once
/**
 * @file memory_poller.h
 * @brief Memory-based data source that reads directly from game memory
 *
 * PLACEHOLDER IMPLEMENTATION
 *
 * This is a placeholder for future implementation that will read game state
 * directly from memory by following pointer chains. This approach will be
 * more efficient than HTTP polling as it avoids network overhead.
 *
 * The memory addresses and pointer offsets are currently placeholders and
 * will need to be determined through reverse engineering of the game binary.
 */

#ifndef TELEMETRY_MEMORY_POLLER_H
#define TELEMETRY_MEMORY_POLLER_H

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "engine/v1/engine_http.pb.h"
#include "data_source.h"

namespace TelemetryAgent {

/**
 * @struct MemoryOffsets
 * @brief Placeholder structure for memory pointer chains
 *
 * These offsets will be used to navigate from the game's base address
 * to the relevant data structures in memory.
 */
struct MemoryOffsets {
  static constexpr uintptr_t GAME_CONTEXT_OFFSET = 0x20a0478;
  static constexpr uintptr_t LOBBY_OFFSET = 0x18;
  static constexpr uintptr_t GAMESTATE_OFFSET = 0x20;
  static constexpr uintptr_t PLAYER_ARRAY_OFFSET = 0x178;
  static constexpr uintptr_t PLAYER_STRIDE = 0x250;
  static constexpr uintptr_t PLAYER_COUNT_OFFSET = 0xe2;
  static constexpr uintptr_t PLAYER_STATS_BASE = 0x72ac;
  static constexpr uintptr_t PLAYER_STATS_STRIDE = 0x4d08;

  uintptr_t gameContextBase = GAME_CONTEXT_OFFSET;
  uintptr_t lobbyOffset = LOBBY_OFFSET;
  uintptr_t gameStateOffset = GAMESTATE_OFFSET;
  uintptr_t playerDataOffset = PLAYER_ARRAY_OFFSET;
};

/**
 * @class MemoryPoller
 * @brief Reads game state directly from memory (PLACEHOLDER)
 *
 * This implementation is a placeholder that demonstrates the interface
 * for direct memory access. The actual memory addresses and structures
 * will need to be reverse engineered from the game binary.
 *
 * Benefits over HTTP polling:
 * - No network overhead
 * - Lower latency
 * - Can access data not exposed via HTTP API
 * - More efficient for high-frequency polling
 */
class MemoryPoller : public IDataSource {
 public:
  /**
   * @brief Construct a memory poller
   * @param gameBaseAddress Base address of the game module (echovr.exe)
   */
  explicit MemoryPoller(uintptr_t gameBaseAddress);

  ~MemoryPoller() override;

  // Disable copy
  MemoryPoller(const MemoryPoller&) = delete;
  MemoryPoller& operator=(const MemoryPoller&) = delete;

  // IDataSource interface
  bool Start() override;
  void Stop() override;
  bool IsActive() const override;
  bool GetFrameData(FrameData& data) override;
  std::string GetSessionUUID() const override;
  const char* GetName() const override { return "Memory"; }

  /**
   * @brief Set custom memory offsets
   * @param offsets The memory offset configuration
   */
  void SetOffsets(const MemoryOffsets& offsets);

  /**
   * @brief Check if memory locations are accessible
   * @return true if memory can be read
   */
  bool ValidateMemoryAccess() const;

 private:
  std::string ReadString(uintptr_t address, size_t maxLength) const;

  std::string ReadGUID(uintptr_t address) const;

  bool PopulateSessionResponse(engine::v1::SessionResponse* response) const;

  bool PopulatePlayerBonesResponse(engine::v1::PlayerBonesResponse* response) const;

  bool IsMemoryReadable(uintptr_t address, size_t size) const;

 private:
  uintptr_t m_gameBaseAddress;
  MemoryOffsets m_offsets;
  std::atomic<bool> m_active;
  mutable std::mutex m_mutex;

  mutable std::string m_cachedSessionUUID;
  uint64_t m_frameIndex;
};

}  // namespace TelemetryAgent

#endif  // TELEMETRY_MEMORY_POLLER_H
