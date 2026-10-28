// test_self_provenance.cpp — the runtime's own-signature label (#842): a signed release, a self-built (unsigned or
// refused) build and a dev version each get their word and reason, and the module's own file goes through the verifier.
//
// The decision is Decide (pure); the verifier's verdicts are built directly, so no certificate is needed. The last test
// reads this test binary's own file, which is unsigned, so it proves the file path end to end.

#include <gtest/gtest.h>

#include <ctime>
#include <string>

#include "runtime/ext/self_provenance.h"

namespace {

using nevr_plugin_signature::Reason;
using nevr_plugin_signature::Verdict;

Verdict Accepted() {
  Verdict v;
  v.reason = Reason::Accepted;
  return v;
}

Verdict Refused(Reason reason, const std::string& detail = "") {
  Verdict v;
  v.reason = reason;
  v.detail = detail;
  return v;
}

}  // namespace

TEST(SelfProvenance, SignedReleaseIsOfficial) {
  const auto result = nevr_self_provenance::Decide("5.7.0", false, Accepted());
  EXPECT_STREQ(result.word, "signed-release");
  EXPECT_EQ(result.version, "5.7.0");
  EXPECT_EQ(result.reason, "");
}

TEST(SelfProvenance, UnsignedReleaseVersionIsSelfBuilt) {
  const auto result = nevr_self_provenance::Decide("5.7.0", false, Refused(Reason::Unsigned));
  EXPECT_STREQ(result.word, "self-built");
  EXPECT_EQ(result.version, "5.7.0");
  EXPECT_EQ(result.reason, nevr_plugin_signature::ReasonName(Reason::Unsigned));
}

TEST(SelfProvenance, SignerMismatchIsSelfBuiltWithItsReason) {
  // A file signed by someone else fails the EKU or the chain; the reason carries the verifier's word and detail.
  const auto result = nevr_self_provenance::Decide("5.7.0", false, Refused(Reason::MissingEku, "1.2.3.4"));
  EXPECT_STREQ(result.word, "self-built");
  EXPECT_NE(result.reason.find(nevr_plugin_signature::ReasonName(Reason::MissingEku)), std::string::npos) << result.reason;
  EXPECT_NE(result.reason.find("1.2.3.4"), std::string::npos) << result.reason;
}

TEST(SelfProvenance, DevVersionIsDevEvenWhenSigned) {
  // A dev version is never a CI build on a tag, so the release pipeline never signs one; the word says dev regardless.
  const auto result = nevr_self_provenance::Decide("5.7.1-dev.3+abc1234", false, Accepted());
  EXPECT_STREQ(result.word, "dev");
  EXPECT_EQ(result.version, "5.7.1-dev.3+abc1234");
  EXPECT_EQ(result.reason, "dev version");
}

TEST(SelfProvenance, DevBuildFlagIsDevEvenOnAReleaseVersion) {
  const auto result = nevr_self_provenance::Decide("5.7.0", true, Refused(Reason::Unsigned));
  EXPECT_STREQ(result.word, "dev");
  EXPECT_EQ(result.reason, "dev build");
}

TEST(SelfProvenance, DevVersionOfADevBuildSaysBoth) {
  const auto result = nevr_self_provenance::Decide("5.7.1-dev.3+abc1234", true, Refused(Reason::Unsigned));
  EXPECT_STREQ(result.word, "dev");
  EXPECT_EQ(result.reason, "dev version and dev build");
}

TEST(SelfProvenance, OwnModuleIsVerifiedAndThisUnsignedTestBinaryIsSelfBuilt) {
  // An empty policy trusts no root, so whatever the file's signature says, it is refused; this binary carries none.
  nevr_plugin_signature::Policy policy;
  const auto result = nevr_self_provenance::CheckOwnModule("5.7.0", false, policy, static_cast<int64_t>(std::time(nullptr)));
  EXPECT_STREQ(result.word, "self-built");
  EXPECT_EQ(result.version, "5.7.0");
  EXPECT_EQ(result.reason.rfind(nevr_plugin_signature::ReasonName(Reason::Unsigned), 0), 0u) << result.reason;
}

TEST(SelfProvenance, OwnModuleOfADevVersionIsDevWithoutTheSignatureMattering) {
  nevr_plugin_signature::Policy policy;
  const auto result = nevr_self_provenance::CheckOwnModule("5.7.1-dev.3+abc1234", false, policy,
                                                           static_cast<int64_t>(std::time(nullptr)));
  EXPECT_STREQ(result.word, "dev");
}
