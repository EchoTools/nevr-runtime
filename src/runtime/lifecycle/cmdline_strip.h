#pragma once

// #511 / #515: a dedicated server's fixed time step is NEVR's, so the game must never see a time-step
// argument from the operator. echovr.exe parses its command line in Main_ (0x1400cd550) into a CMainArgs
// from the ANSI string GetCommandLineA() returned (the caller at 0x14151d45c passes it in RCX through
// GameMainWrapper at 0x1400cd510), before PreprocessCommandLine runs. -fixedtimestep is a native token
// there (CncaGame syntax table, 0x1404e7d60), so it would put a server on the game's 30 Hz step;
// -timestep is not a native token: the game would reject it as "not a valid command"
// (ValidateMainArgs, 0x1400da830). A server therefore has both removed from the string before Main_ reads
// it (GameMainWrapperHook, crash_recovery.cpp), and sets the flag and the step itself (boot.cpp,
// config.cpp, server_timestep.h).
//
// Pure: text in, text out, no game memory. The tokenizer follows the game's own (FUN_1400d9000 and
// FUN_1400d3c40): tokens split on ' ', a '"' or '[' opens a token that runs to the matching '"' or ']', a
// token is an option when it starts with '-', '+' or 0x96 and the next character is not a digit, and a
// name lookup skips every leading '-', '+' and 0x96 and lower-cases A-Z (CSymbol64::Lookup, 0x1400ce120).

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

}  // namespace detail

/// Splits `commandLine` into the tokens the game's parser produces. The views point into `commandLine`.
inline std::vector<Token> Tokenize(std::string_view commandLine) {
  std::vector<Token> tokens;
  const std::size_t n = commandLine.size();
  std::size_t i = 0;
  while (i < n) {
    // The game skips bytes 1..0x20 between tokens; a byte with the high bit set starts a token.
    while (i < n && static_cast<unsigned char>(commandLine[i]) <= 0x20) ++i;
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

/// True when `commandLine` carries the exact token `-server` (how boot.cpp decides g_isServer).
inline bool HasServerToken(std::string_view commandLine) {
  for (const Token& t : Tokenize(commandLine)) {
    if (t.text == "-server") return true;
  }
  return false;
}

/// The command line a server hands to the game, and what was taken out of it.
struct StripResult {
  std::string commandLine;            ///< The text to give the game.
  std::vector<std::string> removed;   ///< Each removed token's text, in order; empty when nothing changed.
};

/// On a server (a `-server` token is present) removes every `-fixedtimestep` token and every `-timestep`
/// token with its value (the next token, when that is not an option), and the space before each. On a
/// client the text comes back untouched.
inline StripResult StripServerTimestepArgs(std::string_view commandLine) {
  StripResult result;
  result.commandLine.assign(commandLine);
  if (!HasServerToken(commandLine)) return result;

  const std::vector<Token> tokens = Tokenize(commandLine);
  std::vector<bool> drop(tokens.size(), false);
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    const Token& t = tokens[k];
    if (!t.option) continue;
    if (detail::NameIs(t.text, "fixedtimestep")) {
      drop[k] = true;
    } else if (detail::NameIs(t.text, "timestep")) {
      drop[k] = true;
      if (k + 1 < tokens.size() && !tokens[k + 1].option) drop[k + 1] = true;
    }
  }

  std::string out;
  std::size_t copied = 0;  // input bytes up to here are in `out` or were dropped
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    if (!drop[k]) continue;
    // Keep the bytes before the token, minus the gap that separated it from the previous token.
    std::size_t gapStart = k == 0 ? 0 : tokens[k - 1].end;
    if (gapStart < copied) gapStart = copied;
    out.append(commandLine.substr(copied, gapStart - copied));
    copied = tokens[k].end;
    result.removed.emplace_back(tokens[k].text);
  }
  if (result.removed.empty()) return result;
  out.append(commandLine.substr(copied));
  result.commandLine = std::move(out);
  return result;
}

}  // namespace nevr_cmdline
