#pragma once
// #963: the guild picker of the CREATE GAME wizard, as pure logic. What nakama's "eligible guilds for create" answer
// means, which guild a player starts on, and whether a chosen guild is one the answer offered.
//
// No game, no socket, no hook and no clock in here. The RPC call, the picker page and the create request that carries
// the choice are later changes (issue #963, "Pieces"; ADR 0017). The server re-checks every create request; this file
// only keeps the client from offering or sending a guild the answer did not list.
//
// The answer shape and the RPC name are proposals until the nakama half lands. src/runtime/tests/test_guild_picker.cpp
// pins everything below on the host.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace nevr_guild {

// The nakama RPC that returns the caller's eligible guilds. Proposed name; the nakama issue owns it.
inline constexpr std::string_view kEligibleRpc = "guild/create_eligible";
// An answer longer than this is not looked at. A player in a few dozen guilds stays far below it.
inline constexpr std::size_t kAnswerMaxBytes = 64 * 1024;
// A guild id is a UUID in nakama. Anything longer than this, or with a character outside printable ASCII, is skipped.
inline constexpr std::size_t kIdMaxChars = 64;
// The picker's text container holds 63 symbols plus a terminator (as in patch/invite_wizard.h). A longer label is cut
// to that; only printable ASCII is kept, because the game's font draws nothing else (N148).
inline constexpr std::size_t kLabelMaxChars = 63;
// The key the create request carries the chosen guild under. Not yet matched to the game's create-request layout
// (issue #963, Pieces 2); the wiring that sends it is a later change.
inline constexpr std::string_view kCreateGuildKey = "guild_id";

struct Guild {
  std::string id;     // nakama group id; the create request names it
  std::string label;  // what the picker shows
  bool borrowed = false;  // true for a guild whose servers the caller may borrow (allocator), not one they belong to
};

namespace detail {

inline bool IsPrintableAscii(std::string_view text) {
  for (const char c : text) {
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

inline std::string_view Clip(std::string_view text, std::size_t max_chars) {
  return text.size() <= max_chars ? text : text.substr(0, max_chars);
}

}  // namespace detail

// Reads the RPC answer, {"guilds":[{"id":..,"label":..,"borrowed":..}, ...]}. Returns nullopt when the answer is not
// that shape at all. An entry that is malformed (no string id, a non-printable id or label, a non-boolean borrowed) is
// skipped, not guessed at. A repeated id keeps its first entry.
inline std::optional<std::vector<Guild>> ParseEligible(std::string_view answer) {
  if (answer.size() > kAnswerMaxBytes) return std::nullopt;
  const nlohmann::json doc = nlohmann::json::parse(answer, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
  const auto guilds = doc.find("guilds");
  if (guilds == doc.end() || !guilds->is_array()) return std::nullopt;

  std::vector<Guild> out;
  for (const nlohmann::json& entry : *guilds) {
    if (!entry.is_object()) continue;
    const auto id = entry.find("id");
    const auto label = entry.find("label");
    if (id == entry.end() || !id->is_string() || label == entry.end() || !label->is_string()) continue;

    const std::string& id_text = id->get_ref<const std::string&>();
    const std::string& label_text = label->get_ref<const std::string&>();
    if (id_text.empty() || id_text.size() > kIdMaxChars || !detail::IsPrintableAscii(id_text)) continue;
    if (!detail::IsPrintableAscii(label_text)) continue;

    bool borrowed = false;
    const auto borrowed_field = entry.find("borrowed");
    if (borrowed_field != entry.end()) {
      if (!borrowed_field->is_boolean()) continue;
      borrowed = borrowed_field->get<bool>();
    }

    bool repeated = false;
    for (const Guild& seen : out) {
      if (seen.id == id_text) repeated = true;
    }
    if (repeated) continue;

    out.push_back(Guild{id_text, std::string(detail::Clip(label_text, kLabelMaxChars)), borrowed});
  }
  return out;
}

// The guild a create starts on when the player has not picked one: the first guild the caller belongs to, else the
// first borrowable one. nullopt when the list is empty, which means the picker offers no create at all.
inline std::optional<Guild> DefaultChoice(const std::vector<Guild>& eligible) {
  for (const Guild& guild : eligible) {
    if (!guild.borrowed) return guild;
  }
  if (!eligible.empty()) return eligible.front();
  return std::nullopt;
}

// The guild the answer lists under this id, or nullopt. The client sends only an id it was offered, so a choice made
// outside the list is refused here before it reaches the create request.
inline std::optional<Guild> Choose(const std::vector<Guild>& eligible, std::string_view id) {
  for (const Guild& guild : eligible) {
    if (guild.id == id) return guild;
  }
  return std::nullopt;
}

// The part of the create request that carries the choice.
inline nlohmann::json CreateGuildField(const Guild& guild) {
  return nlohmann::json{{std::string(kCreateGuildKey), guild.id}};
}

}  // namespace nevr_guild
