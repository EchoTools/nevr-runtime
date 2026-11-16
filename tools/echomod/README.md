# EchoVR Combat Arena Mod

Adds custom combat levels to EchoVR with full weapon support (guns, grenades, tactical abilities).

## What's in this folder

```
TurboAHHHH.dll          Single merged hook DLL (inject into running game)
setuparchive.py         Builds the patched game archive (run once)
clone_frisbee.py        Arena clone level builder (used by setuparchive)
clone_combat.py         Combat arena level builder (used by setuparchive)
rad_archive_tool.py     Archive extract/repack tool (used by setuparchive)
hash_lookup.json        Resource name/hash lookup table
radhash.py              Hash utility
plugins_override_generator.py  Makes the PLUGINS-card override set from your own install (see below)
plugins_override_patches.json  Its edit list: hashes and our own bytes, no game data
echomod/                Resource parsing library
```

## Requirements

- EchoVR installed (Oculus version, Dec 2024 patch)
- Python 3.12+ with `zstandard` package
- A DLL injector (e.g. [Xenos](https://github.com/DarthTon/Xenos))

## Setup (one-time)

### 1. Install Python dependency

```
pip install zstandard
```

### 2. Build the patched archive

```
python setuparchive.py "C:\Program Files\Oculus\Software\Software\ready-at-dawn-echo-arena\_data\5932408047"
```

Replace the path with your actual game data folder. This extracts the game archive, creates the custom levels, and repacks everything into `patched_output\`.

### 3. Install the patched archive

**Back up the originals first!**

Copy from `patched_output\rad15\win10\` to your game's `_data\5932408047\rad15\win10\`:
Or launch your game using the `-datadir` flag ex `echovr.exe -datadir "D:\turbo\patched_output"`

### 4. Place the DLL

Create `bin\win10\plugins\` folder, If you have a dbgcore thats not a mod loader (ex: the relay dll), throw it into the plugins folder now it will still load at runtime
Copy `dbgcore.dll` to your games `bin\win10\` folder, this acts as a mod loader. 
Copy `TurboAHHHH.dll` to the `bin\win10\plugins\` folder.

## Running

1. Launch EchoVR normally or with the `-datadir` flag shown above.
2. Load into a server using `mpl_arenacombat`.
3. The mod auto-activates when the arenacombat level loads
4. If at any point you find yourself in the arena chassis, use f9 to swap back to combat.

## What the DLL does

- **Script patching** - Patches the streaming script so combat sub-levels load correctly
- **Mode patching** - Forces combat mode checks and chassis selection
- **Combat patching** - Handles body swap events, weapon enable, respawn teleport, and F9 toggle
- **Clone visibility** - Makes cloned actor models render correctly
- **Network replication** - Sends combat mode events to other players in the session

All hooks activate only when needed. On non-combat levels the DLL stays dormant.

## The PLUGINS-card override set (#1857)

The main-menu PLUGINS card is 13 resource override files for `mnu_master`, each a stock resource with the edits that add
the card. The repository holds no game bytes. `plugins_override_patches.json` holds offsets, lengths, hashes and bytes of
our own, and nothing else: per file, the SHA-256 of the stock resource it is built from, the SHA-256 of the result, and the
edits (copy a range of the stock resource or of another file's stock resource, a game symbol written as its name, or a put
of bytes of our own). A put of 8 bytes or more that can carry data is one of the values the table lists by name in `ours`,
each with its reason (the new actor's hash, the card's component id, its pose, its text); every other put is a count, a size
or a header word shorter than 8 bytes. A put never carries a run of stock: `generate` and `derive` check every put against
every stock resource of the level in your install and stop with `PutCarriesStock` if one does. Make the set from
your own install, into an empty folder that you then copy to `_overrides` next to `echovr.exe`:

```
python plugins_override_generator.py generate --data-dir "<install>\_data\5932408047" --out <empty folder>
```

`--data-dir` can also be the environment variable `NEVR_GAME_DATA_DIR`; there is no default. The install is only read, and
an `--out` inside it is refused. A resource that is not the one the edit list was made from (another game version) stops
the run with a `StockMismatch` line naming the type and both hashes, and nothing is written.
`python plugins_override_generator.py derive --data-dir <install data> --reference <finished set> --out plugins_override_patches.json`
is the maintainer's way to rebuild the edit list from a finished set and their own install when the card's design changes;
it refuses a put that is not listed in `ours` or that carries a stock run.

## Troubleshooting

**Game crashes on level load:**
Restore your backed-up original archive files and re-run `setuparchive.py`.

**Sub-levels don't load / no weapons:**
Check `scriptpatch.log` in the game's `bin\win10\` folder.

**Combat mode doesn't activate:**
Check `combatpatch.log` and `leveldetect.log`. Should show "mpl_arenacombat detected".

**To restore the original game:**
Replace the `manifests\` and `packages\` files with your backups. Remove or stop injecting the DLL.

## Log files

The DLL writes logs to the game's working directory (`bin\win10\`):

- `echovr_combat_mod.log` - Main mod init log
- `combatpatch.log` - Combat events, respawn, F9 toggle
- `scriptpatch.log` - Script DLL patching status
- `modepatch.log` - Mode/chassis hook status
- `startvisible.log` - Model visibility hook status
- `leveldetect.log` - Level load detection
