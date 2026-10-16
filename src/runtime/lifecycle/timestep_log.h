#pragma once

// #515: what boot.cpp says about a server's time-step arguments, as a pure decision so a test drives every
// branch: the level and the text come from what the game-main hook recorded (TimestepStripReport), what
// ApplyServerTimestep and ApplyFlag recorded, and whether boot.cpp took the process for a server. It does not
// read the command line itself; the caller says whether a time-step option is on it.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/lifecycle/server_timestep.h"

namespace nevr_timestep_log {

/// What GameMainWrapperHook did to the game's command line (lifecycle/cmdline_strip.h).
struct TimestepStripReport {
  unsigned tokensRemoved = 0;  ///< Time-step options and their parameters removed; 0 on a client or when none was given.
  std::string options;         ///< The canonical names of the removed options, comma separated.
  bool residual = false;       ///< A time-step option is still in the string handed to the game.
  bool failed = false;         ///< The removal threw; the game got the original string.
};

enum class Level { kInfo, kWarning };

struct Line {
  Level level = Level::kInfo;
  std::string text;
};

/// Everything the decision depends on.
struct Inputs {
  TimestepStripReport report;
  bool bootSaysServer = false;      ///< g_isServer, boot.cpp's reading of the command line.
  bool cmdHasTimestepOption = false;///< A time-step option is on the process's command line.
  nevr_server_timestep::StepOutcome step = nevr_server_timestep::StepOutcome::kNotApplied;
  uint32_t stepUs = 0;              ///< The step written, when step == kSet.
  uint32_t rateHz = 0;
  nevr_server_timestep::FlagOutcome flag = nevr_server_timestep::FlagOutcome::kNotApplied;
};

/// The lines to log: none when nothing concerns the time step; else one line, and a second Warning when a
/// time-step option is still in the string the game parsed.
inline std::vector<Line> Decide(const Inputs& in) {
  using nevr_server_timestep::FlagOutcome;
  using nevr_server_timestep::StepOutcome;
  std::vector<Line> lines;
  const TimestepStripReport& r = in.report;
  if (r.tokensRemoved > 0) {
    char stepText[160];
    if (!in.bootSaysServer) {
      // The game's parser read -server and boot.cpp did not: arguments came out, and no flag or step was set.
      std::snprintf(stepText, sizeof(stepText),
                    "NEVR runs this process as a client, so no fixed-time-step flag or step was set "
                    "(step_state=%s flag_state=%s)",
                    nevr_server_timestep::OutcomeName(in.step), nevr_server_timestep::FlagOutcomeName(in.flag));
      Line l;
      l.level = Level::kWarning;
      l.text = std::string("time-step arguments removed from the game's command line options=") + r.options +
               " tokens_removed=" + std::to_string(r.tokensRemoved) +
               ": the game's parser saw -server and NEVR's reading did not; " + stepText;
      lines.push_back(l);
    } else {
      const bool stepFailed = nevr_server_timestep::StepFailed(in.step);
      const bool flagFailed = in.flag == FlagOutcome::kFieldMissing;
      if (in.step == StepOutcome::kSet) {
        std::snprintf(stepText, sizeof(stepText), "the server step is %u us (%u Hz), set by NEVR",
                      static_cast<unsigned>(in.stepUs), static_cast<unsigned>(in.rateHz));
      } else if (stepFailed) {
        std::snprintf(stepText, sizeof(stepText), "the server step was NOT set (step_state=%s)",
                      nevr_server_timestep::OutcomeName(in.step));
      } else {
        std::snprintf(stepText, sizeof(stepText),
                      "NEVR sets the server step when the local config loads (step_state=%s)",
                      nevr_server_timestep::OutcomeName(in.step));
      }
      Line l;
      l.level = (stepFailed || flagFailed) ? Level::kWarning : Level::kInfo;
      l.text = std::string("server time-step arguments removed from the game's command line options=") +
               r.options + " tokens_removed=" + std::to_string(r.tokensRemoved) + "; " + stepText +
               "; flag_state=" + nevr_server_timestep::FlagOutcomeName(in.flag) +
               "; the game's own \"CL Args\" line still shows the original command line";
      lines.push_back(l);
    }
    if (r.residual) {
      Line l;
      l.level = Level::kWarning;
      l.text = "a time-step option is still in the game's command line after the removal (residual=true): "
               "the game may read it";
      lines.push_back(l);
    }
    return lines;
  }
  if (!in.cmdHasTimestepOption) return lines;
  Line l;
  if (in.bootSaysServer) {
    l.level = Level::kWarning;
    l.text = std::string("a time-step argument is on the command line and was NOT removed (tokens_removed=0 "
                         "hook_failed=") +
             (r.failed ? "true" : "false") + "): the game reads it. NEVR still sets the server step";
  } else {
    l.level = Level::kInfo;
    l.text = "time-step arguments passed to the game unchanged: NEVR sets the time step on a server only";
  }
  lines.push_back(l);
  return lines;
}

}  // namespace nevr_timestep_log
