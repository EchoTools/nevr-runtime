#include "quest/social/social_frames.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hook_log.h"
#include "quest/social/social_request_log.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"

namespace quest_social {
namespace {

using sentinel::LogFields;
using sentinel::LogLevel;

constexpr std::uint8_t kMarker[8] = {0xf6, 0x40, 0xbb, 0x78, 0xa2, 0xe7, 0x8c, 0xbb};

std::uint64_t Le64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void Send(const Ports& ports, const char* what, const std::vector<SocialParty::Message>& messages) {
  if (messages.empty()) return;
  const bool sent = ports.send != nullptr && ports.send(messages);
  LogRequests(what, messages, sent, 0);  // one line per request, with the account or party it is aimed at
}

// Asks for the display name of `accountId` once per session, but only if the reply can be read: the profile
// is zstd-compressed and the Quest build registers no decoder (SocialNames::SetDecoder) unless an adapter
// links one. Without it every reply is unreadable, so no request is sent and the roster shows account ids.
void WantName(const Ports& ports, const char* what, std::uint64_t accountId) {
  if (SocialNames::DecoderSlot().load(std::memory_order_acquire) == nullptr) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
      LogFields(LogLevel::kWarn, "social_names", {{"result", "no_profile_decoder_requests_skipped"}});
    }
    return;
  }
  Send(ports, what, SocialNames::GlobalResolver().Want(accountId));
}

const char* KnownName(std::uint64_t symbol) {
  if (symbol == kSymFriendStatusNotify) return "FriendStatusNotify";
  if (symbol == kSymFriendListResponse) return "FriendListResponse";
  if (symbol == SocialRoster::kFriendPresenceNotify) return "FriendPresenceNotify";
  if (symbol == SocialRoster::kRecentlyMetListResponse) return "RecentlyMetListResponse";
  if (symbol == SocialNames::kProfileSuccess) return "OtherUserProfileSuccess";
  if (symbol == SocialParty::kPartyDataNotify) return "PartyDataNotify";
  if (const char* name = SocialParty::ReplyName(symbol)) return name;
  return SocialParty::RequestName(symbol);
}

// SNSPartyDataNotify: party or member data (a JSON object) for the party model, which holds it for the facade's Update
// to load into the game's CJson. Only a JSON object goes on. Returns whether the model took it; else `why` names the
// reason.
bool ApplyPartyData(const Ports& ports, const std::uint8_t* payload, std::size_t len, const char** why) {
  SocialParty::DataNotify notify;
  if (!SocialParty::ParseDataNotify(payload, len, &notify)) {
    *why = "party_data_unreadable";
    return false;
  }
  const nlohmann::json parsed = nlohmann::json::parse(notify.json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    *why = "party_data_not_a_json_object";
    return false;
  }
  const SocialParty::DataOutcome outcome = ports.party->ReceiveData(notify.partyId, notify.memberId, notify.json);
  const auto headset = parsed.find("headsettype");
  LogFields(LogLevel::kInfo, "social_party_data_received",
            {{"party", static_cast<long long>(notify.partyId)}, {"member", static_cast<long long>(notify.memberId)},
             {"seq", static_cast<long long>(notify.seq)}, {"bytes", notify.json.size()},
             {"keys", parsed.size()}, {"headsettype", headset != parsed.end() ? headset->dump().c_str() : "-"},
             {"outcome", SocialParty::DataOutcomeName(outcome)}});
  if (outcome == SocialParty::DataOutcome::kOwnIgnored) {
    *why = "party_data_own";
    return false;
  }
  if (outcome == SocialParty::DataOutcome::kOtherParty) {
    *why = "party_data_other_party";
    return false;
  }
  return true;
}

bool ApplyServerMessage(const Ports& ports, std::uint64_t sym, const std::uint8_t* payload, std::size_t len,
                        std::uint64_t now, const char** why) {
  bool consumed = false;
  *why = "no_change";

  if (sym == SocialNames::kProfileSuccess) {
    std::uint64_t accountId = 0;
    std::string displayName;
    if (SocialNames::DecodeProfile(payload, len, &accountId, &displayName)) {
      ports.friends->SetName(accountId, displayName);
      ports.party->SetName(accountId, displayName);
      consumed = true;
    }
  } else if (sym == SocialRoster::kFriendPresenceNotify) {
    std::uint64_t friendId = 0;
    SocialRoster::Presence presence;
    if (SocialRoster::ParsePresenceNotify(payload, len, &friendId, &presence)) {
      ports.friends->SetPresence(friendId, presence);
      consumed = true;
    } else {
      LogFields(LogLevel::kWarn, "social_frame", {{"name", "FriendPresenceNotify"}, {"result", "unreadable"}, {"bytes", len}});
    }
  } else if (sym == SocialRoster::kRecentlyMetListResponse) {
    std::vector<SocialRoster::Entry> people;
    if (SocialRoster::ParseRecentlyMetResponse(payload, len, &people)) {
      ports.recent->SetList(std::move(people));
      consumed = true;
    } else {
      ports.recent->EndRefresh();  // the game is polling the refresh; an unreadable answer ends it
      LogFields(LogLevel::kWarn, "social_frame", {{"name", "RecentlyMetListResponse"}, {"result", "unreadable"}, {"bytes", len}});
    }
  } else if (sym == kSymFriendStatusNotify) {
    std::uint64_t friendId = 0;
    std::uint8_t status = 0;
    if (SocialRoster::ParseStatusNotify(payload, len, &friendId, &status)) {
      ports.friends->Notify(friendId, status);
      WantName(ports, "friend name lookup", friendId);
      consumed = true;
    }
  } else if (sym == kSymFriendListResponse) {
    std::uint32_t confirmed = 0;
    if (SocialRoster::ParseListResponse(payload, len, &confirmed)) {
      ports.friends->BeginList(confirmed);
      consumed = true;
    }
  }

  if (sym == SocialParty::kPartyDataNotify) consumed = ApplyPartyData(ports, payload, len, why) || consumed;

  // A friend added, accepted, removed or withdrawn carries no presence: ask for the list again.
  if (SocialRoster::IsFriendChangeSymbol(sym)) {
    Send(ports, "friend list refresh", ports.party->RefreshFriends());
    consumed = true;
  }

  // Party messages update the party model; requests that were waiting on the reply (invites queued
  // behind the party's creation) go out now.
  std::vector<SocialParty::Message> outgoing;
  if (ports.party->Feed(sym, payload, len, now, &outgoing)) {
    consumed = true;
    Send(ports, "party follow-up", outgoing);
  }
  // A member or an invite sender with no name yet: ask for the profile, as for friends.
  for (const std::uint64_t accountId : ports.party->TakeUnnamed()) WantName(ports, "party name lookup", accountId);
  return consumed;
}

}  // namespace

FrameStats ObserveFrames(const Ports& ports, Direction direction, const std::uint8_t* data, std::size_t len,
                         std::uint64_t nowSeconds) noexcept {
  FrameStats stats;
  if (data == nullptr || ports.party == nullptr || ports.friends == nullptr || ports.recent == nullptr) return stats;
  try {
    const std::uint8_t* p = data;
    std::size_t remaining = len;
    while (remaining >= kFrameHeaderBytes) {
      if (std::memcmp(p, kMarker, sizeof(kMarker)) != 0) {
        stats.malformed = 1;
        break;
      }
      const std::uint64_t sym = Le64(p + 8);
      const std::uint64_t payloadLen = Le64(p + 16);
      if (payloadLen > remaining - kFrameHeaderBytes) {  // truncated or corrupt: stop, never read past it
        stats.malformed = 1;
        break;
      }
      ++stats.messages;
      const std::uint8_t* payload = p + kFrameHeaderBytes;
      const std::size_t n = static_cast<std::size_t>(payloadLen);
      if (const char* name = KnownName(sym)) {
        LogFields(LogLevel::kInfo, "social_frame",
                  {{"dir", direction == Direction::kServerToGame ? "server_to_game" : "game_to_server"},
                   {"name", name}, {"bytes", n}});
      }
      if (direction == Direction::kServerToGame) {
        const char* why = "no_change";
        if (ApplyServerMessage(ports, sym, payload, n, nowSeconds, &why)) {
          ++stats.consumed;
        } else if (const char* name = KnownName(sym)) {
          // A social frame the observer recognised and changed nothing for: counted for the reporter, logged here
          // (the network adapter's thread, not the game's).
          NoteFrameIgnored();
          LogFields(LogLevel::kWarn, "social_frame_ignored", {{"name", name}, {"why", why}, {"bytes", n}});
        }
      }
      p += kFrameHeaderBytes + n;
      remaining -= kFrameHeaderBytes + n;
    }
  } catch (const std::exception&) {
    stats.malformed = 1;
    LogFields(LogLevel::kError, "social_frame", {{"result", "exception"}, {"action", "frame_dropped"}});
  }
  return stats;
}

}  // namespace quest_social
