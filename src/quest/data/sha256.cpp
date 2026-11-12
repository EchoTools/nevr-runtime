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

// FIPS 180-4: a message is processed in 512-bit blocks of 64 rounds, and the schedule is words of 32 bits.
constexpr std::size_t kBlockBytes = 64;
constexpr std::size_t kRounds = 64;
constexpr std::size_t kBlockWords = kBlockBytes / sizeof(std::uint32_t);
constexpr std::size_t kStateWords = 8;
constexpr unsigned kWordBits = 32;
constexpr unsigned kBitsPerByte = 8;
// The message length is appended as a 64-bit big-endian count of bits, after a 0x80 byte and zeros up to the last eight bytes of a block.
constexpr std::size_t kLengthBytes = 8;
constexpr std::size_t kPaddingLimit = kBlockBytes - kLengthBytes;
constexpr unsigned char kPaddingMarker = 0x80;
constexpr std::size_t kDigestBytes = 32;
constexpr unsigned kHexDigitBits = 4;
constexpr unsigned kHexDigitMask = 0xf;
constexpr std::size_t kHexCharsPerByte = 2;
// A 128-bit NEON vector holds four of those words, so one SHA256H / SHA256H2 pair does four rounds.
constexpr std::size_t kVectorBytes = 16;
constexpr std::size_t kWordsPerVector = kVectorBytes / sizeof(std::uint32_t);
constexpr std::size_t kBlockVectors = kBlockBytes / kVectorBytes;
constexpr std::size_t kScheduleVectors = kRounds / kWordsPerVector;
// Vector i of the schedule is SHA256SU1(SHA256SU0(vector i-4, vector i-3), vector i-2, vector i-1) (the ARMv8 Cryptography Extensions).
constexpr std::size_t kSu0First = kBlockVectors;
constexpr std::size_t kSu0Second = kBlockVectors - 1;
constexpr std::size_t kSu1First = 2;
constexpr std::size_t kSu1Second = 1;
// The Linux hardware capability bit for the aarch64 SHA-2 instructions (HWCAP_SHA2, arch/arm64/include/uapi/asm/hwcap.h).
constexpr unsigned kHwcapSha2Bit = 6;
constexpr unsigned long kHwcapSha2 = 1ul << kHwcapSha2Bit;
// FIPS 180-4 section 4.1.2: the rotation and shift amounts of the functions sigma0, sigma1 (the schedule) and Sigma0, Sigma1 (the rounds).
constexpr unsigned kSigma0Rotate1 = 7, kSigma0Rotate2 = 18, kSigma0Shift = 3;
constexpr unsigned kSigma1Rotate1 = 17, kSigma1Rotate2 = 19, kSigma1Shift = 10;
constexpr unsigned kBigSigma1Rotate1 = 6, kBigSigma1Rotate2 = 11, kBigSigma1Rotate3 = 25;
constexpr unsigned kBigSigma0Rotate1 = 2, kBigSigma0Rotate2 = 13, kBigSigma0Rotate3 = 22;
// The schedule reaches back 15, 2, 16 and 7 words (FIPS 180-4 section 6.2.2).
constexpr std::size_t kScheduleBack15 = 15, kScheduleBack2 = 2, kScheduleBack16 = 16, kScheduleBack7 = 7;
// The eight working variables a to h of FIPS 180-4 section 6.2.2, by their place in the state.
enum : std::size_t { kA, kB, kC, kD, kE, kF, kG, kH };
// The initial hash value (FIPS 180-4 section 5.3.3).
constexpr std::uint32_t kInitialState[kStateWords] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::uint32_t kRound[kRounds] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t Rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (kWordBits - n)); }

// The portable compression function: one 64-byte block into the eight state words.
void ScalarBlock(std::uint32_t* state, const unsigned char* block) noexcept {
  std::uint32_t w[kRounds];
  for (std::size_t i = 0; i < kBlockWords; ++i) {
    const unsigned char* word = block + sizeof(std::uint32_t) * i;
    std::uint32_t value = 0;
    for (std::size_t k = 0; k < sizeof(std::uint32_t); ++k) value = (value << kBitsPerByte) | word[k];  // big endian
    w[i] = value;
  }
  for (std::size_t i = kBlockWords; i < kRounds; ++i) {
    const std::uint32_t s0 = Rotr(w[i - kScheduleBack15], kSigma0Rotate1) ^ Rotr(w[i - kScheduleBack15], kSigma0Rotate2) ^ (w[i - kScheduleBack15] >> kSigma0Shift);
    const std::uint32_t s1 = Rotr(w[i - kScheduleBack2], kSigma1Rotate1) ^ Rotr(w[i - kScheduleBack2], kSigma1Rotate2) ^ (w[i - kScheduleBack2] >> kSigma1Shift);
    w[i] = w[i - kScheduleBack16] + s0 + w[i - kScheduleBack7] + s1;
  }
  std::uint32_t a = state[kA], b = state[kB], c = state[kC], d = state[kD];
  std::uint32_t e = state[kE], f = state[kF], g = state[kG], h = state[kH];
  for (std::size_t i = 0; i < kRounds; ++i) {
    const std::uint32_t big1 = Rotr(e, kBigSigma1Rotate1) ^ Rotr(e, kBigSigma1Rotate2) ^ Rotr(e, kBigSigma1Rotate3);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + big1 + ch + kRound[i] + w[i];
    const std::uint32_t big0 = Rotr(a, kBigSigma0Rotate1) ^ Rotr(a, kBigSigma0Rotate2) ^ Rotr(a, kBigSigma0Rotate3);
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
  state[kA] += a;
  state[kB] += b;
  state[kC] += c;
  state[kD] += d;
  state[kE] += e;
  state[kF] += f;
  state[kG] += g;
  state[kH] += h;
}

void ScalarBlocks(std::uint32_t* state, const unsigned char* p, std::size_t count) noexcept {
  for (; count != 0; --count, p += kBlockBytes) ScalarBlock(state, p);
}

#if defined(NEVR_SHA256_ARM64)
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
  uint32x4_t efgh = vld1q_u32(&state[kWordsPerVector]);
  for (; count != 0; --count, data += kBlockBytes) {
    const uint32x4_t abcdSave = abcd, efghSave = efgh;
    uint32x4_t w[kScheduleVectors];
    for (std::size_t i = 0; i < kBlockVectors; ++i) w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + kVectorBytes * i)));  // the block's big-endian words
    for (std::size_t i = kBlockVectors; i < kScheduleVectors; ++i) w[i] = vsha256su1q_u32(vsha256su0q_u32(w[i - kSu0First], w[i - kSu0Second]), w[i - kSu1First], w[i - kSu1Second]);
    for (std::size_t i = 0; i < kScheduleVectors; ++i) {
      const uint32x4_t wk = vaddq_u32(w[i], vld1q_u32(&kRound[kWordsPerVector * i]));
      const uint32x4_t before = abcd;
      abcd = vsha256hq_u32(abcd, efgh, wk);
      efgh = vsha256h2q_u32(efgh, before, wk);
    }
    abcd = vaddq_u32(abcd, abcdSave);
    efgh = vaddq_u32(efgh, efghSave);
  }
  vst1q_u32(&state[0], abcd);
  vst1q_u32(&state[kWordsPerVector], efgh);
}

bool CpuHasSha2() noexcept {
  static const bool has = (getauxval(AT_HWCAP) & kHwcapSha2) != 0;
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
  std::memcpy(state_, kInitialState, sizeof(state_));
  total_bytes_ = 0;
  buffered_ = 0;
}

void Sha256::Blocks(const unsigned char* blocks, std::size_t count) noexcept { CurrentBackend()(state_, blocks, count); }

void Sha256::Update(const void* bytes, std::size_t length) noexcept {
  const unsigned char* p = static_cast<const unsigned char*>(bytes);
  total_bytes_ += length;
  if (buffered_ != 0) {
    const std::size_t take = (kBlockBytes - buffered_) < length ? (kBlockBytes - buffered_) : length;
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take;
    p += take;
    length -= take;
    if (buffered_ < kBlockBytes) return;
    Blocks(buffer_, 1);
    buffered_ = 0;
  }
  if (length >= kBlockBytes) {
    const std::size_t whole = length / kBlockBytes;
    Blocks(p, whole);
    p += whole * kBlockBytes;
    length -= whole * kBlockBytes;
  }
  if (length != 0) {
    std::memcpy(buffer_, p, length);
    buffered_ = length;
  }
}

std::string Sha256::Hex() {
  const std::uint64_t bits = total_bytes_ * kBitsPerByte;
  unsigned char pad[kBlockBytes + kLengthBytes] = {kPaddingMarker};
  const std::size_t pad_length = (buffered_ < kPaddingLimit) ? (kPaddingLimit - buffered_) : (kBlockBytes + kPaddingLimit - buffered_);
  for (std::size_t i = 0; i < kLengthBytes; ++i) {
    pad[pad_length + i] = static_cast<unsigned char>(bits >> (kBitsPerByte * (kLengthBytes - 1 - i)));
  }
  Update(pad, pad_length + kLengthBytes);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(kDigestBytes * kHexCharsPerByte, '0');
  for (std::size_t i = 0; i < kStateWords; ++i) {
    for (std::size_t j = 0; j < sizeof(std::uint32_t); ++j) {
      const unsigned char byte = static_cast<unsigned char>(state_[i] >> (kBitsPerByte * (sizeof(std::uint32_t) - 1 - j)));
      const std::size_t at = (i * sizeof(std::uint32_t) + j) * kHexCharsPerByte;
      out[at] = kHex[byte >> kHexDigitBits];
      out[at + 1] = kHex[byte & kHexDigitMask];
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
