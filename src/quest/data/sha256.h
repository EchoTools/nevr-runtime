/* SHA-256 (FIPS 180-4), streaming. On aarch64 Linux and Android the compression function is the CPU's own SHA-2 instructions when
 * /proc's hardware capabilities (getauxval AT_HWCAP, HWCAP_SHA2) say it has them, and the portable code below otherwise; both give
 * the same digest (sha256_backend_test). The cargo objects are hashed once, while they download, and this is the hash.
 The pin on the game-data zip is one digest of the whole file
 * (docs/adr/0011-quest-game-data-first-run.md), so the downloader feeds every body byte through Update and compares
 * Hex() with the pin. No allocation, no exceptions.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::data {

// The code that runs the compression function. The default (kAuto) is the aarch64 SHA-2 instructions when the CPU has them and the
// scalar code otherwise; the digest is the same either way. The selection is process-wide and is for tests and diagnostics.
enum class Sha256Backend { kAuto, kScalar, kArm64Sha2 };
bool Sha256HardwareAvailable() noexcept;                  // the build has the aarch64 code and this CPU has the instructions
const char* Sha256BackendName() noexcept;                 // "scalar" or "arm64-sha2": the one in use
bool Sha256SelectBackend(Sha256Backend backend) noexcept; // false (and nothing changes) when the backend is not available

class Sha256 {
 public:
  Sha256() noexcept { Reset(); }
  void Reset() noexcept;
  void Update(const void* bytes, std::size_t length) noexcept;
  // Finishes the hash and returns 64 lowercase hex characters. The object is Reset afterwards.
  std::string Hex();

 private:
  void Blocks(const unsigned char* blocks, std::size_t count) noexcept;
  std::uint32_t state_[8];
  std::uint64_t total_bytes_;
  unsigned char buffer_[64];
  std::size_t buffered_;
};

// The digest of `length` bytes, as Hex.
std::string Sha256Hex(const void* bytes, std::size_t length);

}  // namespace nevr_quest::data
