# ADR 0031: one step names the headset, and one table holds the settings each headset gets

Status: proposed. Applies to the Quest build (`src/quest` and its plugins) and the PC client (`src/runtime`). Works with
ADR 0032 (fixes test the condition, not the headset) and ADR 0033 (controllers).

## Context

The game itself knows two Quest answers. libr15 labels VrApi system property 0 value `0x140` as "Quest 2" and any other
value "quest" (`docs/adr/0022-render-config-log.md`, the headset row), and its script gates compare `CVR::HeadsetType()`
with 3, the Quest kind (`src/quest/plugins/q15t/src/headset_gates.h`).

The tree picks per-headset values in several places, each from its own signal:

- `src/quest/sentinel/texture_quality.h` (`HeadsetForStockSize`) guesses from the runtime's suggested eye width: 1680 or
  more is a Quest 3, anything else a Quest 2. Any other headset is put into one of the two.
- `src/quest/plugins/q15t/src/heaps.cpp` and `src/quest/plugins/q15t/src/refresh_rate.cpp` treat
  `ro.product.device == "hollywood"` as a Quest 2 (512 MiB system heap, 72 Hz) and every other value as the Quest 3 case.
- `src/quest/sentinel/render_config_hook.cpp`, `src/quest/sentinel/hand_pose_log_hook.cpp` and
  `src/quest/integration/crash_send_step.cpp` read `ro.product.model` for their log lines.

Players already run our builds on headsets outside that pair: #1765 is a crash report from Steam Frame's Lepton
Android container on 6.12.0, and EchoFrame runs the Quest APK on Steam Frame through an OpenXR runtime
([EchoFrame README](https://github.com/heisthecat31/EchoFrame), "Steam Frame", at `7beb80bb`).

Prior art for the same problem:

- Unreal Engine picks an Android device profile with match rules on the device model and the GPU family (equality or
  regex), checked in order; the first match wins, and generic `Android_Low`, `Android_Mid` and `Android_High` profiles are
  the fallback when no rule fits
  ([Epic: device profiles and scalability for Android](https://dev.epicgames.com/documentation/en-us/unreal-engine/customizing-device-profiles-and-scalability-in-unreal-engine-projects-for-android)).
- Mesa keeps its per-application and per-device settings in one file, Mesa's own `00-mesa-defaults.conf`, one entry per
  application with the options it gets
  ([mesa-commit: driconf workarounds for Teardown](https://lists.freedesktop.org/archives/mesa-commit/2021-April/116936.html)).
- OpenXR names the system the app runs on: `xrGetSystemProperties` fills `XrSystemProperties`, whose `vendorId` and
  `systemName` identify the vendor and the system
  ([Khronos: XrSystemProperties](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSystemProperties.html),
  [xrGetSystemProperties](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrGetSystemProperties.html)).

## Decision

1. **One identity step per platform**, run once at start, before any consumer reads a setting. Quest inputs:
   `ro.product.manufacturer`, `ro.product.model`, `ro.product.device` and, where an OpenXR runtime is present,
   `XrSystemProperties.systemName` and `vendorId`. PC inputs: the headset the render configuration already reads
   (`src/runtime/patch/render_config.h`). It writes one Info `headset_profile` line: every input, the rule that matched
   and the profile chosen.
2. **Ordered match rules, first match wins.** A rule names its inputs and their exact values; it never infers a headset
   from another quantity. The last rule of each platform is a generic profile, and the `headset_profile` line says when
   the generic profile was taken.
3. **A profile holds settings only**: values that any headset could run, chosen per headset because they suit it. Per-eye
   size for each texture-quality level, lobby and match refresh rates, heap sizes, the order of the data-root candidates.
   A fix for something that breaks is not a setting (ADR 0032). Controller handling is not a setting (ADR 0033).
4. **Consumers read the profile.** Outside the identity step no code reads the identity inputs to choose behaviour.
   Reading them only to write them into a log line is allowed.
5. **The rules and the table are plain code** with no platform header, as `texture_quality.h` is today, so one host test
   covers every rule and every row, and a row that applies to both builds is written once.
6. **A new headset is one new rule and one new row.** Each value in a row either cites the measurement behind it in the
   row's comment or is the generic profile's value.

## Finding a violation

The sweep that follows this ADR, and the sensor that replaces the sweep, look for these shapes. Scope: `src/quest`,
`src/runtime`, `src/core`, `sdk` and the Python tools that build or patch the Quest APK. Out of scope: tests, documents,
and `src/reconstruction` (the game's own code, transcribed).

- **R1.** A read of `ro.product.device`, `ro.product.model`, `ro.product.name` or `ro.product.manufacturer`, or of VrApi
  system property 0 (`vrapi_GetSystemPropertyInt` with id 0), whose value decides a branch outside the identity step. A
  read that only feeds a log line is not a violation.
- **R2.** A device codename or model name (`hollywood`, `eureka`, `seacliff`, `panther`, `Quest 2`, `Quest 3`,
  `Quest Pro`, `Steam Frame`, `Lepton`) compared in a condition.
- **R3.** A headset inferred from another quantity: an eye size, a refresh rate, a memory size, a count of driver
  extensions. `HeadsetForStockSize` is one instance.
- **R4.** A per-headset value (a size, a rate, a heap size) chosen at its consumer instead of read from the table.

## Consequences

- Steam Frame, Quest 3S and Quest Pro each become one rule and one row, not a new branch in every consumer.
- `texture_quality.h`'s width split goes; its size table becomes the rows' per-level sizes.
- q15t's two `hollywood` checks become rows when the module track moves those modules into the runtime.

## Not claimed

The identity values of any headset other than a Quest 2 (`hollywood`, `src/quest/plugins/q15t/src/heaps.cpp`). Which
property or file tells a Lepton container apart from a headset's own Android is not measured here; the first Steam
Frame rule needs that measurement.
