// #1845: the CreateDirectoryW hook's log level for a relative path behind the \\?\ prefix. The decision is
// pure (modules/platform-compat/src/nt_path_log.h); the hook itself is Windows-only.

#include <gtest/gtest.h>

#include "modules/platform-compat/src/nt_path_log.h"

namespace {

using nevr_platform_compat::NtPathFixLevel;
using nevr_platform_compat::RelativeNtPathLogLevel;

// The game passes this path on native Windows as well as under Wine, so it is never a Warning.
TEST(RelativeNtPathLog, TheGamesOwnPathIsLoggedAtDebugOnEveryPlatform) {
  EXPECT_EQ(RelativeNtPathLogLevel(), NtPathFixLevel::Debug);
}

}  // namespace
