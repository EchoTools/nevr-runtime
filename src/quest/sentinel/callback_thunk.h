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
 *   int MyHandler(Thunk::Fn original, int a, const char* b) noexcept;
 *   Thunk::Arm(&MyHandler);
 *   hook.Install(target, Thunk::EntryAddress(), Thunk::OriginalOut());
 *
 * The original. Entry reads the original-call pointer once and hands that value to
 * the handler, so resetting or replacing the published pointer while a call is in
 * flight cannot make an in-flight call lose its original. GotHook::Install
 * publishes the original before it changes the slot and keeps it published if the
 * slot was ever written, so the game cannot reach the "no original" path
 * (value-initialised Ret, logged) through a hook installed by GotHook.
 *
 * Exceptions: none cross the thunk, in either direction.
 *
 * Measured on the built Android artifact and on the APK's libc++_shared.so:
 *   - libr15.so NEEDs libc++_shared.so. That library is a libgcc-style unwinder
 *     build (its .comment names GCC 4.9.x and clang 5.0) and exports
 *     _Unwind_Find_FDE, _Unwind_GetCFA, _Unwind_GetIP, _Unwind_RaiseException,
 *     _Unwind_Resume, __gxx_personality_v0 and __cxa_throw.
 *   - The sentinel does not NEED it. It links LLVM libunwind and libc++abi
 *     statically; _Unwind_Resume, _Unwind_GetIP, __unw_getcontext,
 *     __gxx_personality_v0, __cxa_throw and __cxa_begin_catch are LOCAL symbols.
 *   - A function that can catch or clean up carries an LSDA under a CIE whose
 *     augmentation is "zPLR", i.e. it names the sentinel's own personality.
 * A game exception unwinding through such a frame would hand the game's
 * _Unwind_Context to the sentinel's personality and unwinder helpers, which read it
 * as their own structure. (That consequence is inferred from the layouts; no such
 * crossing has been run on a device.) The same holds in the other direction.
 *
 * So the contract is structural:
 *   - This header is included only by translation units built with -fno-exceptions
 *     (enforced by the #error below). Entry has no landing pad, no LSDA and no
 *     personality; the unwinder walks past its frame using the CFI alone, which
 *     both runtimes handle. An exception thrown by the game's original therefore
 *     passes through Entry untouched.
 *   - Handlers are `noexcept` (Handler is a noexcept function pointer type, so a
 *     handler that is not declared noexcept does not compile) and live in the same
 *     kind of translation unit. A handler must not contain a try/catch or an object
 *     with a destructor around a call that can reach game code. A try/catch in a
 *     sentinel-only frame that never calls the game is fine.
 *   - tests/quest TestHookFramesCarryNoPersonality pins the built artifact: the
 *     frames of every CallbackThunk member and of the hook handler sit under the
 *     personality-free "zR" CIE.
 */
#pragma once

#if defined(__cpp_exceptions)
#error "callback_thunk.h must be included only by translation units built with -fno-exceptions (see the contract above)"
#endif

#include <atomic>
#include <cstdint>

#include "hook_log.h"

namespace sentinel {

template <typename Tag, typename Signature>
class CallbackThunk;

template <typename Tag, typename Ret, typename... Args>
class CallbackThunk<Tag, Ret(Args...)> {
 public:
  using Fn = Ret (*)(Args...);
  using Handler = Ret (*)(Fn original, Args... args) noexcept;

  // The address to install into the GOT slot.
  static void* EntryAddress() noexcept { return reinterpret_cast<void*>(&Entry); }

  // Where Install stores the original function (the `originalOut` argument).
  static void** OriginalOut() noexcept { return &original_; }

  // The real original function.
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
  static Ret Entry(Args... args) {
    calls_.fetch_add(1, std::memory_order_relaxed);
    const Fn original = Original();
    if (original == nullptr) {
      ReportFault("no_original", "default_return");
      return Ret();
    }
    const Handler handler = handler_.load(std::memory_order_acquire);
    if (handler == nullptr) return original(args...);
    return handler(original, args...);
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
};

}  // namespace sentinel
