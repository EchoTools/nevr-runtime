#include "quest/invite/join_game_quest.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <pthread.h>

#include "abi/echovr.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"
#include "runtime/patch/invite_join.h"
#include "runtime/patch/invite_match_api.h"
#include "runtime/patch/invite_wizard.h"

// #966 S4: see join_game_quest.h. The Quest's game object is CR15Game; its netgame is the frame hook's `self`.

namespace nevr_quest_invite_join {

namespace {

using nevr_invite_join::kQuestLayout;
using nevr_invite_join::LobbyBytes;
using nevr_invite_join::PriorStage;
using nevr_invite_join::StageOutcome;
using nevr_invite_join::StageStep;

constexpr const char* kTag = "[NEVR.INVITE]";

sentinel::LogLevel ToSentinel(nevr_invite::LogLevel level) {
  if (level == nevr_invite::LogLevel::Warning) return sentinel::LogLevel::kWarn;
  if (level == nevr_invite::LogLevel::Error) return sentinel::LogLevel::kError;
  return sentinel::LogLevel::kInfo;
}

// The game object's bytes. The game object outlives the frame that calls the join, and the join runs on the game thread, so the
// staging reads and writes it directly; the offsets are the Layout's (kQuestLayout), which the host test checks.
class GameMemoryRef final : public nevr_invite_join::StagingMemory {
 public:
  explicit GameMemoryRef(void* game) : game_(static_cast<unsigned char*>(game)) {}

  bool Read(std::uintptr_t offset, void* out, std::size_t size) override {
    if (game_ == nullptr || out == nullptr) return false;
    std::memcpy(out, game_ + offset, size);
    return true;
  }

  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override {
    if (game_ == nullptr || in == nullptr) return false;
    std::memcpy(game_ + offset, in, size);
    return true;
  }

 private:
  unsigned char* game_;
};

// The frame hook's netgame: its state at netgame+0 (game_layout::kStateOffset), its game object at netgame+8 (kGameOffset).
std::optional<EchoVR::NetGameState> NetStateOf(void* netGame) {
  if (netGame == nullptr) return std::nullopt;
  std::int32_t state = 0;
  std::memcpy(&state, static_cast<const unsigned char*>(netGame) + sentinel::pinned::game_layout::kStateOffset, sizeof(state));
  return static_cast<EchoVR::NetGameState>(state);
}

void* GameOf(void* netGame) {
  if (netGame == nullptr) return nullptr;
  void* game = nullptr;
  std::memcpy(&game, static_cast<const unsigned char*>(netGame) + sentinel::pinned::game_layout::kGameOffset, sizeof(game));
  return game;
}

// libr15's load address, or 0 when the image is not found.
std::uintptr_t LibR15Base() {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return 0;
  return static_cast<std::uintptr_t>(image.base);
}

class ProductionAccess final : public GameAccess {
 public:
  bool OnTickThread() override { return nevr_quest::invite::frame::OnTickThread(); }
  void* NetGame() override { return nevr_quest::invite::frame::NetGame(); }

  bool ProloguesValid() override {
    const std::uintptr_t base = LibR15Base();
    if (base == 0) {
      Log(nevr_invite::LogLevel::Warning, std::string(kTag) + " join prologue check failed library=libr15 reason=not_found");
      return false;
    }
    return Pinned("EnableTransitionMenu", base + sentinel::pinned::kEnableTransitionMenuVaddr,
                  sentinel::pinned::kEnableTransitionMenuCode) &&
           Pinned("BeginMultiplayer", base + sentinel::pinned::kBeginMultiplayerVaddr, sentinel::pinned::kBeginMultiplayerCode);
  }

  void EnableMenu(void* game) override {
    const std::uintptr_t base = LibR15Base();
    if (base == 0 || game == nullptr) return;
    reinterpret_cast<void (*)(void*)>(base + sentinel::pinned::kEnableTransitionMenuVaddr)(game);
  }

  void Begin(void* game) override {
    const std::uintptr_t base = LibR15Base();
    if (base == 0 || game == nullptr) return;
    reinterpret_cast<void (*)(void*)>(base + sentinel::pinned::kBeginMultiplayerVaddr)(game);
  }

  void Log(nevr_invite::LogLevel level, const std::string& line) override {
    sentinel::LogFields(ToSentinel(level), "join_line", {{"line", line.c_str()}});
  }

  std::int64_t NowMs() override {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

 private:
  // The 16 pinned bytes at `at` (a libr15 address), checked with the pinned words.
  bool Pinned(const char* name, std::uintptr_t at, const std::uint32_t (&code)[4]) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(at);
    std::uint32_t words[4] = {0, 0, 0, 0};
    std::memcpy(words, bytes, sizeof(words));
    bool same = true;
    for (std::size_t i = 0; i < 4; ++i) same = same && words[i] == code[i];
    if (!same) {
      char va[19];
      sentinel::LogFields(sentinel::LogLevel::kWarn, "join_prologue_mismatch",
                          {{"function", name}, {"va", sentinel::HexString(va, at)}});
    }
    return same;
  }
};

}  // namespace

// The GameMemory the staging uses: the Layout offsets within the game object.
class Offerer::GameMemory final : public nevr_invite_join::StagingMemory {
 public:
  explicit GameMemory(void* game) : ref_(game) {}
  bool Read(std::uintptr_t offset, void* out, std::size_t size) override { return ref_.Read(offset, out, size); }
  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override { return ref_.Write(offset, in, size); }

 private:
  GameMemoryRef ref_;
};

Offerer::Offerer(GameAccess& access) : access_(access), joiner_(MakeSeam(this)) {}

nevr_invite_join::Reason Offerer::Offer(std::string_view match_id, bool feature_on) { return joiner_.Offer(match_id, feature_on); }

nevr_invite_join::Seam Offerer::MakeSeam(Offerer* self) {
  nevr_invite_join::Seam seam;
  seam.is_server = [] { return false; };  // the Quest is a client; a dedicated server never runs this
  seam.on_game_thread = [self] { return self->access_.OnTickThread(); };
  seam.prologues_valid = [self] { return self->access_.ProloguesValid(); };
  seam.net_state = [self]() -> std::optional<EchoVR::NetGameState> { return NetStateOf(self->access_.NetGame()); };
  seam.stage_lobby = [self](const LobbyBytes& lobby, std::uint16_t team) { return self->StageLobby(lobby, team); };
  seam.begin = [self] { return self->Begin(); };
  seam.unstage = [self] { return self->Unstage(); };
  seam.now_ms = [self] { return self->access_.NowMs(); };
  seam.log = [self](nevr_invite::LogLevel level, const std::string& line) { self->Say(level, line); };
  return seam;
}

void* Offerer::GameObject() { return GameOf(access_.NetGame()); }

void Offerer::Say(nevr_invite::LogLevel level, const std::string& line) { access_.Log(level, line); }

StageOutcome Offerer::StageLobby(const LobbyBytes& lobby, std::uint16_t team) {
  void* game = GameObject();
  if (game == nullptr) {
    StageOutcome outcome;
    outcome.step = StageStep::Read;
    outcome.field = nevr_invite_join::StageField::Lobby;
    return outcome;
  }
  GameMemory memory(game);
  const auto log = [this](nevr_invite::LogLevel level, const std::string& line) { Say(level, line); };
  StageOutcome outcome = nevr_invite_join::StageAndVerify(memory, lobby, team, log, kQuestLayout);
  if (outcome.ok()) {
    stagedGame_ = game;
    stagedPrior_ = outcome.prior;
  }
  return outcome;
}

// False when there is no game object to begin on; nothing is called then. The menu event runs first, then the begin. That is
// this file's order and is not measured on Quest (the headset run is owed); the PC join calls only the begin
// (src/runtime/patch/invite_join_game.cpp, Begin).
bool Offerer::Begin() {
  void* game = GameObject();
  if (game == nullptr) {
    Say(nevr_invite::LogLevel::Warning, std::string(kTag) + " join begin not called, no game object after the stage");
    return false;
  }
  access_.EnableMenu(game);
  access_.Begin(game);
  return true;
}

bool Offerer::Unstage() {
  if (stagedGame_ == nullptr) return false;
  GameMemory memory(stagedGame_);
  const auto log = [this](nevr_invite::LogLevel level, const std::string& line) { Say(level, line); };
  const bool restored = nevr_invite_join::Unstage(memory, stagedPrior_, log, kQuestLayout);
  stagedGame_ = nullptr;
  return restored;
}

GameAccess& LiveAccess() {
  static ProductionAccess access;
  return access;
}

namespace {

// The access the frame work's offerer calls through: the live access, unless a test points it at a fake (SetFrameAccessForTest).
class FrameAccess final : public GameAccess {
 public:
  GameAccess* target = nullptr;

  bool OnTickThread() override { return Live().OnTickThread(); }
  void* NetGame() override { return Live().NetGame(); }
  bool ProloguesValid() override { return Live().ProloguesValid(); }
  void EnableMenu(void* game) override { Live().EnableMenu(game); }
  void Begin(void* game) override { Live().Begin(game); }
  void Log(nevr_invite::LogLevel level, const std::string& line) override { Live().Log(level, line); }
  std::int64_t NowMs() override { return Live().NowMs(); }

 private:
  GameAccess& Live() { return target != nullptr ? *target : LiveAccess(); }
};

FrameAccess& FrameAccessInstance() {
  static FrameAccess access;
  return access;
}

Offerer& TheOfferer() {
  static Offerer offerer(FrameAccessInstance());
  return offerer;
}

// The frame work: each resolved match the service has answered since the last frame is offered to the join. The service does the
// one call that may throw (its own TU); this file keeps no exception path (the frame path, see the header).
bool ApplyAnswers(void* /*context*/) noexcept {
  // #1415: nothing resolved waits, so the frame costs one atomic load and takes no lock (the PC applier takes the same early-out).
  if (!nevr_invite_match::ResolvedMatchPending()) return true;
  char match[nevr_invite_match::kMaxMatchIdBytes];
  std::size_t length = 0;
  while (nevr_invite_match::TakeResolvedMatch(match, sizeof(match), &length)) {
    TheOfferer().Offer(std::string_view(match, length), nevr_invite_match::ServiceActive());
  }
  return true;
}

}  // namespace

bool RegisterAnswerApplier() noexcept { return nevr_quest::invite::frame::RegisterFrameWork(ApplyAnswers, nullptr); }

void SetFrameAccessForTest(GameAccess* access) { FrameAccessInstance().target = access; }

}  // namespace nevr_quest_invite_join
