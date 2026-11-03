# ADR 0033: controllers are mapped by OpenXR interaction profile inside the VrApi layer, not by controller model above it

Status: proposed. Applies to the Quest build. The PC client reads its controllers through the Oculus SDK and is out of
scope until it has an OpenXR path.

## Context

The game reads its controllers through VrApi: it enumerates input devices, reads their capabilities and polls their
tracking state. `src/quest/sentinel/hand_pose_log_hook.cpp` hooks two of those imports to log each controller's type and
raw pose (#1986). The release APK's VrApi is not Meta's: it is the reference build's `libvrapi.so`, which carries
EchoFrame's VrApi-on-OpenXR runtime (`tools/quest_release_base.py` names it among the files the release keeps). That
runtime binds the Steam Frame controllers (`XR_VALVE_frame_controller_interaction`), the Valve Index controllers, the
Khronos simple controller and Touch through OpenXR
([EchoFrame README](https://github.com/heisthecat31/EchoFrame), "Steam Frame", at `7beb80bb`).

#1986 reports a Quest 2 whose hands are flipped with Touch Pro or Touch Plus controllers.
`docs/design/2026-11-02-quest-hand-flip-toggle.md` proposes a fix in the sentinel's hooks, keyed on the controller.

OpenXR's own model: an application suggests bindings once per interaction profile it supports
(`xrSuggestInteractionProfileBindings`), the runtime binds whatever device is present, and
`xrGetCurrentInteractionProfile` reports the profile in use for a top-level user path
([Khronos: xrSuggestInteractionProfileBindings](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrSuggestInteractionProfileBindings.html),
[xrGetCurrentInteractionProfile](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetCurrentInteractionProfile.html)).
A runtime may run an application in compatibility mode for a profile it supports when the device present has no
suggested bindings ([Meta: OpenXR actions, action sets and bindings](https://developers.meta.com/horizon/documentation/native/android/mobile-openxr-actions-actionsets-bindings/)).

## Decision

1. **Controller differences are handled in the VrApi-on-OpenXR layer**: button layouts as suggested bindings per
   interaction profile, and pose corrections (grip or aim, an orientation offset) keyed by the path
   `xrGetCurrentInteractionProfile` reports.
2. **Code above the VrApi boundary does not branch on the controller to change input.** That covers the sentinel,
   plugins, and hooks on libr15. It may read and log the controller, as `hand_pose_log_hook.cpp` does.
3. **The layer is built from source by this repo's recipes** (ADR 0035). A controller fix goes into that source. A fix
   made above the boundary for players who need it now is marked at its site as known-wrong, with the issue that moves
   it into the layer.
4. **Each controller the layer binds is logged once per session**: the interaction profile path and the pose source used.

## Finding a violation

Scope: `src/quest` and its plugins, and the Python tools that patch the Quest APK. Out of scope: tests, documents,
`src/reconstruction`, the logging-only reads in `hand_pose_log_hook.cpp`, and dev-only input drivers that replay inputs
per hand and never ship to players (the autopilot, `docs/adr/0025-quest-autopilot-route.md`).

- **C1.** Outside the VrApi layer, a read of the controller's identity (its model or type: an input device type, its
  capability flags, a Touch Pro or Touch Plus bit, the headset model used as a stand-in for the controller) that changes a
  pose, a button or a binding. The hand a controller is held in is not its identity: code that addresses the left or right
  controller is not a violation.
- **C2.** A per-controller table (button map, pose offset, handedness flip) in the sentinel, a plugin or a tool.
- **C3.** A design or issue that places a controller fix above the boundary without the known-wrong marker of rule 3.

## Consequences

- #1986's fix lands in the layer's binding for the controllers it affects, and every headset with those controllers gets
  it.
- Steam Frame's and Index's layouts stay where EchoFrame put them.

## Not claimed

That Touch Pro and Touch Plus report a distinct interaction profile path on a Quest 2 under Meta's OpenXR runtime; the
fix's key depends on that measurement.
