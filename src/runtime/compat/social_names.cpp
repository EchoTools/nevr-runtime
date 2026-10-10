// social_names.cpp — see social_names.h. The zstd decode and the JSON read of the display name.
// Compiled with SKIP_PRECOMPILE_HEADERS: it needs only zstd, nlohmann and std.

#include "runtime/compat/social_names.h"

#include <zstd.h>

#include <nlohmann/json.hpp>

namespace SocialNames {
namespace {

// The profile JSON is a few KB; this bounds what an unexpected frame can make us allocate.
constexpr std::size_t kMaxProfileBytes = 1u << 20;

bool DecodeProfilePayload(const std::uint8_t* payload, std::size_t len, std::uint64_t* accountId,
                          std::string* displayName) {
  // EvrId(platform u64, account u64), u32 length word, then the zstd frame.
  constexpr std::size_t kHeader = 16 + 4;
  if (payload == nullptr || accountId == nullptr || displayName == nullptr || len <= kHeader) return false;
  std::uint64_t account = 0;
  for (int i = 7; i >= 0; --i) account = (account << 8) | payload[8 + i];

  ZSTD_DCtx* ctx = ZSTD_createDCtx();
  if (ctx == nullptr) return false;
  std::string json;
  ZSTD_inBuffer in{payload + kHeader, len - kHeader, 0};
  std::string chunk(16 * 1024, '\0');
  bool ok = true;
  while (true) {
    ZSTD_outBuffer out{chunk.data(), chunk.size(), 0};
    const std::size_t rc = ZSTD_decompressStream(ctx, &out, &in);
    if (ZSTD_isError(rc)) { ok = false; break; }
    json.append(chunk.data(), out.pos);
    if (json.size() > kMaxProfileBytes) { ok = false; break; }
    if (rc == 0) break;                                 // the frame is complete
    if (in.pos == in.size && out.pos == 0) { ok = false; break; }  // truncated input
  }
  ZSTD_freeDCtx(ctx);
  if (!ok) return false;
  while (!json.empty() && json.back() == '\0') json.pop_back();

  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
  if (!parsed.is_object()) return false;
  const auto it = parsed.find("displayname");
  if (it == parsed.end() || !it->is_string()) return false;
  *accountId = account;
  *displayName = it->get<std::string>();
  return !displayName->empty();
}

}  // namespace

void RegisterDefaultDecoder() { SetDecoder(&DecodeProfilePayload); }

#ifndef NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION
namespace {
// Registers the decoder when the runtime loads (the tests that do not link this file keep none). The Quest
// build defines NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION: its sentinel may carry no dynamic initializer
// (tools/check_quest_static_init.sh), so the installer calls RegisterDefaultDecoder() instead.
const bool g_decoderRegistered = (RegisterDefaultDecoder(), true);
}  // namespace
#endif

}  // namespace SocialNames
