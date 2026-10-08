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
 * Exceptions. The game libraries use C++ exceptions (they import __cxa_throw,
 * __cxa_begin_catch and _Unwind_Resume and carry .gcc_except_table), so the entry
 * is not noexcept and an exception thrown by the ORIGINAL function reaches the
 * game's own handler unchanged, exactly as it would without the hook. The `Fn`
 * a handler receives is a proxy for the original that records whether it was
 * called, whether it returned, and what it returned. What happens when the
 * handler itself throws a std::exception:
 *
 *   - the original was not called yet: the failure is logged and counted, and the
 *     original runs once with the call's arguments;
 *   - the original threw: that exception is rethrown to the game (not ours);
 *   - the original already returned: the failure is logged and counted, the
 *     original is NOT called again, and its recorded result is returned.
 *
 * An exception that is not a std::exception thrown by a handler propagates to the
 * game's frames like any other; handlers must not throw one. The recorded result
 * requires Ret to be copy-constructible.
 *
 * Until the original has been published a call returns a value-initialised Ret
 * and logs. GotHook::Install publishes the original before it changes the slot,
 * so the game cannot reach that path through a hook installed by GotHook.
 */
#pragma once

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
  struct Empty {};
  using Stored = std::conditional_t<std::is_void_v<Ret>, Empty, Ret>;

  // What the handler did with the original during one call.
  struct CallState {
    unsigned calls = 0;
    bool threw = false;  // set before the original runs, cleared when it returns
    std::optional<Stored> result;
  };

  // The `Fn original` a handler receives. Outside a handler call it just forwards.
  static Ret Proxy(Args... args) {
    const Fn original = Original();
    CallState* const state = current_;
    if (state == nullptr) return original(args...);
    ++state->calls;
    state->threw = true;
    if constexpr (std::is_void_v<Ret>) {
      original(args...);
      state->threw = false;
    } else {
      state->result.emplace(original(args...));
      state->threw = false;
      return *state->result;
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
    struct Scope {
      CallState* outer;
      explicit Scope(CallState* inner) : outer(current_) { current_ = inner; }
      ~Scope() { current_ = outer; }
    } scope(&state);
    try {
      return handler(&Proxy, args...);
    } catch (const std::exception&) {
      if (state.threw) throw;
      if (state.calls == 0) {
        ReportFault("handler_threw", "call_original");
        return original(args...);
      }
      ReportFault("handler_threw_after_original", "return_original_result");
      if constexpr (!std::is_void_v<Ret>) return *state.result;
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

  inline static void* original_ = nullptr;
  inline static std::atomic<Handler> handler_{nullptr};
  inline static std::atomic<std::uint64_t> calls_{0};
  inline static std::atomic<std::uint64_t> faults_{0};
  inline static thread_local CallState* current_ = nullptr;
};

}  // namespace sentinel
