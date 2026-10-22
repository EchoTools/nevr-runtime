// test_plugin_signature.cpp — the plugin signature verifier (ADR 0010, #741) against signed fixtures.
//
// The fixtures are made by tools/gen_plugin_signature_fixtures.py (throwaway PKI, osslsigncode, offline
// timestamps) into the directory named by NEVR_SIGFIX_DIR; `just test-auth-unit` makes them and sets it.
// A missing directory fails every test loudly: a verifier test that skips is a verifier nobody tested.

#include "runtime/ext/plugin_signature.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using nevr_plugin_signature::Policy;
using nevr_plugin_signature::Reason;
using nevr_plugin_signature::Verdict;

constexpr int64_t kDay = 86400;

// A Unix path as Wine takes it from a Windows API.
std::string WinPath(const std::string& p) {
  if (p.empty() || p[0] != '/') return p;
  std::string out = "Z:";
  for (char c : p) out += (c == '/') ? '\\' : c;
  return out;
}

std::vector<uint8_t> ReadBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string s = ss.str();
  return {s.begin(), s.end()};
}

class PluginSignature : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* dir = std::getenv("NEVR_SIGFIX_DIR");
    ASSERT_NE(dir, nullptr) << "NEVR_SIGFIX_DIR is not set: run tools/gen_plugin_signature_fixtures.py <dir> "
                               "and export it (just test-auth-unit does)";
    dir_ = dir;
    std::ifstream in(dir_ + "/fixtures.json");
    ASSERT_TRUE(in.good()) << "no fixtures.json in " << dir_;
    manifest_ = nlohmann::json::parse(in);
    policy_.rootDer = ReadBytes(dir_ + "/" + manifest_["root_der"].get<std::string>());
    ASSERT_FALSE(policy_.rootDer.empty());
    policy_.profileEkus = {manifest_["profile_eku"].get<std::string>()};
    now_ = manifest_["now"].get<int64_t>();
  }

  Verdict Verify(const std::string& name) {
    const std::string path = WinPath(dir_ + "/" + manifest_["files"][name].get<std::string>());
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(h, INVALID_HANDLE_VALUE) << path << " error " << GetLastError();
    if (h == INVALID_HANDLE_VALUE) return {};
    const Verdict v = nevr_plugin_signature::VerifyPluginFile(h, policy_, now_);
    CloseHandle(h);
    return v;
  }

  std::string dir_;
  nlohmann::json manifest_;
  Policy policy_;
  int64_t now_ = 0;
};

TEST_F(PluginSignature, GoodFileIsAccepted) {
  const Verdict v = Verify("good");
  EXPECT_TRUE(v.accepted()) << v.Describe();
  EXPECT_EQ(v.signingTime, manifest_["stamp_time"]["good"].get<int64_t>());
}

TEST_F(PluginSignature, GoodFileWithoutTimestampIsAcceptedWhileTheCertificateIsValid) {
  const Verdict v = Verify("good_unstamped");
  EXPECT_TRUE(v.accepted()) << v.Describe();
  EXPECT_EQ(v.signingTime, 0);
}

TEST_F(PluginSignature, UnsignedFileIsRefused) {
  EXPECT_EQ(Verify("unsigned").reason, Reason::Unsigned);
}

TEST_F(PluginSignature, OtherSignersProfileEkuIsRefused) {
  const Verdict v = Verify("wrong_eku");
  EXPECT_EQ(v.reason, Reason::MissingEku) << v.Describe();
  EXPECT_EQ(v.detail, "no accepted profile EKU");
}

TEST_F(PluginSignature, RootNotPinnedIsPartialChain) {
  const Verdict v = Verify("other_root");
  EXPECT_EQ(v.reason, Reason::ChainStatus) << v.Describe();
  EXPECT_EQ(v.chainStatus & 0x10000u, 0x10000u) << v.Describe();  // CERT_TRUST_IS_PARTIAL_CHAIN
}

TEST_F(PluginSignature, RootForgedInsideTheFileIsUntrusted) {
  const Verdict v = Verify("forged_root");
  EXPECT_EQ(v.reason, Reason::ChainStatus) << v.Describe();
  EXPECT_NE(v.chainStatus, 0u);
  // The forged root has the pinned root's subject and serial but not its key: it is not the pinned root, so the
  // chain engine calls it untrusted.
  EXPECT_EQ(v.chainStatus, 0x20u) << v.Describe();  // CERT_TRUST_IS_UNTRUSTED_ROOT
}

TEST_F(PluginSignature, ChangedCodeByteIsADigestMismatch) {
  EXPECT_EQ(Verify("tampered_code").reason, Reason::DigestMismatch);
}

TEST_F(PluginSignature, ChangedSignatureByteIsRefused) {
  EXPECT_EQ(Verify("tampered_signature").reason, Reason::SignatureInvalid);
}

TEST_F(PluginSignature, ExpiredCertificateWithATimestampInsideItsWindowIsAccepted) {
  const Verdict v = Verify("expired_stamped_inside");
  EXPECT_TRUE(v.accepted()) << v.Describe();
  EXPECT_EQ(v.signingTime, manifest_["stamp_time"]["expired_stamped_inside"].get<int64_t>());
}

TEST_F(PluginSignature, ExpiredCertificateWithoutATimestampIsRefused) {
  const Verdict v = Verify("expired_unstamped");
  EXPECT_EQ(v.reason, Reason::NoTimestampAndExpired) << v.Describe();
  EXPECT_NE(v.chainStatus & 0x1u, 0u);
}

TEST_F(PluginSignature, TimestampAfterTheCertificateExpiredIsRefused) {
  const Verdict v = Verify("stamped_after_expiry");
  EXPECT_EQ(v.reason, Reason::ChainStatus) << v.Describe();
  EXPECT_NE(v.chainStatus & 0x1u, 0u);  // CERT_TRUST_IS_NOT_TIME_VALID at the stamped time
}

TEST_F(PluginSignature, TimestampTokenCopiedFromAnotherSignatureIsNotBound) {
  const Verdict v = Verify("token_copied");
  EXPECT_EQ(v.reason, Reason::TimestampNotBound) << v.Describe();
}

TEST_F(PluginSignature, TimestampFromAnAuthorityTheRootDidNotIssueIsRefused) {
  const Verdict v = Verify("tsa_untrusted");
  EXPECT_EQ(v.reason, Reason::TimestampInvalid) << v.Describe();
  EXPECT_EQ(v.chainStatus, 0x20u) << v.Describe();  // CERT_TRUST_IS_UNTRUSTED_ROOT
}

// The two attacks CMSG_CTRL_VERIFY_SIGNATURE does not stop under Wine: it checks the signature over the signed
// attributes but never compares messageDigest to the content. Both fixtures carry a real signature.
TEST_F(PluginSignature, ContentSwappedUnderAGenuineSignatureIsRefused) {
  const Verdict v = Verify("content_swap");
  EXPECT_EQ(v.reason, Reason::SignatureInvalid) << v.Describe();
  EXPECT_NE(v.detail.find("messageDigest differs"), std::string::npos) << v.Describe();
}

TEST_F(PluginSignature, BackdatedTimestampTokenIsRefused) {
  const Verdict v = Verify("backdated_token");
  EXPECT_EQ(v.reason, Reason::TimestampInvalid) << v.Describe();
  EXPECT_NE(v.detail.find("messageDigest differs"), std::string::npos) << v.Describe();
}

// Each of these carries a GENUINE RSA signature (re-made with the throwaway leaf key over edited signed attributes),
// so only the structural check under test can refuse it.
TEST_F(PluginSignature, SecondMessageDigestAttributeIsRefused) {
  const Verdict v = Verify("two_message_digests");
  EXPECT_EQ(v.reason, Reason::MalformedSignature) << v.Describe();
  EXPECT_EQ(v.detail, "messageDigest attribute");
}

TEST_F(PluginSignature, MissingContentTypeAttributeIsRefused) {
  const Verdict v = Verify("no_content_type");
  EXPECT_EQ(v.reason, Reason::MalformedSignature) << v.Describe();
  EXPECT_EQ(v.detail, "signed attributes lack contentType or messageDigest");
}

TEST_F(PluginSignature, ContentTypeAttributeThatDisagreesWithTheContentIsRefused) {
  const Verdict v = Verify("other_content_type");
  EXPECT_EQ(v.reason, Reason::MalformedSignature) << v.Describe();
  EXPECT_EQ(v.detail, "contentType attribute");
}

TEST_F(PluginSignature, SignatureAlgorithmOffTheAllowlistIsRefused) {
  const Verdict v = Verify("sha1_signature_algorithm");
  EXPECT_EQ(v.reason, Reason::MalformedSignature) << v.Describe();
  EXPECT_NE(v.detail.find("signature algorithm 1.2.840.113549.1.1.5 refused"), std::string::npos) << v.Describe();
}

TEST_F(PluginSignature, GenuineTokenDatedInTheFutureIsRefused) {
  const Verdict v = Verify("token_future");
  EXPECT_EQ(v.reason, Reason::TimestampInvalid) << v.Describe();
  EXPECT_EQ(v.detail, "the token's time is in the future");
}

TEST_F(PluginSignature, Sha1SignedFileIsRefused) {
  const Verdict v = Verify("sha1_digest");
  EXPECT_EQ(v.reason, Reason::MalformedSignature) << v.Describe();
  EXPECT_NE(v.detail.find("SHA-256 or stronger"), std::string::npos) << v.Describe();
}

TEST_F(PluginSignature, SignerWithA1024BitKeyIsRefused) {
  const Verdict v = Verify("weak_key");
  EXPECT_EQ(v.reason, Reason::ChainStatus) << v.Describe();
  EXPECT_EQ(v.detail, "the signer's key is too weak");
}

TEST_F(PluginSignature, TimeStampAuthorityWithA1024BitKeyIsRefused) {
  const Verdict v = Verify("tsa_weak");
  EXPECT_EQ(v.reason, Reason::TimestampInvalid) << v.Describe();
  EXPECT_EQ(v.detail, "the time-stamp key is too weak");
}

TEST_F(PluginSignature, AnExceptionWhileVerifyingIsARefusalNotAThrow) {
  nevr_plugin_signature::TestHook_ThrowInVerify(true);
  const Verdict v = Verify("good");
  nevr_plugin_signature::TestHook_ThrowInVerify(false);
  EXPECT_EQ(v.reason, Reason::Unreadable) << v.Describe();
  EXPECT_NE(v.detail.find("exception while verifying"), std::string::npos) << v.Describe();
  EXPECT_TRUE(Verify("good").accepted());
}

TEST_F(PluginSignature, DeniedSerialIsRefused) {
  policy_.deniedSerials = {manifest_["serials"]["good"].get<std::string>()};
  const Verdict v = Verify("good");
  EXPECT_EQ(v.reason, Reason::DenylistedSerial) << v.Describe();
}

TEST_F(PluginSignature, OtherSerialInTheDenylistChangesNothing) {
  policy_.deniedSerials = {manifest_["serials"]["old"].get<std::string>()};
  EXPECT_TRUE(Verify("good").accepted());
}

TEST_F(PluginSignature, DeniedSigningWindowRefusesATimestampInsideIt) {
  const int64_t t = manifest_["stamp_time"]["good"].get<int64_t>();
  policy_.deniedWindows = {{t - kDay, t + kDay}};
  EXPECT_EQ(Verify("good").reason, Reason::DenylistedWindow);
}

TEST_F(PluginSignature, DeniedSigningWindowLeavesALaterTimestampAlone) {
  const int64_t t = manifest_["stamp_time"]["good"].get<int64_t>();
  policy_.deniedWindows = {{t - 3 * kDay, t - 2 * kDay}};
  EXPECT_TRUE(Verify("good").accepted());
}

TEST_F(PluginSignature, DeniedProfileEkuIsRefused) {
  policy_.deniedProfileEkus = {manifest_["profile_eku"].get<std::string>()};
  EXPECT_EQ(Verify("good").reason, Reason::DenylistedProfile);
}

TEST_F(PluginSignature, PinnedSubjectMatchesAndMismatches) {
  policy_.subject = manifest_["good_subject"].get<std::string>();
  EXPECT_TRUE(Verify("good").accepted());
  policy_.subject = "CN=Somebody Else";
  EXPECT_EQ(Verify("good").reason, Reason::SubjectMismatch);
}

TEST_F(PluginSignature, EkuIsComparedAsAWholeOidNotAPrefix) {
  // A policy that lists only a prefix of the real profile EKU accepts nothing.
  std::string eku = manifest_["profile_eku"].get<std::string>();
  policy_.profileEkus = {eku.substr(0, eku.rfind('.'))};
  EXPECT_EQ(Verify("good").reason, Reason::MissingEku);
}

// The loader holds the file it verified (FILE_SHARE_READ only) across the load (ADR 0010 section 1). This
// measures that Wine honours the sharing mode against another open of the same file: a writer, a deleter and
// a rename all fail while the handle is held.
TEST_F(PluginSignature, HeldVerifyHandleStopsAWriteDeleteOrRenameOfTheFile) {
  const std::string source = WinPath(dir_ + "/" + manifest_["files"]["good"].get<std::string>());
  const std::string copy = WinPath(dir_ + "/held_copy.dll");
  ASSERT_TRUE(CopyFileA(source.c_str(), copy.c_str(), FALSE));
  HANDLE held = CreateFileA(copy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(held, INVALID_HANDLE_VALUE);
  EXPECT_TRUE(nevr_plugin_signature::VerifyPluginFile(held, policy_, now_).accepted());

  HANDLE writer = CreateFileA(copy.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  EXPECT_EQ(writer, INVALID_HANDLE_VALUE) << "a writer opened the file while it was held";
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
  if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
  EXPECT_FALSE(DeleteFileA(copy.c_str()));
  EXPECT_FALSE(MoveFileA(copy.c_str(), (copy + ".moved").c_str()));

  CloseHandle(held);
  EXPECT_TRUE(DeleteFileA(copy.c_str())) << "the file could not be removed after the handle closed";
}

TEST_F(PluginSignature, ChainStatusNamesTheBits) {
  EXPECT_EQ(nevr_plugin_signature::ChainStatusNames(0x10000), "CERT_TRUST_IS_PARTIAL_CHAIN");
  EXPECT_EQ(nevr_plugin_signature::ChainStatusNames(0x21),
            "CERT_TRUST_IS_NOT_TIME_VALID,CERT_TRUST_IS_UNTRUSTED_ROOT");
  EXPECT_EQ(nevr_plugin_signature::ChainStatusNames(0), "");
}

}  // namespace
