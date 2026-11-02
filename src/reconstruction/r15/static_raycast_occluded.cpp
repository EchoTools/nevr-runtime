// CncaGameSpace::StaticRaycastOccluded
// Quest libr15.so VA 0x19066f4 (RVA 0x18066f4); ReVault binary 576.
// ReVault raw parent 100256014, reconstruction node 100600242; grade M: ARM64 body is a vtable tail-call and the bool result follows the q15t hook signature.
// @0x19066f4 — NRadEngine::CncaGameSpace::StaticRaycastOccluded
/* @confidence: M */

#include <cstddef>

namespace NRadEngine {
struct C3Vector {
    float x;
    float y;
    float z;
};

class CncaGameSpace {
public:
    bool StaticRaycastOccluded(const C3Vector& start,
                               const C3Vector& end) const;
};

using StaticRaycastDispatch = bool (*)(void*, const C3Vector*, const C3Vector*);

bool CncaGameSpace::StaticRaycastOccluded(
    const C3Vector& start, const C3Vector& end) const {
    const auto* bytes = reinterpret_cast<const std::byte*>(this);
    void* subobject = *reinterpret_cast<void* const*>(bytes + 8);
    auto** vtable = *reinterpret_cast<void***>(subobject);
    auto dispatch = reinterpret_cast<StaticRaycastDispatch>(
        vtable[0x278 / sizeof(void*)]);
    return dispatch(subobject, &start, &end);
}
} // namespace NRadEngine
