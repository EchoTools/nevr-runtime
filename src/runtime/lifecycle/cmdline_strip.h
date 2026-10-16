#pragma once

// #511 / #515: a dedicated server's fixed time step is NEVR's, and nothing about it is settable from
// outside: the operator gives no time-step argument, so a server cannot be pointed at a different step
// by a stray -fixedtimestep or -timestep. echovr.exe parses its command line in Main_ (0x1400cd550)
// into a CMainArgs from the ANSI string GetCommandLineA() returned (the caller at 0x14151d45c passes it
// in RCX through GameMainWrapper at 0x1400cd510), before PreprocessCommandLine runs. -fixedtimestep is a
// native token there (CncaGame syntax table, 0x1404e7d60; ApplyCommandLineEngineFlags ORs 0x2000000
// into [pGame+0x828] for it at 0x1405045bf, the bit NEVR sets itself in server_timestep.h). A server
// therefore has both removed from the string before Main_ reads it (GameMainWrapperHook,
// crash_recovery.cpp); NEVR sets the flag and the step (boot.cpp, config.cpp).
//
// Pure: text in, text out, no game memory. The tokenizer follows the game's own (FUN_1400d9000 and
// FUN_1400d3c40): tokens split on ' ', a '"' or '[' opens a token that runs to the matching '"' or ']', a
// token is an option when it starts with '-', '+' or 0x96 and the next character is not a digit, and a
// name lookup skips every leading '-', '+' and 0x96 and lower-cases A-Z (CSymbol64::Lookup, 0x1400ce120).
// The game attaches every non-option token after an option to it as that option's parameters
// (GetNumParameters, 0x1400d5d50; ValidateMainArgs, 0x1400da830, counts them against the syntax entry), so
// removing an option removes the parameters that follow it too.
//
// Which "server" wins: StripServerTimestepArgs decides from what the GAME's parser sees, an option token
// exactly "-server", because it edits the string the game parses. boot.cpp decides g_isServer from
// CommandLineToArgvW, which differs on quotes: `[-server]` is not a server there and here, `"-server"`
// is a server to boot.cpp and a parameter (not an option) to the game, and `-config "C:\dir\" -server` is
// a server to the game's parser and a quoted string to CommandLineToArgvW. Neither is changed. The report
// the hook records (TimestepStripReport, timestep_log.h) is what boot.cpp logs: where arguments came out of
// a line boot.cpp took for a client, boot.cpp logs a Warning (no flag or step was set); where it took the
// line for a server and nothing was removed, it also logs a Warning.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nevr_cmdline {

/// One token of a command line, as the game's parser sees it.
struct Token {
  std::size_t begin = 0;  ///< First byte of the token in the input, including an opening quote or bracket.
  std::size_t end = 0;    ///< One past the last byte, including a closing quote or bracket when present.
  std::string_view text;  ///< The token without its opening and closing quote or bracket.
  bool option = false;    ///< Starts with '-', '+' or 0x96 and the next character is not a digit.
};

namespace detail {

inline bool IsDash(char c) { return c == '-' || c == '+' || static_cast<unsigned char>(c) == 0x96; }

inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ' ') : c; }

/// The game's name lookup: leading dashes skipped, A-Z lower-cased.
inline bool NameIs(std::string_view token, std::string_view lowerName) {
  std::size_t i = 0;
  while (i < token.size() && IsDash(token[i])) ++i;
  token.remove_prefix(i);
  if (token.size() != lowerName.size()) return false;
  for (std::size_t k = 0; k < token.size(); ++k) {
    if (Lower(token[k]) != lowerName[k]) return false;
  }
  return true;
}

/// The two options a server loses, by their canonical names; an empty view for any other token.
inline std::string_view TimestepOptionName(const Token& t) {
  if (!t.option) return {};
  if (NameIs(t.text, "fixedtimestep")) return "-fixedtimestep";
  if (NameIs(t.text, "timestep")) return "-timestep";
  return {};
}

inline bool IsSpace(char c) { return static_cast<unsigned char>(c) <= 0x20; }

}  // namespace detail

/// Splits `commandLine` into the tokens the game's parser produces. The views point into `commandLine`.
inline std::vector<Token> Tokenize(std::string_view commandLine) {
  std::vector<Token> tokens;
  const std::size_t n = commandLine.size();
  std::size_t i = 0;
  while (i < n) {
    // The game skips bytes 1..0x20 between tokens; a byte with the high bit set starts a token.
    while (i < n && detail::IsSpace(commandLine[i])) ++i;
    if (i >= n) break;
    Token t;
    t.begin = i;
    const char first = commandLine[i];
    const bool dashFirst = detail::IsDash(first);
    t.option = dashFirst && !(i + 1 < n && commandLine[i + 1] >= '0' && commandLine[i + 1] <= '9');
    if (first == '"' || first == '[') {
      const char close = first == '"' ? '"' : ']';
      std::size_t j = i + 1;
      while (j < n && commandLine[j] != close) ++j;
      t.text = commandLine.substr(i + 1, j - (i + 1));
      t.end = j < n ? j + 1 : j;
      i = t.end;
    } else {
      std::size_t j = i;
      while (j < n && commandLine[j] != ' ') ++j;
      t.text = commandLine.substr(i, j - i);
      t.end = j;
      i = j;
    }
    tokens.push_back(t);
  }
  return tokens;
}

/// True when the game's parser reads `-server` in `commandLine`: an option token exactly "-server". See the
/// header comment for how this differs from boot.cpp's CommandLineToArgvW reading.
inline bool HasServerToken(std::string_view commandLine) {
  for (const Token& t : Tokenize(commandLine)) {
    if (t.option && t.text == "-server") return true;
  }
  return false;
}

/// True when `commandLine` still holds a time-step option the game would read.
inline bool HasTimestepOption(std::string_view commandLine) {
  for (const Token& t : Tokenize(commandLine)) {
    if (!detail::TimestepOptionName(t).empty()) return true;
  }
  return false;
}

/// The command line a server hands to the game, and what was taken out of it.
struct StripResult {
  std::string commandLine;             ///< The text to give the game.
  std::vector<std::string> removed;    ///< Each removed token's text (an option or a parameter), in order.
  std::vector<std::string> options;    ///< The canonical name of each removed option, in order.
  std::vector<std::string> kept;       ///< The text of each token left in `commandLine`, in order.
};

namespace detail {

/// Appends `piece` to `out`, with one space between them when the two would otherwise abut into one token.
/// The game ends an unquoted token only at ' ' (FUN_1400d9000: `while (b != ' ' && b != 0)`), so a tab or
/// another control byte at the seam does not separate them: the test is for ' ' alone.
inline void Append(std::string& out, std::string_view piece) {
  if (piece.empty()) return;
  if (!out.empty() && out.back() != ' ' && piece.front() != ' ') out.push_back(' ');
  out.append(piece);
}

}  // namespace detail

/// On a server (a `-server` option is present) removes every `-fixedtimestep` and `-timestep` option with
/// the parameters the game would attach to it (every non-option token that follows), and the space before
/// each. Where the text that is kept would abut after a removal, one space is put between. On a client the
/// text comes back untouched.
inline StripResult StripServerTimestepArgs(std::string_view commandLine) {
  StripResult result;
  result.commandLine.assign(commandLine);
  if (!HasServerToken(commandLine)) return result;

  const std::vector<Token> tokens = Tokenize(commandLine);
  std::vector<bool> drop(tokens.size(), false);
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    const std::string_view name = detail::TimestepOptionName(tokens[k]);
    if (name.empty()) continue;
    drop[k] = true;
    result.options.emplace_back(name);
    for (std::size_t v = k + 1; v < tokens.size() && !tokens[v].option; ++v) drop[v] = true;
  }

  std::string out;
  std::size_t copied = 0;  // input bytes up to here are in `out` or were dropped
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    if (!drop[k]) continue;
    // Keep the bytes before the token, minus the gap that separated it from the previous token.
    const std::size_t gapStart = k == 0 ? 0 : tokens[k - 1].end;
    detail::Append(out, commandLine.substr(copied, gapStart - copied));
    copied = tokens[k].end;
    result.removed.emplace_back(tokens[k].text);
  }
  if (result.removed.empty()) return result;
  detail::Append(out, commandLine.substr(copied));
  result.commandLine = std::move(out);
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    if (!drop[k]) result.kept.emplace_back(tokens[k].text);
  }
  return result;
}

/// What GameMainWrapperHook hands the game, and what it took out. `commandLine` is `original` itself when
/// nothing was removed (a client, nothing to remove, or a null `original`), else `storage.c_str()`.
struct HandOff {
  const char* commandLine = nullptr;
  unsigned tokensRemoved = 0;             ///< Options and their parameters.
  std::vector<std::string> options;       ///< Canonical names of the removed options, in order.
  std::string optionsText;                ///< `options` joined with ',' (what the report logs).
  bool residual = false;                  ///< A time-step option is still in the text handed over.
};

/// The one decision GameMainWrapperHook makes: the string to give the game. `storage` holds the stripped
/// text and must outlive the game's parse (the hook keeps it static). May throw std::bad_alloc.
inline HandOff PrepareForGame(const char* original, std::string& storage) {
  HandOff h;
  h.commandLine = original;
  if (original == nullptr) return h;
  StripResult stripped = StripServerTimestepArgs(original);
  if (stripped.removed.empty()) return h;
  storage = std::move(stripped.commandLine);
  h.commandLine = storage.c_str();
  h.tokensRemoved = static_cast<unsigned>(stripped.removed.size());
  h.options = std::move(stripped.options);
  for (const std::string& option : h.options) {
    if (!h.optionsText.empty()) h.optionsText += ',';
    h.optionsText += option;
  }
  h.residual = HasTimestepOption(storage);
  return h;
}

}  // namespace nevr_cmdline
