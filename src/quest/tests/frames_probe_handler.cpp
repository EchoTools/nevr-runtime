// Built -fno-exceptions. A hook whose handler calls a helper with a personality directly, across the
// call into the "game" (original): the frame sensor's controls (tests/quest frames_sensor_test.go).
// Never run: the sensor reads the linked executable.
//
//   frames_probe_bad  the helper is NOT annotated: it is live across the call, so the sensor MUST fail it
//   frames_probe_ok   the helper is annotated NEVR_OUTSIDE_GAME_CALL: the sensor MUST pass it and list it
#include "callback_thunk.h"

namespace probe {
// Defined in frames_probe_helper.inc (exceptions enabled): a try/catch, so its CIE names a personality.
int LiveHelper(int value) noexcept;
}  // namespace probe

namespace {
struct ProbeTag {};
using ProbeThunk = sentinel::CallbackThunk<ProbeTag, int(int)>;

int ProbeHandler(ProbeThunk::Fn original, int value) noexcept {
  const int result = original(value);  // the call into the game
  return probe::LiveHelper(result);    // a direct edge from the handler
}

NEVR_HOOK_RECORD(kProbeHook, ProbeThunk, &ProbeHandler);
}  // namespace

int main() {
  ProbeThunk::Arm(kProbeHook);
  return ProbeThunk::EntryAddress() == nullptr ? 1 : 0;
}
