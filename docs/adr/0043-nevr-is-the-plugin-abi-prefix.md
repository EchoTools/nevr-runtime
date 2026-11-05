# ADR 0043: `Nevr` is the prefix of the plugin and module C ABI, and `Nvr` is a deprecated alias

Status: proposed. Applies to `sdk/include/nevr/`, `src/extension/`, both plugin loaders (`src/runtime/ext/plugin_loader.cpp`,
`src/quest/plugin/plugin_loader.cpp`) and every plugin built against them. Amends `docs/standards/naming.md`.

## Context

The C ABI that plugins and statically linked modules compile against spells its prefix `Nvr`: `NvrPluginInfo`,
`NvrGameContext`, `NvrPluginInit`, `NvrModuleContext` and the rest. Everything else in the project spells the name
`NEVR`, `Nevr` or `nevr` (`docs/standards/naming.md`). No record gives a reason for the shorter prefix.

The set was frozen, not renamed, because a rename of the C ABI has a cost the source cannot see. The host resolves each
plugin entry point by name (`GetProcAddress` on PC, `dlsym` on Quest), so a plugin already built exports the old names, and a
host that looks only for new ones refuses it. A frozen set kept two prefixes in the published surface: new interfaces were
`Nevr*`, the lifecycle types and entry points stayed `Nvr*`, and three SDK headers (`plugin_interface.h`,
`plugin_logger.h`, `pc/plugin_assets.h`) mixed both in one signature.

## Decision

1. **`Nevr` is the prefix.** Every type, typedef, function-pointer typedef, function and exported entry point of the
   plugin and module C ABI is `Nevr*`; macros stay `NEVR_*`. A plugin exports `NevrPluginGetInfo`, `NevrPluginGetApiVersion`,
   `NevrPluginGetCapabilities`, `NevrPluginInit`, `NevrPluginInitEx`, `NevrPluginOnFrame`, `NevrPluginOnGameStateChange` and
   `NevrPluginShutdown`.
2. **`Nvr` stays as a deprecated alias, not a second name.** The SDK headers declare each old type name as a
   `[[deprecated("use Nevr...")]]` alias of the new one (the same type, so no layout changes); the one old function,
   `NvrModuleApiVersionSupported`, is a deprecated wrapper. Code written against the old SDK still compiles, with a warning.
3. **Both loaders accept both export spellings.** For each entry point the loader resolves the `Nevr*` export first and
   falls back to the `Nvr*` export. A plugin that loaded through at least one deprecated export gets one Info line,
   `[NEVR.PLUGIN] plugin=<name> file=<file> loaded with deprecated Nvr exports (...)`, so an operator can see which
   plugins still need a rebuild. A plugin exporting both spellings runs the `Nevr*` ones.
4. **Nothing in the tree is written against `Nvr` any more.** The in-repo plugins, fixtures and callers use `Nevr*`.
   `tools/tests/test_naming.py` allows `Nvr` only as the deprecated aliases in the two headers and in the named places that
   prove the old names still load.

## Alternatives

- **Keep the `Nvr*` set frozen.** No plugin is affected, and nothing changes in the tree. It leaves two prefixes in the
  published ABI for good, with the lifecycle types (the surface plugin authors touch first) spelled the odd way, and every
  new interface that joins one of the old structs (a new `Nevr*` function-pointer type in an `Nvr*` context) mixes them again.
- **Hard rename, no alias and no fallback.** One prefix and no compatibility code. Every plugin already built, including
  any a third party published and no one here can rebuild, fails to load and loads again only after its author ships a new build.

## Reason

A prefix is chosen once and every plugin author reads it. The alias and the fallback cost a few lines each and let the
rename land without stranding a built plugin; the Info line gives the evidence for when the cost can stop.

## Removing the aliases and the fallback

Remove both when no plugin that ships or that a supported config names still exports `Nvr*`. The evidence is the loader's own
Info line: a release cycle in which no run reports `loaded with deprecated Nvr exports` for a plugin the project distributes
or knows of. The removal is one change: the aliases in the two headers, the fallback in both loaders, and
`DEPRECATED_NVR_USES` in `tools/tests/test_naming.py`.

## Not claimed

Which third-party plugins still export `Nvr*`: the tree has no list of them. `NvrContentOverlay` is a third party's plugin
name and file name, and keeps its spelling.
