// Quest libr15.so VA 0x1091a40; ReVault binary 576.
// ReVault raw parent 100208181, reconstruction node 100600581; grade H.
namespace NRadEngine {

class TSysThreadSrwLock;

class CSysReadWriteLock {
public:
    static void WriteLock(TSysThreadSrwLock* lock);
};

class CLockReadWrite {
public:
    void WriteLock();
};

// @0x1091a40 — NRadEngine::CLockReadWrite::WriteLock
/* @confidence: H — the complete one-instruction body tail-forwards this to CSysReadWriteLock::WriteLock with the same object pointer. */
void CLockReadWrite::WriteLock() {
    CSysReadWriteLock::WriteLock(
        reinterpret_cast<TSysThreadSrwLock*>(this));
}

}  // namespace NRadEngine
