# Universal JSON Override Hook

## Overview

This hook provides a powerful mechanism to override almost any JSON-based configuration file in the game without modifying the original game assets. It works by intercepting the game's file loading function for JSON files and applying user-defined patches on the fly.

This allows for rapid prototyping, balance changes, re-enabling disabled content, and extensive modding.

## How It Works

1.  **Intercepts File Loading:** The hook targets `CJson_LoadFromPath` (`0x1405f0990`), the game's primary function for loading JSON files from disk.
2.  **Checks for Overrides:** It checks a master configuration file, `config/json_overrides.json`, to see if a patch exists for the file being loaded (e.g., `mp_equipment.json`).
3.  **Merges on the Fly:** If an override is found, the hook:
    a.  Reads the original game file into memory.
    b.  Parses both the original JSON and the override JSON.
    c.  Performs a "merge patch" operation, where the values in your override replace the values in the original.
    d.  Serializes the new, merged JSON back into a string.
4.  **Injects into Parser:** The hook then calls the game's internal JSON *parser* function (`CJson_Parse` at `0x1405f0bd0`) with the modified JSON buffer, tricking the game into using your configuration.
5.  **Passes Through:** If no override is found for a given file, the hook does nothing and calls the original file loading function, ensuring normal game behavior.

## How to Use

1.  **Open the Master Override File:**
    - `nevr-server/dbghooks/config/json_overrides.json`

2.  **Add an Entry:**
    - The top-level keys in this file are the **filenames** of the JSON files you want to override.
    - The value for each key is a JSON object containing the data you want to change.

3.  **Follow the Original Structure:**
    - The structure of your override object must mirror the structure of the original JSON file. You only need to include the keys and values you wish to change.

### Example: Re-enabling Disabled Weapons

The default `json_overrides.json` is configured to re-enable the `magnum`, `smg`, `chain`, and `rifle`.

**File:** `config/json_overrides.json`
```json
{
  "mp_equipment.json": {
    "gear_table": {
      "weapons": [
        // This re-enables the 4 disabled weapons by replacing the original list.
        // NOTE: This is a full replacement, not a merge, because it's a JSON array.
        { "name": "assault", "display_name": "PULSAR" },
        { "name": "blaster", "display_name": "NOVA" },
        { "name": "scout", "display_name": "COMET" },
        { "name": "rocket", "display_name": "METEOR" },
        { "name": "magnum", "display_name": "PROTOSTAR" },
        { "name": "smg", "display_name": "QUARK" },
        { "name": "chain", "display_name": "BLAZAR" },
        { "name": "rifle", "display_name": "STARBURST" }
      ]
    }
  }
}
```

### Example: Changing a Weapon's Damage

To change the `assault` rifle's damage from `9.0` to `11.0`:

**File:** `config/json_overrides.json`
```json
{
  "mp_weapon_settings.json": {
    "weapon_properties": {
      "assault": {
        "projectile_damage": 11.0
      }
    }
  }
}
```
*You don't need to copy the entire `assault` object, just the field you are changing.*

## Technical Details

- **Hook Target:** `CJson_LoadFromPath` @ `0x1405f0990`
- **Parser Target:** `CJson_Parse` @ `0x1405f0bd0`
- **Library:** `nlohmann/json` for merging logic.
- **Priority:** The hook loads with priority `5` in `autoload.yaml` to ensure it runs before other hooks that might depend on the modified config values.

## Status

- ✅ Verified working.
- ✅ Thread-safe.
- ✅ Safe unload/reload.
- ✅ Minimal performance impact (only affects file loading, not gameplay loops).
