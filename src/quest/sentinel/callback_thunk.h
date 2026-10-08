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
 * (a value-initialised Ret, counted as a fault) through a hook installed by GotHook.
 *
 * One thunk per hooked slot. The original-call pointer is a static of the instantiation,
 * so give every hooked slot its own Tag; installing two slots through one thunk makes the
 * second overwrite the first's original. (Install can't see this, so nothing enforces it.)
 *
 * Exceptions: the thunk neither catches nor raises one, and it keeps its own frames out of
 * the way of a game exception.
 *
 * Measured on the built Android artifact and on the APK's libc++_shared.so:
 *   - libr15.so NEEDs libc++_shared.so. That library is a libgcc-style unwinder build
 *     (its .comment names GCC 4.9.x and clang 5.0) and exports _Unwind_Find_FDE,
 *     _Unwind_GetCFA, _Unwind_GetIP, _Unwind_RaiseException, _Unwind_Resume,
 *     __gxx_personality_v0 and __cxa_throw.
 *   - The sentinel does not NEED it. It links LLVM libunwind and libc++abi statically;
 *     _Unwind_Resume, _Unwind_GetIP, __unw_getcontext, __gxx_personality_v0, __cxa_throw
 *     and __cxa_begin_catch are LOCAL symbols.
 *   - A sentinel function that can catch or clean up carries an LSDA under a CIE whose
 *     augmentation is "zPLR", i.e. it names the sentinel's own personality.
 * A game exception unwinding through such a frame would hand the game's _Unwind_Context to
 * the sentinel's personality and unwinder helpers, which read it as their own structure.
 * (Inferred from the layouts; no such crossing has been run on a device.) The reverse
 * holds too: a sentinel exception unwinding into game frames meets the game's unwinder.
 *
 * What the contract is, exactly:
 *   1. A frame that is LIVE while game code runs under a hook must be personality-free
 *      ("zR"): no try/catch, no object with a destructor, no cleanup. Live frames are the
 *      thunk's Entry, the handler, and any sentinel function the handler calls and that
 *      is still on the stack when the handler calls `original` (or any other game code).
 *      A function that runs entirely before or after the call into the game is not live
 *      during it and is not restricted by this rule, except that it must not let an
 *      exception out.
 *   2. This header is included only by translation units built with -fno-exceptions (the
 *      #error below), so Entry has no landing pad; the unwinder walks past its frame on CFI
 *      alone and a game exception passes through untouched.
 *   3. A hook is defined by a record: NEVR_HOOK_RECORD(name, Thunk, handler) emits a
 *      {entry, handler} pair into the output section nevr_hook_records, and Thunk::Arm takes
 *      that record, so the only way to arm a handler is to have it recorded. Handlers are
 *      `noexcept` function pointers. Under -fno-exceptions `noexcept` is only a type marker:
 *      it adds no terminate landing pad. Nothing stops an exception raised in a sentinel
 *      callee from leaving the handler, so a callee that can throw must catch inside
 *      sentinel-only frames that are not live across a call into game code (a helper that does
 *      its throwing work, catches, and returns before the handler touches the game).
 *   4. A hook is installed only through InstallThunk<Thunk> (hook_install.h). The raw
 *      GotHook::Install taking an arbitrary function pointer is private; only the test access
 *      class (tests/) may call it, and TestRawInstallOnlyInTests pins that.
 *   5. Helpers. A function a handler calls directly and that can be on the stack across the
 *      call into game code must be personality-free ("zR"): no try/catch, no destructor-bearing
 *      object, and no indirect call (function pointer, virtual, std::function) into code built
 *      with exceptions. The sensor cannot follow indirect calls, so that last part is a rule,
 *      not a check.
 *   6. tests/quest TestHookFramesCarryNoPersonality enforces rule 1 on the built artifact: it
 *      requires exactly one record per thunk Entry (an Entry without a record fails), starts
 *      from every record's entry and handler, follows direct bl/b edges through the sentinel
 *      and fails on any reachable function under a personality-bearing CIE. There is no
 *      allowlist: a hook does not log, so no logging code is reachable from it.
 */
#pragma once

#if defined(__cpp_exceptions)
#error "callback_thunk.h must be included only by translation units built with -fno-exceptions (see the contract above)"
#endif

#include <atomic>
#include <cstdint>

// Defines a hook: records {entry, handler} in the output section nevr_hook_records (found
// by the build-time sensor through the section's relocations) and gives Thunk::Arm the only
// thing it accepts.
// `retain` keeps the record through the linker's --gc-sections (nothing references it).
#if defined(__clang__)
#define NEVR_HOOK_RECORD_ATTRS __attribute__((used, retain, section("nevr_hook_records")))
#else
#define NEVR_HOOK_RECORD_ATTRS __attribute__((used, section("nevr_hook_records")))
#endif
#define NEVR_HOOK_RECORD(name, Thunk, handlerFn)                                  \
  NEVR_HOOK_RECORD_ATTRS constexpr ::sentinel::HookRecord<Thunk> name {            \
    Thunk::EntryFn(), handlerFn                                                    \
  }

namespace sentinel {

template <typename Tag, typename Signature>
class CallbackThunk;

// The two function pointers that make a hook: the thunk's entry and its handler.
template <typename Thunk>
struct HookRecord {
  typename Thunk::Fn entry;
  typename Thunk::Handler handler;
};

template <typename Tag, typename Ret, typename... Args>
class CallbackThunk<Tag, Ret(Args...)> {
 public:
  using Fn = Ret (*)(Args...);
  using Handler = Ret (*)(Fn original, Args... args) noexcept;

  // The thunk's entry as a function pointer (a constant expression, for NEVR_HOOK_RECORD).
  static constexpr Fn EntryFn() noexcept { return &Entry; }

  // The address to install into the GOT slot (InstallThunk does this; nothing else should).
  static void* EntryAddress() noexcept { return reinterpret_cast<void*>(&Entry); }

  // Where Install stores the original function (the `originalOut` argument).
  static void** OriginalOut() noexcept { return &original_; }

  // The real original function.
  static Fn Original() noexcept {
    return reinterpret_cast<Fn>(__atomic_load_n(&original_, __ATOMIC_ACQUIRE));
  }

  // Arms the record's handler; Disarm makes calls pass straight through to the original.
  static void Arm(const HookRecord<CallbackThunk>& record) noexcept {
    handler_.store(record.handler, std::memory_order_release);
  }
  static void Disarm() noexcept { handler_.store(nullptr, std::memory_order_release); }

  static std::uint64_t Calls() noexcept { return calls_.load(std::memory_order_relaxed); }
  static std::uint64_t Faults() noexcept { return faults_.load(std::memory_order_relaxed); }

  // The call and fault counters, for the reporter (hook_report.h): the thunk never logs.
  static const std::atomic<std::uint64_t>& CallCounter() noexcept { return calls_; }
  static const std::atomic<std::uint64_t>& FaultCounter() noexcept { return faults_; }

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
      faults_.fetch_add(1, std::memory_order_relaxed);  // reported by the reporter, not logged here
      return Ret();
    }
    const Handler handler = handler_.load(std::memory_order_acquire);
    if (handler == nullptr) return original(args...);
    return handler(original, args...);
  }

  inline static void* original_ = nullptr;
  inline static std::atomic<Handler> handler_{nullptr};
  inline static std::atomic<std::uint64_t> calls_{0};
  inline static std::atomic<std::uint64_t> faults_{0};
};

}  // namespace sentinel
