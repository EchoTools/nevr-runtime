/* A per-process budget for one kind of log line, for the crash and timing guards (#2178): a kind of line that can repeat has a cap, and the first
 * attempt over the cap writes one Warning that names the kind, the cap and where the count goes on, so a log that goes quiet says why
 * (impactsnd.cpp `Budget`, sensorwd.cpp and dronekill.cpp do the same for their own lines). The counts themselves live in each module's
 * `combat_<module>_stats` line, which is never capped.
 *
 *   {"event":"combat_<module>_log_cap","lines":"<kind>","cap":<n>,"msg":".."}   Warning, once per kind
 *
 * Header only. Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <atomic>
#include <cstdio>

#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat {

// True when this line is inside the budget `used` counts (cap lines of this kind are written). The attempt that makes it cap + 1 writes the one notice and
// answers false, and so does every later one. `module` is the module's name (`fxtrail`), `kind` the line's name, `after` what happens to the count.
inline bool LineBudget(std::atomic<unsigned>* used, unsigned cap, const char* module, const char* kind, const char* after) noexcept {
  const unsigned n = used->fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == cap + 1) {
    char event[48];
    std::snprintf(event, sizeof(event), "combat_%s_log_cap", module);
    sentinel::LogFields(sentinel::LogLevel::kWarn, event, {{"lines", kind}, {"cap", cap}, {"msg", after}});
  }
  return n <= cap;
}

}  // namespace nevr_quest::combat
