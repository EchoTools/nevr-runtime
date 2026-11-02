// NRadEngine::CFactory<NRadEngine::CGVisibilityResource>::CreateData
// Quest libr15.so VA 0x1a06028; ReVault binary 576.
// ReVault raw parent 100260791, reconstruction node 100600584; grade H: the node's @confidence is H, and its comments say the whole body is two instructions (mov x0,xzr; ret) that return nullptr.
// @0x1a06028 — NRadEngine::CFactory<NRadEngine::CGVisibilityResource>::CreateData
/* @confidence: H */
#include <cstddef>

namespace NRadEngine {

class CGVisibilityResource;

template <typename T>
class CFactory {
 public:
  void* CreateData();
};

// ReVault labels this the factory's vslot[4]. The whole body is two instructions
// (0x1a06028 mov x0,xzr; 0x1a0602c ret), so this factory declares that it creates
// no data object for a visibility resource rather than that the slot is missing.
template <>
void* CFactory<CGVisibilityResource>::CreateData() {
  return nullptr;
}

}  // namespace NRadEngine
