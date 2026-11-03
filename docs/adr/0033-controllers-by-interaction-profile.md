# ADR 0033: controllers are mapped by OpenXR interaction profile inside the VrApi layer, not by controller model above it

Status: proposed. Applies to the Quest build. The PC client reads its controllers through the Oculus SDK and is out of
scope until it has an OpenXR path.

## Context

The game reads its controllers through VrApi: it enumerates input devices, reads their capabilities and polls their
tracking state. `src/quest/sentinel/hand_pose_log_hook.cpp` hooks two of those imports to log each controller's type and
raw pose (#1986). The release APK's VrApi is not Meta's: it is the reference build's runtime, `libvrapi.so`, which
`tools/quest_release_base.py` lists among the files the release keeps, and which runs VrApi on OpenXR (a v6.16.4 headset
log shows the game in an OpenXR session). EchoFrame's runtime of the same kind binds the Steam Frame controllers
(`XR_VALVE_frame_controller_interaction`), the Valve Index controllers, the Khronos simple controller and Touch through
OpenXR ([EchoFrame README](https://github.com/heisthecat31/EchoFrame), "Steam Frame", at `7beb80bb`).

#1986 reports a Quest 2 whose hands are flipped with Touch Pro controllers (Touch Plus is not tested there).
`docs/design/2026-11-02-quest-hand-flip-toggle.md` places its fix in the VrApi layer, keyed on the interaction profile,
with an interim roll in the sentinel's tracking hook behind a player's menu toggle, marked `KNOWN-WRONG #1986`.

OpenXR's own model: an application suggests bindings once per interaction profile it supports
(`xrSuggestInteractionProfileBindings`), the runtime chooses among the suggested profiles for the device present, and
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
3. **Where the layer is in this repo's scope, it is ours, reconstructed in this repo** (ADR 0035), with EchoFrame and
   Ducky's reference combat build as cited references, never their sources taken whole (AGENTS.md "Guardrails"); #1737
   decides that scope (its section H15 keeps OpenXR runtime work out of scope today). A controller fix goes into that
   reconstruction. Until then, a fix made above the boundary for players who need it now is marked at its site with the
   literal `KNOWN-WRONG #<issue>`, naming the open issue that tracks moving it into the layer, so
   `tools/check_known_wrong.py` sees it.
4. **Each controller the layer binds is logged once per session**: the interaction profile path and the pose source used.

## Finding a violation

Scope: `src/quest` and its plugins, and the Python tools that patch the Quest APK; for C3, also `docs/design/`,
`docs/adr/` and the open issues. Out of scope: tests, `src/reconstruction`, the logging-only reads in
`hand_pose_log_hook.cpp`, and dev-only input drivers that replay inputs per hand and never ship to players (the autopilot,
`docs/adr/0025-quest-autopilot-route.md`).

- **C1.** Outside the VrApi layer, a read of the controller's identity (its model: a Touch Pro or Touch Plus bit, a
  capability flag that tells one controller model from another, the headset model used as a stand-in for the controller)
  that changes a pose, a button or a binding. Neither the hand a controller is held in nor an input device type that
  separates controllers from tracked hands is a controller's identity: code that addresses the left or right controller,
  or skips hands, is not a hit.
- **C2.** A per-controller table (button map, pose offset, handedness flip) in the sentinel, a plugin or a tool.
- **C3.** A design, an ADR or an open issue that places a controller fix above the boundary without the marker of rule 3.

## Consequences

- #1986's fix lands in the layer's binding for the controllers it affects, and every headset with those controllers gets
  it.
- Steam Frame's and Index's layouts are reconstructed into the layer's bindings, citing EchoFrame.

## Not claimed

That Touch Pro and Touch Plus report a distinct interaction profile path on a Quest 2 under Meta's OpenXR runtime; the
fix's key depends on that measurement.
