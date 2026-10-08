/* Typed callback adapter for a GOT hook.
 *
 * A GOT slot holds a bare C function pointer, so the hook has no place to carry
 * state. CallbackThunk<Tag, Ret(Args...)> gives one hooked game function its own
 * statically allocated entry point, original-call pointer and handler, all typed
 * from the signature the binary was checked against. The game calls
 * EntryAddress(); the entry calls the handler, or the original when no handler
 * is armed.
 *
 *   using Thunk = CallbackThunk<MyTag, int(int, const char*)>;
 *   Thunk::Arm(&MyHandler);                       // int MyHandler(Thunk::Fn, int, const char*)
 *   hook.Install(target, Thunk::EntryAddress(), Thunk::OriginalOut());
 *
 * The original. Entry reads the original-call pointer once and uses that value
 * for the whole call, so resetting or replacing the published pointer while a call
 * is in flight cannot make an in-flight call lose its original. GotHook::Install
 * publishes the original before it changes the slot and keeps it published if it
 * rolls the slot back, so the game cannot reach the "no original" path
 * (value-initialised Ret, logged) through a hook installed by GotHook.
 *
 * Exceptions. The `Fn` a handler receives is a proxy for the original that
 * records whether the original was called, what it returned and which exception
 * it threw. What happens when the handler throws a std::exception:
 *
 *   - it is the exception the original threw (the handler let it propagate):
 *     rethrown, unchanged, not counted;
 *   - the original was not called: logged and counted, and the original runs once;
 *   - the original was called (returned, or threw and the handler swallowed it):
 *     logged and counted, the original is NOT called again, and the recorded
 *     result is returned (a value-initialised Ret if the original never returned).
 *
 * "The original's own exception" is decided by exception object identity, so a
 * handler that catches it and throws a copy has thrown its own exception. An
 * exception that is not a std::exception propagates like any other and is not
 * counted; handlers must not throw one. The recorded result needs Ret to be
 * copy-constructible.
 *
 * Two C++ runtimes. Measured on the built Android artifact: libr15.so NEEDs
 * libc++_shared.so; the sentinel does not, it links libc++ statically, and
 * __cxa_throw, __cxa_begin_catch and __gxx_personality_v0 are LOCAL symbols in
 * it (tests/quest TestStlContract pins the NEEDED list). So an exception thrown
 * by the game's code belongs to libc++_shared's runtime and the catch clauses
 * here belong to the sentinel's own. What this header relies on, and what is
 * inferred rather than measured on a device: a foreign exception passing through
 * the thunk's frames is expected to run their cleanups and continue to the game's
 * handler, and the catch clauses here are not expected to match a foreign
 * std::exception (type_info objects differ between the two runtimes), which is
 * the behaviour the contract above wants. The identity check and rethrow above
 * are only exercised, and tested, with one runtime on the host. A handler in the
 * sentinel therefore cannot catch the game's exceptions by type.
 *
 * Per-thread call state is kept with pthread_getspecific/pthread_setspecific on a
 * key created by Arm(), not with thread_local: with the API 26 toolchain
 * thread_local is emulated and its first use on each thread allocates, which must
 * not happen inside a hooked libc function that signal handlers may call. If the
 * key cannot be created the handler runs untracked (it gets the raw original and
 * the double-call protection above does not apply), with one logged fault.
 */
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>

#include "hook_log.h"

namespace sentinel {

template <typename Tag, typename Signature>
class CallbackThunk;

template <typename Tag, typename Ret, typename... Args>
class CallbackThunk<Tag, Ret(Args...)> {
 public:
  using Fn = Ret (*)(Args...);
  using Handler = Ret (*)(Fn original, Args... args);

  // The address to install into the GOT slot.
  static void* EntryAddress() noexcept { return reinterpret_cast<void*>(&Entry); }

  // Where Install stores the original function (the `originalOut` argument).
  static void** OriginalOut() noexcept { return &original_; }

  // The real original function (not the proxy a handler receives).
  static Fn Original() noexcept {
    return reinterpret_cast<Fn>(__atomic_load_n(&original_, __ATOMIC_ACQUIRE));
  }

  // nullptr disarms: calls pass straight through to the original. Arming creates
  // the per-thread key; call it from initialisation, not concurrently with itself.
  static void Arm(Handler handler) noexcept {
    if (handler != nullptr) EnsureKey();
    handler_.store(handler, std::memory_order_release);
  }

  static std::uint64_t Calls() noexcept { return calls_.load(std::memory_order_relaxed); }
  static std::uint64_t Faults() noexcept { return faults_.load(std::memory_order_relaxed); }

  // Test support: clears the original, handler and counters (the key stays).
  static void Reset() noexcept {
    __atomic_store_n(&original_, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
    handler_.store(nullptr, std::memory_order_release);
    calls_.store(0, std::memory_order_relaxed);
    faults_.store(0, std::memory_order_relaxed);
  }

 private:
  struct Empty {};
  using Stored = std::conditional_t<std::is_void_v<Ret>, Empty, Ret>;

  // What the handler did with the original during one call.
  struct CallState {
    Fn original = nullptr;
    unsigned calls = 0;
    std::exception_ptr originalException;  // set when the original threw a std::exception
    std::optional<Stored> result;
  };

  static bool KeyReady() noexcept { return keyState_.load(std::memory_order_acquire) == kKeyReady; }

  // Creates the key once. Losing a race leaves the winner to finish.
  static void EnsureKey() noexcept {
    int expected = kKeyNone;
    if (!keyState_.compare_exchange_strong(expected, kKeyCreating)) return;
    if (pthread_key_create(&key_, nullptr) == 0) {
      keyState_.store(kKeyReady, std::memory_order_release);
    } else {
      keyState_.store(kKeyFailed, std::memory_order_release);
      LogFields(LogLevel::kError, "callback_thunk",
                {{"status", "thread_key_failed"}, {"action", "untracked_handlers"}});
    }
  }

  // Makes `state` the current call on this thread; restores the previous one on
  // destruction. `ok` is false when the state could not be installed.
  struct Scope {
    CallState* outer = nullptr;
    bool ok = false;
    explicit Scope(CallState* state) {
      if (!KeyReady()) return;
      outer = static_cast<CallState*>(pthread_getspecific(key_));
      ok = pthread_setspecific(key_, state) == 0;
    }
    ~Scope() {
      if (ok) pthread_setspecific(key_, outer);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };

  // The `Fn original` a handler receives. Outside a handler call it forwards.
  static Ret Proxy(Args... args) {
    CallState* const state = KeyReady() ? static_cast<CallState*>(pthread_getspecific(key_)) : nullptr;
    const Fn original = state != nullptr ? state->original : Original();
    if (original == nullptr) {
      ReportFault("no_original", "default_return");
      return Ret();
    }
    if (state == nullptr) return original(args...);
    ++state->calls;
    try {
      if constexpr (std::is_void_v<Ret>) {
        original(args...);
      } else {
        state->result.emplace(original(args...));
        return *state->result;
      }
    } catch (const std::exception&) {
      state->originalException = std::current_exception();
      throw;
    }
  }

  static Ret Entry(Args... args) {
    calls_.fetch_add(1, std::memory_order_relaxed);
    const Fn original = Original();
    if (original == nullptr) {
      ReportFault("no_original", "default_return");
      return Ret();
    }
    const Handler handler = handler_.load(std::memory_order_acquire);
    if (handler == nullptr) return original(args...);

    CallState state;
    state.original = original;
    Scope scope(&state);
    if (!scope.ok) {
      ReportFault("thread_state_unavailable", "untracked_handler");
      return handler(original, args...);
    }
    try {
      return handler(&Proxy, args...);
    } catch (const std::exception&) {
      if (state.originalException && std::current_exception() == state.originalException) throw;
      if (state.calls == 0) {
        ReportFault("handler_threw", "call_original");
        return original(args...);
      }
      ReportFault("handler_threw_after_original", "return_original_result");
      if constexpr (!std::is_void_v<Ret>) {
        if (state.result) return *state.result;
        return Ret();
      }
    }
  }

  // Counts every fault; logs the first and then one in every 4096, so a hook on
  // a per-frame function cannot flood the log.
  static void ReportFault(const char* status, const char* action) noexcept {
    const std::uint64_t n = faults_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 4096 == 0) {
      LogFields(LogLevel::kError, "callback_thunk",
                {{"status", status}, {"action", action}, {"faults", static_cast<long long>(n)}});
    }
  }

  static constexpr int kKeyNone = 0;
  static constexpr int kKeyCreating = 1;
  static constexpr int kKeyReady = 2;
  static constexpr int kKeyFailed = 3;

  inline static void* original_ = nullptr;
  inline static std::atomic<Handler> handler_{nullptr};
  inline static std::atomic<std::uint64_t> calls_{0};
  inline static std::atomic<std::uint64_t> faults_{0};
  inline static std::atomic<int> keyState_{kKeyNone};
  inline static pthread_key_t key_{};
};

}  // namespace sentinel
