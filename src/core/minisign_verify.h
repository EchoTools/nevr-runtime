/* minisign signature verification (docs/adr/0047-custom-maps-channel.md): the one signature over the custom-map channel
 * manifest. Only the prehashed algorithm "ED" is accepted: Ed25519 over BLAKE2b-512(message). Legacy "Ed" (the raw message
 * signed) is kUnsupported. The trusted comment is covered by the global signature, so it is returned only when that checks.
 *
 * Public key line: base64 of "Ed" + key id (8) + Ed25519 key (32). Signature text, four lines:
 *   untrusted comment: <anything>
 *   <base64 of "ED" + key id (8) + signature (64)>
 *   trusted comment: <text>
 *   <base64 of the global signature (64) over signature || text>
 * A line may end in "\r\n". Nothing but empty lines may follow the fourth.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_minisign {

enum class Verdict { kOk, kMalformed, kUnsupported, kUnknownKey, kBadSignature, kBadGlobalSignature };

struct PublicKey {
  std::array<std::uint8_t, 8> key_id{};
  std::array<std::uint8_t, 32> pk{};
};

bool ParsePublicKey(std::string_view base64_line, PublicKey* out);

Verdict Verify(std::string_view message, std::string_view minisig, const std::vector<PublicKey>& keys,
               std::array<std::uint8_t, 8>* key_id_out, std::string* trusted_comment_out);

// The "ED" prehash, BLAKE2b-512, over a message read in pieces: a file too large to hold is verified with
// VerifyPrehashed(stream.Final(), ...), which is Verify without the hashing.
class PrehashStream {
 public:
  PrehashStream();
  ~PrehashStream();
  PrehashStream(const PrehashStream&) = delete;
  PrehashStream& operator=(const PrehashStream&) = delete;
  void Update(const void* data, std::size_t length);
  std::array<std::uint8_t, 64> Final();  // once

 private:
  struct State;
  std::unique_ptr<State> state_;
};

Verdict VerifyPrehashed(const std::array<std::uint8_t, 64>& prehash, std::string_view minisig,
                        const std::vector<PublicKey>& keys, std::array<std::uint8_t, 8>* key_id_out,
                        std::string* trusted_comment_out);

const char* VerdictName(Verdict verdict);

}  // namespace nevr_minisign
