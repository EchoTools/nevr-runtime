#include "quest/invite/join_game_quest.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <pthread.h>

#include "abi/echovr.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/invite/join_log_fields.h"
#include "quest/invite/ui_text_quest.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"
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

// One join_line record: the tag and msg, then each key=value token of the join's text as its own field (#1542), so reason,
// lobby and state_from are keys of the record. The strings the fields point at live in `parsed` for the call.
void LogJoinLine(sentinel::LogLevel level, const std::string& line) {
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
  sentinel::LogFieldList(level, "join_line", fields.data(), fields.size());
}

// The four 32-bit words of a pinned prologue as hex, one 8-digit group per word (the words the check compares).
const char* WordsHex(char (&buf)[33], const std::uint32_t (&words)[4]) {
  for (std::size_t i = 0; i < 4; ++i) std::snprintf(buf + 8 * i, 9, "%08x", static_cast<unsigned>(words[i]));
  return buf;
}

// The game object's bytes. The game object outlives the frame that calls the join, and the join runs on the game thread, so the
// staging reads and writes it directly; the offsets are the Layout's (kQuestLayout), which the host test checks. Each access
// is checked first, as the PC's GameObjectMemory checks it: a read of readable memory, a write of writable memory, nothing else.
class GameMemoryRef final : public nevr_invite_join::StagingMemory {
 public:
  explicit GameMemoryRef(void* game) : game_(static_cast<unsigned char*>(game)) {}

  bool Read(std::uintptr_t offset, void* out, std::size_t size) override {
    if (game_ == nullptr || out == nullptr) return false;
    const unsigned char* at = game_ + offset;
    if (!sentinel::MappingsAllow(reinterpret_cast<std::uintptr_t>(at), size, /*write=*/false)) return false;
    std::memcpy(out, at, size);
    return true;
  }

  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override {
    if (game_ == nullptr || in == nullptr) return false;
    unsigned char* at = game_ + offset;
    if (!sentinel::MappingsAllow(reinterpret_cast<std::uintptr_t>(at), size, /*write=*/true)) return false;
    std::memcpy(at, in, size);
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

  void Log(nevr_invite::LogLevel level, const std::string& line) override { LogJoinLine(ToSentinel(level), line); }

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
      char expected[33];
      char actual[33];
      sentinel::LogFields(sentinel::LogLevel::kWarn, "join_prologue_mismatch",
                          {{"function", name},
                           {"va", sentinel::HexString(va, at)},
                           {"expected", WordsHex(expected, code)},
                           {"actual", WordsHex(actual, words)}});
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

// The texts the service's answers ask the wizard's pages to show (a created code with its expiry and link, a refusal), handed to the
// UI text writer (#1726). The same texts and the same lines as the PC's applier (runtime/patch/invite_answer_text.h); the writer is
// the shared one (runtime/patch/ui_text.h) with the Quest's address table (ui_text_quest.h).
void PostShareTexts() {
  nevr_invite_match::ShareText text;
  while (nevr_invite_match::TakeShareText(&text)) {
    const nevr_invite::LogLevel info = nevr_invite::LogLevel::Info;
    const nevr_invite::LogLevel warning = nevr_invite::LogLevel::Warning;
    GameAccess& log = FrameAccessInstance();
    if (!nevr_quest::ui_text::Ready()) {
      log.Log(warning, nevr_invite_match::NotPostedText(text.field, text.length, "writer_off"));
      continue;
    }
    nevr_ui_text::Request request;
    request.target.actor = text.actor;
    request.target.element = text.element;
    request.text.assign(text.text, text.length);
    const nevr_ui_text::Check check = nevr_quest::ui_text::Post(std::move(request));
    if (check == nevr_ui_text::Check::Ok) {
      log.Log(info, nevr_invite_match::PostedText(text.field, text.length));
    } else {
      log.Log(warning, nevr_invite_match::NotPostedText(text.field, text.length, nevr_ui_text::CheckName(check)));
    }
  }
}

// The frame work: each resolved match the service has answered since the last frame is offered to the join, and each text an answer
// asked for is handed to the writer, which then tries what is due. The service does the one call that may throw (its own TU); this
// file keeps no exception path (the frame path, see the header).
bool ApplyAnswers(void* /*context*/) noexcept {
  // #1415: nothing resolved waits, so the frame costs one atomic load and takes no lock (the PC applier takes the same early-out).
  if (nevr_invite_match::ResolvedMatchPending()) {
    char match[nevr_invite_match::kMaxMatchIdBytes];
    std::size_t length = 0;
    while (nevr_invite_match::TakeResolvedMatch(match, sizeof(match), &length)) {
      TheOfferer().Offer(std::string_view(match, length), nevr_invite_match::ServiceActive());
    }
    PostShareTexts();
  }
  nevr_quest::ui_text::OnFrame();
  return true;
}

}  // namespace

bool RegisterAnswerApplier() noexcept { return nevr_quest::invite::frame::RegisterFrameWork(ApplyAnswers, nullptr); }

void SetFrameAccessForTest(GameAccess* access) { FrameAccessInstance().target = access; }

}  // namespace nevr_quest_invite_join
