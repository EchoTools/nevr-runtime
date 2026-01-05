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
  // Base offset from game module to session data
  // PLACEHOLDER: Replace with actual offset once determined
  uintptr_t sessionBase = 0x020A00E8;

  // Offset chain to reach session UUID
  // PLACEHOLDER: Replace with actual pointer chain
  std::vector<uintptr_t> sessionUUIDChain = {0x0, 0x978};

  // Offset chain to reach game status string
  // PLACEHOLDER: Replace with actual pointer chain
  std::vector<uintptr_t> gameStatusChain = {0x0, 0x100};

  // Offset chain to reach player data array
  // PLACEHOLDER: Replace with actual pointer chain
  std::vector<uintptr_t> playerDataChain = {0x0, 0x200};

  // Offset chain to reach disc data
  // PLACEHOLDER: Replace with actual pointer chain
  std::vector<uintptr_t> discDataChain = {0x0, 0x300};

  // Offset chain to reach team data
  // PLACEHOLDER: Replace with actual pointer chain
  std::vector<uintptr_t> teamDataChain = {0x0, 0x400};

  // Offset to player bones array
  // PLACEHOLDER: Replace with actual offset
  std::vector<uintptr_t> playerBonesChain = {0x0, 0x500};
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
  /**
   * @brief Follow a pointer chain from a base address
   * @param baseOffset Initial offset from game base
   * @param chain Vector of offsets to follow
   * @return Final address, or 0 if invalid
   */
  uintptr_t FollowPointerChain(uintptr_t baseOffset, const std::vector<uintptr_t>& chain) const;

  /**
   * @brief Read a string from memory
   * @param address Memory address to read from
   * @param maxLength Maximum string length
   * @return The string value, or empty if invalid
   */
  std::string ReadString(uintptr_t address, size_t maxLength) const;

  /**
   * @brief Read a GUID from memory
   * @param address Memory address to read from
   * @return GUID as string, or empty if invalid
   */
  std::string ReadGUID(uintptr_t address) const;

  /**
   * @brief Serialize current session state to JSON
   * @return JSON string representation of session data
   */
  std::string SerializeSessionToJson() const;

  /**
   * @brief Serialize player bones to JSON
   * @return JSON string representation of player bones
   */
  std::string SerializePlayerBonesToJson() const;

  /**
   * @brief Check if a memory address is readable
   * @param address The address to check
   * @param size Number of bytes to check
   * @return true if the memory is readable
   */
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
