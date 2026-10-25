#pragma once

// Which capture endpoint the WASAPI mic provider opens on native Windows, in this order (the headset the
// player is wearing first, so a Chromium tab or a webcam that Windows made the default is not picked):
//   1. LibOVR's own answer — the runtime's ovr_GetAudioDeviceInGuidStr, an IMMDevice endpoint id — when it is
//      an active capture endpoint;
//   2. the active capture endpoint whose friendly name names a headset (Oculus / Rift / Quest / Meta Link);
//   3. the OS default capture endpoint (what the provider always used).
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

enum class Choice { LibOvr, NameMatch, OsDefault };

struct Pick {
  Choice choice = Choice::OsDefault;
  Endpoint endpoint;        // empty id: the caller opens the OS default endpoint
  std::string reason;       // why this step won / why the earlier steps did not
};

/// Whether the headset-first choice applies. Wine/Proton keeps the OS default.
inline bool ShouldChooseHeadsetDevice(bool isWine) { return !isWine; }

inline const char* ChoiceName(Choice c) {
  switch (c) {
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
inline Pick Choose(bool isWine, const std::wstring& libovrId, long libovrCode, const std::vector<Endpoint>& active) {
  Pick pick;
  if (!ShouldChooseHeadsetDevice(isWine)) {
    pick.reason = "Wine/Proton keeps the OS default capture device";
    return pick;
  }
  std::string why;
  if (libovrId.empty()) {
    why = "LibOVR returned no device (code " + std::to_string(libovrCode) + ")";
  } else {
    for (const Endpoint& e : active) {
      if (e.id == libovrId) {
        pick.choice = Choice::LibOvr;
        pick.endpoint = e;
        pick.reason = "LibOVR names the headset microphone";
        return pick;
      }
    }
    why = "LibOVR's device is not an active capture endpoint";
  }
  for (const Endpoint& e : active) {
    if (NameLooksLikeHeadset(e.name)) {
      pick.choice = Choice::NameMatch;
      pick.endpoint = e;
      pick.reason = why + "; the endpoint name names a headset";
      return pick;
    }
  }
  pick.reason = why + "; no active capture endpoint names a headset";
  return pick;
}

}  // namespace nevr_mic_device
