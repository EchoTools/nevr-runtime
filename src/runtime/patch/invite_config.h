#pragma once
// #1156 (PR-D slice D1b), docs/adr/0017: the value of the config key network.invite_codes (flat key nevr_invite_codes, N133).
// On unless the key says off: ADR 0017 makes the feature the default for a client. An absent key and an empty one are unset,
// and unset is the default; the startup line says so with `source=default`. A value nobody meant (a misspelling, or a list or
// a map) is off and is reported by the caller at Warning, so a value that is not a switch word never turns on a feature that
// sends the player's token to the service. A key the runtime does not read at all is as if absent: on.
#include <string>
#include <string_view>

namespace nevr_invite {

enum class Switch { Off, On, Unrecognised };

/// Where a value came from, for the startup line: the key's own text, or the default because the key was unset.
enum class Source { Default, Config };

/// The value of an unset key. ADR 0017: the feature is on by default for a client.
inline constexpr Switch kDefault = Switch::On;

/// True when the key is unset: absent (nullptr, as NevrCfgGetFlat returns it) or empty.
inline bool IsUnset(const char* text) { return text == nullptr || text[0] == '\0'; }

/// The switch for the key's text as NevrCfgGetFlat returns it. An unset key is kDefault. `structured` is the key holding a list or
/// a map (NevrCfgGetFlatIsStructured): not a value, so Unrecognised, whatever the text is.
inline Switch ParseSwitch(const char* text, bool structured = false) {
  if (structured) return Switch::Unrecognised;
  if (IsUnset(text)) return kDefault;
  std::string lower;
  for (const char* c = text; *c != '\0'; ++c) lower.push_back((*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c);
  if (lower == "true" || lower == "on" || lower == "yes" || lower == "1") return Switch::On;
  if (lower == "false" || lower == "off" || lower == "no" || lower == "0") return Switch::Off;
  return Switch::Unrecognised;
}

/// Where that value came from: the default when the key is unset, the config otherwise (a structured key is the config's).
inline Source SourceOf(const char* text, bool structured = false) {
  if (structured) return Source::Config;
  return IsUnset(text) ? Source::Default : Source::Config;
}

/// The word the startup line prints for a source.
inline const char* SourceName(Source source) { return source == Source::Default ? "default" : "config"; }

inline bool SwitchIsOn(Switch value) { return value == Switch::On; }

}  // namespace nevr_invite
