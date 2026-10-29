// plugin_signature.h — decides whether a plugin DLL was signed through the release-signer pipeline
// (ADR 0010, #741).
//
// PURE on purpose, like plugin_manifest.h: one function over an open file handle and a policy value, no
// Log, no globals, no game. The loader calls it (a later change); the test links it standalone and feeds it
// signed fixtures. The decision does not rest on WinVerifyTrust: under Wine it returns success for a
// signature from a root nobody trusts (ADR 0010 section 4). The digest, the signature, the chain, the
// EKU, the timestamp and the denylist are each checked here.
//
// Windows only (crypt32, advapi32).

#pragma once

#include <windows.h>
#include <wincrypt.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nevr_plugin_signature {

// The signature of CertCreateCertificateChainEngine, which the verifier calls through (see TestHook_SetEngineCreate).
using EngineCreateFn = BOOL (*)(CERT_CHAIN_ENGINE_CONFIG*, HCERTCHAINENGINE*);

// Why a file was accepted or refused. The name (ReasonName) is the word the refusal log line carries.
enum class Reason {
  Accepted,
  Unreadable,            // the file could not be read, or is not a PE
  Unsigned,              // no embedded Authenticode signature (a catalog signature does not count)
  MalformedSignature,    // the signature block does not parse, or has an unsupported shape
  DigestMismatch,        // the file's bytes do not hash to the digest the signature covers
  SignatureInvalid,      // the CMS signature does not verify under the signer certificate
  ChainStatus,           // the chain to the pinned root has a nonzero dwErrorStatus
  NoTimestampAndExpired, // no timestamp, and the signer certificate is not valid now
  TimestampInvalid,      // a timestamp is present and does not verify
  TimestampNotBound,     // the timestamp's imprint is not the hash of this signature
  MissingEku,            // the signer certificate lacks code signing, or carries no accepted profile EKU
  SubjectMismatch,       // the signer subject is not the pinned subject
  DenylistedSerial,
  DenylistedWindow,
  DenylistedProfile,
};

const char* ReasonName(Reason reason);

// An inclusive span of unix seconds.
struct TimeWindow {
  int64_t from = 0;
  int64_t until = 0;
};

// What a signature must show. Values come from release-signer (ADR 0010 section 2); a test passes its own.
struct Policy {
  std::vector<uint8_t> rootDer;                // the pinned root certificate, DER
  std::vector<std::string> profileEkus;        // accepted profile EKU OIDs (whole-string compare)
  std::string subject;                         // pinned signer subject, as CertNameToStrA(X500 | REVERSE) prints it; empty = not pinned
  std::vector<std::string> deniedSerials;      // leaf serial numbers, lowercase hex without leading zeros
  std::vector<TimeWindow> deniedWindows;       // signing times (timestamp, else now) refused
  std::vector<std::string> deniedProfileEkus;  // profile EKUs refused even when listed in profileEkus
};

struct Verdict {
  Reason reason = Reason::Unreadable;
  std::string detail;         // what the reason line adds: a chain-status name list, an EKU, a serial, an error
  uint32_t chainStatus = 0;   // dwErrorStatus when reason is ChainStatus or NoTimestampAndExpired
  int64_t signingTime = 0;    // the timestamp's genTime (unix seconds) when one verified, else 0
  uint32_t engineCbSize = 0;   // when accepted: the CERT_CHAIN_ENGINE_CONFIG cbSize the chain engine was made with (88, or 80 on a Wine before 11.5)

  bool accepted() const { return reason == Reason::Accepted; }
  // "<reason>" or "<reason>: <detail>"; no file name, the caller adds it.
  std::string Describe() const;
};

// The names of the CERT_TRUST_* error bits set in `status`, e.g. "CERT_TRUST_IS_PARTIAL_CHAIN".
std::string ChainStatusNames(uint32_t status);

// Verifies the PE file open on `file` (read access). `nowUnix` is the current time, passed in so a test can
// fix it. Reads the file from offset 0; does not close it or rely on the file pointer afterwards.
// Never throws: an exception while verifying (an allocation failure reading a large file) is a refusal.
Verdict VerifyPluginFile(HANDLE file, const Policy& policy, int64_t nowUnix);

// The DLL names a PE file asks the loader for, lowercased and without duplicates: its import table, its
// delay-load import table and the DLL half of every forwarded export ("DLL.Symbol", the export's RVA inside the
// export directory), the last as "dll.dll". Reads from offset 0 like VerifyPluginFile. False, with `error`, when
// any of the three tables does not parse to the end of the file: a plugin whose dependencies cannot be listed
// cannot be vouched for.
struct Dependencies {
  std::vector<std::string> imports;
  std::vector<std::string> delayImports;
  std::vector<std::string> forwarders;
};
bool ListDependencies(HANDLE file, Dependencies* out, std::string* error);

#if defined(NEVR_HOST_TESTING)
// Test only, and only in a unit compiled with NEVR_HOST_TESTING (nevr_host_core): makes the next VerifyPluginFile
// calls raise std::bad_alloc inside, to prove they come back as a refusal. No shipped DLL carries it.
void TestHook_ThrowInVerify(bool enable);
// Test only, same guard: replaces CertCreateCertificateChainEngine inside the verifier (nullptr restores it), so a test
// can stand in for a Wine that rejects the 88-byte struct (#1262) and see every cbSize the verifier tries.
void TestHook_SetEngineCreate(EngineCreateFn create);
#endif

}  // namespace nevr_plugin_signature
