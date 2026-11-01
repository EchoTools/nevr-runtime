// ReVault binary 576, raw parent 100266459, reconstruction node 100600310; grade H.
// Quest libr15.so VA 0x1b45a8c (ELF RVA 0x1a45a8c); PC counterpart not located.
// @0x1b45a8c — NRadEngine::CBVHTree::Shutdown
/* @confidence: H — the Quest body is exactly a no-op. */
namespace NRadEngine {
struct SBVHTreeData;
struct CRay;
struct SBVHHit;
struct C3Vector;
class CBVHTree {
public:
    void Initialize(const SBVHTreeData& data);
    void Shutdown();
    bool Intersect(const CRay& ray, SBVHHit& hit) const;
    bool Occluded(const C3Vector& from, const C3Vector& to) const;
};
void CBVHTree::Shutdown() {}
}
