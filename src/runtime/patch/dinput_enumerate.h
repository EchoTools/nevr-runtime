#pragma once
// DirectInput game controllers are enumerated only in desktop mode. The game's enumeration thread enumerates every attached
// game controller once a second (which opens each HID device); a player who is not in desktop mode does not have DirectInput
// enumerated, and so has no DirectInput device for the game to poll. One hook, at the game's one enumeration site:
// NRadEngine::CEnumerateThread::Run (echovr.exe 0x1410566e0), reconstructed with that rule added. Rules: dinput_enumerate_rules.h.

#include <cstdint>

namespace nevr_dinput_enumerate {

// Replaces CEnumerateThread::Run after checking its bytes from its start through the EnumDevices call; a mismatch is logged with
// the expected and actual bytes and nothing is installed (the game's own loop runs). Call before the engine starts the thread.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_dinput_enumerate
