// ReVault binary 576, raw parent 100266462, reconstruction node 100600311; grade H.
// Quest libr15.so VA 0x1b45b38 (ELF RVA 0x1a45b38); PC counterpart not located.
// @0x1b45b38 — NRadEngine::CBVHTree::Occluded
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
std::uint8_t g_occluded_not_implemented = 0;
}
bool CBVHTree::Occluded(const C3Vector&, const C3Vector&) const {
    if ((g_occluded_not_implemented & 1U) == 0) {
        g_occluded_not_implemented = 1;
        NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(4), 0,
            "NotImplemented_(%s) : %s:%d (%s)", "Embree",
            "d:\\projects\\rad\\dev\\src\\engine\\libs\\spatial\\cbvhtree.cpp",
            0xb9, "Occluded");
    }
    return false;
}
}
