/* The first line of the development probe file `nevr-data-probe` (data_wiring.h, "Debug probe"), parsed.
 *
 * `<kind> <argument>`: the kind is one of the words below followed by one space; the argument is the rest of the line.
 *   toast <text>      the text through the presenter the floor message uses (a Toast made on the android_main thread)
 *   ui-toast <text>   the text through a Toast whose handler runs on the main looper (toast_presenter.h, ShowOnMainLooper)
 *   ui-post <text>    the text queued to the UI thread's native looper, which makes the Toast there (toast_presenter.h, ShowOnUiThread)
 *   dialog <text>     the text in a dialog (no presenter exists yet: the probe logs `no_presenter`)
 *   block <seconds>   hold the android_main thread
 * A kind is matched whole, so `ui-toast x` is never read as `toast`. Anything else is kUnknown. Pure: no file, no log.
 */
#pragma once

#include <cstddef>

namespace nevr_quest::data_wiring {

enum class ProbeKind { kUnknown, kToast, kUiToast, kUiPost, kDialog, kBlock };

struct ProbeCommand {
  ProbeKind kind;
  const char* arg;  // inside the parsed line; "" for kUnknown
};

inline ProbeCommand ParseProbeCommand(const char* line) noexcept {
  struct Word {
    const char* text;  // the kind and its one space
    ProbeKind kind;
  };
  static constexpr Word kWords[] = {
      {"toast ", ProbeKind::kToast},
      {"ui-toast ", ProbeKind::kUiToast},
      {"ui-post ", ProbeKind::kUiPost},
      {"dialog ", ProbeKind::kDialog},
      {"block ", ProbeKind::kBlock},
  };
  for (const Word& w : kWords) {
    std::size_t n = 0;
    while (w.text[n] != '\0') ++n;
    std::size_t i = 0;
    while (i < n && line[i] == w.text[i]) ++i;
    if (i == n) return {w.kind, line + n};
  }
  return {ProbeKind::kUnknown, ""};
}

}  // namespace nevr_quest::data_wiring
