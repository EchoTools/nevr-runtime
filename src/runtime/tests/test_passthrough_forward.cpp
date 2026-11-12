#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/log/passthrough_forward.h"

using nevr_passthrough_forward::CanForward;

namespace {

constexpr const char* kPlainLine = "[NEVR.LOGFILTER] repeated count=3 window_s=5";
constexpr const char* kConversionLine = "[NEVR.PATCH] hook value=%d";

}  // namespace

// #548: a format with no conversion needs no arguments, so it is forwarded even when the engine passed null varargs.
TEST(PassthroughForward, PlainFormatIsForwardedWithNullVarargs) {
  EXPECT_TRUE(CanForward(kPlainLine, nullptr));
}

// A conversion with null varargs would make the engine read an argument that is not there.
TEST(PassthroughForward, ConversionWithNullVarargsIsNotForwarded) {
  EXPECT_FALSE(CanForward(kConversionLine, nullptr));
}

TEST(PassthroughForward, FormatWithVarargsIsForwarded) {
  std::int64_t args[2] = {7, 0};
  EXPECT_TRUE(CanForward(kConversionLine, args));
  EXPECT_TRUE(CanForward(kPlainLine, args));
}

TEST(PassthroughForward, NullFormatIsNotForwarded) {
  std::int64_t args[2] = {7, 0};
  EXPECT_FALSE(CanForward(nullptr, args));
  EXPECT_FALSE(CanForward(nullptr, nullptr));
}
