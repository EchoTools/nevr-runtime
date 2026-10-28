#pragma once
// #1156 (PR-D slice D1b), docs/adr/0017: the value of the config key network.invite_codes (flat key nevr_invite_codes, N133).
// Off unless the key says on. A value nobody meant (a misspelling) is off too and is reported by the caller at Warning, so a
// typo never turns a feature on that sends the player's token to the service.
#include <string>
#include <string_view>

namespace nevr_invite {

enum class Switch { Off, On, Unrecognised };

inline Switch ParseSwitch(const char* text) {
  if (text == nullptr || text[0] == '\0') return Switch::Off;
  std::string lower;
  for (const char* c = text; *c != '\0'; ++c) lower.push_back((*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c);
  if (lower == "true" || lower == "on" || lower == "yes" || lower == "1") return Switch::On;
  if (lower == "false" || lower == "off" || lower == "no" || lower == "0") return Switch::Off;
  return Switch::Unrecognised;
}

inline bool SwitchIsOn(Switch value) { return value == Switch::On; }

}  // namespace nevr_invite
