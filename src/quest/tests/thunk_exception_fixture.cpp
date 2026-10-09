// Host-test fixture built WITH exceptions (the thunk translation units are built without).
// It throws from an "original" and catches around a call into a thunk entry, so the
// exception crosses the thunk's frames the way a game exception would.
#include <cstring>
#include <exception>
#include <stdexcept>

extern "C" {

int fx_throwing_original(int, int) { throw std::runtime_error("original failure"); }

// Returns 0 if `entry` returned (its value in *result), 1 if it threw the runtime_error
// thrown by fx_throwing_original, 2 for any other std::exception.
int fx_call_catching(int (*entry)(int, int), int a, int b, int* result) {
  try {
    *result = entry(a, b);
    return 0;
  } catch (const std::runtime_error& e) {
    return std::strcmp(e.what(), "original failure") == 0 ? 1 : 2;
  } catch (const std::exception&) {
    return 2;
  }
}

}  // extern "C"
