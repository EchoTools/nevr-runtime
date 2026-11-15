// The PC DirectInput enumeration rule (runtime/patch/dinput_enumerate_rules.h) on a POSIX host: the game's pass, reconstructed
// (no interface: wait 5000 ms; the game's input off: no enumeration, wait 1000 ms), and the added rule that a player who is not
// in desktop mode (the game's No VR flag clear) is not enumerated. The Win32 hook (patch/dinput_enumerate.cpp) runs Decide and
// WaitAfter on every pass.

#include <cstdint>
#include <cstdio>
#include <string>

#include "quest/tests/test_check.h"
#include "runtime/compat/hmd_serial.h"
#include "runtime/patch/dinput_enumerate_rules.h"

namespace {

using namespace nevr_dinput_enumerate;

// The game's own values, as its code holds them (dinput_enumerate_rules.h names each site).
constexpr std::uint32_t kGameWaitNoInterfaceMs = 5000;  // mov edx,0x1388 at 0x14105671d
constexpr std::uint32_t kGameWaitMs = 1000;             // mov edx,0x3e8 at 0x141056841
constexpr std::uint32_t kGameInputOffBit = 0x80;        // shr rax,7 at 0x14105673d

void CaseTheValuesAreTheGames() {
  QCHECK(kGameNoVrFlag == nevr_hmd_serial::kNoVrFlag);  // the No VR flag the login reads (CR15NetGame::LogIn)
  QCHECK(kWaitNoInterfaceMs == kGameWaitNoInterfaceMs);
  QCHECK(kWaitBetweenPassesMs == kGameWaitMs);
  QCHECK(kEngineInputOffBit == kGameInputOffBit);
}

PassState Desktop() {
  PassState s;
  s.have_interface = true;
  s.have_engine = true;
  s.have_game = true;
  s.game_flags = kGameNoVrFlag;
  return s;
}

void CaseDesktopIsEnumerated() {
  QCHECK(Decide(Desktop()) == Pass::kEnumerate);
  QCHECK(WaitAfter(Pass::kEnumerate) == kWaitBetweenPassesMs);
}

void CaseNotDesktopIsNotEnumerated() {
  PassState s = Desktop();
  s.game_flags = 0;
  QCHECK(Decide(s) == Pass::kNotDesktop);
  QCHECK(WaitAfter(Pass::kNotDesktop) == kWaitBetweenPassesMs);
  s = Desktop();
  s.have_game = false;  // no game instance: not known to be desktop
  QCHECK(Decide(s) == Pass::kNotDesktop);
  QCHECK(!DesktopMode(s));
}

void CaseTheGameOwnGates() {
  PassState s = Desktop();
  s.have_interface = false;
  QCHECK(Decide(s) == Pass::kNoInterface);
  QCHECK(WaitAfter(Pass::kNoInterface) == kWaitNoInterfaceMs);
  s = Desktop();
  s.have_engine = false;
  QCHECK(Decide(s) == Pass::kInputOff);
  s = Desktop();
  s.engine_flags = kEngineInputOffBit;
  QCHECK(Decide(s) == Pass::kInputOff);
  QCHECK(WaitAfter(Pass::kInputOff) == kWaitBetweenPassesMs);
  s.engine_flags = ~kEngineInputOffBit;
  QCHECK(Decide(s) == Pass::kEnumerate);
}

void CaseNames() {
  QCHECK(std::string(PassName(Pass::kNoInterface)) == "no_interface");
  QCHECK(std::string(PassName(Pass::kInputOff)) == "input_off");
  QCHECK(std::string(PassName(Pass::kNotDesktop)) == "not_desktop");
  QCHECK(std::string(PassName(Pass::kEnumerate)) == "enumerate");
}

}  // namespace

int main() {
  CaseTheValuesAreTheGames();
  CaseDesktopIsEnumerated();
  CaseNotDesktopIsNotEnumerated();
  CaseTheGameOwnGates();
  CaseNames();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "dinput_enumerate_rules_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("dinput_enumerate_rules_test: all checks passed\n");
  return 0;
}
