#include "core/minisign_verify.h"

#include <cstring>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace nevr_minisign {

namespace {

constexpr std::string_view kUntrustedPrefix = "untrusted comment: ";
constexpr std::string_view kTrustedPrefix = "trusted comment: ";
constexpr std::size_t kSignatureBlobSize = 2 + 8 + 64;
constexpr std::size_t kPublicKeyBlobSize = 2 + 8 + 32;

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Standard alphabet with padding, nothing else: no whitespace, no missing padding, no stray bits in the last group.
bool DecodeBase64(std::string_view text, std::vector<std::uint8_t>* out) {
  out->clear();
  if (text.empty() || text.size() % 4 != 0) return false;
  out->reserve(text.size() / 4 * 3);
  for (std::size_t at = 0; at < text.size(); at += 4) {
    const bool last = at + 4 == text.size();
    int v[4];
    int padding = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = text[at + k];
      if (c == '=' && last && k >= 2) {
        v[k] = 0;
        ++padding;
        continue;
      }
      if (padding > 0) return false;  // a character after '='
      v[k] = Base64Value(c);
      if (v[k] < 0) return false;
    }
    const std::uint32_t group = (std::uint32_t(v[0]) << 18) | (std::uint32_t(v[1]) << 12) | (std::uint32_t(v[2]) << 6) |
                                std::uint32_t(v[3]);
    if (padding == 2 && (v[1] & 0x0F) != 0) return false;
    if (padding == 1 && (v[2] & 0x03) != 0) return false;
    out->push_back(std::uint8_t(group >> 16));
    if (padding < 2) out->push_back(std::uint8_t(group >> 8));
    if (padding < 1) out->push_back(std::uint8_t(group));
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

bool ParsePublicKey(std::string_view base64_line, PublicKey* out) {
  std::vector<std::uint8_t> blob;
  if (!DecodeBase64(base64_line, &blob) || blob.size() != kPublicKeyBlobSize) return false;
  if (blob[0] != 'E' || blob[1] != 'd') return false;
  std::memcpy(out->key_id.data(), blob.data() + 2, 8);
  std::memcpy(out->pk.data(), blob.data() + 10, 32);
  return true;
}

Verdict Verify(std::string_view message, std::string_view minisig, const std::vector<PublicKey>& keys,
               std::array<std::uint8_t, 8>* key_id_out, std::string* trusted_comment_out) {
  std::array<std::uint8_t, 64> prehash{};
  crypto_blake2b(prehash.data(), prehash.size(), reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
  return VerifyPrehashed(prehash, minisig, keys, key_id_out, trusted_comment_out);
}

struct PrehashStream::State {
  crypto_blake2b_ctx ctx;
};

PrehashStream::PrehashStream() : state_(new State) { crypto_blake2b_init(&state_->ctx, 64); }
PrehashStream::~PrehashStream() = default;

void PrehashStream::Update(const void* data, std::size_t length) {
  crypto_blake2b_update(&state_->ctx, static_cast<const std::uint8_t*>(data), length);
}

std::array<std::uint8_t, 64> PrehashStream::Final() {
  std::array<std::uint8_t, 64> out{};
  crypto_blake2b_final(&state_->ctx, out.data());
  return out;
}

Verdict VerifyPrehashed(const std::array<std::uint8_t, 64>& prehash, std::string_view minisig,
                        const std::vector<PublicKey>& keys, std::array<std::uint8_t, 8>* key_id_out,
                        std::string* trusted_comment_out) {
  const std::vector<std::string_view> lines = SplitLines(minisig);
  if (lines.size() < 4 || !StartsWith(lines[0], kUntrustedPrefix) || !StartsWith(lines[2], kTrustedPrefix)) {
    return Verdict::kMalformed;
  }
  for (std::size_t i = 4; i < lines.size(); ++i) {
    if (!lines[i].empty()) return Verdict::kMalformed;
  }
  std::vector<std::uint8_t> blob, global;
  if (!DecodeBase64(lines[1], &blob) || blob.size() != kSignatureBlobSize) return Verdict::kMalformed;
  if (!DecodeBase64(lines[3], &global) || global.size() != 64) return Verdict::kMalformed;
  if (blob[0] != 'E' || blob[1] != 'D') return Verdict::kUnsupported;

  std::array<std::uint8_t, 8> key_id{};
  std::memcpy(key_id.data(), blob.data() + 2, 8);
  if (key_id_out != nullptr) *key_id_out = key_id;
  const std::uint8_t* signature = blob.data() + 10;
  const std::string_view comment = lines[2].substr(kTrustedPrefix.size());

  std::vector<std::uint8_t> signed_comment(signature, signature + 64);
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
