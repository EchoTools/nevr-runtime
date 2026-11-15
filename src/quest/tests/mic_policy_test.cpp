// The PC mic provider's capture policy (runtime/patch/mic_policy.h) on a POSIX host: a dedicated server captures nothing, and
// neither does a run whose game has its audio off (-noaudio clears the audio bit of the engine flags). The Win32 provider
// (patch/mic_provider.cpp) runs these same functions at each Mic* call; its WASAPI side is checked by the Win32 GTests in
// src/runtime/tests/test_mic_lifecycle.cpp.

#include <cstdint>
#include <cstdio>

#include "quest/tests/test_check.h"
#include "runtime/patch/mic_policy.h"

namespace {

using namespace nevr_mic_policy;

// The constructor default of the engine flags (CncaGame::CncaGame writes 0x137 at echovr.exe 0x1404E0CAD), and that value after
// the game's -noaudio handler (and 0xFFFFFFFD at echovr.exe 0x140503C15).
constexpr std::uint32_t kStockEngineFlags = 0x137u;
constexpr std::uint32_t kNoAudioMask = 0xFFFFFFFDu;

void CaseTheGameAudioBit() {
  QCHECK(GameAudioOn(true, kStockEngineFlags));
  QCHECK(!GameAudioOn(true, kStockEngineFlags & kNoAudioMask));
  QCHECK(GameAudioOn(false, 0));  // no game instance to read: capture is allowed
  QCHECK(GameAudioOn(true, kEngineAudioBit));
  QCHECK(!GameAudioOn(true, ~kEngineAudioBit));
}

void CaseWhoCaptures() {
  QCHECK(ProvidesCapture(/*isServer=*/false, /*gameAudioOn=*/true));
  QCHECK(!ProvidesCapture(/*isServer=*/false, /*gameAudioOn=*/false));
  QCHECK(!ProvidesCapture(/*isServer=*/true, /*gameAudioOn=*/true));
  QCHECK(!ProvidesCapture(/*isServer=*/true, /*gameAudioOn=*/false));
}

}  // namespace

int main() {
  CaseTheGameAudioBit();
  CaseWhoCaptures();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "mic_policy_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("mic_policy_test: all checks passed\n");
  return 0;
}
