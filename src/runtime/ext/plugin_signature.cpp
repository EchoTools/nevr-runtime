// plugin_signature.cpp — see plugin_signature.h (ADR 0010, #741).

#include "runtime/ext/plugin_signature.h"

#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#ifndef CALG_SHA_256
#define CALG_SHA_256 (ALG_CLASS_HASH | ALG_TYPE_ANY | 12)
#define CALG_SHA_384 (ALG_CLASS_HASH | ALG_TYPE_ANY | 13)
#define CALG_SHA_512 (ALG_CLASS_HASH | ALG_TYPE_ANY | 14)
#endif

namespace nevr_plugin_signature {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr size_t kMaxFileBytes = size_t{512} * 1024 * 1024;
constexpr DWORD kEncoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

constexpr const char* kOidCodeSigning = "1.3.6.1.5.5.7.3.3";
constexpr const char* kOidTimeStamping = "1.3.6.1.5.5.7.3.8";
constexpr const char* kOidRfc3161Token = "1.3.6.1.4.1.311.3.3.1";
constexpr const char* kOidTstInfo = "1.2.840.113549.1.9.16.1.4";
constexpr const char* kOidSha256 = "2.16.840.1.101.3.4.2.1";
constexpr const char* kOidSha384 = "2.16.840.1.101.3.4.2.2";
constexpr const char* kOidSha512 = "2.16.840.1.101.3.4.2.3";

Verdict Refuse(Reason reason, std::string detail = {}) {
  Verdict v;
  v.reason = reason;
  v.detail = std::move(detail);
  return v;
}

std::string Hex(uint32_t value) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%x", value);
  return buf;
}

// ---------------------------------------------------------------------------------------------------------
// RAII
// ---------------------------------------------------------------------------------------------------------

struct MsgCloser {
  void operator()(void* handle) const {
    if (handle != nullptr) CryptMsgClose(handle);
  }
};
using Msg = std::unique_ptr<void, MsgCloser>;

struct StoreCloser {
  void operator()(void* handle) const {
    if (handle != nullptr) CertCloseStore(handle, 0);
  }
};
using Store = std::unique_ptr<void, StoreCloser>;

struct CertFreer {
  void operator()(const CERT_CONTEXT* cert) const {
    if (cert != nullptr) CertFreeCertificateContext(cert);
  }
};
using Cert = std::unique_ptr<const CERT_CONTEXT, CertFreer>;

struct ChainFreer {
  void operator()(const CERT_CHAIN_CONTEXT* chain) const {
    if (chain != nullptr) CertFreeCertificateChain(chain);
  }
};
using Chain = std::unique_ptr<const CERT_CHAIN_CONTEXT, ChainFreer>;

struct EngineFreer {
  void operator()(void* engine) const {
    if (engine != nullptr) CertFreeCertificateChainEngine(engine);
  }
};
using Engine = std::unique_ptr<void, EngineFreer>;

// ---------------------------------------------------------------------------------------------------------
// DER: just enough to read an Authenticode signature, an indirect-data content and a TSTInfo
// ---------------------------------------------------------------------------------------------------------

struct Tlv {
  uint8_t tag = 0;
  const uint8_t* body = nullptr;
  size_t len = 0;
  size_t total = 0;  // header plus body
};

bool ReadTlv(const uint8_t* p, size_t n, Tlv* out) {
  if (n < 2) return false;
  const uint8_t tag = p[0];
  if ((tag & 0x1F) == 0x1F) return false;  // no high tag numbers in the structures we read
  size_t len = p[1];
  size_t hdr = 2;
  if ((len & 0x80) != 0) {
    const size_t k = len & 0x7F;
    if (k == 0 || k > 4 || n < 2 + k) return false;  // indefinite or oversized lengths are not DER
    len = 0;
    for (size_t i = 0; i < k; ++i) len = (len << 8) | p[2 + i];
    hdr = 2 + k;
  }
  if (len > n - hdr) return false;
  out->tag = tag;
  out->body = p + hdr;
  out->len = len;
  out->total = hdr + len;
  return true;
}

// The children of a constructed element, or false when they do not parse exactly to its end.
bool Children(const Tlv& parent, std::vector<Tlv>* kids) {
  kids->clear();
  size_t pos = 0;
  while (pos < parent.len) {
    Tlv kid;
    if (!ReadTlv(parent.body + pos, parent.len - pos, &kid)) return false;
    kids->push_back(kid);
    pos += kid.total;
  }
  return true;
}

std::string OidToString(const uint8_t* p, size_t n) {
  if (n == 0) return {};
  std::string out;
  const unsigned first = p[0];
  out = first < 80 ? std::to_string(first / 40) + "." + std::to_string(first % 40)
                   : "2." + std::to_string(first - 80);
  uint64_t acc = 0;
  for (size_t i = 1; i < n; ++i) {
    acc = (acc << 7) | (p[i] & 0x7F);
    if ((p[i] & 0x80) == 0) {
      out += "." + std::to_string(acc);
      acc = 0;
    }
  }
  return out;
}

bool IsOid(const Tlv& t) { return t.tag == 0x06; }

// ---------------------------------------------------------------------------------------------------------
// Hashing (CryptoAPI; BCrypt is deliberately not used in this DLL, see src/runtime/link)
// ---------------------------------------------------------------------------------------------------------

bool AlgFromOid(const std::string& oid, ALG_ID* alg) {
  if (oid == kOidSha256) *alg = CALG_SHA_256;
  else if (oid == kOidSha384) *alg = CALG_SHA_384;
  else if (oid == kOidSha512) *alg = CALG_SHA_512;
  else return false;
  return true;
}

class Hasher {
 public:
  explicit Hasher(ALG_ID alg) {
    if (!CryptAcquireContextW(&prov_, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return;
    if (!CryptCreateHash(prov_, alg, 0, 0, &hash_)) hash_ = 0;
  }
  ~Hasher() {
    if (hash_ != 0) CryptDestroyHash(hash_);
    if (prov_ != 0) CryptReleaseContext(prov_, 0);
  }
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  bool ok() const { return hash_ != 0; }
  bool Update(const uint8_t* p, size_t n) {
    if (n == 0) return true;
    return CryptHashData(hash_, p, static_cast<DWORD>(n), 0) != FALSE;
  }
  bool Final(Bytes* out) {
    DWORD size = 0;
    DWORD cb = sizeof(size);
    if (!CryptGetHashParam(hash_, HP_HASHSIZE, reinterpret_cast<BYTE*>(&size), &cb, 0)) return false;
    out->assign(size, 0);
    return CryptGetHashParam(hash_, HP_HASHVAL, out->data(), &size, 0) != FALSE;
  }

 private:
  HCRYPTPROV prov_ = 0;
  HCRYPTHASH hash_ = 0;
};

bool HashOf(ALG_ID alg, const uint8_t* p, size_t n, Bytes* out) {
  Hasher h(alg);
  return h.ok() && h.Update(p, n) && h.Final(out);
}

// ---------------------------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------------------------

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// GeneralizedTime "YYYYMMDDHHMMSS[.f...]Z" to unix seconds.
bool ParseGeneralizedTime(const uint8_t* p, size_t n, int64_t* unix) {
  if (n < 15 || p[n - 1] != 'Z') return false;
  for (size_t i = 0; i < 14; ++i) {
    if (p[i] < '0' || p[i] > '9') return false;
  }
  auto num = [&](size_t at, size_t len) {
    int v = 0;
    for (size_t i = 0; i < len; ++i) v = v * 10 + (p[at + i] - '0');
    return v;
  };
  const int year = num(0, 4), month = num(4, 2), day = num(6, 2), hour = num(8, 2), minute = num(10, 2),
            second = num(12, 2);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
  *unix = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
          hour * 3600 + minute * 60 + second;
  return true;
}

FILETIME UnixToFileTime(int64_t unix) {
  const uint64_t ticks = static_cast<uint64_t>(unix + 11644473600LL) * 10000000ULL;
  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
  ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return ft;
}

// ---------------------------------------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------------------------------------

bool ReadWholeFile(HANDLE file, Bytes* out, std::string* error) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) {
    *error = "cannot take the file size or it is over the limit";
    return false;
  }
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
    *error = "cannot seek to the start: error " + std::to_string(GetLastError());
    return false;
  }
  out->assign(static_cast<size_t>(size.QuadPart), 0);
  size_t done = 0;
  while (done < out->size()) {
    const DWORD want = static_cast<DWORD>(std::min<size_t>(out->size() - done, size_t{1} << 20));
    DWORD got = 0;
    if (!ReadFile(file, out->data() + done, want, &got, nullptr) || got == 0) {
      *error = "read failed at offset " + std::to_string(done) + ": error " + std::to_string(GetLastError());
      return false;
    }
    done += got;
  }
  return true;
}

uint32_t U32(const Bytes& d, size_t at) {
  return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
         (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
uint16_t U16(const Bytes& d, size_t at) {
  return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}

struct Section {
  uint32_t rawPointer = 0;
  uint32_t rawSize = 0;
};

struct PeLayout {
  size_t checksumOffset = 0;
  size_t certDirOffset = 0;
  uint32_t sizeOfHeaders = 0;
  uint32_t certOffset = 0;
  uint32_t certSize = 0;
  std::vector<Section> sections;
};

bool ParsePe(const Bytes& d, PeLayout* pe) {
  if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
  const size_t lfanew = U32(d, 0x3C);
  if (lfanew > d.size() || d.size() - lfanew < 24 || std::memcmp(&d[lfanew], "PE\0\0", 4) != 0) return false;
  const size_t sections = U16(d, lfanew + 6);
  const size_t optSize = U16(d, lfanew + 20);
  const size_t opt = lfanew + 24;
  if (optSize < 96 || d.size() - opt < optSize) return false;
  const uint16_t magic = U16(d, opt);
  size_t dirs = 0;
  size_t countAt = 0;
  if (magic == 0x20B) {
    dirs = opt + 112;
    countAt = opt + 108;
  } else if (magic == 0x10B) {
    dirs = opt + 96;
    countAt = opt + 92;
  } else {
    return false;
  }
  if (countAt + 4 > opt + optSize || U32(d, countAt) < 5 || dirs + 5 * 8 > opt + optSize) return false;
  pe->checksumOffset = opt + 64;
  pe->certDirOffset = dirs + 4 * 8;
  pe->sizeOfHeaders = U32(d, opt + 60);
  pe->certOffset = U32(d, pe->certDirOffset);
  pe->certSize = U32(d, pe->certDirOffset + 4);
  const size_t table = opt + optSize;
  if (sections > 96 || d.size() < table || d.size() - table < sections * 40) return false;
  for (size_t i = 0; i < sections; ++i) {
    Section s;
    s.rawSize = U32(d, table + i * 40 + 16);
    s.rawPointer = U32(d, table + i * 40 + 20);
    pe->sections.push_back(s);
  }
  return pe->checksumOffset + 4 <= pe->certDirOffset && pe->certDirOffset + 8 <= pe->sizeOfHeaders &&
         pe->sizeOfHeaders <= d.size();
}

// The Authenticode image hash: every byte except the checksum, the certificate-table directory entry and
// the certificate table itself, sections in file order.
bool AuthenticodeHash(const Bytes& d, const PeLayout& pe, ALG_ID alg, Bytes* out) {
  Hasher h(alg);
  if (!h.ok()) return false;
  const uint8_t* base = d.data();
  bool ok = h.Update(base, pe.checksumOffset) &&
            h.Update(base + pe.checksumOffset + 4, pe.certDirOffset - (pe.checksumOffset + 4)) &&
            h.Update(base + pe.certDirOffset + 8, pe.sizeOfHeaders - (pe.certDirOffset + 8));
  if (!ok) return false;
  std::vector<Section> ordered = pe.sections;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Section& a, const Section& b) { return a.rawPointer < b.rawPointer; });
  uint64_t hashed = pe.sizeOfHeaders;
  for (const Section& s : ordered) {
    if (s.rawSize == 0) continue;
    if (static_cast<uint64_t>(s.rawPointer) + s.rawSize > d.size()) return false;
    if (!h.Update(base + s.rawPointer, s.rawSize)) return false;
    hashed += s.rawSize;
  }
  const uint64_t certEnd = static_cast<uint64_t>(pe.certOffset) + pe.certSize;
  if (pe.certOffset < hashed || certEnd > d.size()) return false;
  // Everything after the sections that is not the certificate table (normally nothing before it, nothing after).
  if (!h.Update(base + hashed, static_cast<size_t>(pe.certOffset - hashed))) return false;
  if (!h.Update(base + certEnd, static_cast<size_t>(d.size() - certEnd))) return false;
  return h.Final(out);
}

// ---------------------------------------------------------------------------------------------------------
// CMS
// ---------------------------------------------------------------------------------------------------------

struct Decoded {
  Msg msg;
  Store store;
  Cert signer;
  Bytes signerInfo;       // CMSG_SIGNER_INFO blob; its pointers point into this buffer
  Bytes content;          // the encapsulated content
  std::string contentType;
  const CMSG_SIGNER_INFO* info() const { return reinterpret_cast<const CMSG_SIGNER_INFO*>(signerInfo.data()); }
};

bool GetParam(HCRYPTMSG msg, DWORD param, DWORD index, Bytes* out) {
  DWORD cb = 0;
  if (!CryptMsgGetParam(msg, param, index, nullptr, &cb)) return false;
  out->assign(cb, 0);
  if (!CryptMsgGetParam(msg, param, index, out->data(), &cb)) return false;
  out->resize(cb);
  return true;
}

// Decodes a PKCS#7 SignedData with exactly one signer, finds the signer certificate in it and verifies the
// signature over the signed attributes. On failure `reason` names the step.
bool DecodeSigned(const uint8_t* p, size_t n, Decoded* out, std::string* reason) {
  out->msg.reset(CryptMsgOpenToDecode(kEncoding, 0, 0, 0, nullptr, nullptr));
  if (!out->msg) {
    *reason = "CryptMsgOpenToDecode error " + std::to_string(GetLastError());
    return false;
  }
  if (!CryptMsgUpdate(out->msg.get(), p, static_cast<DWORD>(n), TRUE)) {
    *reason = "does not parse as PKCS#7: error " + std::to_string(GetLastError());
    return false;
  }
  DWORD type = 0;
  DWORD cb = sizeof(type);
  if (!CryptMsgGetParam(out->msg.get(), CMSG_TYPE_PARAM, 0, &type, &cb) || type != CMSG_SIGNED) {
    *reason = "not a SignedData message";
    return false;
  }
  DWORD signers = 0;
  cb = sizeof(signers);
  if (!CryptMsgGetParam(out->msg.get(), CMSG_SIGNER_COUNT_PARAM, 0, &signers, &cb) || signers != 1) {
    *reason = "expected exactly one signer, found " + std::to_string(signers);
    return false;
  }
  if (!GetParam(out->msg.get(), CMSG_SIGNER_INFO_PARAM, 0, &out->signerInfo)) {
    *reason = "no signer info: error " + std::to_string(GetLastError());
    return false;
  }
  if (!GetParam(out->msg.get(), CMSG_CONTENT_PARAM, 0, &out->content)) {
    *reason = "no content: error " + std::to_string(GetLastError());
    return false;
  }
  Bytes innerType;
  if (GetParam(out->msg.get(), CMSG_INNER_CONTENT_TYPE_PARAM, 0, &innerType) && !innerType.empty()) {
    out->contentType.assign(reinterpret_cast<const char*>(innerType.data()));
  }
  out->store.reset(CertOpenStore(CERT_STORE_PROV_MSG, kEncoding, 0, 0, out->msg.get()));
  if (!out->store) {
    *reason = "no certificate store: error " + std::to_string(GetLastError());
    return false;
  }
  CERT_INFO wanted{};
  wanted.Issuer = out->info()->Issuer;
  wanted.SerialNumber = out->info()->SerialNumber;
  out->signer.reset(CertGetSubjectCertificateFromStore(out->store.get(), kEncoding, &wanted));
  if (!out->signer) {
    *reason = "the signer certificate is not in the message";
    return false;
  }
  return true;
}

bool VerifySignerSignature(Decoded& d) {
  return CryptMsgControl(d.msg.get(), 0, CMSG_CTRL_VERIFY_SIGNATURE, d.signer->pCertInfo) != FALSE;
}

// SpcIndirectDataContent ::= SEQUENCE { data SpcAttributeTypeAndOptionalValue,
//                                       messageDigest DigestInfo { AlgorithmIdentifier, OCTET STRING } }
bool ParseSpcDigest(const Bytes& content, std::string* algOid, Bytes* digest) {
  Tlv outer;
  if (!ReadTlv(content.data(), content.size(), &outer) || outer.tag != 0x30 || outer.total != content.size()) {
    return false;
  }
  std::vector<Tlv> kids;
  if (!Children(outer, &kids) || kids.size() < 2 || kids[1].tag != 0x30) return false;
  std::vector<Tlv> info;
  if (!Children(kids[1], &info) || info.size() != 2 || info[0].tag != 0x30 || info[1].tag != 0x04) return false;
  std::vector<Tlv> alg;
  if (!Children(info[0], &alg) || alg.empty() || !IsOid(alg[0])) return false;
  *algOid = OidToString(alg[0].body, alg[0].len);
  digest->assign(info[1].body, info[1].body + info[1].len);
  return true;
}

// The raw bytes of the signer's signature (the OCTET STRING of SignerInfo.signature), read from the DER of a
// SignedData ContentInfo. The RFC 3161 imprint is the hash of exactly these bytes.
bool RawSignerSignature(const uint8_t* p, size_t n, Bytes* signature) {
  Tlv ci;
  if (!ReadTlv(p, n, &ci) || ci.tag != 0x30) return false;
  std::vector<Tlv> ciKids;
  if (!Children(ci, &ciKids) || ciKids.size() < 2 || ciKids[1].tag != 0xA0) return false;
  Tlv sd;
  if (!ReadTlv(ciKids[1].body, ciKids[1].len, &sd) || sd.tag != 0x30) return false;
  std::vector<Tlv> sdKids;
  if (!Children(sd, &sdKids) || sdKids.empty() || sdKids.back().tag != 0x31) return false;
  std::vector<Tlv> signers;
  if (!Children(sdKids.back(), &signers) || signers.size() != 1 || signers[0].tag != 0x30) return false;
  std::vector<Tlv> si;
  if (!Children(signers[0], &si)) return false;
  // version, sid, digestAlgorithm, [0] signedAttrs?, signatureAlgorithm, signature, [1] unsignedAttrs?
  size_t i = 3;
  if (i < si.size() && si[i].tag == 0xA0) ++i;
  i += 1;  // signatureAlgorithm
  if (i >= si.size() || si[i].tag != 0x04) return false;
  signature->assign(si[i].body, si[i].body + si[i].len);
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Certificates
// ---------------------------------------------------------------------------------------------------------

bool CertEkus(const CERT_CONTEXT* cert, std::vector<std::string>* ekus) {
  ekus->clear();
  DWORD cb = 0;
  if (!CertGetEnhancedKeyUsage(cert, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &cb)) return false;
  Bytes buf(cb, 0);
  auto* usage = reinterpret_cast<CERT_ENHKEY_USAGE*>(buf.data());
  if (!CertGetEnhancedKeyUsage(cert, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usage, &cb)) return false;
  for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) ekus->push_back(usage->rgpszUsageIdentifier[i]);
  return true;
}

bool Contains(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

std::string SerialHex(const CRYPT_INTEGER_BLOB& serial) {
  std::string hex;
  for (DWORD i = serial.cbData; i > 0; --i) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02x", serial.pbData[i - 1]);
    hex += buf;
  }
  const size_t first = hex.find_first_not_of('0');
  return first == std::string::npos ? "0" : hex.substr(first);
}

// Builds the chain of `cert` to the pinned root alone: the machine's root store is not consulted. `status` is
// dwErrorStatus; the chain must also end in the pinned root certificate.
bool BuildChainToPinnedRoot(const CERT_CONTEXT* cert, HCERTSTORE additional, const Bytes& rootDer, int64_t atUnix,
                            uint32_t* status, std::string* error) {
  Cert root(CertCreateCertificateContext(kEncoding, rootDer.data(), static_cast<DWORD>(rootDer.size())));
  if (!root) {
    *error = "the pinned root does not parse: error " + std::to_string(GetLastError());
    return false;
  }
  Store rootStore(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr));
  if (!rootStore || !CertAddCertificateContextToStore(rootStore.get(), root.get(), CERT_STORE_ADD_ALWAYS, nullptr)) {
    *error = "cannot make the root store: error " + std::to_string(GetLastError());
    return false;
  }
  CERT_CHAIN_ENGINE_CONFIG config{};
  config.cbSize = sizeof(config);
  config.hExclusiveRoot = rootStore.get();
  HCERTCHAINENGINE rawEngine = nullptr;
  if (!CertCreateCertificateChainEngine(&config, &rawEngine)) {
    *error = "cannot make the chain engine: error " + std::to_string(GetLastError());
    return false;
  }
  Engine engine(rawEngine);
  CERT_CHAIN_PARA para{};
  para.cbSize = sizeof(para);
  const FILETIME when = UnixToFileTime(atUnix);
  const CERT_CHAIN_CONTEXT* rawChain = nullptr;
  if (!CertGetCertificateChain(engine.get(), cert, const_cast<FILETIME*>(&when), additional, &para, 0, nullptr,
                               &rawChain) ||
      rawChain == nullptr) {
    *error = "CertGetCertificateChain error " + std::to_string(GetLastError());
    return false;
  }
  Chain chain(rawChain);
  *status = chain->TrustStatus.dwErrorStatus;
  if (*status == 0) {
    // A clean chain from an exclusive root ends in the root we gave it; check rather than assume.
    if (chain->cChain == 0 || chain->rgpChain[0]->cElement == 0) {
      *status = CERT_TRUST_IS_PARTIAL_CHAIN;
      return true;
    }
    const CERT_SIMPLE_CHAIN* simple = chain->rgpChain[0];
    const CERT_CONTEXT* last = simple->rgpElement[simple->cElement - 1]->pCertContext;
    if (!CertCompareCertificate(kEncoding, root->pCertInfo, last->pCertInfo)) *status = CERT_TRUST_IS_UNTRUSTED_ROOT;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// The RFC 3161 timestamp
// ---------------------------------------------------------------------------------------------------------

struct TimestampResult {
  bool present = false;
  bool ok = false;
  Reason reason = Reason::Accepted;
  std::string detail;
  uint32_t chainStatus = 0;
  int64_t genTime = 0;
};

// Finds the timestamp token attribute among the signer's unauthenticated attributes.
bool FindTokenAttribute(const CMSG_SIGNER_INFO* info, const CRYPT_ATTR_BLOB** token) {
  for (DWORD i = 0; i < info->UnauthAttrs.cAttr; ++i) {
    const CRYPT_ATTRIBUTE& attr = info->UnauthAttrs.rgAttr[i];
    if (attr.pszObjId != nullptr && std::strcmp(attr.pszObjId, kOidRfc3161Token) == 0 && attr.cValue >= 1) {
      *token = &attr.rgValue[0];
      return true;
    }
  }
  return false;
}

// TSTInfo ::= SEQUENCE { version, policy OID, messageImprint SEQUENCE { AlgorithmIdentifier, OCTET STRING },
//                        serialNumber INTEGER, genTime GeneralizedTime, ... }
bool ParseTstInfo(const Bytes& content, std::string* imprintAlg, Bytes* imprint, int64_t* genTime) {
  Tlv outer;
  if (!ReadTlv(content.data(), content.size(), &outer)) return false;
  // CryptoAPI hands back the encapsulated OCTET STRING with its header; the TSTInfo SEQUENCE is inside.
  if (outer.tag == 0x04 && outer.total == content.size() && !ReadTlv(outer.body, outer.len, &outer)) return false;
  if (outer.tag != 0x30) return false;
  std::vector<Tlv> kids;
  if (!Children(outer, &kids) || kids.size() < 5) return false;
  if (kids[2].tag != 0x30) return false;
  std::vector<Tlv> mi;
  if (!Children(kids[2], &mi) || mi.size() != 2 || mi[0].tag != 0x30 || mi[1].tag != 0x04) return false;
  std::vector<Tlv> alg;
  if (!Children(mi[0], &alg) || alg.empty() || !IsOid(alg[0])) return false;
  *imprintAlg = OidToString(alg[0].body, alg[0].len);
  imprint->assign(mi[1].body, mi[1].body + mi[1].len);
  return kids[4].tag == 0x18 && ParseGeneralizedTime(kids[4].body, kids[4].len, genTime);
}

TimestampResult CheckTimestamp(const Decoded& sig, const Bytes& pkcs7, const Policy& policy) {
  TimestampResult result;
  const CRYPT_ATTR_BLOB* token = nullptr;
  if (!FindTokenAttribute(sig.info(), &token)) return result;
  result.present = true;
  auto fail = [&](Reason reason, std::string detail) {
    result.ok = false;
    result.reason = reason;
    result.detail = std::move(detail);
    return result;
  };

  Decoded ts;
  std::string why;
  if (!DecodeSigned(token->pbData, token->cbData, &ts, &why)) return fail(Reason::TimestampInvalid, why);
  if (ts.contentType != kOidTstInfo) return fail(Reason::TimestampInvalid, "the token's content is not a TSTInfo");
  if (!VerifySignerSignature(ts)) return fail(Reason::TimestampInvalid, "the token's signature does not verify");

  std::string imprintAlg;
  Bytes imprint;
  if (!ParseTstInfo(ts.content, &imprintAlg, &imprint, &result.genTime)) {
    return fail(Reason::TimestampInvalid, "the TSTInfo does not parse");
  }

  // The imprint must be the hash of THIS signature's signature bytes (RFC 3161 as Authenticode uses it).
  ALG_ID alg = 0;
  Bytes rawSignature;
  Bytes expected;
  if (!AlgFromOid(imprintAlg, &alg)) return fail(Reason::TimestampInvalid, "imprint hash " + imprintAlg + " unsupported");
  if (!RawSignerSignature(pkcs7.data(), pkcs7.size(), &rawSignature)) {
    return fail(Reason::MalformedSignature, "cannot read the signature bytes");
  }
  if (!HashOf(alg, rawSignature.data(), rawSignature.size(), &expected)) {
    return fail(Reason::TimestampInvalid, "cannot hash the signature");
  }
  if (expected != imprint) return fail(Reason::TimestampNotBound, "");

  // The time-stamp authority's certificate: chains to the pinned root at the stamped time, and may stamp.
  std::vector<std::string> ekus;
  if (!CertEkus(ts.signer.get(), &ekus) || !Contains(ekus, kOidTimeStamping)) {
    return fail(Reason::TimestampInvalid, "the time-stamp certificate lacks the time stamping EKU");
  }
  uint32_t status = 0;
  std::string error;
  if (!BuildChainToPinnedRoot(ts.signer.get(), ts.store.get(), policy.rootDer, result.genTime, &status, &error)) {
    return fail(Reason::TimestampInvalid, error);
  }
  if (status != 0) {
    result.chainStatus = status;
    return fail(Reason::TimestampInvalid, "time-stamp chain status " + Hex(status) + " " + ChainStatusNames(status));
  }
  result.ok = true;
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------------------------

const char* ReasonName(Reason reason) {
  switch (reason) {
    case Reason::Accepted: return "accepted";
    case Reason::Unreadable: return "unreadable";
    case Reason::Unsigned: return "unsigned";
    case Reason::MalformedSignature: return "malformed signature";
    case Reason::DigestMismatch: return "digest mismatch";
    case Reason::SignatureInvalid: return "signature invalid";
    case Reason::ChainStatus: return "chain status";
    case Reason::NoTimestampAndExpired: return "no timestamp and signer certificate expired";
    case Reason::TimestampInvalid: return "timestamp invalid";
    case Reason::TimestampNotBound: return "timestamp not bound to this signature";
    case Reason::MissingEku: return "EKU";
    case Reason::SubjectMismatch: return "other signer";
    case Reason::DenylistedSerial: return "denylisted serial";
    case Reason::DenylistedWindow: return "denylisted signing window";
    case Reason::DenylistedProfile: return "denylisted profile";
  }
  return "unknown";
}

std::string Verdict::Describe() const {
  std::string out = ReasonName(reason);
  if (!detail.empty()) out += ": " + detail;
  return out;
}

std::string ChainStatusNames(uint32_t status) {
  static const std::array<std::pair<uint32_t, const char*>, 18> kBits = {{
      {0x00000001, "CERT_TRUST_IS_NOT_TIME_VALID"},
      {0x00000002, "CERT_TRUST_IS_NOT_TIME_NESTED"},
      {0x00000004, "CERT_TRUST_IS_REVOKED"},
      {0x00000008, "CERT_TRUST_IS_NOT_SIGNATURE_VALID"},
      {0x00000010, "CERT_TRUST_IS_NOT_VALID_FOR_USAGE"},
      {0x00000020, "CERT_TRUST_IS_UNTRUSTED_ROOT"},
      {0x00000040, "CERT_TRUST_REVOCATION_STATUS_UNKNOWN"},
      {0x00000080, "CERT_TRUST_IS_CYCLIC"},
      {0x00000100, "CERT_TRUST_INVALID_EXTENSION"},
      {0x00000200, "CERT_TRUST_INVALID_POLICY_CONSTRAINTS"},
      {0x00000400, "CERT_TRUST_INVALID_BASIC_CONSTRAINTS"},
      {0x00000800, "CERT_TRUST_INVALID_NAME_CONSTRAINTS"},
      {0x00001000, "CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT"},
      {0x00002000, "CERT_TRUST_HAS_NOT_DEFINED_NAME_CONSTRAINT"},
      {0x00004000, "CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT"},
      {0x00008000, "CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT"},
      {0x00010000, "CERT_TRUST_IS_PARTIAL_CHAIN"},
      {0x00020000, "CERT_TRUST_CTL_IS_NOT_TIME_VALID"},
  }};
  std::string out;
  for (const auto& bit : kBits) {
    if ((status & bit.first) != 0) {
      if (!out.empty()) out += ",";
      out += bit.second;
    }
  }
  return out;
}

Verdict VerifyPluginFile(HANDLE file, const Policy& policy, int64_t nowUnix) {
  Bytes data;
  std::string error;
  if (!ReadWholeFile(file, &data, &error)) return Refuse(Reason::Unreadable, error);

  PeLayout pe;
  if (!ParsePe(data, &pe)) return Refuse(Reason::Unreadable, "not a PE file");
  if (pe.certOffset == 0 || pe.certSize == 0) return Refuse(Reason::Unsigned);
  if (pe.certSize < 8 || static_cast<uint64_t>(pe.certOffset) + pe.certSize > data.size()) {
    return Refuse(Reason::MalformedSignature, "the certificate table is outside the file");
  }
  const uint32_t winLength = U32(data, pe.certOffset);
  if (U16(data, pe.certOffset + 4) != 0x0200 || U16(data, pe.certOffset + 6) != 0x0002 || winLength < 8 ||
      winLength > pe.certSize) {
    return Refuse(Reason::MalformedSignature, "the certificate table is not a PKCS#7 WIN_CERTIFICATE");
  }
  const Bytes pkcs7(data.begin() + pe.certOffset + 8, data.begin() + pe.certOffset + winLength);

  Decoded sig;
  std::string why;
  if (!DecodeSigned(pkcs7.data(), pkcs7.size(), &sig, &why)) return Refuse(Reason::MalformedSignature, why);

  std::string algOid;
  Bytes signedDigest;
  if (!ParseSpcDigest(sig.content, &algOid, &signedDigest)) {
    return Refuse(Reason::MalformedSignature, "the signed content is not an Authenticode indirect-data block");
  }
  ALG_ID alg = 0;
  if (!AlgFromOid(algOid, &alg)) return Refuse(Reason::MalformedSignature, "digest algorithm " + algOid + " unsupported");

  Bytes actual;
  if (!AuthenticodeHash(data, pe, alg, &actual)) return Refuse(Reason::MalformedSignature, "cannot hash the image");
  if (actual != signedDigest) return Refuse(Reason::DigestMismatch);
  if (!VerifySignerSignature(sig)) return Refuse(Reason::SignatureInvalid);

  // When was it signed? A verified, bound timestamp says; a present but bad one refuses (never falls back to
  // "now"); no timestamp means now.
  const TimestampResult stamp = CheckTimestamp(sig, pkcs7, policy);
  if (stamp.present && !stamp.ok) {
    Verdict v = Refuse(stamp.reason, stamp.detail);
    v.chainStatus = stamp.chainStatus;
    return v;
  }
  const int64_t signedAt = stamp.present ? stamp.genTime : nowUnix;

  uint32_t status = 0;
  if (!BuildChainToPinnedRoot(sig.signer.get(), sig.store.get(), policy.rootDer, signedAt, &status, &error)) {
    return Refuse(Reason::ChainStatus, error);
  }
  if (status != 0) {
    Verdict v = Refuse(!stamp.present && (status & CERT_TRUST_IS_NOT_TIME_VALID) != 0 ? Reason::NoTimestampAndExpired
                                                                                       : Reason::ChainStatus,
                       "status " + Hex(status) + " " + ChainStatusNames(status));
    v.chainStatus = status;
    return v;
  }

  std::vector<std::string> ekus;
  if (!CertEkus(sig.signer.get(), &ekus) || !Contains(ekus, kOidCodeSigning)) {
    return Refuse(Reason::MissingEku, "no code signing EKU");
  }
  const std::string* profile = nullptr;
  for (const std::string& eku : policy.profileEkus) {
    if (Contains(ekus, eku)) {
      profile = &eku;
      break;
    }
  }
  if (profile == nullptr) return Refuse(Reason::MissingEku, "no accepted profile EKU");

  if (!policy.subject.empty()) {
    char subject[512];
    const DWORD n = CertNameToStrA(kEncoding, &sig.signer->pCertInfo->Subject,
                                   CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, subject, sizeof(subject));
    if (n == 0 || policy.subject != subject) return Refuse(Reason::SubjectMismatch, n == 0 ? "" : subject);
  }

  const std::string serial = SerialHex(sig.signer->pCertInfo->SerialNumber);
  if (Contains(policy.deniedSerials, serial)) return Refuse(Reason::DenylistedSerial, serial);
  for (const TimeWindow& w : policy.deniedWindows) {
    if (signedAt >= w.from && signedAt <= w.until) {
      return Refuse(Reason::DenylistedWindow, std::to_string(signedAt));
    }
  }
  if (Contains(policy.deniedProfileEkus, *profile)) return Refuse(Reason::DenylistedProfile, *profile);

  Verdict ok;
  ok.reason = Reason::Accepted;
  ok.signingTime = stamp.present ? stamp.genTime : 0;
  return ok;
}

}  // namespace nevr_plugin_signature
