// NRadEngine::CSysThread::GetTLS
// Quest libr15.so VA 0x108a22c; ReVault binary 576.
// ReVault raw parent 100208081, reconstruction node 100600557; grade H: one tail branch to imported pthread_getspecific (import thunk 0x1044960 per revault_imports); returns the callee's void*, not Ghidra's void.
// @0x108a22c — NRadEngine::CSysThread::GetTLS
/* @confidence: H */
#include <pthread.h>

namespace NRadEngine {

class CSysThread {
public:
    // The only argument is w0 (the int key); there is no `this` in the disassembly.
    static void* GetTLS(int key);
};

// The disassembly is one `b` to the import thunk 0x1044960, which loads pthread_getspecific
// through its GOT slot and branches. The key register passes through unchanged, and the
// void* that pthread_getspecific returns is what this function returns.
void* CSysThread::GetTLS(int key) {
    return pthread_getspecific(static_cast<pthread_key_t>(key));
}

}  // namespace NRadEngine
