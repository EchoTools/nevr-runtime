#include <gtest/gtest.h>

#include <cstdint>

#include "core/schannel_cred_guard.h"

namespace {

// Stand-ins with the two real structs' shared prefix; only dwVersion matters to the guard.
struct FakeCred {
  std::uint32_t dwVersion;
  std::uint32_t rest[16];
};

TEST(SchannelCredGuard, OnlyAVersion4StructIsEditable) {
  FakeCred legacy{nevr::kSchannelCredVersion, {}};
  FakeCred modern{nevr::kSchCredentialsVersion, {}};
  EXPECT_TRUE(nevr::IsLegacySchannelCred(&legacy));
  EXPECT_FALSE(nevr::IsLegacySchannelCred(&modern)) << "libcurl passes version 5; editing it corrupts it";
  EXPECT_FALSE(nevr::IsLegacySchannelCred(nullptr));
}

TEST(SchannelCredGuard, TheVersionConstantsAreWindowsOwn) {
  EXPECT_EQ(nevr::kSchannelCredVersion, 0x00000004u);    // SCHANNEL_CRED_VERSION
  EXPECT_EQ(nevr::kSchCredentialsVersion, 0x00000005u);  // SCH_CREDENTIALS_VERSION
  FakeCred unknown{0x12345678u, {}};
  EXPECT_EQ(nevr::SchannelAuthVersion(&unknown), 0x12345678u);
  EXPECT_EQ(nevr::SchannelAuthVersion(nullptr), 0u);
  EXPECT_FALSE(nevr::IsLegacySchannelCred(&unknown));
}

}  // namespace
