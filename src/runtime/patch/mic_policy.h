#pragma once

// Whether the runtime answers the game's Mic* symbol lookups with its WASAPI provider (mic_provider.cpp),
// and the boot-log line that says which. Pure, so both are unit-tested without the game or a capture device.
//
// The provider is installed on every platform: pnsrad.dll's mic exports are stubs (and, with the OVR platform
// branch bypassed and "pnsovr" patched to "pnsrad", the game's "OVR provider" mic path ends at those stubs on
// native Windows too), so nothing else captures audio. Which capture device it opens is
// mic_device_choice.h: the headset first on native Windows (#402 was the OS default picking Chromium's mic),
// the OS default under Wine. The platform is nevr_system_info's Wine probe (ntdll!wine_get_version,
// src/core/system_info.cpp).
//
// A dedicated server captures nothing (#879): it opens no capture device, and every Mic* call answers
// "no device". Initialize() installs the lookup hook before the CLI sets g_isServer, so the server check is
// made at call time (mic_provider.cpp), not here.

namespace nevr_mic_policy {

inline bool ShouldInstallProvider(bool /*isWine*/) { return true; }

/// Whether a Mic* call may reach the capture device. False on a dedicated server.
inline bool ProvidesCapture(bool isServer) { return !isServer; }

inline const char* BootLine(bool installed, bool isWine) {
  if (!installed) return "[NEVR.MIC] provider not installed\n";
  return isWine ? "[NEVR.MIC] provider installed: Wine\n" : "[NEVR.MIC] provider installed: native Windows\n";
}

}  // namespace nevr_mic_policy
