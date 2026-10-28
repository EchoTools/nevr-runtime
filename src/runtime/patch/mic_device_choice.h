#pragma once

// Which capture endpoint the WASAPI mic provider opens on native Windows, in this order (the headset the
// player is wearing first, so a Chromium tab or a webcam that Windows made the default is not picked):
//   0. the player's saved choice, `audio.capture_device` in config.yaml (#412): an endpoint id, or a
//      case-insensitive part of a friendly name, when it names an active capture endpoint;
//   1. LibOVR's own answer — the runtime's ovr_GetAudioDeviceInGuidStr, an IMMDevice endpoint id — when it is
//      an active capture endpoint;
//   2. the active capture endpoint whose friendly name names a headset (Oculus / Rift / Quest / Meta Link);
//   3. the OS default capture endpoint (what the provider always used).
// A saved choice that matches no active endpoint is skipped with the reason logged, so an unplugged headset
// falls to the default rule and never leaves the player silent.
// Pure and injectable, so each step and each fallback is unit-tested without WASAPI or LibOVR
// (mic_provider.cpp supplies the real LibOVR call and the real endpoint enumeration).
//
// Wine/Proton keeps the OS default: LibOVR is absent there and the provider's behaviour must not change.

#include <cstddef>
#include <string>
#include <vector>

namespace nevr_mic_device {

struct Endpoint {
  std::wstring id;    // the IMMDevice endpoint id string
  std::wstring name;  // the friendly name, e.g. L"Headset Microphone (Oculus Virtual Audio Device)"
};

enum class Choice { Configured, LibOvr, NameMatch, OsDefault };

struct Pick {
  Choice choice = Choice::OsDefault;
  Endpoint endpoint;        // empty id: the caller opens the OS default endpoint
  std::string reason;       // why this step won / why the earlier steps did not
};

/// Whether the headset-first choice applies. Wine/Proton keeps the OS default.
inline bool ShouldChooseHeadsetDevice(bool isWine) { return !isWine; }

inline const char* ChoiceName(Choice c) {
  switch (c) {
    case Choice::Configured: return "configured";
    case Choice::LibOvr: return "libovr";
    case Choice::NameMatch: return "name-match";
    case Choice::OsDefault: return "os-default";
  }
  return "os-default";
}

/// Lower-cases ASCII only: endpoint names are matched by substring, not collated.
inline std::wstring AsciiLower(std::wstring s) {
  for (wchar_t& c : s) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return s;
}

/// The substrings that name a headset's own microphone. "meta" alone is not one: it is a substring of
/// unrelated product names, so only the two Meta product phrases are.
inline const std::vector<std::wstring>& HeadsetNameFragments() {
  static const std::vector<std::wstring> k = {L"oculus", L"rift", L"quest", L"meta quest", L"meta link"};
  return k;
}

inline bool NameLooksLikeHeadset(const std::wstring& name) {
  const std::wstring lower = AsciiLower(name);
  for (const std::wstring& fragment : HeadsetNameFragments()) {
    if (lower.find(fragment) != std::wstring::npos) return true;
  }
  return false;
}

/// Whether LibOVR's answer is IMMDevice::GetId-shaped: the call returned >= 0, the string is non-empty,
/// terminated well inside the buffer (`length` is its length, under `maxChars`) and starts with '{'. The
/// buffer size LibOVR expects is not known from a header here, so a wrong one must read as "not an id".
inline bool LooksLikeEndpointId(int result, const wchar_t* text, size_t length, size_t maxChars) {
  return result >= 0 && text != nullptr && length > 0 && length < maxChars && text[0] == L'{';
}

/// `libovrId` is what LibOVR returned ("" when the call is unavailable or failed); `libovrCode` is its result
/// (0 when not called), kept so the log can say why step 1 did not win. `active` is every ACTIVE capture
/// endpoint. A step whose answer is not among `active` is skipped, never trusted: a stale id would fail the
/// open and leave the player silent where the OS default would have worked.
/// Whether a saved preference names this endpoint: an exact id, or a case-insensitive part of the friendly name.
inline bool PreferenceNamesEndpoint(const std::wstring& preference, const Endpoint& e) {
  if (preference.empty()) return false;
  if (e.id == preference) return true;
  return AsciiLower(e.name).find(AsciiLower(preference)) != std::wstring::npos;
}

/// `preference` is the saved `audio.capture_device` ("" when unset). Step 0 is the saved choice; a preference
/// that names no active endpoint is recorded in the reason and the rest of the order runs as before.
inline Pick ChooseWithPreference(bool isWine, const std::wstring& preference, const std::wstring& libovrId,
                                 long libovrCode, const std::vector<Endpoint>& active) {
  Pick pick;
  if (!ShouldChooseHeadsetDevice(isWine)) {
    pick.reason = "Wine/Proton keeps the OS default capture device";
    return pick;
  }
  std::string why;
  if (!preference.empty()) {
    for (const Endpoint& e : active) {
      if (PreferenceNamesEndpoint(preference, e)) {
        pick.choice = Choice::Configured;
        pick.endpoint = e;
        pick.reason = "audio.capture_device in config.yaml names this endpoint";
        return pick;
      }
    }
    why = "audio.capture_device names no active capture endpoint";
  }
  // Joins a clause onto the reasons already recorded, so each step's log line says why the earlier steps did not win.
  const auto join = [&why](const std::string& clause) { return why.empty() ? clause : why + "; " + clause; };
  if (libovrId.empty()) {
    why = join("LibOVR returned no device (code " + std::to_string(libovrCode) + ")");
  } else {
    for (const Endpoint& e : active) {
      if (e.id == libovrId) {
        pick.choice = Choice::LibOvr;
        pick.endpoint = e;
        pick.reason = join("LibOVR names the headset microphone");
        return pick;
      }
    }
    why = join("LibOVR's device is not an active capture endpoint");
  }
  for (const Endpoint& e : active) {
    if (NameLooksLikeHeadset(e.name)) {
      pick.choice = Choice::NameMatch;
      pick.endpoint = e;
      pick.reason = join("the endpoint name names a headset");
      return pick;
    }
  }
  pick.reason = join("no active capture endpoint names a headset");
  return pick;
}

/// The rule without a saved preference (the order before #412).
inline Pick Choose(bool isWine, const std::wstring& libovrId, long libovrCode, const std::vector<Endpoint>& active) {
  return ChooseWithPreference(isWine, std::wstring(), libovrId, libovrCode, active);
}

}  // namespace nevr_mic_device
