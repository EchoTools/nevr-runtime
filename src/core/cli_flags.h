#pragma once

// #697: the names of the command-line flags NEVR reads, in one table, and the one function that maps a
// token to the flag it names. Every NEVR flag is kebab-case (`-no-console`). The spellings NEVR read before #697
// were removed in v6.0.0 (#698): they are not in this table, so they name no flag.
//
// A flag is written `-name` or `--name`. The game's own parser skips every leading '-' ('+' and 0x96 too) and
// keys the rest (AddArgSyntax 0x1400d31b0, CSymbol64::Lookup 0x1400ce120), so for the flags NEVR registers
// with it `--name` already works there; this table is NEVR's own reading, which used to be an exact
// `lstrcmpW(arg, L"-name")`. NEVR strips ONE extra leading '-'. Three or more dashes on a flag NEVR
// reads are rejected (Resolved::rejected), because the game's lookup would still accept them and NEVR must
// not silently read the line differently. No NEVR flag takes `=`: a value is the next token.
//
// Pure, no windows.h: unit-tested without the game (src/runtime/tests/test_cmdline_strip.cpp).

#include <cstddef>

namespace nevr_cli {

/// Every flag NEVR reads, by its canonical spelling with its one dash.
inline constexpr const wchar_t* kFlags[] = {
    // NEVR's own.
    L"-server",
    L"-offline",
    L"-windowed",
    L"-config",
    L"-config-path",
    L"-timestamps",
    L"-upnp",
    L"-timestep",
    L"-require-signed-plugins",
    // The game's own tokens that NEVR reads as well (it registers them so the game's parser accepts them).
    L"-region",
    L"-serverregion",
    L"-headless",
    L"-noovr",
    L"-fixedtimestep",
    L"-no-console",
    L"-no-exit-on-error",
    L"-exit-on-error",
    L"-no-telemetry",
    L"-telemetry-rate",
    L"-telemetry-diag",
    L"-trace-exports",
};

/// What a command-line token names.
struct Resolved {
  const wchar_t* flag = nullptr;  ///< Canonical spelling (`-no-console`); nullptr when the token names no NEVR flag.
  bool rejected = false;          ///< Three or more leading dashes on a NEVR flag; `flag` stays nullptr.
};

namespace detail {

inline bool SameText(const wchar_t* a, const wchar_t* b) {
  while (*a != L'\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

}  // namespace detail

/// Maps `token` to the NEVR flag it names: `-x` and `--x` are the flag `-x`; `---x` is rejected; anything else
/// (no dash, a '+' token, an unknown name, a pre-#697 spelling) names nothing. Case-sensitive, as the exact
/// comparison it replaces.
inline Resolved Resolve(const wchar_t* token) {
  Resolved r;
  if (token == nullptr || token[0] != L'-') return r;
  std::size_t dashes = 0;
  while (token[dashes] == L'-') ++dashes;
  const wchar_t* const body = token + dashes;
  for (const wchar_t* flag : kFlags) {
    if (!detail::SameText(body, flag + 1)) continue;
    if (dashes > 2) {
      r.rejected = true;
      return r;
    }
    r.flag = flag;
    return r;
  }
  return r;
}

/// True when `token` names the flag `canonical` (its spelling with one dash).
inline bool IsFlag(const wchar_t* token, const wchar_t* canonical) {
  const Resolved r = Resolve(token);
  return r.flag != nullptr && detail::SameText(r.flag, canonical);
}

}  // namespace nevr_cli
