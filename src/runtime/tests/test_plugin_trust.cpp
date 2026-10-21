// test_plugin_trust.cpp — the pins behind the plugin gate (ADR 0010, #741): the root certificate bytes
// match their digest and are the Microsoft root, the profile EKU is a whole OID, the denylist entries parse,
// and the mode literal says what the build enforces.

#include "runtime/ext/plugin_trust.h"

#include <gtest/gtest.h>

#include <wincrypt.h>

#include <cctype>
#include <cstring>
#include <string>

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

}  // namespace
