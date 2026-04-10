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
Copy `TurboAHHHH.dll` to the newly or previously created `bin\win10\plugins\` folder.

## Running

1. Launch EchoVR normally (or with launch args covered previously)
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
