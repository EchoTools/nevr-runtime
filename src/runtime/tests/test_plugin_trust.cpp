// test_plugin_trust.cpp — the pins behind the plugin gate (ADR 0010, #741): the root certificate bytes
// match their digest and are the Microsoft root, the profile EKU is a whole OID, the denylist entries parse,
// and the mode literal says what the build enforces.

#include "runtime/ext/plugin_trust.h"

#include <gtest/gtest.h>

#include <wincrypt.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace trust = nevr_plugin_trust;

std::string Sha256Hex(const uint8_t* data, size_t size) {
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  EXPECT_TRUE(CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT));
  EXPECT_TRUE(CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash));
  EXPECT_TRUE(CryptHashData(hash, data, static_cast<DWORD>(size), 0));
  BYTE out[32] = {};
  DWORD n = sizeof(out);
  EXPECT_TRUE(CryptGetHashParam(hash, HP_HASHVAL, out, &n, 0));
  CryptDestroyHash(hash);
  CryptReleaseContext(prov, 0);
  std::string hex;
  for (DWORD i = 0; i < n; ++i) {
    char b[3];
    std::snprintf(b, sizeof(b), "%02x", out[i]);
    hex += b;
  }
  return hex;
}

bool IsOid(const std::string& s) {
  if (s.empty() || s.front() == '.' || s.back() == '.') return false;
  bool lastDot = false;
  for (char c : s) {
    if (c == '.') {
      if (lastDot) return false;
      lastDot = true;
    } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      lastDot = false;
    } else {
      return false;
    }
  }
  return s.find('.') != std::string::npos;
}

TEST(PluginTrust, RootBytesHashToThePinnedDigest) {
  EXPECT_EQ(Sha256Hex(trust::RootDer(), trust::RootDerSize()), trust::RootSha256Hex());
}

TEST(PluginTrust, RootIsTheSelfSignedMicrosoftIdentityVerificationRoot) {
  const CERT_CONTEXT* cert =
      CertCreateCertificateContext(X509_ASN_ENCODING, trust::RootDer(), static_cast<DWORD>(trust::RootDerSize()));
  ASSERT_NE(cert, nullptr) << "the pinned root does not parse: error " << GetLastError();
  char subject[256] = {};
  CertNameToStrA(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR, subject, sizeof(subject));
  EXPECT_NE(std::strstr(subject, "Microsoft Identity Verification Root Certificate Authority 2020"), nullptr)
      << subject;
  EXPECT_TRUE(CertCompareCertificateName(X509_ASN_ENCODING, &cert->pCertInfo->Subject, &cert->pCertInfo->Issuer));
  CertFreeCertificateContext(cert);
}

TEST(PluginTrust, ProductionPolicyPinsTheRootAndAWholeProfileEku) {
  const auto& policy = trust::ProductionPolicy();
  ASSERT_EQ(policy.rootDer.size(), trust::RootDerSize());
  EXPECT_EQ(std::memcmp(policy.rootDer.data(), trust::RootDer(), policy.rootDer.size()), 0);
  ASSERT_FALSE(policy.profileEkus.empty());
  for (const std::string& eku : policy.profileEkus) {
    EXPECT_TRUE(IsOid(eku)) << eku;
    // Every Artifact Signing customer shares the 1.3.6.1.4.1.311.97. prefix: the pin is more than that.
    EXPECT_EQ(eku.rfind("1.3.6.1.4.1.311.97.", 0), 0u) << eku;
    EXPECT_GT(eku.size(), std::strlen("1.3.6.1.4.1.311.97.") + 1) << eku;
  }
}

TEST(PluginTrust, EveryDenylistEntryParses) {
  const auto& policy = trust::ProductionPolicy();
  for (const std::string& serial : policy.deniedSerials) {
    ASSERT_FALSE(serial.empty());
    EXPECT_NE(serial.front(), '0') << serial;  // no leading zeros: the verifier compares the stripped form
    for (char c : serial) EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)) != 0 && !std::isupper(static_cast<unsigned char>(c))) << serial;
  }
  for (const auto& w : policy.deniedWindows) EXPECT_LE(w.from, w.until);
  for (const std::string& eku : policy.deniedProfileEkus) EXPECT_TRUE(IsOid(eku)) << eku;
}

TEST(PluginTrust, ModeLiteralNamesTheModeTheBuildEnforces) {
  const std::string literal = trust::ModeLiteral();
  EXPECT_EQ(literal, std::string("NEVR-PLUGIN-TRUST ") + trust::ModeWord(trust::BuildMode()));
}

// The real signed file (#745): the v5.4.3 BugSplat64.dll the release-signer pipeline produced
// (https://echo.taxi/nevr-runtime/windows/nevr-runtime-v5.4.3-windows-signed.zip). `just test-auth-unit` fetches it
// by digest (tools/fetch_real_signed_dll.py) and names it in NEVR_REAL_SIGNED_DLL; a missing variable fails every
// test here loudly, because a pin nobody compared against a real file is the gap this closes.
constexpr const char kRealSignedSha256[] = "a97767e62721503d4e249b016b734c42619a5aaa209ecdcd7a25a434f6d2a629";
constexpr int64_t kRealSigningTime = 1792621150;      // the file's RFC 3161 timestamp: 2026-10-21T22:19:10Z
constexpr int64_t kAfterTheLeafExpired = 1798761600;  // 2027-01-01; the leaf's validity ended 2026-10-23T20:51:08Z
constexpr size_t kFlippedOffset = 0x1000;             // inside the first section's raw data: the digest covers it

// A Unix path as Wine takes it from a Windows API.
std::string WinPath(const std::string& p) {
  if (p.empty() || p[0] != '/') return p;
  std::string out = "Z:";
  for (char c : p) out += (c == '/') ? '\\' : c;
  return out;
}

class RealSigned : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* path = std::getenv("NEVR_REAL_SIGNED_DLL");
    ASSERT_NE(path, nullptr) << "NEVR_REAL_SIGNED_DLL is not set: run tools/fetch_real_signed_dll.py <dir> and export "
                                "the path it prints (just test-auth-unit does)";
    path_ = path;
    std::ifstream in(path_, std::ios::binary);
    ASSERT_TRUE(in.good()) << path_;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    bytes_.assign(raw.begin(), raw.end());
    ASSERT_EQ(Sha256Hex(bytes_.data(), bytes_.size()), kRealSignedSha256) << "not the real signed v5.4.3 file";
  }

  nevr_plugin_signature::Verdict Verify(const std::string& winPath, const nevr_plugin_signature::Policy& policy,
                                        int64_t now) {
    HANDLE h = CreateFileA(winPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(h, INVALID_HANDLE_VALUE) << winPath << " error " << GetLastError();
    if (h == INVALID_HANDLE_VALUE) return {};
    const auto verdict = nevr_plugin_signature::VerifyPluginFile(h, policy, now);
    CloseHandle(h);
    return verdict;
  }

  nevr_plugin_signature::Verdict VerifyReal(const nevr_plugin_signature::Policy& policy, int64_t now) {
    return Verify(WinPath(path_), policy, now);
  }

  std::string path_;
  std::vector<uint8_t> bytes_;
};

TEST(PluginTrust, ThePinsAreConfirmedAndTheSubjectIsPinned) {
  EXPECT_TRUE(trust::PinsConfirmed());
  EXPECT_FALSE(trust::ProductionPolicy().subject.empty());
}

TEST_F(RealSigned, TheProductionPolicyAcceptsTheRealSignedFile) {
  const auto verdict = VerifyReal(trust::ProductionPolicy(), kAfterTheLeafExpired);
  EXPECT_TRUE(verdict.accepted()) << verdict.Describe();
  EXPECT_EQ(verdict.signingTime, kRealSigningTime);
}

TEST_F(RealSigned, TheTimestampKeepsTheFileValidAfterTheLeafExpired) {
  EXPECT_TRUE(VerifyReal(trust::ProductionPolicy(), kRealSigningTime + 3600).accepted());
  EXPECT_TRUE(VerifyReal(trust::ProductionPolicy(), kAfterTheLeafExpired).accepted());
}

TEST_F(RealSigned, OneChangedCodeByteIsRefused) {
  ASSERT_GT(bytes_.size(), kFlippedOffset);
  char dir[MAX_PATH] = {};
  ASSERT_GT(GetTempPathA(sizeof(dir), dir), 0u);
  const std::string copy = std::string(dir) + "nevr-real-signed-flipped.dll";
  std::vector<uint8_t> changed = bytes_;
  changed[kFlippedOffset] ^= 0x01;
  {
    std::ofstream out(copy, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(changed.data()), static_cast<std::streamsize>(changed.size()));
    ASSERT_TRUE(out.good());
  }
  const auto verdict = Verify(copy, trust::ProductionPolicy(), kAfterTheLeafExpired);
  DeleteFileA(copy.c_str());
  EXPECT_EQ(verdict.reason, nevr_plugin_signature::Reason::DigestMismatch) << verdict.Describe();
}

TEST_F(RealSigned, AnotherProfileEkuIsRefused) {
  nevr_plugin_signature::Policy policy = trust::ProductionPolicy();
  ASSERT_FALSE(policy.profileEkus.empty());
  policy.profileEkus[0].back() = policy.profileEkus[0].back() == '7' ? '8' : '7';
  const auto verdict = VerifyReal(policy, kAfterTheLeafExpired);
  EXPECT_EQ(verdict.reason, nevr_plugin_signature::Reason::MissingEku) << verdict.Describe();
}

TEST_F(RealSigned, AnotherSubjectIsRefusedAndTheRfc2253SpellingIsNotTheOneTheLoaderCompares) {
  nevr_plugin_signature::Policy policy = trust::ProductionPolicy();
  policy.subject = "CN=Someone Else, O=Someone Else, L=San Francisco, S=ca, C=US";
  EXPECT_EQ(VerifyReal(policy, kAfterTheLeafExpired).reason, nevr_plugin_signature::Reason::SubjectMismatch);
  // The RFC 2253 text of the same certificate (openssl -nameopt RFC2253) is not what CertNameToStrA prints.
  policy.subject = "CN=Andrew Bates,O=Andrew Bates,L=San Francisco,ST=ca,C=US";
  EXPECT_EQ(VerifyReal(policy, kAfterTheLeafExpired).reason, nevr_plugin_signature::Reason::SubjectMismatch);
}

}  // namespace
