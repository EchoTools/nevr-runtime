// std::__libcpp_verbose_abort for every Quest target that hooks, built -fno-exceptions.
//
// libc++ calls this when a hardening assertion fails (an out-of-range operator[], a bad optional access).
// The NDK's libc++ enables hardening by default (__config_site: _LIBCPP_HARDENING_MODE_DEFAULT 2, the
// "fast" mode), so hook code reaches it. libc++_static's own definition is built with exceptions and sits
// under a CIE with a personality ("zPLR"), and no frame reachable from a hook entry may carry one
// (callback_thunk.h; tests/quest TestHookFramesCarryNoPersonality). libc++ declares the function
// _LIBCPP_OVERRIDABLE_FUNC_VIS in <__verbose_abort> so that a program can define it; this definition is
// linked before libc++_static, so the library's is never pulled in. It writes the message to logcat and to
// the abort message the tombstone records, then aborts: the behaviour of libc++'s default, without the
// personality.

#include <__verbose_abort>

#include <android/log.h>
#include <android/set_abort_message.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#if defined(__EXCEPTIONS) || defined(__cpp_exceptions)
#error "libcpp_verbose_abort.cpp must be built with -fno-exceptions (hook paths reach it)"
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

void __libcpp_verbose_abort(const char* format, ...) noexcept {
  char message[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  __android_log_write(ANDROID_LOG_FATAL, "NEVR-Sentinel", message);
  android_set_abort_message(message);
  std::abort();
}

_LIBCPP_END_NAMESPACE_STD
