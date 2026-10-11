// One structured log line per request the facade or the frame observer sends toward the service, with the
// stable ids it carries (no secret is in any of them): the request name and symbol, the account id the
// request is aimed at, the party it concerns, whether the sender took it.
//
// Where each id comes from:
//  - a Standard message carries its subject last, at payload +0x20 (the invite target, the party, the scope
//    or the policy): logged as `arg`;
//  - a Targeted message (kick, pass, invite response) carries only the UUID derived from the account id, and
//    its +0x20 is the session id, always 0, so the builder records the account id in Message::target and
//    that is what is logged as `target`, with the Targeted parameter (+0x28) as `param`;
//  - a profile request carries EvrId(platform, account): the account is logged as `target`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hook_log.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"

namespace quest_social {

inline std::uint64_t PayloadU64(const std::string& payload, std::size_t offset) {
  if (payload.size() < offset + 8) return 0;
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<std::uint8_t>(payload[offset + static_cast<std::size_t>(i)]);
  return v;
}

// `party` is the party id the request concerns (0: none).
inline void LogRequests(const char* what, const std::vector<nevr_social_party::Message>& messages, bool sent,
                        std::uint64_t party) {
  for (const nevr_social_party::Message& m : messages) {
    const char* name = nevr_social_party::RequestName(m.symbol);
    std::uint64_t target = m.target;
    std::uint64_t arg = 0;
    std::uint64_t param = 0;
    if (m.symbol == nevr_social_names::kProfileRequest) {
      name = "OtherUserProfileRequest";
      target = PayloadU64(m.payload, 8);  // EvrId(platform, account)
    } else if (m.target != 0) {
      param = PayloadU64(m.payload, 0x28) & 0xFFFFFFFFULL;  // the Targeted parameter
    } else {
      arg = PayloadU64(m.payload, 0x20);  // a Standard message's subject
    }
    char symbol[19];
    sentinel::LogFields(sent ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "social_send",
                        {{"what", what},
                         {"name", name != nullptr ? name : "unnamed"},
                         {"symbol", sentinel::HexString(symbol, m.symbol)},
                         {"target", static_cast<long long>(target)},
                         {"party", static_cast<long long>(party)},
                         {"arg", static_cast<long long>(arg)},
                         {"param", static_cast<long long>(param)},
                         {"sent", sent ? "yes" : "NOT_sent"}});
  }
}

}  // namespace quest_social
