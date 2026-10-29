#include "runtime/patch/ui_text_writer.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/patch/canvas_set_text.h"

namespace nevr_ui_text {
namespace {

// All three are echovr.exe (ReVault, reconstructed in the PR that introduced this file).
//
// The game object: CR15Game is a singleton whose pointer is stored once, by CR15Game::CreateNetServiceProviders
// (0x140109843), at 0x1420a0478.
constexpr std::uint64_t kImageBase = 0x140000000;
constexpr std::uint64_t kGameObjectVA = 0x1420A0478;

// CR15Game::GetGameSpace(game, index) = `index < game[+0x1f80] ? game[+0x1f50][index * 16 + 8] : 0`:
// CMP RDX,[RCX+0x1f80]; JAE; MOV RAX,[RCX+0x1f50]; ADD RDX,RDX; MOV RAX,[RAX+RDX*8+8]; RET; XOR EAX,EAX; RET.
// The whole function is pinned: it has no relative operand.
constexpr std::uint64_t kGetGameSpaceVA = 0x1404F35E0;
constexpr std::array<std::uint8_t, 28> kGetGameSpacePrologue = {0x48, 0x3B, 0x91, 0x80, 0x1F, 0x00, 0x00, 0x73, 0x10, 0x48,
                                                                0x8B, 0x81, 0x50, 0x1F, 0x00, 0x00, 0x48, 0x03, 0xD2, 0x48,
                                                                0x8B, 0x44, 0xD0, 0x08, 0xC3, 0x33, 0xC0, 0xC3};

// Find(out, gamespace, ident, componentSymbol): looks the canvas-component container up in the gamespace, asks each
// component for a handle and keeps the first the validity slot accepts, then recurses to the parent gamespace; it sets
// *out = 0 when there is none. out = {CCanvasUICS*, u32 handle}. ident = kActorIdentBase | actor index. The pin is the
// prologue up to the first relative operand (the LEA at +0x29 is not pinned).
// MOV [RSP+8],RBX; MOV [RSP+0x18],R8; MOV [RSP+0x10],RDX; PUSH RSI; PUSH RDI; PUSH R14; SUB RSP,0x30; MOV RBX,R9;
// MOV RAX,RDX; MOV R14,RCX; TEST RDX,RDX; JZ +0x9c.
constexpr std::uint64_t kFindVA = 0x14091DAF0;
constexpr std::array<std::uint8_t, 41> kFindPrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83,
    0xEC, 0x30, 0x49, 0x8B, 0xD9, 0x48, 0x8B, 0xC2, 0x4C, 0x8B, 0xF1, 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x9C, 0x00, 0x00, 0x00};
constexpr std::uint64_t kSymCanvasUi = 0x35EC9A0128EC916FULL;  // SYM_CANVASUI

struct CanvasHandle {
  void* canvas = nullptr;
  std::uint32_t handle = 0;
};
using GetGameSpaceFn = void* (*)(void* game, std::uint64_t index);
using FindFn = void* (*)(CanvasHandle* out, void* gamespace, std::uint64_t ident, std::uint64_t componentSymbol);

GetGameSpaceFn g_getGameSpace = nullptr;
FindFn g_find = nullptr;
nevr_canvas_set_text::SetTextFn g_setText = nullptr;
std::uintptr_t g_base = 0;
std::atomic<bool> g_ready{false};
Pending g_pending;

std::uint64_t NowMs() { return GetTickCount64(); }

struct RealPlatform {
  std::uintptr_t Game() {
    const void* slot = reinterpret_cast<const void*>(g_base + (kGameObjectVA - kImageBase));
    void* game = nullptr;
    if (!nevr_crash_recovery::IsReadableMemory(slot, sizeof(game))) return 0;
    std::memcpy(&game, slot, sizeof(game));
    return reinterpret_cast<std::uintptr_t>(game);
  }
  std::uintptr_t GameSpace(std::uintptr_t game, std::uint64_t index) {
    return reinterpret_cast<std::uintptr_t>(g_getGameSpace(reinterpret_cast<void*>(game), index));
  }
  bool Read(std::uintptr_t address, void* out, std::size_t length) {
    if (!nevr_crash_recovery::IsReadableMemory(reinterpret_cast<const void*>(address), length)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), length);
    return true;
  }
  bool FindCanvas(std::uintptr_t gs, std::uint64_t ident, std::uintptr_t* cs, std::uint32_t* handle) {
    CanvasHandle found;
    g_find(&found, reinterpret_cast<void*>(gs), ident, kSymCanvasUi);
    *cs = reinterpret_cast<std::uintptr_t>(found.canvas);
    *handle = found.handle;
    return found.canvas != nullptr;
  }
  bool SetText(std::uintptr_t cs, std::uint32_t handle, std::uint64_t element, const char* text) {
    return g_setText(reinterpret_cast<void*>(cs), handle, element, text) != 0;
  }
};

/// Looks `va` up and returns it when its bytes match `expected`; `ourDetour` accepts our own jump there (a function
/// another patch of ours detours has our jump for a prologue, and calling it runs that patch and then the original).
void* Pinned(std::uint64_t va, const std::uint8_t* expected, std::size_t length, bool ourDetour, const char** state) {
  void* target = nevr::ResolveVA_Checked(g_base, va);
  if (target == nullptr) {
    *state = "unmapped";
    return nullptr;
  }
  if (nevr::ValidatePrologue(target, expected, length)) {
    *state = "ok";
    return target;
  }
  if (ourDetour && nevr_hook_guard::IsOurDetour(target)) {
    *state = "ok_detoured";
    return target;
  }
  *state = "prologue_mismatch";
  return nullptr;
}

/// What the gamespaces look like when a request finds no actor: one line per gamespace, so a miss explains itself.
void ProbeSpaces(RealPlatform& p) {
  const std::uintptr_t game = p.Game();
  if (game == 0) return;
  for (std::size_t i = 0; i < kMaxGameSpaces; ++i) {
    const std::uintptr_t gs = p.GameSpace(game, i);
    if (gs == 0) {
      Log(EchoVR::LogLevel::Info, "[NEVR.UI] text probe spaces=%zu", i);
      return;
    }
    std::uint64_t table = 0, names = 0, count = 0;
    const bool read = p.Read(gs + kSpaceActorTableOffset, &table, sizeof(table)) && table != 0 &&
                      p.Read(static_cast<std::uintptr_t>(table) + kTableNamesOffset, &names, sizeof(names)) &&
                      p.Read(static_cast<std::uintptr_t>(table) + kTableCountOffset, &count, sizeof(count));
    Log(EchoVR::LogLevel::Info, "[NEVR.UI] text probe space=%zu gs=0x%llx table=0x%llx names=0x%llx count=%llu read=%d", i,
        static_cast<unsigned long long>(gs), static_cast<unsigned long long>(table), static_cast<unsigned long long>(names),
        static_cast<unsigned long long>(count), read ? 1 : 0);
  }
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_base = gameBase;
  const char* getState = "";
  const char* findState = "";
  const char* setState = "";
  void* getGameSpace = Pinned(kGetGameSpaceVA, kGetGameSpacePrologue.data(), kGetGameSpacePrologue.size(), false, &getState);
  void* find = Pinned(kFindVA, kFindPrologue.data(), kFindPrologue.size(), false, &findState);
  void* setText = Pinned(nevr_canvas_set_text::kSetTextVA, nevr_canvas_set_text::kSetTextPrologue.data(),
                         nevr_canvas_set_text::kSetTextPrologue.size(), true, &setState);
  if (getGameSpace == nullptr || find == nullptr || setText == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.UI] text writer off game_space=%s find_canvas=%s set_text=%s", getState, findState, setState);
    return;
  }
  g_getGameSpace = reinterpret_cast<GetGameSpaceFn>(getGameSpace);
  g_find = reinterpret_cast<FindFn>(find);
  g_setText = reinterpret_cast<nevr_canvas_set_text::SetTextFn>(setText);
  g_ready.store(true, std::memory_order_release);
  Log(EchoVR::LogLevel::Info, "[NEVR.UI] text writer ready game_space=%s find_canvas=%s set_text=%s", getState, findState, setState);
}

bool Ready() { return g_ready.load(std::memory_order_acquire); }

Check Post(Request request) {
  const Check check = g_pending.Post(std::move(request), NowMs());
  if (check != Check::Ok) Log(EchoVR::LogLevel::Warning, "[NEVR.UI] text refused reason=%s", CheckName(check));
  return check;
}

void OnFrame() {
  if (!Ready() || g_pending.Size() == 0) return;
  RealPlatform platform;
  static std::atomic<bool> s_firstWrite{false};
  for (const Report& r : g_pending.Drain(platform, NowMs())) {
    if (!r.done) {
      // The first miss and then one a 5 s: a request the page never answers must explain itself before its give-up.
      if (r.attempts == 1 || r.attempts % 50 == 0) {
        Log(EchoVR::LogLevel::Info, "[NEVR.UI] text waiting actor=%s element=%s bytes=%zu attempts=%d outcome=%s",
            Hex64(r.request.target.actor).c_str(), Hex64(r.request.target.element).c_str(), r.request.text.size(), r.attempts,
            OutcomeName(r.outcome));
        if (r.attempts == 1 && r.outcome == Outcome::NoActor) ProbeSpaces(platform);
      }
      continue;
    }
    Log(r.dropped ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", ReportLine(r).c_str());
    if (!r.dropped && !s_firstWrite.exchange(true)) {
      Log(EchoVR::LogLevel::Info, "[NEVR.UI] text first write thread=%lu game_loop_thread=%lu",
          static_cast<unsigned long>(GetCurrentThreadId()), GameLoopThreadId());
    }
  }
}

}  // namespace nevr_ui_text
