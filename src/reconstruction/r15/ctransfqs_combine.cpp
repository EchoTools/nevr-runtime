// NRadEngine::CTransfQS::Combine
// Quest libr15.so VA 0x1430498; ReVault binary 576.
// ReVault raw parent 100227262, reconstruction node 100600697; grade H: all 6 listing instructions matched; no placeholder in the source (import stub 0x103c400 is not named, its target Append is); signature follows the merged Combine member declaration.
// @0x1430498 — NRadEngine::CTransfQS::Combine
/* @confidence: H */

#include <cstddef>

namespace NRadEngine {

// 48 bytes: quaternion value[0..3], translation value[4..6], per-axis scale value[7..9].
struct CTransfQS {
    float value[12];

    void Append(const CTransfQS& other);  // NRadEngine::CTransfQS::Append @ 0x14302b8, reached through the import stub at 0x103c400
    CTransfQS Combine(const CTransfQS& other) const;
};
static_assert(sizeof(CTransfQS) == 0x30);
static_assert(offsetof(CTransfQS, value) == 0x00);

CTransfQS CTransfQS::Combine(const CTransfQS& other) const {
    // 0x1430498..0x14304a8: ldp q2,q0 from *this+0x10 and ldr q1 from *this+0x00, stored into the sret result (x8).
    CTransfQS result = *this;
    // 0x14304ac: b 0x103c400. x0 = result (this for Append); x1 = other, never written by the listing.
    result.Append(other);
    return result;
}

}  // namespace NRadEngine
