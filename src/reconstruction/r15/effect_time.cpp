// Game function: NRadEngine::CParticleEffectCS::EffectTime
// Quest libr15.so (ReVault binary 576) RVA: 0x16647e4 (ReVault VA 0x17647e4)
// PC echovr.exe RVA: not located in the project-wide ReVault search
// ReVault node: 100600146; grade: reconstructed (H)
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CParticleEffectCS {
 public:
  float EffectTime(std::uint64_t handle) const;
};

// @0x17647e4 — NRadEngine::CParticleEffectCS::EffectTime [Quest: confirmed]
/* @confidence: H */
float CParticleEffectCS::EffectTime(std::uint64_t handle) const {
  const auto* object = reinterpret_cast<const std::byte*>(this);
  std::uintptr_t index_table;
  std::uintptr_t entries;
  std::memcpy(&index_table, object + 0xc8, sizeof(index_table));
  std::memcpy(&entries, object + 0xf8, sizeof(entries));

  const auto slot_offset = static_cast<std::size_t>(handle & 0xffffu) * 4;
  std::uint16_t slot;
  std::memcpy(&slot, reinterpret_cast<const std::byte*>(index_table) + slot_offset, sizeof(slot));

  const auto* entry = reinterpret_cast<const std::byte*>(entries) + static_cast<std::size_t>(slot) * 0x18;
  std::uintptr_t instance;
  std::memcpy(&instance, entry + 0x10, sizeof(instance));
  if (instance == 0) {
    return 0.0f;
  }

  float effect_time;
  std::memcpy(&effect_time, reinterpret_cast<const std::byte*>(instance) + 0x340, sizeof(effect_time));
  return effect_time;
}

}  // namespace NRadEngine
