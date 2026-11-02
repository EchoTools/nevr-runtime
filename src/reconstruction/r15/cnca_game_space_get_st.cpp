// NRadEngine::CncaGameSpace::GetST
// Quest libr15.so VA 0x18ff2fc; ReVault binary 576.
// ReVault raw parent 100255908, reconstruction node 100600559; grade H: bare ret, the thunk leaves x0 alone, so the returned value is this
// @0x18ff2fc — NRadEngine::CncaGameSpace::GetST
/* @confidence: H */

namespace NRadEngine {

class CncaGameSpace {
public:
    CncaGameSpace* GetST();
};

CncaGameSpace* CncaGameSpace::GetST() {
    return this;
}

} // namespace NRadEngine
