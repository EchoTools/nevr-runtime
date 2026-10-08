/* Typed callback adapter for a GOT hook.
 *
 * A GOT slot holds a bare C function pointer, so the hook has no place to carry
 * state. CallbackThunk<Tag, Ret(Args...)> gives one hooked game function its own
 * statically allocated entry point, original-call pointer and handler, all typed
 * from the signature the binary was checked against. The game calls Entry();
 * Entry() calls the handler, or the original when no handler is armed.
 *
 *   using Thunk = CallbackThunk<MyTag, int(int, const char*)>;
 *   Thunk::Arm(&MyHandler);                       // int MyHandler(Thunk::Fn, int, const char*)
 *   hook.Install(target, Thunk::EntryAddress(), Thunk::OriginalOut());
 *
 * Contract for a handler:
 *   - it receives the original function and the call's arguments by value;
 *   - it returns what the game should see;
 *   - it must not throw. A std::exception that escapes is caught here, logged,
 *     counted, and the call falls back to the original function, so a handler
 *     that already called the original before throwing causes a second call.
 *     Call the original last.
 * An exception that is not a std::exception cannot cross the noexcept entry
 * point and terminates the process; handlers must not throw one.
 *
 * Until the original has been published a call returns a value-initialised Ret
 * and logs once. Install publishes the original before it changes the slot, so
 * the game cannot reach that path through a hook installed by GotHook.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>

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

  static Fn Original() noexcept {
    return reinterpret_cast<Fn>(__atomic_load_n(&original_, __ATOMIC_ACQUIRE));
  }

  // nullptr disarms: calls pass straight through to the original.
  static void Arm(Handler handler) noexcept { handler_.store(handler, std::memory_order_release); }

  static std::uint64_t Calls() noexcept { return calls_.load(std::memory_order_relaxed); }
  static std::uint64_t Faults() noexcept { return faults_.load(std::memory_order_relaxed); }

  // Test support: clears the original, handler and counters.
  static void Reset() noexcept {
    __atomic_store_n(&original_, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
    handler_.store(nullptr, std::memory_order_release);
    calls_.store(0, std::memory_order_relaxed);
    faults_.store(0, std::memory_order_relaxed);
  }

 private:
  static Ret Entry(Args... args) noexcept {
    calls_.fetch_add(1, std::memory_order_relaxed);
    const Fn original = Original();
    if (original == nullptr) {
      ReportFault("no_original", "default_return");
      return Ret();
    }
    const Handler handler = handler_.load(std::memory_order_acquire);
    if (handler == nullptr) return original(args...);
    try {
      return handler(original, args...);
    } catch (const std::exception&) {
      ReportFault("handler_threw", "call_original");
    }
    return original(args...);
  }

  // Counts every fault; logs the first and then one in every 4096, so a hook on
  // a per-frame function cannot flood the log.
  static void ReportFault(const char* status, const char* action) noexcept {
    const std::uint64_t n = faults_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 4096 == 0) {
      LogEvent(LogLevel::kError, "event=callback_thunk status=%s action=%s faults=%llu", status,
               action, static_cast<unsigned long long>(n));
    }
  }

  inline static void* original_ = nullptr;
  inline static std::atomic<Handler> handler_{nullptr};
  inline static std::atomic<std::uint64_t> calls_{0};
  inline static std::atomic<std::uint64_t> faults_{0};
};

}  // namespace sentinel
