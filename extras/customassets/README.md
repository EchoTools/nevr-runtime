# CustomAssets DLL - Custom Loadout Asset Loader

## Overview

The CustomAssets DLL (`dbgcustomassets.dll`) enables loading custom cosmetic assets for EchoVR loadouts. It intercepts the game's asset loading system and allows players to use custom IDs (prefixed with `custom_`) that load assets from HTTP URLs with local caching.

## Features

- **HTTP Asset Loading**: Download custom assets from any HTTP/HTTPS URL
- **Local Caching**: Assets are cached locally with HTTP ETag/Last-Modified support
- **Client & Server Configuration**: Supports both client-side config files and server-provided URLs
- **Fallback to Defaults**: Gracefully falls back to default assets if custom assets fail to load
- **JSON Asset Format**: Simple JSON-based asset bundles with base64-encoded or externally-referenced resources

## Architecture

### Components

- **ConfigManager**: Loads client-side configuration and manages server-provided asset mappings
- **CacheManager**: Manages local cache with metadata (ETags, timestamps)
- **HttpClient**: Performs HTTP requests with retry logic and cache validation
- **AssetInterceptor**: Hooks into `Loadout_ResolveDataFromId` (0x1404f37a0) to intercept custom asset requests
- **AssetConverter**: Parses JSON asset bundles and converts them to game format (WIP)

### Hook Points

The DLL hooks the following game function:
- `Loadout_ResolveDataFromId` at address `0x1404f37a0` (261 call sites in binary)

## Configuration

### Client Configuration

Create a configuration file at: `%APPDATA%/EchoVR/CustomAssets/config.json`

```json
{
  "version": "1.0",
  "enabled": true,
  "cache_directory": null,
  "assets": {
    "custom_sprockee_emblem": {
      "url": "https://example.com/assets/sprockee_emblem.json",
      "override_server": true,
      "etag": ""
    }
  },
  "http_settings": {
    "timeout_ms": 30000,
    "max_retries": 3,
    "user_agent": "EchoVR-CustomAssets/1.0"
  }
}
```

### Asset Bundle Format

Custom assets are JSON files with the following structure:

```json
{
  "version": "1.0",
  "asset_id": "custom_sprockee_emblem",
  "asset_type": "decal",
  "metadata": {
    "name": "Sprockee's Custom Emblem",
    "author": "Sprockee",
    "description": "A custom team emblem"
  },
  "resources": [
    {
      "type": "texture",
      "slot": "diffuse",
      "format": "png",
      "data": "base64_encoded_png_data...",
      "width": 1024,
      "height": 1024
    }
  ],
  "cache_control": {
    "etag": "\"abc123...\"",
    "max_age": 86400
  }
}
```

## Cache Location

Cached assets are stored in: `%LOCALAPPDATA%/EchoVR/CustomAssets/cache/`

- `assets/`: Downloaded JSON asset bundles
- `converted/`: Converted game-format assets (future)
- `*.meta`: Cache metadata files with ETags and timestamps

## Build Requirements

- C++17 compiler
- CMake 3.20+
- WinHTTP library (Windows SDK)
- MinHook (vendored in `extern/`)
- Third-party libraries:
  - nlohmann/json (single-header, included)
  - stb_image (single-header, included)
  - base64 codec (custom implementation, included)

## Usage

1. Build the project: `cmake --build build --target CustomAssets`
2. Copy `CustomAssets.dll` to `dist/dbgcustomassets.dll`
3. Load the DLL alongside other NEVR DLLs
4. Create a configuration file with custom asset URLs
5. Use custom asset IDs in loadout configurations (e.g., `"decal": "custom_sprockee_emblem"`)

## Current Limitations

- **Asset Conversion Not Implemented**: The JSON-to-game-format conversion is stubbed out and returns `nullptr`, causing fallback to default assets
- **Hash Computation**: Uses FNV-1a hash which may not match EchoVR's exact symbol hash algorithm
- **No Validation**: Asset bundles are not cryptographically validated
- **Windows Only**: Uses Windows-specific APIs (WinHTTP, SHGetKnownFolderPath)

## Future Work

1. **Reverse Engineer Asset Format**: Analyze game's internal asset structures for textures, meshes, materials
2. **Implement Asset Conversion**: Convert PNG/JPEG textures and OBJ/GLTF meshes to game format
3. **Hash Algorithm**: Verify and match EchoVR's exact symbol hash implementation
4. **Cross-Platform**: Abstract Windows-specific code for Linux support
5. **Asset Validation**: Add cryptographic signatures for asset bundles
6. **Server Integration**: Extend protobuf definitions to include `CustomAssetMapping` in `GameServerSaveLoadoutMessage`

## Debug Logging

All operations are logged through the EchoVR logging system with the `[CustomAssets]` prefix:

- Initialization and shutdown
- Config loading
- Cache operations
- HTTP requests and responses
- Asset interception and conversion attempts

Look for these logs in the game's console or log file.

## License

Part of the NEVR Server project. See main project LICENSE for details.
