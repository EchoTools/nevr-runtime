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
// A dedicated server captures nothing (#879), and neither does a run with -noaudio: it opens no capture device, and
// every Mic* call answers "no device". Initialize() installs the lookup hook before the CLI sets g_isServer and before
// the game reads -noaudio, so both checks are made at call time (mic_provider.cpp), not here.

#include <cstdint>

namespace nevr_mic_policy {

inline bool ShouldInstallProvider(bool /*isWine*/) { return true; }

/// The audio bit of the game's engine flags (game+0x1D4, runtime/hook/addresses.h GAME_ENGINE_FLAGS_OFFSET). The game's own
/// argument handler clears it for `-noaudio` (`and dword ptr [rbx+0x1D4], 0xFFFFFFFD` at echovr.exe 0x140503C15,
/// ENGINE_FLAGS_NOAUDIO_MASK), and the runtime's headless patch clears it too (PatchEnableHeadless, mode_patches.cpp).
inline constexpr std::uint32_t kEngineAudioBit = 0x2u;

/// Whether the game runs with its audio on. A game instance that cannot be read counts as audio on (capture is allowed).
inline bool GameAudioOn(bool flagsReadable, std::uint32_t engineFlags) {
  return !flagsReadable || (engineFlags & kEngineAudioBit) != 0;
}

/// Whether a Mic* call may reach the capture device. False on a dedicated server (#879), and false when the game runs with
/// `-noaudio`: the player asked for no audio, so no capture device is opened and every Mic* call answers "no device".
inline bool ProvidesCapture(bool isServer, bool gameAudioOn) { return !isServer && gameAudioOn; }

inline const char* BootLine(bool installed, bool isWine) {
  if (!installed) return "[NEVR.MIC] info; provider not installed\n";
  return isWine ? "[NEVR.MIC] info; provider installed: Wine\n" : "[NEVR.MIC] info; provider installed: native Windows\n";
}

}  // namespace nevr_mic_policy
