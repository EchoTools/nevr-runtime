#pragma once
// One pointer-sized value per thread, for code that runs inside a hooked game call, in place of `thread_local`.
//
// The Quest build targets API 26, where the compiler emulates thread_local (`__emutls_v.*`): a thread's first use allocates, and a
// hooked call must not allocate (the sentinel's TestNoEmulatedTLSInHookPath holds the same line for its own library; the arm64
// check `no_emutls` in src/quest/tests/q15t holds it for this plugin). A pthread key keeps its value in an array of the thread's own
// static TLS block (bionic: `bionic_tls::key_data`), made when the thread is created, so Get and Set never allocate or lock.
//
// The key is made once, by Create() at the module's install, before the first hook that reads or writes the value can run. It is
// never deleted: nothing uninstalls a q15t module once a word is swapped, and a deleted key could hand a stale value to a thread.
// Before Create, or after a Create that failed, Get answers 0 and Set does nothing, the same as a thread that never set a value.
// Create answers 0, or the error pthread_key_create returned (EAGAIN: the process has no key left); the caller logs it as `error`.
// A module with several per-thread values has one ThreadWord for each (bionic gives a process 128 keys).
//
// A ThreadWord has no constructor that runs at load (the repack refuses a plugin with a static initializer).
#include <pthread.h>

#include <atomic>
#include <cstdint>

namespace q15t {

class ThreadWord {
 public:
  // Makes the key. Idempotent; safe from any thread. 0: the key exists. Otherwise the error pthread_key_create returned (the C
  // library has no key left), and the caller must not install.
  int Create() {
    pthread_mutex_lock(&mu_);
    int error = 0;
    if (!ready_.load(std::memory_order_relaxed)) {
      error = pthread_key_create(&key_, nullptr);
      ready_.store(error == 0, std::memory_order_release);
    }
    pthread_mutex_unlock(&mu_);
    return error;
  }

  // This thread's value; 0 until this thread sets one.
  std::uintptr_t Get() const {
    if (!ready_.load(std::memory_order_acquire)) return 0;
    return reinterpret_cast<std::uintptr_t>(pthread_getspecific(key_));
  }

  // Sets this thread's value. No other thread sees it.
  void Set(std::uintptr_t value) {
    if (!ready_.load(std::memory_order_acquire)) return;
    pthread_setspecific(key_, reinterpret_cast<void*>(value));
  }

 private:
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_key_t key_{};
  std::atomic<bool> ready_{false};
};

}  // namespace q15t
