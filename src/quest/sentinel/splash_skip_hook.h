/* A button press skips each startup splash on the headset (#962), the Quest twin of the PC hook in
 * src/runtime/patch/splash_skip.cpp. The decisions are the PC rules header (runtime/patch/splash_skip_rules.h), shared;
 * only the hook point and the reads differ.
 *
 * What the game does (pinned libr15; the disassembly names in pinned_targets.h): CR15VRMenuWidgetCS::Update runs every frame. It
 * finds the element the pointer ray hovers and, when the player presses a button the widget polls, sends that element's
 * click symbol (+0x58 of its definition) to the game space with the element's link keys. The splash's Continue element
 * carries the click symbol continue_clicked, so the skip sends that event with the element's own keys, after the Continue
 * element has been live for 750 ms and at most once per 1500 ms.
 *
 * One hook, on the widget's Update entry (the engine reaches it through a function pointer, so there is no GOT slot): the
 * row `splash_skip.widget_update` of the build-time patch table (docs/adr/0021). The hook calls the original, then looks
 * for the Continue element and sends the click on a rising edge. Install resolves SendComponentEvent and
 * CInputListener::Pressed by their first four words once Attach has verified libr15, then activates the row through
 * InstallRowThunk. A mismatch or an unverified row logs its status and installs nothing (the row stays pass-through).
 *
 * Logs (JSON, event "splash_skip"): one "live" line when the Continue element first appears, one "skipped" line per
 * skip. Install logs its outcome once. Nothing logs on the game's call path except those two lines.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "got_hook.h"

namespace nevr_quest::splash_skip {

// Activates the row on libr15's CR15VRMenuWidgetCS::Update. True when the hook is live. Called from the
// integration step that runs once libr15 is loaded (production_steps.cpp), before the first splash is drawn. `lookup` finds
// the loaded libr15; a host test passes its own.
bool Install(sentinel::ImageLookup lookup = sentinel::FindLoadedImage) noexcept;

}  // namespace nevr_quest::splash_skip
