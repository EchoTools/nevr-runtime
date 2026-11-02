// NRadEngine::CParticleEffectCS::EffectTime
// Quest libr15.so VA 0x17647e4; ReVault binary 576.
// ReVault raw parent 100247105, reconstruction node 100600470; grade H: all 14 instructions and both return paths (cbz taken and not taken) match, and the stored size 56 ends at the final ret.
// @0x17647e4 — NRadEngine::CParticleEffectCS::EffectTime
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CParticleEffectCS {
 public:
  // SCompactPoolHandle arrives as one 64-bit register (x1); only its low 16 bits are read.
  float EffectTime(std::uint64_t handle) const;
};

}  // namespace NRadEngine

float NRadEngine::CParticleEffectCS::EffectTime(std::uint64_t handle) const {
  const auto* object = reinterpret_cast<const std::byte*>(this);

  std::uintptr_t index_table = 0;
  std::memcpy(&index_table, object + 0xc8, sizeof(index_table));
  const auto index_offset = static_cast<std::size_t>(handle & 0xffffu) * 4;
  std::uint16_t slot = 0;
  std::memcpy(&slot, reinterpret_cast<const std::byte*>(index_table) + index_offset, sizeof(slot));

  std::uintptr_t entries = 0;
  std::memcpy(&entries, object + 0xf8, sizeof(entries));
  const auto* entry = reinterpret_cast<const std::byte*>(entries) + static_cast<std::size_t>(slot) * 0x18;

  std::uintptr_t instance = 0;
  std::memcpy(&instance, entry + 0x10, sizeof(instance));
  if (instance == 0) {
    return 0.0f;
  }

  float effect_time = 0.0f;
  std::memcpy(&effect_time, reinterpret_cast<const std::byte*>(instance) + 0x340, sizeof(effect_time));
  return effect_time;
}
