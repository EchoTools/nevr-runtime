/* The pinned in-VR word the frozen-frame exit reads (frozen_frame_exit.h, #2377 under #1737), the code that writes it and the code that clears
 * it. Nothing here is hooked or written: the module only reads the word.
 *
 * Every address is an ELF virtual address in the store libr15 (the store file's build ID is `kLibR15StockBuildId` in pinned_targets.h); the
 * disassembly's address (ReVault, project echovr, binary libr15.so) is the ELF address plus 0x100000. src/quest/tests/frozen_frame_exit_pinned_test.cpp
 * checks each word, symbol, PLT stub and JUMP_SLOT against the real file (`just test-quest-hooks-pinned`, which needs the store APK). The module
 * checks the words again at install against the libr15 that is running (the patched file keeps them: same bytes at every address below).
 *
 * The word. 0x37a33f0, 8 bytes, in .bss (.bss 0x3761180, size 0x713b8, from the section table), so the file holds zero and the game writes it at
 * run time: the `ovrMobile*` session `vrapi_EnterVrMode` returned, zero when the game is not in VR mode. `NRadEngine::DoVRSubmission` calls
 * `vrapi_SubmitFrame2` only when it is non-zero (docs/quest/combat/frozen_frame_exit.md, "The frame-submit gate").
 *
 * The writer, `NRadEngine::CVR::UpdateTracking` (0x1a71b60): `adrp x24, 0x37a3000` and `add x24, x24, #0x3f0` name the word at its top; the call
 * to `vrapi_EnterVrMode` through its PLT stub (0xef59d0, JUMP_SLOT 0x36d23d0) is followed by `str x0, [x24]`, which stores the session (zero when
 * EnterVrMode failed).
 *
 * The clearers. In UpdateTracking, `adrp x25, 0x37a3000` names the word's page; when `CSysJava::SessionActive()` (its PLT stub 0xf3ef30, JUMP_SLOT
 * 0x36f6e80) returns 0 (`cbz w0`), or returns non-zero and `CSysJava::NativeWindow()` (`bl 0xef3120` at 0x1a71c40, PLT stub 0xef3120, JUMP_SLOT
 * 0x36d0f78) returns a null window (`cbz x0` at 0x1a71c44, to the same place), the loop loads the word (`ldr x0, [x25, #0x3f0]`), and when it is non-zero
 * calls `vrapi_LeaveVrMode` (stub 0xedc2b0, JUMP_SLOT 0x36c5840) and stores zero (`str xzr, [x25, #0x3f0]`). So the word is zero whenever the headset's
 * session is not active or the game has no native window. Only game code on the main thread clears it: a pause that blocks that thread (inside
 * vrapi_SubmitFrame2 too) leaves it set (inferred; see the note on UpdateTracking and DoVRSubmission below). In
 * `NRadEngine::CVR::ShutdownGraphics` (0x1a712b0, byte-checked reconstruction cvr_shutdown_graphics.cpp in the proven tree): `adrp x19, 0x37a3000`,
 * the LeaveVrMode call, then `str xzr, [x19, #0x3f0]`.
 *
 * Not established here: UpdateTracking and DoVRSubmission are not in the proven reconstruction tree, so what the comments above say
 * they do beyond the pinned words is read from their disassembly, not from a whole reconstruction.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::frozen_frame_exit::targets {

// The in-VR word: 8 bytes in .bss.
inline constexpr std::uint64_t kInVrWordVaddr = 0x37a33f0;
inline constexpr std::size_t kInVrWordBytes = 8;
// .bss of the store libr15 (section table): the word lies inside it.
inline constexpr std::uint64_t kBssVaddr = 0x3761180;
inline constexpr std::uint64_t kBssBytes = 0x713b8;

// The two functions the words below lie in.
inline constexpr pinned::Symbol kUpdateTracking = {"_ZN10NRadEngine3CVR14UpdateTrackingEv", 0x1a71b60};
inline constexpr pinned::Symbol kShutdownGraphics = {"_ZN10NRadEngine3CVR16ShutdownGraphicsEv", 0x1a712b0};

// The writer, the clearers and the gate on SessionActive, as instruction words.
inline constexpr pinned::Word kWords[] = {
    {"UpdateTracking: adrp x24, 0x37a3000 (the in-VR word's page)", 0x1a71ba8, 0xd000e998},
    {"UpdateTracking: add x24, x24, #0x3f0 (the in-VR word)", 0x1a71bac, 0x910fc318},
    {"UpdateTracking: adrp x25, 0x37a3000 (the in-VR word's page, for the clear)", 0x1a71bb0, 0xd000e999},
    {"UpdateTracking: bl CSysJava::SessionActive", 0x1a71c38, 0x97d334be},
    {"UpdateTracking: cbz w0 (session not active: leave VR)", 0x1a71c3c, 0x34000360},
    {"UpdateTracking: bl CSysJava::NativeWindow", 0x1a71c40, 0x97d20538},
    {"UpdateTracking: cbz x0 (no native window: leave VR)", 0x1a71c44, 0xb4000320},
    {"UpdateTracking: bl vrapi_EnterVrMode", 0x1a71c84, 0x97d20f53},
    {"UpdateTracking: str x0, [x24] (the session EnterVrMode returned)", 0x1a71c88, 0xf9000300},
    {"UpdateTracking: ldr x0, [x25, #0x3f0] (the session to leave)", 0x1a71ca8, 0xf941fb20},
    {"UpdateTracking: bl vrapi_LeaveVrMode", 0x1a71cb0, 0x97d1a980},
    {"UpdateTracking: str xzr, [x25, #0x3f0] (out of VR)", 0x1a71cb4, 0xf901fb3f},
    {"ShutdownGraphics: adrp x19, 0x37a3000 (the in-VR word's page)", 0x1a712b4, 0xd000e993},
    {"ShutdownGraphics: bl vrapi_LeaveVrMode", 0x1a712cc, 0x97d1abf9},
    {"ShutdownGraphics: str xzr, [x19, #0x3f0] (out of VR)", 0x1a712d0, 0xf901fa7f},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};

// The words that name the in-VR word (each adrp with the add, ldr or str that completes the address).
struct Address {
  std::uint64_t page;  // the adrp's ELF address
  std::uint64_t low;   // the add, ldr or str whose immediate is the word's offset in the page (0: the add's register is used as is)
  bool viaAdd;         // `low` is `add` (its immediate is the offset) rather than a load or store with the offset
};
inline constexpr Address kAddresses[] = {
    {0x1a71ba8, 0x1a71bac, true},   // x24 = the word; the writer stores at [x24]
    {0x1a71bb0, 0x1a71ca8, false},  // [x25, #0x3f0] read for LeaveVrMode
    {0x1a71bb0, 0x1a71cb4, false},  // [x25, #0x3f0] cleared
    {0x1a712b4, 0x1a712d0, false},  // [x19, #0x3f0] cleared at ShutdownGraphics
};

// Each call to a VrApi or CSysJava function above: the call's address and word, the PLT stub it reaches, and the JUMP_SLOT that stub loads.
struct Call {
  const char* what;
  std::uint64_t site;
  std::uint32_t word;
  std::uint64_t stub;
  std::uint64_t slot;
  const char* symbol;
};
inline constexpr Call kCalls[] = {
    {"UpdateTracking: vrapi_EnterVrMode", 0x1a71c84, 0x97d20f53, 0xef59d0, 0x36d23d0, "vrapi_EnterVrMode"},
    {"UpdateTracking: vrapi_LeaveVrMode", 0x1a71cb0, 0x97d1a980, 0xedc2b0, 0x36c5840, "vrapi_LeaveVrMode"},
    {"ShutdownGraphics: vrapi_LeaveVrMode", 0x1a712cc, 0x97d1abf9, 0xedc2b0, 0x36c5840, "vrapi_LeaveVrMode"},
    {"UpdateTracking: CSysJava::SessionActive", 0x1a71c38, 0x97d334be, 0xf3ef30, 0x36f6e80, "_ZN10NRadEngine8CSysJava13SessionActiveEv"},
    {"UpdateTracking: CSysJava::NativeWindow", 0x1a71c40, 0x97d20538, 0xef3120, 0x36d0f78, "_ZN10NRadEngine8CSysJava12NativeWindowEv"},
};

}  // namespace nevr_quest::combat::frozen_frame_exit::targets
