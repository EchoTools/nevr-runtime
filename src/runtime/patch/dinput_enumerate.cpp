#include "runtime/patch/dinput_enumerate.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/config.h"  // g_pGame
#include "runtime/patch/dinput_enumerate_rules.h"

namespace nevr_dinput_enumerate {
namespace {

// NRadEngine::CEnumerateThread::Run (vslot 2 of the vtable at 0x141cdf7d8), the bytes from its start through
// `call [rax+0x20]` (IDirectInput8::EnumDevices) at 0x141056788. They pin every address the reconstruction below names: the
// spin flag and the snapshot (the two LEAs at 0x141056708), CEngine (0x141056727), the flags offset (0x141056737) and the
// callback (0x14105677c).
constexpr std::uint64_t kRunVA = 0x1410566E0;
constexpr std::array<std::uint8_t, 171> kRunBytes = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0x10, 0x01, 0x00,
    0x00, 0x48, 0x8b, 0xd9, 0x8b, 0x41, 0x18, 0x85, 0xc0, 0x0f, 0x84, 0x56, 0x01, 0x00, 0x00, 0x33, 0xff, 0x8d, 0x77, 0x01,
    0x48, 0x8d, 0x2d, 0xb1, 0x80, 0x08, 0x01, 0x4c, 0x8d, 0x35, 0xea, 0x80, 0x08, 0x01, 0x48, 0x83, 0x7b, 0x28, 0x00, 0x75,
    0x0a, 0xba, 0x88, 0x13, 0x00, 0x00, 0xe9, 0x1f, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x42, 0x9d, 0x04, 0x01, 0x48, 0x85,
    0xc0, 0x0f, 0x84, 0x0a, 0x01, 0x00, 0x00, 0x8b, 0x80, 0xf0, 0xcf, 0x02, 0x00, 0x48, 0xc1, 0xe8, 0x07, 0xf7, 0xd0, 0x83,
    0xe0, 0x01, 0x0f, 0x84, 0xf5, 0x00, 0x00, 0x00, 0x48, 0x89, 0x7c, 0x24, 0x40, 0xb9, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8d,
    0x44, 0x24, 0x48, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0x89, 0x38, 0x48, 0x8d, 0x40, 0x30, 0x48, 0x83, 0xe9, 0x01, 0x75, 0xf4,
    0x48, 0x8b, 0x4b, 0x28, 0x48, 0x8b, 0x01, 0x89, 0x74, 0x24, 0x20, 0x4c, 0x8d, 0x4c, 0x24, 0x40, 0x4c, 0x8d, 0x05, 0x9d,
    0xf0, 0xff, 0xff, 0xba, 0x04, 0x00, 0x00, 0x00, 0xff, 0x50, 0x20};

constexpr std::uint64_t kEngineSlotVA = 0x1420A0470;       // CEngine* (the global the pass reads)
constexpr std::size_t kEngineFlagsOffset = 0x2CFF0;        // CEngine flags; bit 7 is input off
constexpr std::uint64_t kSnapshotLockVA = 0x1420DE7C0;     // u32 spin flag: 0 free, 1 held
constexpr std::uint64_t kSnapshotVA = 0x1420DE800;         // the published device snapshot
constexpr std::uint64_t kEnumCallbackVA = 0x141055820;     // LPDIENUMDEVICESCALLBACK the game passes
constexpr std::size_t kRunningOffset = 0x18;               // CEnumerateThread: u32, non-zero while the thread runs
constexpr std::size_t kDirectInputOffset = 0x28;           // CEnumerateThread: IDirectInput8*
// The snapshot the pass publishes: 0xC8 bytes (twelve 16-byte moves and one 8-byte move from 0x1410567b9). The game clears its count and
// each entry's first word before the pass; the reconstruction clears all of it.
constexpr std::size_t kSnapshotBytes = 0xC8;
// A failed byte check logs this many bytes from the first one that differs.
constexpr std::size_t kShownBytes = 8;
constexpr std::size_t kHexCharsPerByte = 2;
// IDirectInput8 vtable slot 4 (offset 0x20), and the two arguments the game passes it.
constexpr std::size_t kEnumDevicesSlot = 4;
constexpr DWORD kDevClassGameCtrl = 4;    // DI8DEVCLASS_GAMECTRL (`mov edx,4` at 0x141056783)
constexpr DWORD kEnumAttachedOnly = 1;    // DIEDFL_ATTACHEDONLY (`mov [rsp+0x20],esi`, esi = 1)

using EnumDevicesFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD devType, void* callback, void* ref, DWORD flags);
using RunFn = void (*)(void* thread);

std::uintptr_t g_gameBase = 0;
RunFn g_originalRun = nullptr;
std::atomic<int> g_lastPass{-1};

template <typename T>
T ReadAt(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

void* Game(std::uint64_t va) { return nevr::ResolveVA_Unchecked(g_gameBase, va); }

PassState ReadState(void* thread) {
  PassState s;
  s.have_interface = ReadAt<void*>(thread, kDirectInputOffset) != nullptr;
  void* const engine = ReadAt<void*>(Game(kEngineSlotVA), 0);
  s.have_engine = engine != nullptr;
  if (s.have_engine) s.engine_flags = ReadAt<std::uint32_t>(engine, kEngineFlagsOffset);
  void* const game = g_pGame;
  s.have_game = game != nullptr;
  if (s.have_game) s.game_flags = ReadAt<std::uint64_t>(game, nevr_patch_addresses::GAME_WINDOWED_FLAGS_OFFSET);
  return s;
}

// The game's enumeration and publish, as Run does them: enumerate into a cleared snapshot, then copy it out under the spin flag.
void EnumerateAndPublish(void* thread) {
  std::array<std::uint8_t, kSnapshotBytes> snapshot{};
  void* const input = ReadAt<void*>(thread, kDirectInputOffset);
  void** const vtable = ReadAt<void**>(input, 0);
  reinterpret_cast<EnumDevicesFn>(vtable[kEnumDevicesSlot])(input, kDevClassGameCtrl, Game(kEnumCallbackVA), snapshot.data(),
                                                             kEnumAttachedOnly);
  auto* const lock = static_cast<volatile LONG*>(Game(kSnapshotLockVA));
  for (;;) {
    while (*lock != 0) {
    }
    if (InterlockedCompareExchange(lock, 1, 0) == 0) break;
  }
  std::memcpy(Game(kSnapshotVA), snapshot.data(), kSnapshotBytes);
  InterlockedExchange(lock, 0);
}

void NoteChange(Pass pass, const PassState& s) {
  const int now = static_cast<int>(pass);
  const int before = g_lastPass.exchange(now);
  if (before == now) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] dinput enumeration pass from=%s to=%s desktop=%s (game controllers are enumerated and polled only in "
      "desktop mode, and only while the game has input on)",
      before < 0 ? "start" : PassName(static_cast<Pass>(before)), PassName(pass), DesktopMode(s) ? "true" : "false");
}

// CEnumerateThread::Run with the desktop-mode rule: the game's loop, its waits and its stop flag, and its enumeration only
// when Decide says so.
void RunHook(void* thread) {
  while (ReadAt<std::uint32_t>(thread, kRunningOffset) != 0) {
    const PassState s = ReadState(thread);
    const Pass pass = Decide(s);
    NoteChange(pass, s);
    if (pass == Pass::kEnumerate) EnumerateAndPublish(thread);
    Sleep(WaitAfter(pass));  // fcn.1401d3df0, the game's wait, is Sleep(ms)
  }
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  void* const target = nevr::ResolveVA_Checked(gameBase, kRunVA);
  if (target == nullptr) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] dinput enumerate hook not installed name=CEnumerateThread::Run va=0x%llx reason=unreadable (the page "
        "is not committed; the game enumerates DirectInput controllers in every mode)",
        static_cast<unsigned long long>(kRunVA));
    return;
  }
  if (!nevr::ValidatePrologue(target, kRunBytes.data(), kRunBytes.size())) {
    const auto* actual = static_cast<const std::uint8_t*>(target);
    std::size_t at = 0;
    while (at < kRunBytes.size() && actual[at] == kRunBytes[at]) ++at;
    const std::size_t shown = std::min(kShownBytes, kRunBytes.size() - std::min(at, kRunBytes.size()));
    char expected[kShownBytes * kHexCharsPerByte + 1] = {};
    char found[kShownBytes * kHexCharsPerByte + 1] = {};
    for (std::size_t i = 0; i < shown; ++i) {
      std::snprintf(expected + i * kHexCharsPerByte, kHexCharsPerByte + 1, "%02x", kRunBytes[at + i]);
      std::snprintf(found + i * kHexCharsPerByte, kHexCharsPerByte + 1, "%02x", actual[at + i]);
    }
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] dinput enumerate hook not installed name=CEnumerateThread::Run va=0x%llx reason=bytes_mismatch "
        "first_difference=%zu expected=%s actual=%s (the game enumerates DirectInput controllers in every mode)",
        static_cast<unsigned long long>(kRunVA), at, expected, found);
    return;
  }
  g_originalRun = reinterpret_cast<RunFn>(target);
  if (PatchDetour(&g_originalRun, reinterpret_cast<PVOID>(&RunHook), "CEnumerateThread::Run")) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] dinput enumerate hook installed va=0x%llx (DirectInput game controllers are enumerated only in desktop "
        "mode)",
        static_cast<unsigned long long>(kRunVA));
  }
}

}  // namespace nevr_dinput_enumerate
