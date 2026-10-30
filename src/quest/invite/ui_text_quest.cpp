#include "quest/invite/ui_text_quest.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "quest/invite/frame_dispatch.h"
#include "quest/invite/join_log_fields.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"
#include "quest/sentinel/pinned_targets.h"

// #1726: see ui_text_quest.h.

namespace nevr_quest::ui_text {

namespace {

constexpr std::uint64_t kSymCanvasUi = 0x35EC9A0128EC916FULL;  // CSymbol64("canvasui"), the same symbol the PC passes

struct CanvasHandle {
  void* canvas = nullptr;
  std::uint32_t handle = 0;
};
using GameSpaceFn = void* (*)(void* game, std::uint64_t index);
using FindFn = void (*)(CanvasHandle* out, void* gamespace, std::uint64_t ident, std::uint64_t componentSymbol);
using SetTextFn = sentinel::pinned::CanvasSetTextSig;

sentinel::LogLevel ToSentinel(nevr_ui_text::LogLevel level) {
  return level == nevr_ui_text::LogLevel::Warning ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo;
}

// One `ui_text` record: the tag and msg, then each key=value token of the writer's line as its own field, as the join's lines are
// (#1542). The writer's text stays the one source; this only splits it.
void LogTextLine(nevr_ui_text::LogLevel level, const std::string& line) {
  const nevr_quest_invite_join::JoinLine parsed = nevr_quest_invite_join::SplitJoinLine(line);
  std::vector<sentinel::LogField> fields;
  fields.emplace_back("tag", parsed.tag.c_str());
  fields.emplace_back("msg", parsed.msg.c_str());
  for (const nevr_quest_invite_join::JoinField& f : parsed.fields) {
    if (f.number) {
      fields.emplace_back(f.key.c_str(), f.value);
    } else {
      fields.emplace_back(f.key.c_str(), f.text.c_str());
    }
  }
  sentinel::LogFieldList(ToSentinel(level), "ui_text", fields.data(), fields.size());
}

std::uintptr_t LibR15Base() {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return 0;
  return static_cast<std::uintptr_t>(image.base);
}

// The first `count` words at `at` against the pinned ones.
bool WordsMatch(std::uintptr_t at, const std::uint32_t* words, std::size_t count) {
  if (!sentinel::MappingsAllow(at, count * sizeof(std::uint32_t), /*write=*/false)) return false;
  return std::memcmp(reinterpret_cast<const void*>(at), words, count * sizeof(std::uint32_t)) == 0;
}

class LiveGameCalls final : public GameCalls {
 public:
  bool Pin(const char* (&state)[3]) override {
    const std::uintptr_t base = LibR15Base();
    base_ = base;
    const char* none = base == 0 ? "not_found" : nullptr;
    namespace pinned = sentinel::pinned;
    state[0] = none != nullptr ? none : (WordsMatch(base + pinned::kGameSpaceVaddr, pinned::kGameSpaceCode,
                                                    sizeof(pinned::kGameSpaceCode) / 4) ? "ok" : "prologue_mismatch");
    state[1] = none != nullptr ? none : (WordsMatch(base + pinned::kCanvasFindVaddr, pinned::kCanvasFindCode,
                                                    sizeof(pinned::kCanvasFindCode) / 4) ? "ok" : "prologue_mismatch");
    state[2] = none != nullptr ? none : (WordsMatch(base + pinned::kCanvasSetTextVaddr, pinned::kCanvasSetTextCode,
                                                    sizeof(pinned::kCanvasSetTextCode) / 4) ? "ok" : "prologue_mismatch");
    return std::strcmp(state[0], "ok") == 0 && std::strcmp(state[1], "ok") == 0 && std::strcmp(state[2], "ok") == 0;
  }

  // The game object is the frame hook's netgame +8 (game_layout::kGameOffset), as the join reads it.
  std::uintptr_t Game() override {
    void* netGame = nevr_quest::invite::frame::NetGame();
    if (netGame == nullptr) return 0;
    void* game = nullptr;
    if (!Read(reinterpret_cast<std::uintptr_t>(netGame) + sentinel::pinned::game_layout::kGameOffset, &game, sizeof(game))) return 0;
    return reinterpret_cast<std::uintptr_t>(game);
  }

  std::uintptr_t GameSpace(std::uintptr_t game, std::uint64_t index) override {
    auto fn = reinterpret_cast<GameSpaceFn>(base_ + sentinel::pinned::kGameSpaceVaddr);
    return reinterpret_cast<std::uintptr_t>(fn(reinterpret_cast<void*>(game), index));
  }

  bool Read(std::uintptr_t address, void* out, std::size_t length) override {
    if (!sentinel::MappingsAllow(address, length, /*write=*/false)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), length);
    return true;
  }

  bool FindCanvas(std::uintptr_t gs, std::uint64_t ident, std::uintptr_t* canvas, std::uint32_t* handle) override {
    auto fn = reinterpret_cast<FindFn>(base_ + sentinel::pinned::kCanvasFindVaddr);
    CanvasHandle found;
    fn(&found, reinterpret_cast<void*>(gs), ident, kSymCanvasUi);
    *canvas = reinterpret_cast<std::uintptr_t>(found.canvas);
    *handle = found.handle;
    return found.canvas != nullptr;
  }

  bool SetText(std::uintptr_t canvas, std::uint32_t handle, std::uint64_t element, const char* text) override {
    auto fn = reinterpret_cast<SetTextFn*>(base_ + sentinel::pinned::kCanvasSetTextVaddr);
    return fn(reinterpret_cast<void*>(canvas), handle, element, text) != 0;
  }

  std::uint64_t NowMs() override {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  std::string FirstWriteLine() override {
    return std::string("[NEVR.UI] text first write on_game_thread=") + (nevr_quest::invite::frame::OnTickThread() ? "true" : "false");
  }

  void Log(nevr_ui_text::LogLevel level, const std::string& line) override { LogTextLine(level, line); }

 private:
  std::uintptr_t base_ = 0;
};

// The platform nevr_ui_text::Apply and RunFrame run on: the Quest's actor table over the seam.
struct QuestPlatform {
  GameCalls& calls;
  std::uintptr_t Game() { return calls.Game(); }
  std::uintptr_t GameSpace(std::uintptr_t game, std::uint64_t index) { return calls.GameSpace(game, index); }
  bool Read(std::uintptr_t address, void* out, std::size_t length) { return calls.Read(address, out, length); }
  bool FindActor(std::uintptr_t gs, std::uint64_t actor, std::uint64_t* index) {
    return nevr_ui_text::FindActorInSortedMap(*this, gs, actor, index);
  }
  std::string ActorTableProbe(std::uintptr_t gs) {
    std::uint64_t table = 0, entries = 0, count = 0;
    const bool read = Read(gs + nevr_ui_text::kSpaceActorTableOffset, &table, sizeof(table)) && table != 0 &&
                      Read(static_cast<std::uintptr_t>(table) + nevr_ui_text::kQuestMapEntriesOffset, &entries, sizeof(entries)) &&
                      Read(static_cast<std::uintptr_t>(table) + nevr_ui_text::kQuestMapCountOffset, &count, sizeof(count));
    return " table=" + nevr_ui_text::HexShort(table) + " entries=" + nevr_ui_text::HexShort(entries) +
           " count=" + std::to_string(count) + " read=" + (read ? "1" : "0");
  }
  std::string FirstWriteLine() { return calls.FirstWriteLine(); }
  bool FindCanvas(std::uintptr_t gs, std::uint64_t ident, std::uintptr_t* cs, std::uint32_t* handle) {
    return calls.FindCanvas(gs, ident, cs, handle);
  }
  bool SetText(std::uintptr_t cs, std::uint32_t handle, std::uint64_t element, const char* text) {
    return calls.SetText(cs, handle, element, text);
  }
};

// Every object with a non-trivial destructor is a function-local static (the sentinel's static-init check).
nevr_ui_text::Pending& Queue() {
  static nevr_ui_text::Pending pending;
  return pending;
}
std::atomic<GameCalls*>& CallsSlot() {
  static std::atomic<GameCalls*> slot{nullptr};
  return slot;
}
GameCalls& Calls() {
  GameCalls* injected = CallsSlot().load(std::memory_order_acquire);
  return injected != nullptr ? *injected : LiveCalls();
}

std::atomic<bool> g_ready{false};
std::atomic<bool> g_checked{false};
std::atomic<bool> g_firstWrite{false};

void Log(nevr_ui_text::LogLevel level, const std::string& line) { Calls().Log(level, line); }

void LogShim(nevr_ui_text::LogLevel level, const std::string& line) { Log(level, line); }

}  // namespace

GameCalls& LiveCalls() {
  static LiveGameCalls calls;
  return calls;
}

bool Ready() {
  if (g_checked.load(std::memory_order_acquire)) return g_ready.load(std::memory_order_acquire);
  const char* state[3] = {"", "", ""};
  const bool ok = Calls().Pin(state);
  g_ready.store(ok, std::memory_order_release);
  g_checked.store(true, std::memory_order_release);
  Log(ok ? nevr_ui_text::LogLevel::Info : nevr_ui_text::LogLevel::Warning,
      std::string("[NEVR.UI] text writer ") + (ok ? "ready" : "off") + " game_space=" + state[0] + " find_canvas=" + state[1] +
          " set_text=" + state[2]);
  return ok;
}

nevr_ui_text::Check Post(nevr_ui_text::Request request) {
  const nevr_ui_text::Check check = Queue().Post(std::move(request), Calls().NowMs());
  if (check != nevr_ui_text::Check::Ok) {
    Log(nevr_ui_text::LogLevel::Warning, std::string("[NEVR.UI] text refused reason=") + nevr_ui_text::CheckName(check));
  }
  return check;
}

void OnFrame() {
  if (!g_ready.load(std::memory_order_acquire) || Queue().Size() == 0) return;
  QuestPlatform platform{Calls()};
  nevr_ui_text::RunFrame(Queue(), platform, Calls().NowMs(), g_firstWrite, &LogShim);
}

void ResetForTest(GameCalls* calls) {
  CallsSlot().store(calls, std::memory_order_release);
  g_ready.store(false, std::memory_order_release);
  g_checked.store(false, std::memory_order_release);
  g_firstWrite.store(false, std::memory_order_release);
  Queue().Clear();
}

}  // namespace nevr_quest::ui_text
