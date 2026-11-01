// Quest libr15.so VA 0x1091a3c; ReVault binary 576.
// ReVault raw parent 100208180, reconstruction node 100600580; grade H.
namespace NRadEngine {

class TSysThreadSrwLock;

class CSysReadWriteLock {
public:
    static void ReadLock(TSysThreadSrwLock* lock);
};

class CLockReadWrite {
public:
    void ReadLock();
};

// @0x1091a3c — NRadEngine::CLockReadWrite::ReadLock
/* @confidence: H — the complete one-instruction body tail-forwards this to CSysReadWriteLock::ReadLock with the same object pointer. */
void CLockReadWrite::ReadLock() {
    CSysReadWriteLock::ReadLock(
        reinterpret_cast<TSysThreadSrwLock*>(this));
}

}  // namespace NRadEngine
