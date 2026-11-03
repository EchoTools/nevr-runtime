# Quest controller hand flip: a menu toggle (design, not implemented)

Tracking issue: #1986. The measurement that decides whether this is built is in the tree already (`src/quest/sentinel/hand_pose_log_hook.h`, smoke row Q34). This document is the design that follows only if that log shows the roll the prediction names.

## Condition
Build it only if the two logs (Touch and Touch Pro or Touch Plus, same Quest 2, same pose) show a relative rotation of 180 degrees (170 to 190) about the controller's local Z axis, with the axis within 15 degrees of Z. Otherwise the logged quaternions decide the transform and this design changes.

## What the game does
Disassembly of libr15 (pinned build `b243509c08ce677aeb95fa348016949b3fc45230`; the disassembly VA is the pinned VA + 0x100000). `CVR::PollTracking` (0x1a7142c) calls `FUN 0x1a71808`: enumerate the input devices (call 0x1a71840), `vrapi_GetInputTrackingState` twice per device (0x1a718c4 and 0x1a718dc), and for type 4 only the capabilities (0x1a718fc). The hand is `!((ControllerCapabilities >> 2) & 1)`. The pose goes unchanged into the hand's slot (stride 0x68). `PollTracking` then applies one tracking-space quaternion (reset to identity by `CVR::Shutdown`, 0x1a6f128) equally to the head and both hands. Nothing depends on the controller model, so a roll comes from the pose VrApi reports for that controller.

## The hook
Reuse the two hooks of `hand_pose_log_hook.cpp` (enumerate slot 0x36d5dd0, tracking slot 0x36dc280; `ovrTracking` is Status at 0, padding, then the orientation x, y, z, w at 8). In the tracking handler, after the original returns success, when the flag is on and the device is in the type-4 table, post-multiply the orientation by a 180 degree roll about local Z: `q' = q * (0, 0, 1, 0) = (y, -x, w, -z)` for `q = (x, y, z, w)`. Position, velocities, accelerations, status and times stay as written. Hand-tracking and unknown ids are never touched. Default off: players on a Quest 2 with Touch Pro or Touch Plus controllers turn it on. No automatic choice exists: no documented capability tells the controllers apart.

## The menu toggle beside Texture Resolution
- Menu: `texture_menu.cpp` `WalkMenu` keeps the `graphics|msaa` row alone on a page renamed GRAPHICS; it becomes a list of two rows, Texture Resolution and "Controller Hand Flip" (STRING ARRAY Off / On, idx 0). `WalkGameSettings` removes any quest hiding entry for the new row's path.
- Carrier field (needs a consumer audit across every binary first): a persisted field the render path does not read. From `ValidateGfxSettings` (pinned 0x20e98a4) and the reconstructed `ApplyRuntimeGfxSettings` (pinned 0x20e8e0c), flags bit 0 (`graphics|fullscreen`), `+0x04` (`display`, unclamped) and `+0x50` (clamped to 0 or 1) are not read in Apply. Preferred: `graphics|fullscreen`; fallback `+0x04`. Unverified: readers of those fields outside Apply.
- Live apply: `HandleApply` (`texture_menu_hook.cpp`) sees every delivery. Read the field; the first delivery is the stored choice or the default; a later change sets the flag the tracking handler reads on its next call (next poll, no restart) and queues a save.
- Persistence: its own file beside the texture marker, `{"version":1,"hand_flip":0|1}`, written only by a menu change (a derived value is never saved; an unversioned or other-version file is ignored and logged), read before the first tracking call. The texture marker's version is not bumped.
- Log lines: `hand_flip_start` (source, flip, unversioned_marker_ignored), `hand_flip_saved` (result, flip, errno); the existing `hand_flip_device` line gains `flip` and `by`.

## Tests
Unit: the roll (`(y, -x, w, -z)`; applied twice gives `-q`); only type 4 rolled; flag off leaves the bytes identical, on changes only the four orientation floats; both polls of a frame rolled; menu rewrite (second row after Texture Resolution, shape mismatch, texture rows unchanged); start resolver (no file means off and nothing written, a menu change writes the versioned file, unversioned and other-version files ignored); live apply with no restart; golden log lines; pinned slots; static-initializer and code-page-write scans. Each fails with the hook removed.
Headset: Quest 2 with Touch Pro or Touch Plus, toggle Off then On from the menu without a restart: the hands are palm-correct on the next frame with On; a restart logs `hand_flip_start source=player_marker flip=1`; Quest 2 with Touch controllers and the toggle Off is unchanged.

## Not verified
The carrier field's other readers; the `ovrTracking` offsets beyond the orientation (the world-frame velocities are left as written, the consumers of velocity are not checked); Touch Plus on a Quest 2 and Touch Pro on a Quest 3 on any headset.
