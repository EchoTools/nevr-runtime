#pragma once

// Friend display names for the social facade.
//
// Nakama's friend messages carry account ids only (SNSFriendListResponse is counts, and
// SNSFriendStatusNotify is id + status), so the friends tab would show bare Discord ids. The game
// already has a request that returns a player's profile, which includes their display name: for each
// friend the runtime sends the game's own OtherUserProfileRequest and reads `displayname` from the
// OtherUserProfileSuccess reply (EvrId(16) + u32 + a zstd-compressed profile JSON). This header holds
// the pure pieces; the zstd decode lives in social_names.cpp and is registered at startup, so a
// binary that does not link zstd (the tests that compile the bridge) simply resolves no names.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "runtime/compat/social_party.h"

namespace SocialNames {

constexpr std::uint64_t kProfileRequest = 0x1231172031050cb2ULL;  // SNSOtherUserProfileRequest
constexpr std::uint64_t kProfileSuccess = 0x1230073227050cb5ULL;  // SNSOtherUserProfileSuccess
constexpr std::uint64_t kPlatformOvrOrg = 4;  // the platform the bridge logs the game in as

/// OtherUserProfileRequest payload: EvrId(platform u64, account u64) then the request JSON, null
/// terminated and uncompressed. The server answers for any JSON, so this sends an empty object.
inline SocialParty::Message BuildProfileRequest(std::uint64_t accountId) {
  SocialParty::Message m;
  m.symbol = kProfileRequest;
  SocialParty::AppendLe(m.payload, kPlatformOvrOrg, 8);
  SocialParty::AppendLe(m.payload, accountId, 8);
  m.payload += "{}";
  m.payload.push_back('\0');
  return m;
}

/// Decodes an OtherUserProfileSuccess payload into the account id and display name it carries.
using ProfileDecoder = bool (*)(const std::uint8_t* payload, std::size_t len, std::uint64_t* accountId,
                                std::string* displayName);

inline std::atomic<ProfileDecoder>& DecoderSlot() {
  static std::atomic<ProfileDecoder> slot{nullptr};
  return slot;
}
inline void SetDecoder(ProfileDecoder decoder) { DecoderSlot().store(decoder, std::memory_order_release); }

inline bool DecodeProfile(const std::uint8_t* payload, std::size_t len, std::uint64_t* accountId,
                          std::string* displayName) {
  const ProfileDecoder decoder = DecoderSlot().load(std::memory_order_acquire);
  return decoder != nullptr && decoder(payload, len, accountId, displayName);
}

/// Remembers which friends a name was already asked for, so one session asks once per friend.
class Resolver {
 public:
  /// The request for `accountId`'s profile, or none if it was already asked for.
  std::vector<SocialParty::Message> Want(std::uint64_t accountId) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<SocialParty::Message> out;
    if (accountId != 0 && asked_.insert(accountId).second) out.push_back(BuildProfileRequest(accountId));
    return out;
  }

  void Reset() {
    std::lock_guard<std::mutex> guard(mutex_);
    asked_.clear();
  }

 private:
  std::mutex mutex_;
  std::set<std::uint64_t> asked_;
};

inline Resolver& GlobalResolver() {
  static Resolver resolver;
  return resolver;
}

}  // namespace SocialNames
