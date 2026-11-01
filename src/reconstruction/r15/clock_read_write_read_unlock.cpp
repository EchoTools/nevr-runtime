// Quest libr15.so VA 0x1091a4c; ReVault binary 576.
// ReVault raw parent 100208184, reconstruction node 100600582; grade H.
namespace NRadEngine {

class TSysThreadSrwLock;

class CSysReadWriteLock {
public:
    static void ReadUnlock(TSysThreadSrwLock* lock);
};

class CLockReadWrite {
public:
    void ReadUnlock();
};

// @0x1091a4c — NRadEngine::CLockReadWrite::ReadUnlock
/* @confidence: H — the complete one-instruction body tail-forwards this to CSysReadWriteLock::ReadUnlock with the same object pointer. */
void CLockReadWrite::ReadUnlock() {
    CSysReadWriteLock::ReadUnlock(
        reinterpret_cast<TSysThreadSrwLock*>(this));
}

}  // namespace NRadEngine
