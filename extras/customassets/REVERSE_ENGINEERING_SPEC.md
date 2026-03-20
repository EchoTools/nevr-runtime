# Reverse Engineering Specification: EchoVR Asset Format

## Project Context

We have implemented a custom asset loading system for EchoVR that allows players to use custom cosmetic items (decals, emotes, patterns, etc.) loaded from HTTP URLs. The infrastructure is **100% complete** and working, but we need to understand the game's internal asset format to convert our JSON-based assets into the binary format the game expects.

**Current Status**: 
- ✅ HTTP downloading with caching (ETag support)
- ✅ JSON parsing with base64/URL image loading
- ✅ Hook into `Loadout_ResolveDataFromId` (intercepts asset loading at `0x1404f37a0`)
- ❌ **Asset format conversion (BLOCKER)** - returns `nullptr`, needs implementation

## Objective

Document the binary format used by EchoVR for cosmetic assets so we can convert standard formats (PNG, JPEG, OBJ, glTF) into the game's native format.

---

## Target Binary

**File**: `r14.exe` (EchoVR game executable)
**Platform**: Windows x64 PE32+ executable
**Language**: C++ (likely MSVC compiled)
**Graphics API**: DirectX 11

**Binary Location**: Installed with EchoVR game (Steam or Oculus version)
**Typical Path**: `C:\Program Files\Oculus\Software\ready-at-dawn-echo-arena\bin\win10\r14.exe`

---

## Key Functions & Addresses

### 1. Primary Hook Point (CONFIRMED)
```cpp
void* Loadout_ResolveDataFromId(void* context, uint64_t cosmetic_id)
```
- **Address**: `0x1404f37a0` (r14.exe base + offset)
- **Purpose**: Main entry point for asset resolution
- **Called**: 261 times throughout binary (heavy usage)
- **Parameters**:
  - `context` - Unknown struct pointer (likely game state/session)
  - `cosmetic_id` - FNV-1a 64-bit hash of asset ID string
- **Returns**: `void*` pointer to loaded asset data (format unknown)

### 2. Asset Loading Pipeline (NEEDS INVESTIGATION)

We need to trace the execution flow from `Loadout_ResolveDataFromId` to understand:

1. **What struct does it return?** (memory layout, size, fields)
2. **How are textures stored?** (compression, mipmaps, format)
3. **How are meshes stored?** (vertex layout, indices, materials)
4. **Are there asset type discriminators?** (texture vs mesh vs animation)

---

## Asset Types to Document

### Priority 1: Textures (Decals, Patterns, Emotes)
Most common asset type, used for:
- Player decals (chest emblems, etc.)
- Disc patterns
- Emote icons
- UI elements in loadout system

**What We Need**:
- Binary structure of texture header
- Pixel format (RGB, RGBA, BC1/DXT1, BC3/DXT5, etc.)
- Dimensions (width, height, mipmaps)
- Data layout (interleaved, planar, compressed blocks)
- DirectX resource view setup (if any)

**Example Asset IDs** (for testing):
- `rdy_decal_logo` (Ready At Dawn logo decal)
- `rdy_pattern_hexagon` (hexagon disc pattern)

### Priority 2: Meshes (3D Models)
Used for:
- Custom emote animations (character poses)
- Potentially custom disc models (future feature)

**What We Need**:
- Vertex buffer layout (position, normal, UV, color)
- Index buffer format (16-bit or 32-bit)
- Submesh/material groupings
- Bone/skeleton data (if rigged)

### Priority 3: Animations (Lower Priority)
- Keyframe data
- Bone transforms
- Timing/interpolation

---

## Research Methodology

### Recommended Tools
1. **Ghidra** (free, excellent decompiler) - https://ghidra-sre.org/
2. **IDA Pro** (commercial, industry standard)
3. **x64dbg** (debugger for runtime analysis)
4. **Cheat Engine** (memory scanning/poking)
5. **RenderDoc** (DirectX frame capture) - https://renderdoc.org/

### Approach 1: Static Analysis (Preferred)

1. **Start at `0x1404f37a0`** - Decompile `Loadout_ResolveDataFromId`
2. **Follow the returns** - What does the returned pointer reference?
3. **Find texture upload calls** - Look for DirectX calls:
   - `ID3D11Device::CreateTexture2D`
   - `ID3D11DeviceContext::UpdateSubresource`
   - `D3DX11CreateTextureFromMemory` (legacy)
4. **Trace back to data** - What format is the source data?
5. **Find asset file readers** - Look for:
   - `fopen`, `CreateFile`, `ReadFile`
   - Memory mapped file operations
   - Embedded resource extraction

**Search Strings** (useful for finding related code):
- "asset", "cosmetic", "loadout", "decal", "pattern", "emote"
- "texture", "mesh", "model", "material"
- ".dds", ".png", ".tga" (possible file format clues)
- "cdn.echo.games" (asset CDN URL - may reveal download/parse code)

### Approach 2: Runtime Analysis

1. **Set breakpoint at `0x1404f37a0`**
2. **Load a known asset** (e.g., default decal in game)
3. **Inspect returned pointer** - Dump memory, identify structure
4. **Follow DirectX calls** - Use RenderDoc to capture frame with asset
5. **Compare before/after** - Memory diff between loading different assets

### Approach 3: Asset File Inspection

EchoVR likely caches assets locally after downloading from CDN:
- **Potential Cache Locations**:
  - `%LOCALAPPDATA%\rad\`
  - `%APPDATA%\EchoVR\`
  - Game installation `\assets\` or `\cache\` directory

**If cache files exist**:
- Examine file headers (magic numbers, version)
- Compare multiple asset files (find common structure)
- Try hex-editing known asset (see if changes appear in-game)

---

## Expected Output

Please provide documentation in the following format:

### 1. Asset Structure Definition (C/C++)

```cpp
// Example (this is what we need you to determine)
struct EchoVR_TextureAsset {
    uint32_t magic;           // File signature (e.g., 'EVRT' for EchoVR Texture)
    uint32_t version;         // Format version
    uint32_t width;           // Texture width in pixels
    uint32_t height;          // Texture height in pixels
    uint32_t mipmap_count;    // Number of mipmap levels
    uint32_t format;          // Pixel format (DXGI_FORMAT enum)
    uint32_t data_size;       // Size of pixel data in bytes
    uint8_t  data[];          // Pixel data (variable length)
};
```

### 2. Conversion Pseudo-code

```cpp
// Example of what conversion logic should look like
void* ConvertPNGToEchoVRTexture(const uint8_t* png_data, size_t png_size) {
    // 1. Decode PNG to raw RGBA
    // 2. Generate mipmaps (if needed)
    // 3. Compress to BC3/DXT5 (if needed)
    // 4. Build EchoVR_TextureAsset struct
    // 5. Return pointer to struct
}
```

### 3. Key Findings Document

- **Memory Layout**: Offsets, sizes, alignment requirements
- **Pixel Format**: DXGI_FORMAT value or equivalent
- **Compression**: None, DXT1/5, BC1/3/7, custom?
- **Mipmaps**: Required? Auto-generated? Pre-generated?
- **Dimensions**: Power-of-2 required? Max size? Aspect ratio limits?
- **Color Space**: sRGB, linear, gamma-corrected?

### 4. Validation Test Case

Provide a minimal example:
```
Input: 64x64 RGBA PNG (solid red #FF0000)
Output: Binary hex dump of expected EchoVR asset format
Expected Size: X bytes
```

---

## Existing Research & Documentation

We have extensive documentation on the loadout system from prior reverse engineering work:

**Key Documents** (available in `/home/andrew/src/evr-reconstruction/docs/features/`):
1. **`cosmetic_loadout_system.md`** (460 lines)
   - Complete loadout architecture
   - Asset ID hash computation (FNV-1a confirmed)
   - Session management and networking
   
2. **`asset_cdn_system.md`** (320 lines)
   - CDN download pipeline
   - HTTP request patterns
   - Cache behavior

**Relevant Memory Structures** (already documented):
```cpp
// From prior RE work - loadout structure
namespace EchoVR {
  struct CosmeticLoadout {
    Symbol decal;           // 0x00 - FNV-1a hash
    Symbol pattern;         // 0x08
    Symbol emote;           // 0x10
    // ... (full structure available)
  };
  
  struct Symbol {
    uint64_t hash;          // FNV-1a 64-bit
  };
}
```

**CDN Asset URL Pattern** (known):
```
https://cdn.echo.games/assets/{asset_id}_{hash}.asset
```

---

## Integration Points

Once you provide the format specification, we will implement:

**File**: `CustomAssets/asset_converter.cpp`
**Functions to implement**:
```cpp
namespace CustomAssets {
  class AssetConverter {
    // Parse our JSON asset bundle format
    AssetBundle ParseAssetBundle(const std::string& json);
    
    // Convert to game format (YOUR SPEC GOES HERE)
    void* ConvertToGameFormat(const AssetBundle& bundle);
    
    // Type-specific converters
    void* ConvertTexture(const TextureResource& resource);  // TODO
    void* ConvertMesh(const MeshResource& resource);        // TODO
  };
}
```

**Our Asset Bundle Format** (already implemented):
```json
{
  "version": "1.0",
  "asset_id": "custom_my_decal",
  "type": "texture",
  "resources": [
    {
      "type": "texture",
      "slot": "diffuse",
      "format": "png",
      "data": "base64_encoded_png_data...",
      "width": 512,
      "height": 512
    }
  ]
}
```

We will:
1. Take your struct definition
2. Implement conversion from PNG/JPEG → game format
3. Allocate memory and populate struct fields
4. Return pointer to game's `Loadout_ResolveDataFromId` hook

---

## Testing & Validation

### How to Test Your Findings

1. **Modify existing asset**: 
   - Find a cached asset file
   - Edit pixel data (change color)
   - Verify change appears in-game
   
2. **Create minimal asset**:
   - Build struct with solid color texture
   - Inject via memory patching
   - Check rendering

3. **Compare with official asset**:
   - Download asset from CDN
   - Parse with your specification
   - Validate all fields make sense

### Success Criteria

✅ We can load a custom 64x64 solid-color texture that displays in-game  
✅ We can load a custom 512x512 PNG with transparency  
✅ We understand all struct fields and can document them  
✅ We can handle errors (invalid format, wrong dimensions)  

---

## Timeline & Deliverables

**Estimated Effort**: 8-16 hours (experienced reverse engineer)

**Deliverables**:
1. ✅ **Asset Format Specification** (C struct definitions)
2. ✅ **Conversion Algorithm** (pseudo-code or reference implementation)
3. ✅ **Test Case** (binary example with known output)
4. ✅ **Notes Document** (findings, observations, gotchas)

**Optional Bonus**:
- DirectX shader analysis (how game renders these assets)
- Animation format (if you discover it)
- Material system (PBR parameters, etc.)

---

## Contact & Questions

If you need clarification or additional resources:
- **Hook Location**: `0x1404f37a0` in `r14.exe`
- **Asset IDs**: Use existing game cosmetics as reference (see `cosmetic_loadout_system.md`)
- **Test Environment**: Game launches and loads to main menu successfully
- **Prior Work**: Full documentation available in `/docs/features/`

**Questions to Consider**:
1. Does the game use standard DirectX formats (DDS) or custom?
2. Are assets encrypted or compressed (zlib, LZ4, etc.)?
3. Are there version numbers or magic bytes in asset headers?
4. How does the game handle different asset types (switch/discriminator)?
5. Are there size/dimension restrictions we should enforce?

---

## Appendix: Quick Reference

### FNV-1a Hash Implementation (CONFIRMED ALGORITHM)
```cpp
uint64_t ComputeSymbolHash(const char* str) {
  uint64_t hash = 0xcbf29ce484222325ULL;  // FNV offset basis
  while (*str) {
    hash ^= static_cast<uint64_t>(*str++);
    hash *= 0x100000001b3ULL;             // FNV prime
  }
  return hash;
}
```

### Example Asset IDs (for testing)
- `rdy_decal_logo` → `0x9c8e7a6b5d4f3e2a` (example hash)
- `custom_test_decal` → Computed at runtime
- `default` → Default/fallback asset

### DirectX 11 Texture Creation (Expected Pattern)
```cpp
// Game likely does something like this:
D3D11_TEXTURE2D_DESC desc = {
  .Width = width,
  .Height = height,
  .MipLevels = mip_count,
  .Format = DXGI_FORMAT_BC3_UNORM,  // or R8G8B8A8_UNORM
  .Usage = D3D11_USAGE_DEFAULT,
  // ...
};
device->CreateTexture2D(&desc, &data, &texture);
```

---

**Good luck, and thank you for your help making custom assets possible in EchoVR!** 🚀
