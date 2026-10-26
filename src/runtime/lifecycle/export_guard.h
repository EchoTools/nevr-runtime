#pragma once

// The one place the runtime's export entries (runtime_exports.cpp) and its initialize (runtime_entry.cpp) stop an
// exception. The host calls these
// entries from plugin and game code across the DLL boundary, and an exception must not cross it: a C++ exception
// unwinding into a caller built with another toolchain, or into a frame with no handler, ends the process.
// Guarded runs the function behind an entry, reports once what escaped it, and gives the caller the entry's
// failure value, the same value the host's thunks return before the runtime is up.

#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

namespace nevr_runtime_exports {

/// Where a guarded entry reports what escaped it. `what` is the std::exception's text; it is null when the thrown
/// value was not a std::exception.
using ThrewReporter = void (*)(const char* entry, const char* what) noexcept;

/// Runs `fn`; when it throws, reports through `report` and returns `failure`.
template <class Result, class Fn>
Result Guarded(const char* entry, ThrewReporter report, Result failure, Fn&& fn) noexcept {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::exception& error) {
    report(entry, error.what());
    return failure;
  } catch (...) {  // exceptions must not cross the DLL boundary, so a throw of any other type ends here too
    report(entry, nullptr);
    return failure;
  }
}

/// The same for an entry that returns nothing.
template <class Fn>
void GuardedVoid(const char* entry, ThrewReporter report, Fn&& fn) noexcept {
  try {
    std::forward<Fn>(fn)();
  } catch (const std::exception& error) {
    report(entry, error.what());
  } catch (...) {  // exceptions must not cross the DLL boundary, so a throw of any other type ends here too
    report(entry, nullptr);
  }
}

/// What the runtime's initialize returns when it threw. The host reads any nonzero code as a failed initialize and
/// stops the game; the runtime's own status codes are positive, so this one cannot be mistaken for them.
inline constexpr std::int32_t kInitializeThrew = -2;

/// The runtime's initialize, behind the same guard: the host calls it from its detour on the game's main thread,
/// and an exception must not unwind into the host.
template <class Fn>
std::int32_t GuardedInitialize(ThrewReporter report, Fn&& fn) noexcept {
  return Guarded("initialize", report, kInitializeThrew, std::forward<Fn>(fn));
}

}  // namespace nevr_runtime_exports
