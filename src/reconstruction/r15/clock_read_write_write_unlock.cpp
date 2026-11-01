// Quest libr15.so VA 0x1091a50; ReVault binary 576.
// ReVault raw parent 100208185, reconstruction node 100600583; grade H.
namespace NRadEngine {

class TSysThreadSrwLock;

class CSysReadWriteLock {
public:
    static void WriteUnlock(TSysThreadSrwLock* lock);
};

class CLockReadWrite {
public:
    void WriteUnlock();
};

// @0x1091a50 — NRadEngine::CLockReadWrite::WriteUnlock
/* @confidence: H — the complete one-instruction body tail-forwards this to CSysReadWriteLock::WriteUnlock with the same object pointer. */
void CLockReadWrite::WriteUnlock() {
    CSysReadWriteLock::WriteUnlock(
        reinterpret_cast<TSysThreadSrwLock*>(this));
}

}  // namespace NRadEngine
