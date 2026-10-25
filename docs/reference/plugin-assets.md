# Plugin assets (plugin API v7)

A plugin carries its own levels and resources inside its own DLL and registers them with the runtime. The
runtime hands the engine a pointer into the plugin's mapped image, so there is no extraction step, no sidecar
file and no copy. One plugin, one file.

This page is the contract for plugin authors. The declarations are in `src/extension/plugin_interface.h`
(`NEVR_PLUGIN_API_VERSION` 7, `NevrAssetStatus`, `NevrRegisterAsset_fn`, `NevrDeregisterAssets_fn`), and
`plugins/common/include/plugin_assets.h` wraps the lookup.

## The two exports

The runtime module (deployed as `BugSplat64.dll`; older deployments name it `dbgcore.dll`) exports:

```c
int  NEVR_RegisterAsset(uint64_t type_hash, uint64_t name_hash,
                        const void* data, uint64_t size, const char* label);
void NEVR_DeregisterAssets(const void* data_start, const void* data_end);
```

| Argument | Meaning |
| --- | --- |
| `type_hash` | The engine's 64-bit symbol hash of the resource type. The same value `NEVR_RegisterResourceOverride` takes. |
| `name_hash` | The 64-bit symbol hash of the resource name. A level's name is the hash of the level's name; give it a name no shipped level has. |
| `data`, `size` | Bytes in the plugin image, `1..0xFFFFFFFF`. Must stay valid until deregistered: a `const` array or a resource section, never the heap. |
| `label` | Short name for the log and for the login report. Copied. |

`NEVR_RegisterAsset` returns an `NevrAssetStatus`:

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `NEVR_ASSET_OK` | Registered. |
| 1 | `NEVR_ASSET_BAD_ARGUMENT` | Null or empty data, a zero type or name, or a size above `0xFFFFFFFF`. |
| 2 | `NEVR_ASSET_DUPLICATE` | That (type, name) is already registered. The first registration is kept. |
| 3 | `NEVR_ASSET_ALREADY_LOADED` | The engine has already loaded that (type, name). Too late to change what it reads. |

Every refusal is also a `Warning` in the log: `[NEVR.ASSET] register refused label=... reason=...`.

## Order and lifetime

- Register from `NvrPluginInit` / `NvrPluginInitEx`. That is before the engine's first load of the name.
  Registering after the engine has loaded a name is refused (`NEVR_ASSET_ALREADY_LOADED`); it is never ignored.
- Deregister from `NvrPluginShutdown` with the plugin's own image range:
  `NEVR_DeregisterAssets(image_base, image_base + SizeOfImage)`.
- The runtime also drops every asset inside a plugin's image before it frees that image
  (`[NEVR.ASSET] released N asset(s) still registered by a plugin at unload`), so a plugin that forgets cannot
  leave the engine holding a pointer into freed memory.

## What is served

The runtime serves a registered (type, name) through the engine's own resource manager. A load of that name
runs the engine's normal completion and initialization with your pointer as the resource's data, so the
engine's own parsing and error handling apply to your bytes.

- **Single-buffer resources only.** A resource type that asks for a second buffer is not served; the engine's
  own error follows and the runtime logs `[NEVR.ASSET] not served ... reason=buffer_mask_0x3`.
- **Loads through the resource manager** (`LoadResource`, the path levels and most resources take) are served.
  The engine's texture mip streaming and archive loader read packages directly; a registered name arriving
  there is logged as `registered asset requested through a loader path that reads packages` and the read fails.
- **Needs the engine's task scheduler.** Without it the engine initializes a resource inline on the completing
  thread; the runtime declines (`reason=no_task_scheduler`) rather than risk a lock inversion.
- A level is a graph of resources. Everything the level references that the game does not ship has to be
  registered too, each under its own (type, name); references to shipped resources resolve normally.

## What the game service sees

The login carries the registered assets next to the plugin list, so the game service knows which levels this
game client can load:

```json
"nevr_plugins": [ { "name": "maps", "loaded": true, "ver": "1.0.0", "api": 6, "caps": 4 } ],
"nevr_assets":  [ { "type": "0xa71a14664bd4e047", "name": "0x1111222233334444", "label": "ruins", "bytes": 1048576 } ]
```

`nevr_assets` is absent when no plugin registered anything, so a login with no asset-carrying plugin is
byte-identical to before.

## How it works, for reviewers

`CResourceManager` keys everything by (type symbol, name symbol). `0x140fa3220` submits a load: it records the
buffers the resource expects (its own `vfunc[0x48]` per buffer index) in the object's pending mask (`+0x68`),
then queues one package read per buffer through `0x140fa1fe0` with completion callback `0x140fa16d0`. For a
registered key the runtime's hook on `0x140fa3220` does the part before the queueing, then calls `0x140fa16d0`
itself with `{status 4, resource, buffer 0, your pointer, size}`: the engine's completion stores the buffer,
clears the pending bit and schedules initialization. "Already loaded" is the engine's own predicate
`0x140fa3410`, the call the manifest merge (`0x140fa09f0`) uses before it refuses to apply a package.

All addresses are echovr.exe `34.4.631547` (ImageBase `0x140000000`) and were read from the decompilation.
The engine's package manifest format, which a plugin does not need, is in `package-manifest-format.md`.

## Not covered yet

- Quest. The PC path is implemented; the Quest build has its own addresses and is a follow-up.
- Multi-buffer resource types.
- Texture mip streaming and archive loads of a plugin-carried name.
