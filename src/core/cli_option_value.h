#pragma once

// #587: which token of a command line is the value of a value option. boot.cpp's parse used to take
// argv[i + 1] whatever it was, so `-timestep -no-console` lost -no-console. A token is an option by the
// game's own rule (cmdline_strip.h, the game's tokenizer FUN_1400d9000): it starts with '-' or '+' and the
// next character is not a digit, so `-5` is a value and `-no-console` is not. Pure, so the parse is
// unit-tested without the game.

#include <cstddef>

namespace nevr_cli {

inline bool IsOptionToken(const wchar_t* token) {
  if (token == nullptr) return false;
  if (token[0] != L'-' && token[0] != L'+') return false;
  return !(token[1] >= L'0' && token[1] <= L'9');
}

/// The value of the value option at argv[index]: argv[index + 1] when it exists and is not an option,
/// else nullptr (the option was given without a value, and the next token is left to the parse).
inline const wchar_t* OptionValue(int argc, wchar_t* const* argv, int index) {
  if (argv == nullptr || index < 0 || index + 1 >= argc) return nullptr;
  const wchar_t* next = argv[index + 1];
  if (next == nullptr || IsOptionToken(next)) return nullptr;
  return next;
}

}  // namespace nevr_cli
