// The sentinel constructor's sequence (docs/adr/0003, "Integration"): what runs, in which order, what
// depends on what, and how one piece failing leaves the others running.
//
// The sequence is pure policy over an abstract `Steps`, so the host test drives it with fakes and checks
// the order, the gating by feature, the counter registration before the single StartReporter, and the
// failure isolation. The production Steps (production_steps.cpp) bind each step to the real library.
//
// Order:
//   1  ArmCrashReporter              breakpad, before anything else can crash
//   2  ResolveConfig                 InitActivation: embedded defaults + nevr-quest.json, every state logged
//   3  RegisterCounters              every counter of every hook that will be installed, BEFORE ...
//   4  StartReporter                 ... the single StartReporter (the reporter refuses a later register)
//   5  InstallClockHook              the always-on proof hook
//   6  StartTokenAuth                needed by the bridge (the remote JWT) and the login rewrite
//   7  StartBridge                   loopback listener + router; the redirect needs its port
//   8  InstallRedirect               CJson::TString thunks on libr15
//   9  InstallSocial                 the NEVR social facade on libr15's CNSProvider::Social slot
//  10  InstallDlopenHook             the post-load installs: login hook, matchmaking redirect
//
// Dependencies (a failed or skipped piece disables what needs it and nothing else):
//   token auth fails        -> bridge, login, social and the redirect-through-the-bridge are not started
//   bridge fails            -> login, social and the redirect-through-the-bridge are not started
//   redirect counters fail  -> redirect (and the matchmaking install) is not installed
//   dlopen counters fail    -> the dlopen hook, and so the login and matchmaking installs, are not installed
//   social counters fail    -> social is not installed
// "redirect-through-the-bridge" is the redirect when the bridge feature is effective: a redirect that
// points the game at a loopback port nobody listens on, or straight at a TLS endpoint the game cannot
// speak, is worse than leaving the game's own hosts.
//
// Nothing here blocks: no step waits on the network, a thread or a lock held by the game.
#pragma once

#include <cstdint>

#include "quest/sentinel/quest_config.h"

namespace nevr_quest::integration {

enum class StepState : std::uint8_t {
  kSkipped,   // not wanted (feature off) or a dependency failed; `reason` says which
  kOk,
  kFailed,    // the step reported failure
  kThrew,     // the step threw a std::exception (contained)
};

const char* StepStateName(StepState state);

// One entry per step, in the order they ran, for the log and for the tests.
enum class StepId : std::uint8_t {
  kArmCrashReporter,
  kResolveConfig,
  kRegisterClockCounters,
  kRegisterRedirectCounters,
  kRegisterDlopenCounters,
  kRegisterSocialCounters,
  kStartReporter,
  kInstallClockHook,
  kStartTokenAuth,
  kStartBridge,
  kInstallRedirect,
  kInstallSocial,
  kInstallDlopenHook,
  kCount,
};

const char* StepName(StepId id);

struct StepOutcome {
  StepState state = StepState::kSkipped;
  const char* reason = "not_run";  // fixed token
};

struct ConstructorReport {
  StepOutcome steps[static_cast<int>(StepId::kCount)];
  StepId order[static_cast<int>(StepId::kCount)] = {};  // the ids in the order they were attempted
  int attempted = 0;
  const StepOutcome& at(StepId id) const { return steps[static_cast<int>(id)]; }
};

// Every method may throw std::exception (contained by the sequence) or return false.
class Steps {
 public:
  virtual ~Steps() = default;

  virtual void ArmCrashReporter() = 0;
  // InitActivation, then the resolved configuration.
  virtual const nevr_quest::ResolvedConfig& ResolveConfig() = 0;
  // features.social from the config file text, combined with the effective features; true when the
  // social facade is wanted.
  virtual bool SocialWanted(const nevr_quest::Features& effective) = 0;

  virtual bool RegisterClockCounters() = 0;
  virtual bool RegisterRedirectCounters() = 0;
  virtual bool RegisterDlopenCounters() = 0;
  virtual bool RegisterSocialCounters() = 0;
  virtual bool StartReporter() = 0;

  virtual bool InstallClockHook() = 0;
  virtual bool StartTokenAuth() = 0;
  virtual bool StartBridge() = 0;
  virtual bool InstallRedirect() = 0;
  virtual bool InstallSocial() = 0;
  // `login` / `matchmaking`: which post-load installs the dlopen hook is for.
  virtual bool InstallDlopenHook(bool login, bool matchmaking) = 0;

  // One line per step outcome and per decision. `step` and `reason` are fixed tokens.
  virtual void Note(const char* step, const char* state, const char* reason) = 0;
};

// Runs the sequence. Never throws, never blocks.
ConstructorReport RunConstructorSequence(Steps& steps) noexcept;

}  // namespace nevr_quest::integration
