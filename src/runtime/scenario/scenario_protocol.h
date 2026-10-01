#pragma once

// The scenario control protocol: one JSON object per line from the runner, one per line back. Pure
// (no game, no sockets) so every build's unit tests cover it, including builds where the control
// endpoint itself is compiled out. See docs/design/2026-10-01-social-scenario-harness.md.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/compat/social_party.h"

namespace ScenarioProtocol {

enum class Op { kState, kInjectFriendStatus, kInjectFriendNotify, kInjectPartyInvite, kFireFriendInvite, kFireAddFriend,
                kFireRespondInvite };

struct Command {
  Op op = Op::kState;
  std::uint64_t friendId = 0;   // inject FriendStatusNotify
  std::uint8_t status = 0;      // inject FriendStatusNotify: 0 online, 1 busy, 2 offline
  std::string user;             // fire friend_invite / add_friend: the user id string the node passes
  std::uint64_t notifySymbol = 0;  // inject a friend change notify
  std::string notifyName;
  std::uint64_t partyId = 0;       // inject PartyInviteNotify
  std::uint64_t inviterId = 0;
  std::uint32_t inviteIndex = 0;   // fire respond_to_invite: the game's invite index (newest first)
  bool accept = false;
};

/// SNSPartyInviteNotify: PartyID(8) InviterID(8) (nakama server/evr_pipeline_party.go sends it to
/// the invitee).
constexpr std::uint64_t kPartyInviteNotifySymbol = 0x218f721f09026dabULL;

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
    const FriendNotify* notify = FindFriendNotify(msg);
    if (msg != "FriendStatusNotify" && notify == nullptr) {
      *error = "inject supports msg \"FriendStatusNotify\", \"PartyInviteNotify\" and the friend notifies (FriendAcceptNotify, "
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
    if (action != "friend_invite" && action != "add_friend") {
      *error = "fire supports action \"friend_invite\", \"add_friend\" and \"respond_to_invite\"";
      return false;
    }
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
  SocialParty::Message m;
  m.symbol = kFriendStatusNotifySymbol;
  SocialParty::AppendLe(m.payload, 0, 8);
  SocialParty::AppendLe(m.payload, friendId, 8);
  SocialParty::AppendLe(m.payload, status, 1);
  SocialParty::AppendLe(m.payload, 0, 7);
  return SocialParty::Frame(m);
}

/// A friend-change notify frame: Header(8) FriendID(8), and StatusCode(1) Reserved(7) when the
/// message has a status (accept notify / success; 0 = success).
inline std::string BuildFriendNotify(const FriendNotify& notify, std::uint64_t friendId) {
  SocialParty::Message m;
  m.symbol = notify.symbol;
  SocialParty::AppendLe(m.payload, 0, 8);
  SocialParty::AppendLe(m.payload, friendId, 8);
  if (notify.hasStatus) SocialParty::AppendLe(m.payload, 0, 8);
  return SocialParty::Frame(m);
}

inline std::string BuildPartyInviteNotify(std::uint64_t partyId, std::uint64_t inviterId) {
  SocialParty::Message m;
  m.symbol = kPartyInviteNotifySymbol;
  SocialParty::AppendLe(m.payload, partyId, 8);
  SocialParty::AppendLe(m.payload, inviterId, 8);
  return SocialParty::Frame(m);
}

}  // namespace ScenarioProtocol
