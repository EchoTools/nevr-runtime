#pragma once

// The scenario control protocol: one JSON object per line from the runner, one per line back. Pure
// (no game, no sockets) so every build's unit tests cover it, including builds where the control
// endpoint itself is compiled out. See docs/design/2026-10-01-social-scenario-harness.md.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/compat/social_party.h"

namespace ScenarioProtocol {

enum class Op { kState, kInjectFriendStatus, kFireFriendInvite };

struct Command {
  Op op = Op::kState;
  std::uint64_t friendId = 0;   // inject FriendStatusNotify
  std::uint8_t status = 0;      // inject FriendStatusNotify: 0 online, 1 busy, 2 offline
  std::string user;             // fire friend_invite: the user id string the friend row passes
};

/// SNSFriendStatusNotify, as the game's symbol table names it ("FriendStatusNotify").
constexpr std::uint64_t kFriendStatusNotifySymbol = 0x26a19dc4d2d5579dULL;

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
    if (!j.contains("msg") || !j["msg"].is_string() || j["msg"].get<std::string>() != "FriendStatusNotify") {
      *error = "inject supports only msg \"FriendStatusNotify\"";
      return false;
    }
    if (!j.contains("id") || !j["id"].is_number_unsigned() || j["id"].get<std::uint64_t>() == 0) {
      *error = "inject FriendStatusNotify needs a nonzero unsigned \"id\"";
      return false;
    }
    if (!j.contains("status") || !j["status"].is_number_unsigned() || j["status"].get<std::uint64_t>() > 2) {
      *error = "inject FriendStatusNotify needs \"status\" 0 (online), 1 (busy) or 2 (offline)";
      return false;
    }
    cmd.op = Op::kInjectFriendStatus;
    cmd.friendId = j["id"].get<std::uint64_t>();
    cmd.status = static_cast<std::uint8_t>(j["status"].get<std::uint64_t>());
  } else if (op == "fire") {
    if (!j.contains("action") || !j["action"].is_string() || j["action"].get<std::string>() != "friend_invite") {
      *error = "fire supports only action \"friend_invite\"";
      return false;
    }
    if (!j.contains("user") || !j["user"].is_string() || j["user"].get<std::string>().empty() ||
        j["user"].get<std::string>().size() >= 40) {
      *error = "fire friend_invite needs a \"user\" id string shorter than 40 characters";
      return false;
    }
    cmd.op = Op::kFireFriendInvite;
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

}  // namespace ScenarioProtocol
