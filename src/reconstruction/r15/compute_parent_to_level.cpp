// NRadEngine::CncaGameSpace::ComputeParentToLevel
// Quest libr15.so VA 0x19070d8; ReVault binary 576.
// ReVault raw parent 100256038, reconstruction node 100600454; grade M: every branch and store matches the listing; callee sret types and const qualifiers are inferred.
// @0x19070d8 — NRadEngine::CncaGameSpace::ComputeParentToLevel
/* @confidence: M */
// Grade M: every branch and store matches the listing; callee sret types and const qualifiers are inferred.

#include <cstddef>
#include <cstring>

namespace NRadEngine {

// 48 bytes: quaternion value[0..3], translation value[4..6], per-axis scale value[7..9].
// value[10..11] is zeroed on the found path and copied from the combined transform on the null path.
struct CTransfQS {
    float value[12];

    CTransfQS Combine(const CTransfQS& other) const;  // NRadEngine::CTransfQS::Combine @ 0x1430498 (thunk 0x101c900)
};
static_assert(sizeof(CTransfQS) == 0x30);

// Only the parent link at +0x60 and these three methods are used here; the layout is not reconstructed.
struct CNode3D {
    CTransfQS ComputeParentToWorldTransfQS() const;  // NRadEngine::CNode3D::ComputeParentToWorldTransfQS @ 0x1a4ed00 (thunk 0x100eb50)
    CTransfQS ComputeWorldToLocalTransfQS() const;   // NRadEngine::CNode3D::ComputeWorldToLocalTransfQS @ 0x1a4ebcc (thunk 0x10163b0)
    CTransfQS ComputeParentToHigherTransfQ(const CNode3D& other) const;  // NRadEngine::CNode3D::ComputeParentToHigherTransfQ @ 0x1a4dbdc (thunk 0xfdac20)
};

// Layout not reconstructed; the level node is the pointer at +0x60.
struct CncaGameSpace {
    void ComputeParentToLevel(CTransfQS& out, const CNode3D& node) const;
};

namespace {

template <typename T>
T LoadAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(T));
    return value;
}

// One ldr/str pair of the listing: copies `bytes` bytes between two transforms.
void CopyBytes(CTransfQS& dst, std::size_t dst_offset, const CTransfQS& src,
               std::size_t src_offset, std::size_t bytes) {
    std::memcpy(reinterpret_cast<std::byte*>(dst.value) + dst_offset,
                reinterpret_cast<const std::byte*>(src.value) + src_offset, bytes);
}

// One str xzr: zeroes `bytes` bytes of a transform.
void ZeroBytes(CTransfQS& dst, std::size_t dst_offset, std::size_t bytes) {
    std::memset(reinterpret_cast<std::byte*>(dst.value) + dst_offset, 0, bytes);
}

}  // namespace

void CncaGameSpace::ComputeParentToLevel(CTransfQS& out, const CNode3D& node) const {
    // 0x19070e8: x0 = this->[0x60].
    const CNode3D* const level_node = LoadAt<const CNode3D*>(this, 0x60);
    // 0x19070f0: x8 = node.
    const CNode3D* cur = &node;
    do {
        // 0x19070f4: x8 = x8->[0x60], walking up the node's parent chain.
        cur = LoadAt<const CNode3D*>(cur, 0x60);
        // 0x19070f8: cbz x8, 0x1907150.
        if (cur == nullptr) {
            // 0x1907150..0x1907158: ComputeParentToWorldTransfQS on node, result at sp+0x30.
            const CTransfQS parent_to_world = node.ComputeParentToWorldTransfQS();
            // 0x190715c: x0 = this->[0x60], reloaded after the call above.
            const CNode3D* const level_node_after_call = LoadAt<const CNode3D*>(this, 0x60);
            // 0x1907160..0x1907164: ComputeWorldToLocalTransfQS on the level node, result at sp+0x00.
            const CTransfQS world_to_local = level_node_after_call->ComputeWorldToLocalTransfQS();
            // 0x1907168..0x1907174: CTransfQS::Combine, this = sp+0x30, other = sp+0x00, result at sp+0x60.
            const CTransfQS combined = parent_to_world.Combine(world_to_local);
            // 0x1907178..0x1907184: 48-byte copy of the combined transform to out.
            out = combined;
            return;
        }
    } while (cur != level_node);  // 0x19070fc..0x1907100: b.ne back to 0x19070f4 while cur != level node.

    // 0x1907104..0x190710c: ComputeParentToHigherTransfQ, this = level node, other = node, result at sp+0x60.
    const CTransfQS higher = level_node->ComputeParentToHigherTransfQ(node);
    // 0x1907110..0x190711c: stage higher[0x00..0x1b] at sp+0x30 (q0, x8, w9).
    CTransfQS staged{};
    CopyBytes(staged, 0x00, higher, 0x00, 16);
    CopyBytes(staged, 0x10, higher, 0x10, 8);
    CopyBytes(staged, 0x18, higher, 0x18, 4);
    // 0x190712c: stp w10, w10 to out+0x1c and out+0x20; w10 is higher[0x1c..0x1f] (ldp at sp+0x78).
    CopyBytes(out, 0x1c, higher, 0x1c, 4);
    CopyBytes(out, 0x20, higher, 0x1c, 4);
    // 0x1907130: str w10 to out+0x24.
    CopyBytes(out, 0x24, higher, 0x1c, 4);
    // 0x1907134: str xzr to out+0x28.
    ZeroBytes(out, 0x28, 8);
    // 0x1907128, 0x1907138: ldur q1 = staged[0x0c..0x1b] (sp+0x3c), stur to out+0x0c.
    CopyBytes(out, 0x0c, staged, 0x0c, 16);
    // 0x190713c: str q0 = higher[0x00..0x0f] to out+0x00.
    CopyBytes(out, 0x00, higher, 0x00, 16);
}

}  // namespace NRadEngine
