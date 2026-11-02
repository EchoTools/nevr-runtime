// Quest libr15.so VA 0x10a2258; ReVault binary libr15.so.
// ReVault raw parent 100208538, reconstruction node 100600608; grade H.
#include <cstdint>

// @0x10a2258 — NRadEngine::CJson::~CJson
/* @confidence: H */

namespace NRadEngine {

class CJson {
public:
    void ResetCache();
    void Clear(char* text, std::uint32_t reset_children);
    ~CJson();
};

CJson::~CJson() {
    ResetCache();
    Clear(const_cast<char*>(""), 1);
}

}  // namespace NRadEngine
