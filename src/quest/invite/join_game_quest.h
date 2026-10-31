#pragma once
// #966 slice S4 (docs/adr/0017, plan "S4 - Quest join on a resolved code"): the Quest's join of the match a resolved invite code
// names. It is the PC join (src/runtime/patch/invite_join.h, Joiner) run against libr15's CR15Game through a GameAccess seam:
//   - the netgame state is read at netgame+0 (game_layout::kStateOffset); the game object is netgame+8 (kGameOffset);
//   - the staged lobby id (16 bytes, Windows GUID layout) goes at game+0x8538 and the team word 0xFFFF at game+0x8558
//     (kQuestLayout; disassembly libr15 0x1301d88 BeginMultiplayer reads both);
//   - the route fields are read first (flags +0x8518, gate +0x8520, lobby +0x8528 vs the invalid id), and nothing is staged when
//     the game's own begin would go elsewhere (other_route). The gametype word +0x8560 is never read or written (disassembly: no
//     #0x8560 operand in the 281-instruction listing of 0x1301d88);
//   - the begin is EnableTransitionMenu(game) (libr15 0x12005fc, the menu event) then BeginMultiplayer(game) (0x1201be4), both on
//     the game thread, through the frame dispatcher (frame_dispatch.h).
//
// The answers reach the join through RegisterAnswerApplier: a frame work item that takes the service's answers (TakeAnswers) and
// offers each resolved one. Nothing here runs from a worker thread; the join refuses (wrong_thread) when it is not the game thread.

#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/patch/invite_join.h"

namespace nevr_quest_invite_join {

/// Everything the join touches in the game, behind one seam: production reads libr15 and the frame hook's netgame; the host test
/// fakes it. Every call is made on the game thread (the join refuses otherwise).
class GameAccess {
 public:
  virtual ~GameAccess() = default;
  virtual bool OnTickThread() = 0;                                  // the calling thread is the game's
  virtual void* NetGame() = 0;                                      // the current CR15NetGame; null before the first frame
  virtual bool ProloguesValid() = 0;                                // EnableTransitionMenu and BeginMultiplayer at their pins
  virtual void EnableMenu(void* game) = 0;                          // CR15Game::EnableTransitionMenu(game)
  virtual void Begin(void* game) = 0;                               // CR15Game::BeginMultiplayer(game)
  virtual void Log(nevr_invite::LogLevel level, const std::string& line) = 0;
  virtual std::int64_t NowMs() = 0;                                 // monotonic milliseconds
};

/// The join for this run. Holds the join window and the last verified stage (the undo when the begin cannot run).
class Offerer {
 public:
  explicit Offerer(GameAccess& access);
  Offerer(const Offerer&) = delete;
  Offerer& operator=(const Offerer&) = delete;

  /// Offers the match an answer names. `feature_on` is the invite service's state (nevr_invite_service::Active()). Returns the
  /// reason the join did not start, or None when the begin ran once.
  nevr_invite_join::Reason Offer(std::string_view match_id, bool feature_on);

 private:
  class GameMemory;
  static nevr_invite_join::Seam MakeSeam(Offerer* self);
  void* GameObject();
  nevr_invite_join::StageOutcome StageLobby(const nevr_invite_join::LobbyBytes& lobby, std::uint16_t team);
  bool Begin();
  bool Unstage();
  void Say(nevr_invite::LogLevel level, const std::string& line);

  GameAccess& access_;
  void* stagedGame_ = nullptr;
  nevr_invite_join::PriorStage stagedPrior_;
  nevr_invite_join::Joiner joiner_;
};

/// The production seam (libr15 through sentinel::FindLoadedImage, the frame hook's netgame).
GameAccess& LiveAccess();

/// Registers the frame work that takes the service's answers and offers each resolved one. Game thread only through the dispatcher.
/// False when the dispatcher has no room (frame::RegisterFrameWork).
bool RegisterAnswerApplier() noexcept;

/// Test support: the frame work offers through `access` from the next frame on; null restores the live access. Call only while
/// no frame runs.
void SetFrameAccessForTest(GameAccess* access);

}  // namespace nevr_quest_invite_join
