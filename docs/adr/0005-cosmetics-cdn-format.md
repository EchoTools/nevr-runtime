# ADR 0005: Cosmetics arrive as `.evrp` packages listed in a JSON manifest on a CDN

Status: accepted, implemented (`src/runtime/patch/asset_cdn.cpp`, `src/runtime/patch/evrp_package.cpp`).
Game version: 34.4.631399.1.

## Context

Tint cosmetics are produced by Go CLI tools (`nevr-cdn-tools`, Track A) and consumed by C++
game hooks in this repo (Track B). Both sides must agree on the bytes exactly.

## Decision

Cosmetic assets are distributed as `.evrp` files, a flat binary format invented for this
project, listed in a JSON manifest under `https://r2.echo.taxi/v1/`. Both sides MUST
produce and consume data conforming to the layout below. The accepting test vector, a
complete 108-byte tint for symbol `0x74d228d09dc5dc86`, and one rejecting case per
validation rule are in `src/runtime/tests/test_evrp_package.cpp`.

---

### 1. `.evrp` Package Binary Layout

`.evrp` is a flat binary file format invented for this project. Each file contains exactly one cosmetic asset.

Header: 28 bytes, followed by variable-length asset data.

| Offset | Size | Field            | Type      | Description                                     |
| ------ | ---- | ---------------- | --------- | ----------------------------------------------- |
| `0x00` | 4    | `magic`          | `[4]byte` | `"EVRP"` (bytes `0x45 0x56 0x52 0x50`)          |
| `0x04` | 4    | `format_version` | `uint32`  | Little-endian. Must be `1`.                     |
| `0x08` | 8    | `symbol_id`      | `int64`   | Little-endian. CSymbol64_Hash of the asset name |
| `0x10` | 1    | `slot_type`      | `uint8`   | Cosmetic slot enum (see section 2)              |
| `0x11` | 7    | `reserved`       | `[7]byte` | Zero-filled. Must be all zeroes.                |
| `0x18` | 4    | `data_length`    | `uint32`  | Little-endian. Byte length of `asset_data`      |
| `0x1C` | var  | `asset_data`     | `[]byte`  | Type-specific payload                           |

**Total header size**: 28 bytes (`0x1C`).

**Total file size**: `28 + data_length` bytes. For tints: `28 + 80 = 108` bytes.

---

### 2. Slot Type Enum

| Value         | Name     | Status                                          |
| ------------- | -------- | ----------------------------------------------- |
| `0x00`        | Reserved | Must not be used                                |
| `0x01`        | Tint     | Defined (see section 3)                         |
| `0x02`-`0xFF` | Reserved | Reserved for future types (mesh, texture, etc.) |

Parsers MUST reject files with unknown slot types.

---

### 3. Tint Asset Data (`slot_type = 0x01`)

`asset_data` is exactly **80 bytes**: 5 colors, 16 bytes each.

Each color is 4 little-endian `float32` values in RGBA order:

| Byte Offset (within asset_data) | Size | Field   | Description |
| ------------------------------- | ---- | ------- | ----------- |
| `0x00`                          | 16   | Color 0 | Main 1      |
| `0x10`                          | 16   | Color 1 | Accent 1    |
| `0x20`                          | 16   | Color 2 | Main 2      |
| `0x30`                          | 16   | Color 3 | Accent 2    |
| `0x40`                          | 16   | Color 4 | Body        |

Each 16-byte color:

| Offset | Size | Field | Type      |
| ------ | ---- | ----- | --------- |
| `+0`   | 4    | R     | `float32` |
| `+4`   | 4    | G     | `float32` |
| `+8`   | 4    | B     | `float32` |
| `+12`  | 4    | A     | `float32` |

#### Relationship to TintEntry (96 bytes)

The game's internal `TintEntry` struct is 96 bytes:

```
[0x00..0x08)  uint64    resourceID      <-- NOT in .evrp (symbol_id in header)
[0x08..0x18)  float32×4 color 0 (Main 1)
[0x18..0x28)  float32×4 color 1 (Accent 1)
[0x28..0x38)  float32×4 color 2 (Main 2)
[0x38..0x48)  float32×4 color 3 (Accent 2)
[0x48..0x58)  float32×4 color 4 (Body)
[0x58..0x60)  [8]byte   reserved        <-- NOT in .evrp (omitted)
```

The `.evrp` `asset_data` is the **middle 80 bytes** of a `TintEntry` — the 5 color values only. The 8-byte `resourceID` prefix is replaced by the `.evrp` header's `symbol_id` field. The 8-byte reserved suffix is omitted entirely.

**Track A** (Go tools): Extract the 80-byte color region from `TintEntry` when packing.
**Track B** (C++ hooks): Write these 80 bytes directly into the color portion of a `TintEntry` in game memory.

---

### 4. Manifest JSON Schema

The manifest is a JSON file listing all available packages on the CDN.

```json
{
  "version": 1,
  "game_version": "34.4.631399.1",
  "packages": {
    "74d228d09dc5dc86": {
      "url": "packages/74d228d09dc5dc86.evrp",
      "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "slot_type": "tint",
      "size": 108
    }
  }
}
```

### Field Definitions

**Top-level fields:**

| Field          | Type   | Required | Description                                     |
| -------------- | ------ | -------- | ----------------------------------------------- |
| `version`      | int    | Yes      | Manifest format version. Must be `1`.           |
| `game_version` | string | Yes      | Target game build version string                |
| `packages`     | object | Yes      | Map of symbol ID hex strings to package entries |

**Package entry fields:**

| Field       | Type   | Required | Description                                      |
| ----------- | ------ | -------- | ------------------------------------------------ |
| `url`       | string | Yes      | Path relative to CDN version root                |
| `sha256`    | string | Yes      | Hex-encoded SHA-256 of the complete `.evrp` file |
| `slot_type` | string | Yes      | Human-readable slot type name (`"tint"`)         |
| `size`      | int    | Yes      | Total file size in bytes (header + asset_data)   |

### Package Key Format

Keys in the `packages` object are the `symbol_id` from the `.evrp` header, encoded as:

- Lowercase hexadecimal
- No `0x` prefix
- 16 characters, zero-padded
- Example: `"74d228d09dc5dc86"`

### Slot Type String Mapping

| `slot_type` value in `.evrp` | String in manifest |
| ---------------------------- | ------------------ |
| `0x01`                       | `"tint"`           |

---

### 5. CDN URL Scheme

**Base URL**: `https://r2.echo.taxi/`

| Resource | URL                                                     |
| -------- | ------------------------------------------------------- |
| Manifest | `https://r2.echo.taxi/v1/manifest.json`                |
| Package  | `https://r2.echo.taxi/v1/packages/{symbolid_hex}.evrp` |

- `v1` is the format version path segment. Future incompatible format changes use `v2`, etc.
- `{symbolid_hex}` is the lowercase hex-encoded symbol ID (same format as manifest keys).
- Package URLs in the manifest are relative to the version root (`https://r2.echo.taxi/v1/`).

---

### 6. Byte Order and Validation

### Byte Order

All multi-byte integer and floating-point fields are **little-endian**. This matches x86/x64 native byte order and the game's internal data layout.

### Validation Rules

Parsers MUST enforce all of the following. Reject the file on any violation.

| Rule                    | Check                                                       |
| ----------------------- | ----------------------------------------------------------- |
| Magic bytes             | Bytes `[0x00..0x04)` must be exactly `0x45 0x56 0x52 0x50`  |
| Format version          | `format_version` must be `1`                                |
| Slot type               | `slot_type` must be a known value (currently only `0x01`)   |
| Reserved bytes          | `reserved` bytes `[0x11..0x18)` must all be zero            |
| Data length consistency | `header_size (28) + data_length` must equal total file size |
| Tint data length        | If `slot_type == 0x01`, `data_length` must be exactly `80`  |

### Float Validation (Advisory)

Color float values SHOULD be in the range `[0.0, 1.0]` but parsers MUST NOT reject values outside this range. The game engine handles out-of-range colors (e.g., HDR bloom effects use values > 1.0).

### Float representation

Track A tools MUST use `float32` arithmetic throughout and never convert decimal strings to
`float64` and truncate; a value such as 0.8 is stored as its nearest float32 (`0x3F4CCCCD`).
