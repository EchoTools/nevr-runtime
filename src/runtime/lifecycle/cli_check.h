// cli_check.h: the command-line rules that need no game state (#1253; cli_reject.h ends the run for them).
//
// A token the run cannot use is refused before the game, the plugins or the bridge start. The rules:
//   - a flag spelled with more than two leading dashes (core/cli_flags.h reads no such spelling);
//   - a flag that takes a value, given with none (`-region` at the end of the line, or before another option);
//   - `-telemetry-rate` with a value that is not a number above zero (boot.cpp would otherwise use 10 without a word);
//   - `-server` with `-offline`, or with `-windowed` (a server has no window and no headset).
// The first rule that holds is reported. argv[0], the program path, is never checked.
//
// Not a rule here: a token NEVR's table does not name. The game's own syntax table is not known before the game
// starts, and a native game flag such as `-novr` is a legal token that NEVR's table does not list, so an allowlist
// would refuse it.
//
// Pure, no windows.h: unit-tested without the game (src/runtime/tests/test_cli_check.cpp).

#include "core/cli_flags.h"
#include "core/cli_option_value.h"

#include <cwchar>
#include <string>
#include <utility>

namespace nevr_cli_check {

/// The token a run is refused for, and why.
struct Rejection {
  int index = -1;    ///< The argv index of the token the refusal names.
  std::wstring why;  ///< What is wrong, in words.
};

namespace detail {

inline bool Named(const wchar_t* flag, const wchar_t* name) { return std::wcscmp(flag, name) == 0; }

// The flags that take one value (AddArgSyntax(..., 1, 1, ...) in lifecycle/cli.cpp, and the value NEVR reads for them).
inline bool TakesValue(const wchar_t* flag) {
  static const wchar_t* const kValueFlags[] = {
      L"-config",  L"-config-file", L"-config-path", L"-region", L"-serverregion",
      L"-trace-exports", L"-telemetry-rate", L"-timestep",
  };
  for (const wchar_t* name : kValueFlags) {
    if (Named(flag, name)) return true;
  }
  return false;
}

// A number above zero, spelled with digits only.
inline bool IsPositiveNumber(const wchar_t* text) {
  if (text == nullptr || text[0] == L'\0') return false;
  for (const wchar_t* c = text; *c != L'\0'; ++c) {
    if (*c < L'0' || *c > L'9') return false;
  }
  return std::wcstoul(text, nullptr, 10) > 0;
}

inline bool Refuse(Rejection* out, int index, std::wstring why) {
  out->index = index;
  out->why = std::move(why);
  return true;
}

}  // namespace detail

/// True when a rule holds for the command line; `out` then names the token and why. argv[0] is skipped.
inline bool Check(int argc, wchar_t* const* argv, Rejection* out) {
  int offline = -1;
  int windowed = -1;
  bool server = false;
  for (int i = 1; i < argc; ++i) {
    const nevr_cli::Resolved named = nevr_cli::Resolve(argv[i]);
    if (named.rejected) {
      return detail::Refuse(out, i, L"NEVR reads no flag with more than two leading dashes");
    }
    if (named.flag == nullptr) continue;
    const wchar_t* const flag = named.flag;
    if (detail::Named(flag, L"-server")) {
      server = true;
    } else if (detail::Named(flag, L"-offline")) {
      if (offline < 0) offline = i;
    } else if (detail::Named(flag, L"-windowed")) {
      if (windowed < 0) windowed = i;
    }
    if (!detail::TakesValue(flag)) continue;
    const wchar_t* const value = nevr_cli::OptionValue(argc, argv, i);
    if (value == nullptr) {
      return detail::Refuse(out, i, L"takes 1 argument; none was given");
    }
    if (detail::Named(flag, L"-telemetry-rate") && !detail::IsPositiveNumber(value)) {
      return detail::Refuse(out, i, std::wstring(L"value \"") + value + L"\" is not a number above zero");
    }
    ++i;  // the value is not a flag
  }
  if (server && offline >= 0) {
    return detail::Refuse(out, offline, L"-server and -offline are mutually exclusive");
  }
  if (server && windowed >= 0) {
    return detail::Refuse(out, windowed, L"-server cannot be used with -windowed: a server has no window");
  }
  return false;
}

}  // namespace nevr_cli_check
