#pragma once

// Whether the runtime answers the game's Mic* symbol lookups with its WASAPI provider (mic_provider.cpp),
// and the boot-log line that says which. Pure, so both are unit-tested without the game or a capture device.
//
// The provider exists for Wine/Proton, where pnsrad.dll's mic exports are stubs and nothing else captures
// audio. On native Windows the game's own mic path works, and the provider picked the wrong capture device
// (#402), so the game resolves its own symbols there. The platform is nevr_system_info's Wine probe
// (ntdll!wine_get_version, src/core/system_info.cpp).

namespace nevr_mic_policy {

inline bool ShouldInstallProvider(bool isWine) { return isWine; }

inline const char* BootLine(bool installed) {
  return installed ? "[NEVR.MIC] provider installed: Wine\n"
                   : "[NEVR.MIC] provider not installed: native Windows\n";
}

}  // namespace nevr_mic_policy
