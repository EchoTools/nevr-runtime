#include "core/minisign_verify.h"

#include <algorithm>
#include <cstring>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace nevr_minisign {

namespace {

using namespace nevr_maps;  // the format's sizes and tags (map_format.h)

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + kBase64LowerFirst;
  if (c >= '0' && c <= '9') return c - '0' + kBase64DigitFirst;
  if (c == '+') return static_cast<int>(kBase64Plus);
  if (c == '/') return static_cast<int>(kBase64Slash);
  return -1;
}

// Standard alphabet with padding, nothing else: no whitespace, no missing padding, no stray bits in the last group.
bool DecodeBase64(std::string_view text, std::vector<std::uint8_t>* out) {
  constexpr unsigned kByteBits = 8;
  out->clear();
  if (text.empty() || text.size() % kBase64GroupChars != 0) return false;
  out->reserve(text.size() / kBase64GroupChars * kBase64GroupBytes);
  for (std::size_t at = 0; at < text.size(); at += kBase64GroupChars) {
    const bool last = at + kBase64GroupChars == text.size();
    unsigned v[kBase64GroupChars];
    std::size_t padding = 0;
    for (std::size_t k = 0; k < kBase64GroupChars; ++k) {
      const char c = text[at + k];
      if (c == '=' && last && k >= kBase64GroupChars - kBase64MaxPadding) {
        v[k] = 0;
        ++padding;
        continue;
      }
      if (padding > 0) return false;  // a character after '='
      const int value = Base64Value(c);
      if (value < 0) return false;
      v[k] = static_cast<unsigned>(value);
    }
    std::uint32_t group = 0;
    for (std::size_t k = 0; k < kBase64GroupChars; ++k) group = (group << kBase64CharBits) | v[k];
    if (padding == kBase64MaxPadding && (v[kBase64TwoPadLastChar] & kBase64TwoPadSpareMask) != 0) return false;
    if (padding == 1 && (v[kBase64OnePadLastChar] & kBase64OnePadSpareMask) != 0) return false;
    for (std::size_t b = 0; b < kBase64GroupBytes - padding; ++b) {
      out->push_back(static_cast<std::uint8_t>(group >> (kByteBits * (kBase64GroupBytes - 1 - b))));
    }
  }
  return true;
}

std::vector<std::string_view> SplitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  return lines;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool ParsePublicKeyText(std::string_view text, PublicKey* out) {
  constexpr std::string_view kSpace = " \t\r";
  while (!text.empty()) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    std::string_view line = text.substr(0, end);
    text.remove_prefix(std::min(end + 1, text.size()));
    const std::size_t first = line.find_first_not_of(kSpace);
    if (first == std::string_view::npos) continue;
    line = line.substr(first, line.find_last_not_of(kSpace) + 1 - first);
    if (StartsWith(line, kMinisignUntrustedKeyword)) continue;
    return ParsePublicKey(line, out);
  }
  return false;
}

bool ParsePublicKey(std::string_view base64_line, PublicKey* out) {
  std::vector<std::uint8_t> blob;
  if (!DecodeBase64(base64_line, &blob) || blob.size() != kMinisignPublicKeyBlobBytes) return false;
  if (blob[0] != kMinisignAlgorithmFirst || blob[1] != kMinisignKeyAlgorithm) return false;
  std::memcpy(out->key_id.data(), blob.data() + kMinisignKeyIdAt, kMinisignKeyIdBytes);
  std::memcpy(out->pk.data(), blob.data() + kMinisignPayloadAt, kEd25519PublicKeyBytes);
  return true;
}

Verdict Verify(std::string_view message, std::string_view minisig, const std::vector<PublicKey>& keys,
               std::array<std::uint8_t, kMinisignKeyIdBytes>* key_id_out, std::string* trusted_comment_out) {
  std::array<std::uint8_t, kBlake2b512Bytes> prehash{};
  crypto_blake2b(prehash.data(), prehash.size(), reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
  return VerifyPrehashed(prehash, minisig, keys, key_id_out, trusted_comment_out);
}

struct PrehashStream::State {
  crypto_blake2b_ctx ctx;
};

PrehashStream::PrehashStream() : state_(new State) { crypto_blake2b_init(&state_->ctx, kBlake2b512Bytes); }
PrehashStream::~PrehashStream() = default;

void PrehashStream::Update(const void* data, std::size_t length) {
  crypto_blake2b_update(&state_->ctx, static_cast<const std::uint8_t*>(data), length);
}

std::array<std::uint8_t, kBlake2b512Bytes> PrehashStream::Final() {
  std::array<std::uint8_t, kBlake2b512Bytes> out{};
  crypto_blake2b_final(&state_->ctx, out.data());
  return out;
}

Verdict VerifyPrehashed(const std::array<std::uint8_t, kBlake2b512Bytes>& prehash, std::string_view minisig,
                        const std::vector<PublicKey>& keys, std::array<std::uint8_t, kMinisignKeyIdBytes>* key_id_out,
                        std::string* trusted_comment_out) {
  const std::vector<std::string_view> lines = SplitLines(minisig);
  if (lines.size() < kMinisignSignatureLines || !StartsWith(lines[0], kMinisignUntrustedPrefix) ||
      !StartsWith(lines[kMinisignTrustedLine], kMinisignTrustedPrefix)) {
    return Verdict::kMalformed;
  }
  for (std::size_t i = kMinisignSignatureLines; i < lines.size(); ++i) {
    if (!lines[i].empty()) return Verdict::kMalformed;
  }
  std::vector<std::uint8_t> blob, global;
  if (!DecodeBase64(lines[kMinisignSignatureLine], &blob) || blob.size() != kMinisignSignatureBlobBytes) {
    return Verdict::kMalformed;
  }
  if (!DecodeBase64(lines[kMinisignGlobalLine], &global) || global.size() != kEd25519SignatureBytes) {
    return Verdict::kMalformed;
  }
  if (blob[0] != kMinisignAlgorithmFirst || blob[1] != kMinisignPrehashedAlgorithm) return Verdict::kUnsupported;

  std::array<std::uint8_t, kMinisignKeyIdBytes> key_id{};
  std::memcpy(key_id.data(), blob.data() + kMinisignKeyIdAt, kMinisignKeyIdBytes);
  if (key_id_out != nullptr) *key_id_out = key_id;
  const std::uint8_t* signature = blob.data() + kMinisignPayloadAt;
  const std::string_view comment = lines[kMinisignTrustedLine].substr(kMinisignTrustedPrefix.size());

  std::vector<std::uint8_t> signed_comment(signature, signature + kEd25519SignatureBytes);
  signed_comment.insert(signed_comment.end(), comment.begin(), comment.end());

  // Two pinned keys may share an id (an id is only 8 bytes), so each one with the id is tried.
  bool known = false;
  bool body_ok = false;
  for (const PublicKey& key : keys) {
    if (key.key_id != key_id) continue;
    known = true;
    if (crypto_ed25519_check(signature, key.pk.data(), prehash.data(), prehash.size()) != 0) continue;
    body_ok = true;
    if (crypto_ed25519_check(global.data(), key.pk.data(), signed_comment.data(), signed_comment.size()) != 0) continue;
    if (trusted_comment_out != nullptr) trusted_comment_out->assign(comment.data(), comment.size());
    return Verdict::kOk;
  }
  if (!known) return Verdict::kUnknownKey;
  return body_ok ? Verdict::kBadGlobalSignature : Verdict::kBadSignature;
}

const char* VerdictName(Verdict verdict) {
  switch (verdict) {
    case Verdict::kOk: return "ok";
    case Verdict::kMalformed: return "malformed";
    case Verdict::kUnsupported: return "unsupported";
    case Verdict::kUnknownKey: return "unknown_key";
    case Verdict::kBadSignature: return "bad_signature";
    case Verdict::kBadGlobalSignature: return "bad_global_signature";
  }
  return "unknown";
}

}  // namespace nevr_minisign
