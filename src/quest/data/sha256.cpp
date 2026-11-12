#include "quest/data/sha256.h"

#include <atomic>
#include <cstring>

#if defined(__aarch64__) && (defined(__linux__) || defined(__ANDROID__))
#include <arm_neon.h>
#include <sys/auxv.h>
#define NEVR_SHA256_ARM64 1
#endif

namespace nevr_quest::data {

namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t Rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }

// The portable compression function: one 64-byte block into the eight state words.
void ScalarBlock(std::uint32_t* state, const unsigned char* block) noexcept {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
           (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t big1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + big1 + ch + kRound[i] + w[i];
    const std::uint32_t big0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = big0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void ScalarBlocks(std::uint32_t* state, const unsigned char* p, std::size_t count) noexcept {
  for (; count != 0; --count, p += 64) ScalarBlock(state, p);
}

#if defined(NEVR_SHA256_ARM64)
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
// The target attribute turns the instructions on for this function alone, so the rest of the library still runs on any aarch64 CPU.
#if defined(__clang__)
#define NEVR_SHA2_TARGET __attribute__((target("sha2")))
#else
#define NEVR_SHA2_TARGET __attribute__((target("+sha2")))
#endif

// The compression function on the SHA-2 instructions (ARMv8 Cryptography Extensions): the message schedule is SHA256SU0 and SHA256SU1
// four words at a time, and each SHA256H / SHA256H2 pair is four rounds. The state is held as ABCD and EFGH.
NEVR_SHA2_TARGET void Arm64Blocks(std::uint32_t* state, const unsigned char* data, std::size_t count) noexcept {
  uint32x4_t abcd = vld1q_u32(&state[0]);
  uint32x4_t efgh = vld1q_u32(&state[4]);
  for (; count != 0; --count, data += 64) {
    const uint32x4_t abcdSave = abcd, efghSave = efgh;
    uint32x4_t w[16];
    for (int i = 0; i < 4; ++i) w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));  // the block's big-endian words
    for (int i = 4; i < 16; ++i) w[i] = vsha256su1q_u32(vsha256su0q_u32(w[i - 4], w[i - 3]), w[i - 2], w[i - 1]);
    for (int i = 0; i < 16; ++i) {
      const uint32x4_t wk = vaddq_u32(w[i], vld1q_u32(&kRound[4 * i]));
      const uint32x4_t before = abcd;
      abcd = vsha256hq_u32(abcd, efgh, wk);
      efgh = vsha256h2q_u32(efgh, before, wk);
    }
    abcd = vaddq_u32(abcd, abcdSave);
    efgh = vaddq_u32(efgh, efghSave);
  }
  vst1q_u32(&state[0], abcd);
  vst1q_u32(&state[4], efgh);
}

bool CpuHasSha2() noexcept {
  static const bool has = (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
  return has;
}
#endif

using BlocksFn = void (*)(std::uint32_t*, const unsigned char*, std::size_t) noexcept;
std::atomic<BlocksFn> g_blocks{nullptr};  // null until the first use or selection

BlocksFn AutoBackend() noexcept {
#if defined(NEVR_SHA256_ARM64)
  if (CpuHasSha2()) return &Arm64Blocks;
#endif
  return &ScalarBlocks;
}

BlocksFn CurrentBackend() noexcept {
  BlocksFn f = g_blocks.load(std::memory_order_relaxed);
  if (f == nullptr) {
    f = AutoBackend();
    g_blocks.store(f, std::memory_order_relaxed);
  }
  return f;
}

}  // namespace

bool Sha256HardwareAvailable() noexcept {
#if defined(NEVR_SHA256_ARM64)
  return CpuHasSha2();
#else
  return false;
#endif
}

const char* Sha256BackendName() noexcept {
#if defined(NEVR_SHA256_ARM64)
  if (CurrentBackend() == &Arm64Blocks) return "arm64-sha2";
#endif
  return "scalar";
}

bool Sha256SelectBackend(Sha256Backend backend) noexcept {
  switch (backend) {
    case Sha256Backend::kAuto:
      g_blocks.store(AutoBackend(), std::memory_order_relaxed);
      return true;
    case Sha256Backend::kScalar:
      g_blocks.store(&ScalarBlocks, std::memory_order_relaxed);
      return true;
    case Sha256Backend::kArm64Sha2:
#if defined(NEVR_SHA256_ARM64)
      if (CpuHasSha2()) {
        g_blocks.store(&Arm64Blocks, std::memory_order_relaxed);
        return true;
      }
#endif
      return false;
  }
  return false;
}

void Sha256::Reset() noexcept {
  static constexpr std::uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(state_, kInit, sizeof(state_));
  total_bytes_ = 0;
  buffered_ = 0;
}

void Sha256::Blocks(const unsigned char* blocks, std::size_t count) noexcept { CurrentBackend()(state_, blocks, count); }

void Sha256::Update(const void* bytes, std::size_t length) noexcept {
  const unsigned char* p = static_cast<const unsigned char*>(bytes);
  total_bytes_ += length;
  if (buffered_ != 0) {
    const std::size_t take = (64 - buffered_) < length ? (64 - buffered_) : length;
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take;
    p += take;
    length -= take;
    if (buffered_ < 64) return;
    Blocks(buffer_, 1);
    buffered_ = 0;
  }
  if (length >= 64) {
    const std::size_t whole = length / 64;
    Blocks(p, whole);
    p += whole * 64;
    length -= whole * 64;
  }
  if (length != 0) {
    std::memcpy(buffer_, p, length);
    buffered_ = length;
  }
}

std::string Sha256::Hex() {
  const std::uint64_t bits = total_bytes_ * 8;
  unsigned char pad[72] = {0x80};
  const std::size_t pad_length = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  for (int i = 0; i < 8; ++i) pad[pad_length + static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (56 - 8 * i));
  Update(pad, pad_length + 8);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 4; ++j) {
      const unsigned char byte = static_cast<unsigned char>(state_[i] >> (24 - 8 * j));
      out[static_cast<std::size_t>(i * 8 + j * 2)] = kHex[byte >> 4];
      out[static_cast<std::size_t>(i * 8 + j * 2 + 1)] = kHex[byte & 15];
    }
  }
  Reset();
  return out;
}

std::string Sha256Hex(const void* bytes, std::size_t length) {
  Sha256 h;
  h.Update(bytes, length);
  return h.Hex();
}

}  // namespace nevr_quest::data
