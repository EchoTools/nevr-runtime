/* SHA-256 (FIPS 180-4), streaming. The pin on the game-data zip is one digest of the whole file
 * (docs/adr/0011-quest-game-data-first-run.md), so the downloader feeds every body byte through Update and compares
 * Hex() with the pin. No allocation, no exceptions.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::data {

class Sha256 {
 public:
  Sha256() noexcept { Reset(); }
  void Reset() noexcept;
  void Update(const void* bytes, std::size_t length) noexcept;
  // Finishes the hash and returns 64 lowercase hex characters. The object is Reset afterwards.
  std::string Hex();

 private:
  void Block(const unsigned char* block) noexcept;
  std::uint32_t state_[8];
  std::uint64_t total_bytes_;
  unsigned char buffer_[64];
  std::size_t buffered_;
};

// The digest of `length` bytes, as Hex.
std::string Sha256Hex(const void* bytes, std::size_t length);

}  // namespace nevr_quest::data
