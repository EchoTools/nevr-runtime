// NRadEngine::CJson::CJson
// Quest libr15.so VA 0x10a2250; ReVault binary 576.
// ReVault raw parent 100208537, reconstruction node 100600558; grade H: both instructions are read and the two zero stores at +0x00 and +0x08 are the whole body; the 16-byte object is the two-word layout cjson_int.cpp records.
// @0x10a2250 — NRadEngine::CJson::CJson
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CJson {
 public:
  CJson();

 private:
  // Two words: json_t* root at +0x00 and the path cache pointer at +0x08 (see cjson_int.cpp).
  alignas(8) std::byte raw[0x10];
};
static_assert(sizeof(CJson) == 0x10);

}  // namespace NRadEngine

NRadEngine::CJson::CJson() {
  // stp xzr, xzr, [x0]: the 64-bit words at +0x00 and +0x08 become zero.
  const std::uint64_t zero = 0;
  std::memcpy(raw, &zero, sizeof zero);
  std::memcpy(raw + 8, &zero, sizeof zero);
}
