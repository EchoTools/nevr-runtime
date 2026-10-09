#include "quest/integration/ctor_sequence.h"

#include <exception>

namespace nevr_quest::integration {

const char* StepStateName(StepState state) {
  switch (state) {
    case StepState::kSkipped: return "skipped";
    case StepState::kOk: return "ok";
    case StepState::kFailed: return "failed";
    case StepState::kThrew: return "threw";
  }
  return "unknown";
}

const char* StepName(StepId id) {
  switch (id) {
    case StepId::kArmCrashReporter: return "arm_crash_reporter";
    case StepId::kResolveConfig: return "resolve_config";
    case StepId::kRegisterClockCounters: return "register_clock_counters";
    case StepId::kRegisterRedirectCounters: return "register_redirect_counters";
    case StepId::kRegisterDlopenCounters: return "register_dlopen_counters";
    case StepId::kRegisterSocialCounters: return "register_social_counters";
    case StepId::kRegisterLoginPromptCounters: return "register_login_prompt_counters";
    case StepId::kStartReporter: return "start_reporter";
    case StepId::kInstallClockHook: return "install_clock_hook";
    case StepId::kStartTokenAuth: return "start_token_auth";
    case StepId::kInstallLoginPrompt: return "install_login_prompt";
    case StepId::kStartBridge: return "start_bridge";
    case StepId::kInstallRedirect: return "install_redirect";
    case StepId::kInstallSocial: return "install_social";
    case StepId::kInstallDlopenHook: return "install_dlopen_hook";
    case StepId::kCount: break;
  }
  return "unknown";
}

namespace {

class Runner {
 public:
  explicit Runner(Steps& steps) : steps_(steps) {}

  ConstructorReport report;

  void Skip(StepId id, const char* reason) {
    report.steps[static_cast<int>(id)] = {StepState::kSkipped, reason};
    Note(id);
  }

  // Runs `fn` (returns success) for step `id`, containing exceptions.
  template <typename Fn>
  bool Run(StepId id, Fn fn) {
    report.order[report.attempted++] = id;
    StepOutcome out;
    try {
      out = fn() ? StepOutcome{StepState::kOk, "ok"} : StepOutcome{StepState::kFailed, "step_reported_failure"};
    } catch (const std::exception&) {
      out = {StepState::kThrew, "exception"};
    }
    report.steps[static_cast<int>(id)] = out;
    Note(id);
    return out.state == StepState::kOk;
  }

 private:
  void Note(StepId id) {
    const StepOutcome& o = report.steps[static_cast<int>(id)];
    try {
      steps_.Note(StepName(id), StepStateName(o.state), o.reason);
    } catch (const std::exception&) {
      // A logging failure must not change what the sequence does.
    }
  }
  Steps& steps_;
};

}  // namespace

ConstructorReport RunConstructorSequence(Steps& steps) noexcept {
  Runner r(steps);
  try {
    r.Run(StepId::kArmCrashReporter, [&] { steps.ArmCrashReporter(); return true; });

    nevr_quest::Features want;  // all off if the configuration cannot be resolved
    r.Run(StepId::kResolveConfig, [&] {
      const nevr_quest::ResolvedConfig& cfg = steps.ResolveConfig();
      want = cfg.effective;
      return true;
    });

    const bool wantRedirect = want.redirect;
    const bool wantBridge = want.bridge;
    const bool wantLogin = want.login;
    const bool wantSocial = want.social;  // the config package already requires login for it
    const bool wantTokenAuth = wantBridge || wantLogin;
    const bool wantDlopen = wantLogin || wantRedirect;

    // 3: every counter, before the one StartReporter. A refused registration turns off only the piece
    // whose counters those are.
    r.Run(StepId::kRegisterClockCounters, [&] { return steps.RegisterClockCounters(); });
    bool redirectCounters = false, dlopenCounters = false, socialCounters = false, promptCounters = false;
    if (wantRedirect) {
      redirectCounters = r.Run(StepId::kRegisterRedirectCounters, [&] { return steps.RegisterRedirectCounters(); });
    } else {
      r.Skip(StepId::kRegisterRedirectCounters, "redirect_off");
    }
    if (wantDlopen) {
      dlopenCounters = r.Run(StepId::kRegisterDlopenCounters, [&] { return steps.RegisterDlopenCounters(); });
    } else {
      r.Skip(StepId::kRegisterDlopenCounters, "login_and_redirect_off");
    }
    if (wantSocial) {
      socialCounters = r.Run(StepId::kRegisterSocialCounters, [&] { return steps.RegisterSocialCounters(); });
    } else {
      r.Skip(StepId::kRegisterSocialCounters, "social_off");
    }
    // The sign-in prompt belongs to the login feature (#239), like the login hook it stands in for while
    // the player signs in.
    if (wantLogin) {
      promptCounters =
          r.Run(StepId::kRegisterLoginPromptCounters, [&] { return steps.RegisterLoginPromptCounters(); });
    } else {
      r.Skip(StepId::kRegisterLoginPromptCounters, "login_off");
    }
    r.Run(StepId::kStartReporter, [&] { return steps.StartReporter(); });

    r.Run(StepId::kInstallClockHook, [&] { return steps.InstallClockHook(); });

    bool tokenOk = false;
    if (wantTokenAuth) {
      tokenOk = r.Run(StepId::kStartTokenAuth, [&] { return steps.StartTokenAuth(); });
    } else {
      r.Skip(StepId::kStartTokenAuth, "bridge_and_login_off");
    }

    // The sign-in prompt in the game's login-error text (#239), behind the login feature. Token auth is
    // what publishes the prompt (its GameTextPresenter), so without it the hook would only pass the game's
    // message through and is not installed.
    if (!wantLogin) {
      r.Skip(StepId::kInstallLoginPrompt, "login_off");
    } else if (!promptCounters) {
      r.Skip(StepId::kInstallLoginPrompt, "counters_refused");
    } else if (!tokenOk) {
      r.Skip(StepId::kInstallLoginPrompt, "token_auth_unavailable");
    } else {
      r.Run(StepId::kInstallLoginPrompt, [&] { return steps.InstallLoginPrompt(); });
    }

    bool bridgeOk = false;
    if (!wantBridge) {
      r.Skip(StepId::kStartBridge, "bridge_off");
    } else if (!tokenOk) {
      r.Skip(StepId::kStartBridge, "token_auth_unavailable");
    } else {
      bridgeOk = r.Run(StepId::kStartBridge, [&] { return steps.StartBridge(); });
    }
    // Bridge requested but not running: nothing that depends on it may go live.
    const bool bridgeBlocked = wantBridge && !bridgeOk;

    bool redirectOk = false;
    if (!wantRedirect) {
      r.Skip(StepId::kInstallRedirect, "redirect_off");
    } else if (!redirectCounters) {
      r.Skip(StepId::kInstallRedirect, "counters_refused");
    } else if (bridgeBlocked) {
      r.Skip(StepId::kInstallRedirect, "bridge_unavailable");
    } else {
      redirectOk = r.Run(StepId::kInstallRedirect, [&] { return steps.InstallRedirect(); });
    }

    if (!wantSocial) {
      r.Skip(StepId::kInstallSocial, "social_off");
    } else if (!socialCounters) {
      r.Skip(StepId::kInstallSocial, "counters_refused");
    } else if (!bridgeOk) {
      r.Skip(StepId::kInstallSocial, "bridge_unavailable");
    } else {
      r.Run(StepId::kInstallSocial, [&] { return steps.InstallSocial(); });
    }

    // The post-load installs: the login hook needs the bridge (the rewrite targets the service the
    // bridge reaches) and token auth; the matchmaking redirect needs the redirect installed.
    const bool loginWanted = wantLogin && bridgeOk && tokenOk;
    const bool matchmakingWanted = redirectOk;
    if (!wantDlopen) {
      r.Skip(StepId::kInstallDlopenHook, "login_and_redirect_off");
    } else if (!dlopenCounters) {
      r.Skip(StepId::kInstallDlopenHook, "counters_refused");
    } else if (!loginWanted && !matchmakingWanted) {
      r.Skip(StepId::kInstallDlopenHook, "nothing_to_install_after_load");
    } else {
      r.Run(StepId::kInstallDlopenHook, [&] { return steps.InstallDlopenHook(loginWanted, matchmakingWanted); });
    }
  } catch (const std::exception&) {
    // Unreachable by construction (every step is contained); the sequence still never throws.
  }
  return r.report;
}

}  // namespace nevr_quest::integration
