// NRadEngine::CSysThread::SetTLS
// Quest libr15.so VA 0x108a228; ReVault binary 576.
// ReVault raw parent 100208080, reconstruction node 100600561; grade H: one tail branch to the pthread_setspecific import thunk (0x100bfc0) with both arguments forwarded; the status it returns is dropped because the declared signature is void.
// @0x108a228 — NRadEngine::CSysThread::SetTLS
/* @confidence: H */

#include <pthread.h>

namespace NRadEngine {

struct CSysThread {
  static void SetTLS(int slot, void* value);
};

}  // namespace NRadEngine

void NRadEngine::CSysThread::SetTLS(int slot, void* value) {
  // The original is a single `b` to pthread_setspecific: x0 (slot) and x1 (value) pass through
  // unchanged, and the status is returned in w0 for a caller that discards it.
  const int status = pthread_setspecific(static_cast<pthread_key_t>(slot), value);
  static_cast<void>(status);
}
