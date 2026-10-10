#pragma once

// The scenario control protocol: one JSON object per line from the runner, one per line back. Pure
// (no game, no sockets) so every build's unit tests cover it, including builds where the control
// endpoint itself is compiled out. See docs/design/2026-10-01-social-scenario-harness.md.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"

namespace nevr_scenario_protocol {

enum class Op { kState, kInjectFriendStatus, kInjectFriendNotify, kInjectPartyInvite, kInjectPartyJoinFailure,
                kInjectPartyMember, kInjectFriendPresence, kInjectPartyData, kInjectRecentlyMet, kFireFriendInvite, kFireAddFriend, kFireRespondInvite,
                kFireAction };

struct Command {
  Op op = Op::kState;
  std::uint64_t friendId = 0;   // inject FriendStatusNotify
  std::uint8_t status = 0;      // inject FriendStatusNotify: 0 online, 1 busy, 2 offline
  std::string user;             // fire friend_invite / add_friend: the user id string the node passes
  std::uint64_t notifySymbol = 0;  // inject a friend change notify
  std::string notifyName;
  std::uint64_t partyId = 0;       // inject PartyInviteNotify
  std::uint64_t inviterId = 0;
  std::uint8_t failureCode = 0;
  std::uint64_t memberId = 0;      // inject PartyJoinNotify / PartyLeaveNotify (partyId 0 = the current party)
  std::string action;              // fire <action> (kFireAction): which node's entry point
  std::uint64_t number = 0;        // fire: mode, mask, policy, index or feature, per action
  bool flag = false;               // fire: lock, mute or enable, per action
  std::string key;                 // fire set_party_*_string: the data key
  std::string value;               // fire set_party_*_string: the value; inject FriendPresenceNotify: the text    // inject PartyJoinFailure: Nakama's code (1 unknown party, 2 refused)
  std::uint32_t inviteIndex = 0;   // fire respond_to_invite: the game's invite index (newest first)
  bool accept = false;
  std::vector<nevr_social_roster::Entry> people;  // inject RecentlyMetListResponse
};

/// SNSPartyInviteNotify: PartyID(8) InviterID(8) (nakama server/evr_pipeline_party.go sends it to
/// the invitee).
constexpr std::uint64_t kPartyInviteNotifySymbol = 0x218f721f09026dabULL;

/// SNSPartyJoinFailure: PartyID(8) ErrorCode(1) (nakama server/evr/sns_party.go).
constexpr std::uint64_t kPartyJoinFailureSymbol = 0xb56f26d44a42e11dULL;

/// SNSFriendStatusNotify, as the game's symbol table names it ("FriendStatusNotify").
constexpr std::uint64_t kFriendStatusNotifySymbol = 0x26a19dc4d2d5579dULL;

/// Friend-change notifies (nakama server/evr/sns_friends.go): Header(8) FriendID(8), plus
/// StatusCode(1) Reserved(7) for the accept ones. Symbols from the game's symbol table.
struct FriendNotify {
  const char* name;
  std::uint64_t symbol;
  bool hasStatus;
};
constexpr FriendNotify kFriendNotifies[] = {
    {"FriendAcceptNotify", 0xc237c84c31d3ae05ULL, true},
    {"FriendAcceptSuccess", 0x1bbda7fa06af4627ULL, true},
    {"FriendInviteNotify", 0xca09b0b36bd981b7ULL, false},
    {"FriendInviteSuccess", 0x7f0c6a3ac83c6f77ULL, false},
    {"FriendRemoveNotify", 0xe06972f49cd72265ULL, false},
    {"FriendWithdrawnNotify", 0x191aa30801ec6d03ULL, false},
    {"FriendRejectNotify", 0xb9b86c0ce8e8d0c1ULL, false},
};

inline const FriendNotify* FindFriendNotify(const std::string& name) {
  for (const FriendNotify& n : kFriendNotifies)
    if (name == n.name) return &n;
  return nullptr;
}

/// social_groups_set_active's "current": the group that is already active.
constexpr std::uint64_t kCurrentGroup = UINT64_MAX;

/// The fire actions beyond the three above: each names a script node whose entry point the control
/// endpoint drives the way the node does (scenario_control.cpp has the addresses).
inline bool ParseFireAction(const nlohmann::json& j, const std::string& action, Command* out, std::string* error) {
  Command cmd;
  cmd.op = Op::kFireAction;
  cmd.action = action;
  const auto u64 = [&](const char* field, std::uint64_t max, std::uint64_t* value) {
    if (!j.contains(field) || !j[field].is_number_unsigned() || j[field].get<std::uint64_t>() > max) {
      *error = "fire " + action + " needs an unsigned \"" + field + "\" up to " + std::to_string(max);
      return false;
    }
    *value = j[field].get<std::uint64_t>();
    return true;
  };
  const auto boolean = [&](const char* field, bool* value) {
    if (!j.contains(field) || !j[field].is_boolean()) {
      *error = "fire " + action + " needs a boolean \"" + field + "\"";
      return false;
    }
    *value = j[field].get<bool>();
    return true;
  };
  const auto text = [&](const char* field, bool required, std::string* value) {
    if (!j.contains(field)) {
      if (required) *error = "fire " + action + " needs a string \"" + field + "\"";
      return !required;
    }
    if (!j[field].is_string() || j[field].get<std::string>().size() >= 0x40) {
      *error = "fire " + action + ": \"" + field + "\" must be a string shorter than 64 characters";
      return false;
    }
    *value = j[field].get<std::string>();
    return true;
  };
  bool ok = false;
  if (action == "invite_users") {
    ok = u64("mode", 2, &cmd.number) && text("user", false, &cmd.user);
  } else if (action == "request_profile") {
    ok = text("user", true, &cmd.user) && !cmd.user.empty();
    if (!ok && error->empty())
      *error = "fire request_profile needs a non-empty \"user\" (an id string, \"self\" or \"friend\")";
  } else if (action == "party_join") {
    ok = u64("party", UINT64_MAX, &cmd.partyId) && cmd.partyId != 0;
    if (!ok && error->empty()) *error = "fire party_join needs a nonzero \"party\"";
  } else if (action == "party_lock") {
    cmd.number = 1;
    ok = boolean("lock", &cmd.flag) && (!j.contains("mask") || u64("mask", 0xFF, &cmd.number));
  } else if (action == "set_join_policy") {
    ok = u64("policy", 3, &cmd.number);
  } else if (action == "voip_mute_self") {
    ok = boolean("mute", &cmd.flag);
  } else if (action == "voip_mute_user") {
    ok = text("user", true, &cmd.user) && !cmd.user.empty() && boolean("mute", &cmd.flag);
    if (!ok && error->empty()) *error = "fire voip_mute_user needs a non-empty \"user\" and a boolean \"mute\"";
  } else if (action == "social_groups_set_active") {
    if (j.contains("index") && j["index"].is_string() && j["index"].get<std::string>() == "current") {
      cmd.number = kCurrentGroup;  // re-select the active group: the path runs, the account's setting stays
      ok = true;
    } else {
      ok = u64("index", 0xFFFF, &cmd.number);
    }
  } else if (action == "enable_social_feature") {
    ok = u64("feature", 4, &cmd.number) && boolean("enable", &cmd.flag);
  } else if (action == "set_party_member_string" || action == "set_party_string") {
    ok = text("key", true, &cmd.key) && text("value", true, &cmd.value) && !cmd.key.empty();
    if (!ok && error->empty()) *error = "fire " + action + " needs a non-empty \"key\"";
  } else if (action == "refresh_friends" || action == "find_arena") {
    ok = true;
  } else if (action == "party_join_failed_callback") {
    ok = u64("code", 0xFFFF, &cmd.number);
  } else if (action == "refresh_recently_met") {
    ok = true;
  } else if (action == "early_quit_lockout") {
    ok = u64("seconds", 7ULL * 24 * 3600, &cmd.number);  // 0 clears the lockout
  } else if (action == "early_quit_countdown_active") {
    ok = boolean("active", &cmd.flag);
  } else if (action == "early_quit_warning") {
    ok = boolean("show", &cmd.flag);
  } else if (action == "dispatch_event") {
    // A 64-bit event symbol as a "0x..." hex string (JSON numbers lose precision past 2^53 in many clients).
    std::string hex;
    ok = text("event", true, &hex) && hex.size() > 2 && hex.size() <= 18 && hex.rfind("0x", 0) == 0 &&
         hex.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos;
    if (ok) cmd.number = std::stoull(hex.substr(2), nullptr, 16);
    if (!ok && error->empty()) *error = "fire dispatch_event needs \"event\": a \"0x...\" hex symbol of up to 16 digits";
  } else if (action == "early_quit_feature_flags") {
    cmd.flag = false;  // raw: store the byte as sent, without the callback's & 0xdd
    ok = u64("flags", 0xFF, &cmd.number) && (!j.contains("raw") || boolean("raw", &cmd.flag));
  } else {
    *error = "fire supports friend_invite, add_friend, respond_to_invite, invite_users, request_profile, party_join, "
             "party_lock, set_join_policy, voip_mute_self, voip_mute_user, social_groups_set_active, enable_social_feature, "
             "set_party_member_string, set_party_string, refresh_friends, refresh_recently_met, find_arena, "
             "party_join_failed_callback, early_quit_lockout, early_quit_countdown_active, early_quit_warning, early_quit_feature_flags and dispatch_event";
    return false;
  }
  if (!ok) return false;
  *out = cmd;
  return true;
}

/// Parses one command line. On failure returns false and sets `error` to a message that names
/// what was wrong, so a runner failure points at the line it sent.
inline bool ParseCommand(const std::string& line, Command* out, std::string* error) {
  if (out == nullptr || error == nullptr) return false;
  const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    *error = "command is not a JSON object";
    return false;
  }
  if (!j.contains("op") || !j["op"].is_string()) {
    *error = "command has no string \"op\"";
    return false;
  }
  const std::string op = j["op"].get<std::string>();
  Command cmd;
  if (op == "state") {
    cmd.op = Op::kState;
  } else if (op == "inject") {
    const std::string msg = j.contains("msg") && j["msg"].is_string() ? j["msg"].get<std::string>() : "";
    if (msg == "PartyInviteNotify") {
      if (!j.contains("party") || !j["party"].is_number_unsigned() || j["party"].get<std::uint64_t>() == 0 ||
          !j.contains("inviter") || !j["inviter"].is_number_unsigned() || j["inviter"].get<std::uint64_t>() == 0) {
        *error = "inject PartyInviteNotify needs nonzero unsigned \"party\" and \"inviter\"";
        return false;
      }
      cmd.op = Op::kInjectPartyInvite;
      cmd.partyId = j["party"].get<std::uint64_t>();
      cmd.inviterId = j["inviter"].get<std::uint64_t>();
      *out = cmd;
      return true;
    }
    if (msg == "PartyJoinNotify" || msg == "PartyLeaveNotify") {
      if (!j.contains("member") || !j["member"].is_number_unsigned() || j["member"].get<std::uint64_t>() == 0 ||
          (j.contains("party") && !j["party"].is_number_unsigned())) {
        *error = "inject " + msg + " needs a nonzero unsigned \"member\" (and \"party\", default the current party)";
        return false;
      }
      cmd.op = Op::kInjectPartyMember;
      cmd.notifyName = msg;
      cmd.memberId = j["member"].get<std::uint64_t>();
      cmd.partyId = j.contains("party") ? j["party"].get<std::uint64_t>() : 0;
      *out = cmd;
      return true;
    }
    if (msg == "FriendPresenceNotify") {
      if (!j.contains("id") || !j["id"].is_number_unsigned() || j["id"].get<std::uint64_t>() == 0 ||
          (j.contains("party") && !j["party"].is_number_unsigned()) ||
          (j.contains("text") && (!j["text"].is_string() || j["text"].get<std::string>().size() > 200))) {
        *error = "inject FriendPresenceNotify needs a nonzero \"id\", optional unsigned \"party\" and \"text\" (<= 200)";
        return false;
      }
      cmd.op = Op::kInjectFriendPresence;
      cmd.friendId = j["id"].get<std::uint64_t>();
      cmd.partyId = j.value("party", std::uint64_t{0});
      cmd.flag = j.contains("joinable") && j["joinable"].is_boolean() ? j["joinable"].get<bool>() : cmd.partyId != 0;
      cmd.value = j.value("text", std::string());
      *out = cmd;
      return true;
    }
    if (msg == "RecentlyMetListResponse") {
      // "users": [{"id", "name", "online", "party", "text"}], in the server's order.
      if (!j.contains("users") || !j["users"].is_array()) {
        *error = "inject RecentlyMetListResponse needs \"users\": [{\"id\", \"name\", \"online\", \"party\", \"text\"}]";
        return false;
      }
      cmd.op = Op::kInjectRecentlyMet;
      for (const auto& u : j["users"]) {
        if (!u.is_object() || !u.contains("id") || !u["id"].is_number_unsigned()) {
          *error = "inject RecentlyMetListResponse: each user needs an unsigned \"id\"";
          return false;
        }
        nevr_social_roster::Entry e;
        e.id = u["id"].get<std::uint64_t>();
        e.name = u.contains("name") && u["name"].is_string() ? u["name"].get<std::string>() : std::string();
        e.online = u.contains("online") && u["online"].is_boolean() && u["online"].get<bool>();
        e.presence.partyId = u.contains("party") && u["party"].is_number_unsigned() ? u["party"].get<std::uint64_t>() : 0;
        e.presence.joinable = e.presence.partyId != 0;
        e.presence.text = u.contains("text") && u["text"].is_string() ? u["text"].get<std::string>() : std::string();
        cmd.people.push_back(std::move(e));
      }
      *out = cmd;
      return true;
    }
    if (msg == "PartyDataNotify") {
      // The server's party data (proposal §3): "member" 0 or absent is the party's, "json" an object
      // (or its text), "party" defaults to the current party.
      if ((j.contains("member") && !j["member"].is_number_unsigned()) ||
          (j.contains("party") && !j["party"].is_number_unsigned()) || !j.contains("json") ||
          !(j["json"].is_object() || j["json"].is_string())) {
        *error = "inject PartyDataNotify needs \"json\" (an object or its text), optional unsigned \"member\" and \"party\"";
        return false;
      }
      cmd.op = Op::kInjectPartyData;
      cmd.memberId = j.value("member", std::uint64_t{0});
      cmd.partyId = j.value("party", std::uint64_t{0});
      cmd.value = j["json"].is_string() ? j["json"].get<std::string>() : j["json"].dump();
      *out = cmd;
      return true;
    }
    if (msg == "PartyJoinSuccess") {
      if (!j.contains("party") || !j["party"].is_number_unsigned() || j["party"].get<std::uint64_t>() == 0 ||
          !j.contains("owner") || !j["owner"].is_number_unsigned() || j["owner"].get<std::uint64_t>() == 0) {
        *error = "inject PartyJoinSuccess needs nonzero unsigned \"party\" and \"owner\"";
        return false;
      }
      cmd.op = Op::kInjectPartyMember;  // PartyID(8) OwnerID(8): the same two-id shape
      cmd.notifyName = msg;
      cmd.partyId = j["party"].get<std::uint64_t>();
      cmd.memberId = j["owner"].get<std::uint64_t>();
      *out = cmd;
      return true;
    }
    if (msg == "PartyJoinFailure") {
      if (!j.contains("party") || !j["party"].is_number_unsigned() || !j.contains("code") ||
          !j["code"].is_number_unsigned() || j["code"].get<std::uint64_t>() > 0xFF) {
        *error = "inject PartyJoinFailure needs an unsigned \"party\" and a \"code\" byte";
        return false;
      }
      cmd.op = Op::kInjectPartyJoinFailure;
      cmd.partyId = j["party"].get<std::uint64_t>();
      cmd.failureCode = static_cast<std::uint8_t>(j["code"].get<std::uint64_t>());
      *out = cmd;
      return true;
    }
    const FriendNotify* notify = FindFriendNotify(msg);
    if (msg != "FriendStatusNotify" && notify == nullptr) {
      *error = "inject supports msg \"FriendStatusNotify\", \"PartyDataNotify\", \"RecentlyMetListResponse\", \"PartyInviteNotify\", \"PartyJoinSuccess\", \"PartyJoinFailure\", \"PartyJoinNotify\", \"PartyLeaveNotify\" and the friend notifies (FriendAcceptNotify, "
               "FriendAcceptSuccess, FriendInviteNotify, FriendInviteSuccess, FriendRemoveNotify, "
               "FriendWithdrawnNotify, FriendRejectNotify)";
      return false;
    }
    if (!j.contains("id") || !j["id"].is_number_unsigned() || j["id"].get<std::uint64_t>() == 0) {
      *error = "inject " + msg + " needs a nonzero unsigned \"id\"";
      return false;
    }
    if (notify != nullptr) {
      cmd.op = Op::kInjectFriendNotify;
      cmd.friendId = j["id"].get<std::uint64_t>();
      cmd.notifySymbol = notify->symbol;
      cmd.notifyName = notify->name;
      *out = cmd;
      return true;
    }
    if (!j.contains("status") || !j["status"].is_number_unsigned() || j["status"].get<std::uint64_t>() > 2) {
      *error = "inject FriendStatusNotify needs \"status\" 0 (online), 1 (busy) or 2 (offline)";
      return false;
    }
    cmd.op = Op::kInjectFriendStatus;
    cmd.friendId = j["id"].get<std::uint64_t>();
    cmd.status = static_cast<std::uint8_t>(j["status"].get<std::uint64_t>());
  } else if (op == "fire") {
    const std::string action = j.contains("action") && j["action"].is_string() ? j["action"].get<std::string>() : "";
    if (action == "respond_to_invite") {
      if (!j.contains("index") || !j["index"].is_number_unsigned() || j["index"].get<std::uint64_t>() > 0xFFFF ||
          !j.contains("accept") || !j["accept"].is_boolean()) {
        *error = "fire respond_to_invite needs an unsigned \"index\" and a boolean \"accept\"";
        return false;
      }
      cmd.op = Op::kFireRespondInvite;
      cmd.inviteIndex = static_cast<std::uint32_t>(j["index"].get<std::uint64_t>());
      cmd.accept = j["accept"].get<bool>();
      *out = cmd;
      return true;
    }
    if (action != "friend_invite" && action != "add_friend") return ParseFireAction(j, action, out, error);
    if (!j.contains("user") || !j["user"].is_string() || j["user"].get<std::string>().empty() ||
        j["user"].get<std::string>().size() >= 40) {
      *error = "fire " + action + " needs a \"user\" id string shorter than 40 characters";
      return false;
    }
    cmd.op = action == "add_friend" ? Op::kFireAddFriend : Op::kFireFriendInvite;
    cmd.user = j["user"].get<std::string>();
  } else {
    *error = "unknown op \"" + op + "\" (state, inject, fire)";
    return false;
  }
  *out = cmd;
  return true;
}

/// The SNSFriendStatusNotify frame Nakama sends when a friend's presence changes: Header(8)
/// FriendID(8) StatusCode(1) Reserved(7) (nakama server/evr/sns_friends.go).
inline std::string BuildFriendStatusNotify(std::uint64_t friendId, std::uint8_t status) {
  nevr_social_party::Message m;
  m.symbol = kFriendStatusNotifySymbol;
  nevr_social_party::AppendLe(m.payload, 0, 8);
  nevr_social_party::AppendLe(m.payload, friendId, 8);
  nevr_social_party::AppendLe(m.payload, status, 1);
  nevr_social_party::AppendLe(m.payload, 0, 7);
  return nevr_social_party::Frame(m);
}

/// A friend-change notify frame: Header(8) FriendID(8), and StatusCode(1) Reserved(7) when the
/// message has a status (accept notify / success; 0 = success).
inline std::string BuildFriendNotify(const FriendNotify& notify, std::uint64_t friendId) {
  nevr_social_party::Message m;
  m.symbol = notify.symbol;
  nevr_social_party::AppendLe(m.payload, 0, 8);
  nevr_social_party::AppendLe(m.payload, friendId, 8);
  if (notify.hasStatus) nevr_social_party::AppendLe(m.payload, 0, 8);
  return nevr_social_party::Frame(m);
}

/// SNSPartyJoinNotify / SNSPartyLeaveNotify: PartyID(8) MemberID(8); SNSPartyJoinSuccess: PartyID(8)
/// OwnerID(8) (nakama server/evr/sns_party.go).
inline std::string BuildPartyMemberNotify(const char* name, std::uint64_t partyId, std::uint64_t memberId) {
  nevr_social_party::Message m;
  m.symbol = nevr_social_party::ReplySymbol(name);
  nevr_social_party::AppendLe(m.payload, partyId, 8);
  nevr_social_party::AppendLe(m.payload, memberId, 8);
  return nevr_social_party::Frame(m);
}

/// SNSFriendPresenceNotify (social_roster.h kFriendPresenceNotify): Header(8) FriendID(8) PartyID(8)
/// Joinable(1) StatusCode(1, online) Reserved(6) TextLen(2) Text.
inline std::string BuildFriendPresenceNotify(std::uint64_t friendId, std::uint64_t partyId, bool joinable,
                                             const std::string& text) {
  nevr_social_party::Message m;
  m.symbol = 0xbdd8dd00c5e97a63ULL;
  nevr_social_party::AppendLe(m.payload, 0, 8);
  nevr_social_party::AppendLe(m.payload, friendId, 8);
  nevr_social_party::AppendLe(m.payload, partyId, 8);
  nevr_social_party::AppendLe(m.payload, joinable ? 1 : 0, 1);
  nevr_social_party::AppendLe(m.payload, 0, 1);
  nevr_social_party::AppendLe(m.payload, 0, 6);
  nevr_social_party::AppendLe(m.payload, text.size(), 2);
  m.payload += text;
  return nevr_social_party::Frame(m);
}

/// SNSRecentlyMetListResponse (social_roster.h kRecentlyMetListResponse).
inline std::string BuildRecentlyMetListResponse(const std::vector<nevr_social_roster::Entry>& people) {
  nevr_social_party::Message m;
  m.symbol = nevr_social_roster::kRecentlyMetListResponse;
  nevr_social_party::AppendLe(m.payload, people.size(), 4);
  for (const nevr_social_roster::Entry& e : people) {
    nevr_social_party::AppendLe(m.payload, e.id, 8);
    nevr_social_party::AppendLe(m.payload, e.presence.partyId, 8);
    nevr_social_party::AppendLe(m.payload, e.presence.joinable ? 1 : 0, 1);
    nevr_social_party::AppendLe(m.payload, e.online ? nevr_social_roster::kStatusOnline : nevr_social_roster::kStatusOffline, 1);
    nevr_social_party::AppendLe(m.payload, 0, 6);
    nevr_social_party::AppendLe(m.payload, e.name.size(), 2);
    m.payload += e.name;
    nevr_social_party::AppendLe(m.payload, e.presence.text.size(), 2);
    m.payload += e.presence.text;
  }
  return nevr_social_party::Frame(m);
}

/// SNSPartyDataNotify (social_party.h kPartyDataNotify): PartyID(8) MemberID(8) Seq(4) JsonLen(4) Json.
inline std::string BuildPartyDataNotify(std::uint64_t partyId, std::uint64_t memberId, std::uint32_t seq,
                                        const std::string& json) {
  nevr_social_party::Message m;
  m.symbol = nevr_social_party::kPartyDataNotify;
  nevr_social_party::AppendLe(m.payload, partyId, 8);
  nevr_social_party::AppendLe(m.payload, memberId, 8);
  nevr_social_party::AppendLe(m.payload, seq, 4);
  nevr_social_party::AppendLe(m.payload, json.size(), 4);
  m.payload += json;
  return nevr_social_party::Frame(m);
}

inline std::string BuildPartyJoinFailure(std::uint64_t partyId, std::uint8_t code) {
  nevr_social_party::Message m;
  m.symbol = kPartyJoinFailureSymbol;
  nevr_social_party::AppendLe(m.payload, partyId, 8);
  nevr_social_party::AppendLe(m.payload, code, 1);
  return nevr_social_party::Frame(m);
}

inline std::string BuildPartyInviteNotify(std::uint64_t partyId, std::uint64_t inviterId) {
  nevr_social_party::Message m;
  m.symbol = kPartyInviteNotifySymbol;
  nevr_social_party::AppendLe(m.payload, partyId, 8);
  nevr_social_party::AppendLe(m.payload, inviterId, 8);
  return nevr_social_party::Frame(m);
}

}  // namespace nevr_scenario_protocol
