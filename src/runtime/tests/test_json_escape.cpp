// #48: one JSON escaper for the boot log (no heap, loader-lock safe) and the runtime log.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "core/json_escape.h"

namespace {

std::string Into(const std::string& in, int size = 256) {
  std::string buf(static_cast<std::size_t>(size), 'X');
  const int n = JsonEscape::Into(in.data(), static_cast<int>(in.size()), &buf[0], size);
  EXPECT_EQ(buf[static_cast<std::size_t>(n)], '\0') << "NUL-terminated";
  return std::string(buf.c_str(), static_cast<std::size_t>(n));
}

TEST(JsonEscape, EscapesTheStructuralCharacters) {
  EXPECT_EQ(Into("a\"b\\c\nd\re\tf"), "a\\\"b\\\\c\\nd\\re\\tf");
}

// The boot log's old escaper passed other control characters through, which is invalid JSON
// (an ANSI escape, 0x1b, in a message produced an unparseable line).
TEST(JsonEscape, EscapesEveryOtherControlCharacter) {
  EXPECT_EQ(Into(std::string("\x1b[31m", 5)), "\\u001b[31m");
  EXPECT_EQ(Into(std::string("\x01\x1f", 2)), "\\u0001\\u001f");
  EXPECT_EQ(Into("plain text 123"), "plain text 123");
}

TEST(JsonEscape, NeverOverrunsOrSplitsAnEscape) {
  // "\u001b" is 6 bytes; with room for 5 + NUL it must not be cut in half.
  EXPECT_EQ(Into(std::string("a\x1b", 2), 7), "a");
  EXPECT_EQ(Into(std::string("a\x1b", 2), 8), "a\\u001b");
  EXPECT_EQ(Into("abcdef", 4), "abc");
  char tiny[1] = {'X'};
  EXPECT_EQ(JsonEscape::Into("abc", 3, tiny, 1), 0);
  EXPECT_EQ(tiny[0], '\0');
  EXPECT_EQ(JsonEscape::Into("abc", 3, nullptr, 10), 0);
}

TEST(JsonEscape, AppendToMatchesInto) {
  const std::string in = std::string("q\"\\\n\x02\x1b z", 9);
  std::string out = "prefix:";
  JsonEscape::AppendTo(out, in.data(), static_cast<int>(in.size()));
  EXPECT_EQ(out, "prefix:" + Into(in, 512));
}

}  // namespace
