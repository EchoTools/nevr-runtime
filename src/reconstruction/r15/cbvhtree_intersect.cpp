// ReVault binary 576, raw parent 100266461, reconstruction node 100600312; grade H.
// Quest libr15.so VA 0x1b45ae4 (ELF RVA 0x1a45ae4); PC counterpart not located.
// @0x1b45ae4 — NRadEngine::CBVHTree::Intersect
/* @confidence: H — raw control flow, warning arguments and false return are reproduced directly. */
#include <cstdint>

namespace NRadEngine {
struct SBVHTreeData;
struct CRay;
struct SBVHHit;
struct C3Vector;
namespace NWriteLog {
enum ELogLevel : int;
void WriteLog(ELogLevel level, unsigned long long flags, const char* format, ...);
}
class CBVHTree {
public:
    void Initialize(const SBVHTreeData& data);
    void Shutdown();
    bool Intersect(const CRay& ray, SBVHHit& hit) const;
    bool Occluded(const C3Vector& from, const C3Vector& to) const;
};
namespace {
std::uint8_t g_intersect_not_implemented = 0;
}
bool CBVHTree::Intersect(const CRay&, SBVHHit&) const {
    if ((g_intersect_not_implemented & 1U) == 0) {
        g_intersect_not_implemented = 1;
        NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(4), 0,
            "NotImplemented_(%s) : %s:%d (%s)", "Embree",
            "d:\\projects\\rad\\dev\\src\\engine\\libs\\spatial\\cbvhtree.cpp",
            0x87, "Intersect");
    }
    return false;
}
}
