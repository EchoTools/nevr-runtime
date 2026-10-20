#pragma once

// #697: the names of the command-line flags NEVR reads, in one table, and the one function that maps a
// token to the flag it names. Every NEVR flag is kebab-case (`-no-console`). The names it used to have
// (`-noconsole`) are deprecated aliases: still accepted, mapped to the new flag, documented nowhere, and
// reported once per run by the caller (kAliasWarningFormat). They are removed in v6.0.0 (#698).
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

/// One flag NEVR reads. `flag` is the canonical spelling with its one dash; `alias` is the deprecated
/// spelling that also selects it, or nullptr.
struct FlagSpec {
  const wchar_t* flag;
  const wchar_t* alias;
};

inline constexpr FlagSpec kFlags[] = {
    // NEVR's own, with no earlier spelling.
    {L"-server", nullptr},
    {L"-offline", nullptr},
    {L"-windowed", nullptr},
    {L"-config", nullptr},
    {L"-config-path", nullptr},
    {L"-timestamps", nullptr},
    {L"-upnp", nullptr},
    {L"-timestep", nullptr},
    // The game's own tokens that NEVR reads as well (it registers them so the game's parser accepts them).
    {L"-region", nullptr},
    {L"-serverregion", nullptr},
    {L"-headless", nullptr},
    {L"-noovr", nullptr},
    {L"-fixedtimestep", nullptr},
    // Renamed in #697.
    {L"-no-console", L"-noconsole"},
    {L"-no-exit-on-error", L"-noexitonerror"},
    {L"-exit-on-error", L"-exitonerror"},
    {L"-no-telemetry", L"-notelemetry"},
    {L"-telemetry-rate", L"-telemetryrate"},
    {L"-telemetry-diag", L"-telemetrydiag"},
    {L"-trace-exports", L"-traceexports"},
    {L"-skip-dbgcore-check", L"-allow-dbgcore"},
};

inline constexpr std::size_t kFlagCount = sizeof(kFlags) / sizeof(kFlags[0]);

/// The Warning an alias earns, once per run per alias: `%ls` is the alias (with its dash), then the flag.
inline constexpr const char* kAliasWarningFormat = "[NEVR.PATCH] %ls is deprecated and will be removed in v6.0.0; use %ls";

/// What a command-line token names.
struct Resolved {
  const wchar_t* flag = nullptr;   ///< Canonical spelling (`-no-console`); nullptr when the token names no NEVR flag.
  const wchar_t* alias = nullptr;  ///< The deprecated spelling (`-noconsole`) when the token used it, else nullptr.
  bool rejected = false;           ///< Three or more leading dashes on a NEVR flag or alias; `flag` stays nullptr.
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
/// (no dash, a '+' token, an unknown name) names nothing. Case-sensitive, as the exact comparison it replaces.
inline Resolved Resolve(const wchar_t* token) {
  Resolved r;
  if (token == nullptr || token[0] != L'-') return r;
  std::size_t dashes = 0;
  while (token[dashes] == L'-') ++dashes;
  const wchar_t* const body = token + dashes;
  for (const FlagSpec& spec : kFlags) {
    const bool isFlag = detail::SameText(body, spec.flag + 1);
    const bool isAlias = !isFlag && spec.alias != nullptr && detail::SameText(body, spec.alias + 1);
    if (!isFlag && !isAlias) continue;
    if (dashes > 2) {
      r.rejected = true;
      return r;
    }
    r.flag = spec.flag;
    if (isAlias) r.alias = spec.alias;
    return r;
  }
  return r;
}

/// True when `token` names the flag `canonical` (its spelling with one dash), through its name or its alias.
inline bool IsFlag(const wchar_t* token, const wchar_t* canonical) {
  const Resolved r = Resolve(token);
  return r.flag != nullptr && detail::SameText(r.flag, canonical);
}

}  // namespace nevr_cli
