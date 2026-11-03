# ADR 0032: a workaround turns on when the condition it fixes is present, never because a headset's name matches

Status: proposed. Applies to every fix this repo makes for something that breaks: a crash, a missing feature, a driver's
behaviour. Settings chosen per headset are ADR 0031.

## Context

Two fixes in and around this tree already test the condition instead of the device:

- `vk_ext_window` (`docs/adr/0014-quest-vulkan-extension-window.md`, `src/quest/sentinel/vk_ext_window.cpp`) rewrites the
  game's extension list only when a name the game requires lies past the 128 entries the game reads. A Quest 2's list
  fits, and the same build passes its call through untouched.
- EchoFrame's VrApi-on-OpenXR runtime leaves out the Vulkan extensions a driver lacks, and its Steam Frame build gives the
  game's uploader the render queue only "when its own reservation fails; with two queues (Quest) it still gets its own"
  ([EchoFrame README](https://github.com/heisthecat31/EchoFrame), "Steam Frame", at `7beb80bb`).

The same README lists the Steam Frame build's three changes to libr15's `CGS::Initialize` for the Mesa driver
(`FRAME_CODE_PATCHES` in its `patcher/patch.py`): ask for Vulkan 1.0 instead of version 1, enable every device
extension the runtime asks for, and fall back to the render queue for the uploader. They are selected by a build flag
(`patch.py --frame`). Each has a condition the running process can observe: the API version the instance accepts, the
extension list, the number of queues in the graphics family.

Prior art: MDN's guidance for the web is to test for the feature instead of identifying the browser, because parsing the
user agent "is very hard to do reliably, and is often a cause of bugs"
([MDN: browser detection using the user agent](https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/Browser_detection_using_the_user_agent)).
Mesa's per-application entries name the application they fix, but each sets a driver option that describes the
condition it handles, such as allowing implicit derivatives after a discard
([mesa-commit: workaround for Golf With Friends](https://lists.freedesktop.org/archives/mesa-commit/2021-April/117766.html)).

## Decision

1. **A workaround tests its condition at the point it matters**: a driver's answer, a count, a capability bit, an error
   return. The headset profile (ADR 0031) never turns a workaround on.
2. **When the condition cannot be observed before the failure**, the workaround says why in its code comment and is
   turned on by a profile flag named for the condition (for example `single_graphics_queue`), never for the headset. The
   row that sets the flag cites the measurement.
3. **Each workaround logs its decision once**, the first time it decides a case it targets, whether it acted or passed
   through, with the condition it observed. A call the workaround does not target (an unrelated permission, a path outside
   its table, a signal it does not guard) passing through is not a decision. `vk_ext_window`'s `action=reordered` and `action=passthrough` lines are the model
   (`docs/standards/logging.md`, Rule 3: silence is not success).
4. **A workaround costs nothing measurable where its condition is absent**: the check, then the original path.
5. **No build variant selects a fix by target device.** One APK carries every workaround; the condition decides at run
   time.

## Finding a violation

Scope: `src/quest`, `src/runtime`, `src/core`, `sdk`, the `justfile` recipes and the Python tools that build or patch
the Quest APK. Out of scope: tests, documents, `src/reconstruction`.

- **W1.** A fix for a failure (a crash guard, a Vulkan or audio change, a skipped or redirected call) gated on headset
  identity: any shape of ADR 0031 R1 to R3, or a profile field named for a headset.
- **W2.** A build flag, preset, recipe parameter or variant that selects a fix by target device.
- **W3.** A workaround that writes no line when it first decides a case it targets, on either branch: it acted, or it
  declined (for example, a replacement it could not make).

## Consequences

- The Steam Frame fixes in EchoFrame's `FRAME_CODE_PATCHES` are rebuilt as hooks that each test their condition, and a
  Quest gets the same APK.
- A headset nobody has measured still gets every fix whose condition it shows.

## Not claimed

That the three Mesa conditions above can each be observed before the failure they cause: the queue count and the
extension list are queried before device creation, but whether a rejected API version can be retried without ending the
process is not measured here.
