#pragma once

// When the game's DirectInput enumeration thread enumerates game controllers (dinput_enumerate.h is the PC hook). Pure, so the
// rule is tested on any host (src/quest/tests/dinput_enumerate_rules_test.cpp).
//
// The game's own pass, NRadEngine::CEnumerateThread::Run (echovr.exe 0x1410566e0, vslot 2), runs on the thread the game names
// "DInput Enumeration Thread" (0x141cdf818), forked once at engine init (fcn.141055d90, after DirectInput8Create). Each pass:
//   no IDirectInput8 (this+0x28 null)              -> sleep 5000 ms;
//   else, when CEngine (*0x1420a0470) is set and bit 7 of its flags (+0x2cff0) is clear (the game's input-off bit)
//        -> IDirectInput8::EnumDevices(DI8DEVCLASS_GAMECTRL, the callback 0x141055820, &snapshot, DIEDFL_ATTACHEDONLY),
//           then the 0xC8-byte snapshot is published to 0x1420de800 under the spin flag at 0x1420de7c0;
//        then sleep 1000 ms.
// The callback creates a device only for two pads (054c:0268, 0810:0001); the game thread polls only devices it created
// (fcn.141057080). Enumerating opens every attached HID device, once a second, for the life of the process.
//
// The added rule: a player who is not in desktop mode does not have DirectInput enumerated (and so has no DirectInput device to
// poll). Desktop mode is the game's own "No VR" flag, bit 20 of the flags at game+0x7ae0 (hmd_serial.h kNoVrFlag), which the
// runtime sets for -windowed before the game starts; it is read on every pass.

#include <cstdint>

namespace nevr_dinput_enumerate {

// The game's waits between passes (echovr.exe 0x14105671d `mov edx,0x1388`, 0x141056841 `mov edx,0x3e8`).
inline constexpr std::uint32_t kWaitNoInterfaceMs = 5000;
inline constexpr std::uint32_t kWaitBetweenPassesMs = 1000;
// CEngine flags bit 7: the game's input-off bit (`shr rax,7; not eax; and eax,1` at 0x14105673d).
inline constexpr std::uint32_t kEngineInputOffBit = 0x80u;
// game+0x7ae0 bit 20, the game's "No VR" flag (CR15NetGame::LogIn 0x14017ef10 reads it).
inline constexpr std::uint64_t kGameNoVrFlag = 0x100000ULL;

enum class Pass { kNoInterface, kInputOff, kNotDesktop, kEnumerate };

struct PassState {
  bool have_interface = false;   // this+0x28 is set
  bool have_engine = false;      // *0x1420a0470 is set
  std::uint32_t engine_flags = 0;  // CEngine+0x2cff0
  bool have_game = false;        // the runtime's game instance is known
  std::uint64_t game_flags = 0;  // game+0x7ae0
};

inline bool DesktopMode(const PassState& s) { return s.have_game && (s.game_flags & kGameNoVrFlag) != 0; }

inline Pass Decide(const PassState& s) {
  if (!s.have_interface) return Pass::kNoInterface;
  if (!s.have_engine || (s.engine_flags & kEngineInputOffBit) != 0) return Pass::kInputOff;
  if (!DesktopMode(s)) return Pass::kNotDesktop;
  return Pass::kEnumerate;
}

inline std::uint32_t WaitAfter(Pass p) { return p == Pass::kNoInterface ? kWaitNoInterfaceMs : kWaitBetweenPassesMs; }

inline const char* PassName(Pass p) {
  switch (p) {
    case Pass::kNoInterface: return "no_interface";
    case Pass::kInputOff: return "input_off";
    case Pass::kNotDesktop: return "not_desktop";
    case Pass::kEnumerate: return "enumerate";
  }
  return "unknown";
}

}  // namespace nevr_dinput_enumerate
