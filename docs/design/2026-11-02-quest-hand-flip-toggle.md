# Quest controller hand flip: placement in the VrApi layer, with a menu toggle (design, not implemented)

Tracking issues: #1986 (the report), #2007 (the placement). The measurement that decides whether this is built is in the tree already (`src/quest/sentinel/hand_pose_log_hook.h`, smoke row Q34 in `docs/testing/smoke-checklist.md`). This document is the design that follows only if that log shows the roll the prediction names. The fix belongs to the VrApi-on-OpenXR layer (Placement). The only fix this tree can make before that layer is reconstructed here is the sentinel interim, which is marked known-wrong below.

## Condition
Build it only if the two logs (Touch and Touch Pro or Touch Plus, same Quest 2, same pose) show a relative rotation of 180 degrees (170 to 190) about the controller's local Z axis, with the axis within 15 degrees of Z. Otherwise the logged quaternions decide the transform and this design changes. The gate applies to the layer's key and to the interim alike.

## What the game does
Disassembly of libr15 (pinned build `b243509c08ce677aeb95fa348016949b3fc45230`; the disassembly VA is the pinned VA + 0x100000). `CVR::PollTracking` (0x1a7142c) calls `FUN 0x1a71808`: enumerate the input devices (call 0x1a71840), `vrapi_GetInputTrackingState` twice per device (0x1a718c4 and 0x1a718dc), and for type 4 only the capabilities (0x1a718fc). The hand is `!((ControllerCapabilities >> 2) & 1)`. The pose goes unchanged into the hand's slot (stride 0x68). `PollTracking` then applies one tracking-space quaternion (reset to identity by `CVR::Shutdown`, 0x1a6f128) equally to the head and both hands. Nothing depends on the controller model, so a roll comes from the pose VrApi reports for that controller.

## Placement

### The layer owns the fix
Controller differences belong in the VrApi-on-OpenXR layer: the code that answers the game's `vrapi_GetInputTrackingState` and `vrapi_GetInputDeviceCapabilities` for its controllers. ADR 0033 (controllers are mapped by OpenXR interaction profile inside the VrApi layer) rule 1 puts them there, with pose corrections keyed by the path `xrGetCurrentInteractionProfile` reports. Its rule 2 keeps code above the boundary from branching on the controller to change input. ADR 0031 to 0035 are proposed under #2013 and are not yet on main, so this document cites them by number and title.

The layer is ours to reconstruct in this repo under ADR 0035, with EchoFrame and echoquest-combat cited as references and never taken whole (AGENTS.md, Guardrails). The reconstruction takes its facts from the reference binary's disassembly and the OpenXR specification, its lines follow `docs/standards/logging.md`, and its tests fail when the change is reverted.

The layer is not in this tree. The release base carries the reference build's `libvrapi.so` (`tools/quest_release_base.py` names it), and `tools/quest_runtime_pin.py` and `tools/quest_repack_libs.py` pin its output into the signed APK. This repo therefore changes no code in the layer today. ADR 0035 makes the layer class 2 once it is reconstructed here.

The scope is open. #1737 row G6A-094 marks the VrApi-on-OpenXR runtime out of scope, and its H15 section keeps Quest on the system VrApi. ADR 0033 and ADR 0035 bring the layer into scope. This design does not settle that, and no layer work is scheduled until the scope is settled (#2007).

### The key: the interaction profile
The layer reads `xrGetCurrentInteractionProfile` for each hand's top-level user path, `/user/hand/left` and `/user/hand/right`. For a hand whose profile is one the measurement below shows flips, the layer applies the roll to the orientation it returns for that hand's tracked-remote device: `q' = q * (0, 0, 1, 0) = (y, -x, w, -z)` for `q = (x, y, z, w)`, the same roll as the interim. Position, velocities, status and times stay as written. Other profiles, hand tracking and unknown devices are not touched.

The key names no headset. #1737 row G6A-096 describes the reference build's default table (Quest 2 none, Quest 3 a 180 degree roll). That table is keyed on the headset, and ADR 0031 and ADR 0032 do not allow a headset-keyed fix, so it is not the key here.

### The measurement that decides the key
Measurement A decides the key when it separates the two controllers. On one Quest 2, record the profile path the layer reports for `/user/hand/left` and `/user/hand/right` with Touch, then with Touch Pro or Touch Plus, one session each. If the paths differ, the key is the path of the flipped controller. The path strings are not stated here: whether Touch Pro and Touch Plus report distinct paths on a Quest 2 is not measured (ADR 0033, Not claimed).

Measurement B applies only when A does not separate them. It keys on the grip-to-aim relative rotation of the same hand, from the grip and aim spaces the layer locates for it. If the Touch and the Touch Pro or Touch Plus logs differ in that quantity by the Condition's amount (170 to 190 degrees about the controller's local Z axis, axis within 15 degrees of Z), the key is that relation, and the roll applies only where the measured relation shows the flipped pose. If B does not separate them either, the layer has no automatic key: no layer fix is built, and the interim is the only control.

Both measurements come from one line per bound controller per session, which the layer writes (ADR 0033 rule 4): the profile path, the pose source, whether the roll was applied, and the grip and aim orientations. The line's tag and fields follow `docs/standards/logging.md` and `docs/standards/naming.md`.

The current logs cannot decide the key. The `hand_flip_device` line (`src/quest/sentinel/hand_pose_log_hook.cpp`) has no profile field, and no code on main calls `xrGetCurrentInteractionProfile` or locates a grip or aim space. Whether the reference build's own log names the profile path is not checked.

## The interim in the sentinel
The interim roll sits in the sentinel's tracking handler, above the VrApi boundary: KNOWN-WRONG #2007. ADR 0033 rule 3 allows that only for players who need the fix before the layer carries it, and the layer's key replaces it when it is built. The interim's type-4 test is the input-device-type read that ADR 0033 C1 names, which is part of why it is an interim.

### The interim hook
Reuse the two hooks of `hand_pose_log_hook.cpp` (enumerate slot 0x36d5dd0, tracking slot 0x36dc280; `ovrTracking` is Status at 0, padding, then the orientation x, y, z, w at 8). KNOWN-WRONG #2007 In the tracking handler, after the original returns success, when the flag is on and the device is in the type-4 table, post-multiply the orientation by a 180 degree roll about local Z: `q' = q * (0, 0, 1, 0) = (y, -x, w, -z)` for `q = (x, y, z, w)`. Position, velocities, accelerations, status and times stay as written. Hand-tracking and unknown ids are never touched. Default off: players on a Quest 2 with Touch Pro or Touch Plus controllers turn it on. The interim makes no automatic choice: the sentinel does not read the interaction profile (see The key: the interaction profile), and no documented capability tells a Touch Pro from a Touch (`src/quest/sentinel/hand_pose_log_hook.h`).

### The menu toggle beside Texture Resolution
The toggle is the player's switch for the interim while the fix is not verified on a headset (#1986). It has no effect on the layer's key. Because it exists only for the unverified interim, its row, its file and the interim's log lines are dropped in the change that moves the fix into the layer.

- Menu: `texture_menu.cpp` `WalkMenu` keeps the `graphics|msaa` row alone on a page renamed GRAPHICS; it becomes a list of two rows, Texture Resolution and "Controller Hand Flip" (STRING ARRAY Off / On, idx 0). `WalkGameSettings` removes any quest hiding entry for the new row's path.
- Carrier field (needs a consumer audit across every binary first): a persisted field the render path does not read. From `ValidateGfxSettings` (pinned 0x20e98a4) and the reconstructed `ApplyRuntimeGfxSettings` (pinned 0x20e8e0c), flags bit 0 (`graphics|fullscreen`), `+0x04` (`display`, unclamped) and `+0x50` (clamped to 0 or 1) are not read in Apply. Preferred: `graphics|fullscreen`; fallback `+0x04`. Unverified: readers of those fields outside Apply.
- Live apply: `HandleApply` (`texture_menu_hook.cpp`) sees every delivery. Read the field; the first delivery is the stored choice or the default. KNOWN-WRONG #2007 A later change sets the flag the tracking handler reads on its next call (next poll, no restart) and queues a save.
- Persistence: its own file beside the texture marker, `{"version":1,"hand_flip":0|1}`, written only by a menu change (a derived value is never saved; an unversioned or other-version file is ignored and logged), read before the first tracking call. The texture marker's version is not bumped.
- Log lines: `hand_flip_start` (source, flip, unversioned_marker_ignored), `hand_flip_saved` (result, flip, errno); the existing `hand_flip_device` line gains `flip` and `by`.

## Tests
Interim unit tests: the roll (`(y, -x, w, -z)`; applied twice gives `-q`); only type 4 rolled; flag off leaves the bytes identical, on changes only the four orientation floats; both polls of a frame rolled; menu rewrite (second row after Texture Resolution, shape mismatch, texture rows unchanged); start resolver (no file means off and nothing written, a menu change writes the versioned file, unversioned and other-version files ignored); live apply with no restart; golden log lines; pinned slots; static-initializer and code-page-write scans. Each fails when the hook is reverted.

Layer tests, when the layer is built: the key on the path, with one flipped and one unflipped profile (the flipped hand's orientation rolls, the other hand's bytes are identical); the roll applied twice gives the negated quaternion; hand tracking and unknown devices untouched; measurement B's rule rolls only in the measured relation; the per-controller line golden. Each fails when the roll is reverted.

## Headset
Interim, Quest 2 with Touch Pro or Touch Plus, toggle Off then On from the menu without a restart: the hands are palm-correct on the next frame with On; a restart logs `hand_flip_start source=player_marker flip=1`; Quest 2 with Touch controllers and the toggle Off is unchanged.

Layer, when built, Quest 2 with Touch Pro or Touch Plus and no toggle: the per-controller line names the flipped profile for each hand, and the hands are palm-correct from the first frame. Quest 2 with Touch controllers: the line names the Touch profile and the hands are unchanged.

## Not verified
The carrier field's other readers; the `ovrTracking` offsets beyond the orientation (the world-frame velocities are left as written, the consumers of velocity are not checked); Touch Plus on a Quest 2 and Touch Pro on a Quest 3 on any headset; whether Touch Pro and Touch Plus report distinct profile paths on a Quest 2 (measurement A); whether the reference build's own log names the profile path; whether the roll sits in the grip pose, the aim pose or the whole controller pose.
