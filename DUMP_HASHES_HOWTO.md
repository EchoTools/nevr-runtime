# How to Dump Message Type Hashes

## Quick Start

1. **Copy the hooked DLL:**
```bash
cp build/bin/gamepatches.dll /path/to/echovr/bin/Win64/dbgcore.dll
```

2. **Run the game in headless mode:**
```bash
cd /path/to/echovr/bin/Win64
wine echovr.exe -headless -server 2>&1 | tee hashes.log
```

3. **Look for these lines:**
```
[MSG_REGISTRY] 0x59e4c5ea6e01083b = "ActualMessageName" (flags=0x1)
```

## What Got Hooked

- **CMatSym_Hash** (0x140107f80) - Captures message type strings
- **SMatSymData_HashA** (0x140107fd0) - Captures finalized hashes  
- **sns_registry_insert_sorted** (0x140f88080) - **THE GOLDEN HOOK** - Direct hash→name mapping

## Why DLL Injection Works

`dbgcore.dll` is loaded by Windows automatically at process startup (before main()). Our hooks install in `DllMain(DLL_PROCESS_ATTACH)` which runs before `sns_registry_insert_sorted` is called.

## Output Format

```
[MSG_REGISTRY] 0x{hash} = "{name}" (flags=0x{flags})
```

Example:
```
[MSG_REGISTRY] 0x2050928ad5a3b7d4 = "SR15NetSensorPing" (flags=0x0)
[MSG_REGISTRY] 0x59e4c5ea6e01083b = "UnknownType" (flags=0x1)
```

## Backup

Original DLL backed up at: `build/bin/gamepatches.dll.backup-20260213-012102`

To revert:
```bash
rm /path/to/echovr/bin/Win64/dbgcore.dll
```
