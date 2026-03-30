# Task 3: Precache Table & Tint Data Availability

## CRC64 Lookup Table — Resolved

### Table Source

The table is computed at runtime by the game binary (`.data` section at VA `0x141ffc480` is zero-initialized on disk, filled during CRT startup). It **cannot** be extracted from the static binary.

The correct table generation algorithm exists in two verified implementations:

1. **C++**: `~/src/evr-reconstruction/tools/hash_tool/csymbol64_hash.cpp:12-58`
2. **Go (nakama)**: `~/src/nakama/server/evr/core_hash_lookup.go:3-28`
3. **C++ (production)**: `~/src/nevr-runtime/src/gamepatches/resource_override.cpp:36-83` — hardcoded 256-entry table, verified correct

### Table Generation Algorithm

The polynomial is `0x95ac9329ac4bc9b5`. This is NOT a standard CRC64 table generation — it uses a custom MSB-first bit-by-bit approach with a final `*2` shift:

```
polynomial = 0x95ac9329ac4bc9b5

for i in 0..255:
    crc = 0

    // Bits 7-6 are handled with hardcoded constants (optimization of the same reduction)
    if i & 0x80 and i & 0x40:
        crc = 0xbef5b57af4dc5adf
    elif i & 0x40:
        crc = polynomial  // 0x95ac9329ac4bc9b5
    elif i & 0x80:
        crc = 0x2b5926535897936a

    // Bits 5-0: standard doubling with conditional XOR
    for bit in [0x20, 0x10, 0x08, 0x04, 0x02, 0x01]:
        crc = crc * 2
        if i & bit:
            crc ^= polynomial

    // Final shift (critical — missing from naive implementations)
    table[i] = crc * 2
```

### First 8 Entries (verification values)

```
table[0] = 0x0000000000000000
table[1] = 0x2b5926535897936a
table[2] = 0x56b24ca6b12f26d4
table[3] = 0x7deb6af5e9b8b5be
table[4] = 0xad64994d625e4da8
table[5] = 0x863dbf1e3ac9dec2
table[6] = 0xfbd6d5ebd3716b7c
table[7] = 0xd08ff3b88be6f816
```

### Hash Algorithm

```
CSymbol64_Hash(str, seed=0xFFFFFFFFFFFFFFFF):
    for each char c in str:
        if c is uppercase (A-Z):
            c += 0x20  // tolower
        seed = c ^ table[(seed >> 56) & 0xFF] ^ (seed << 8)
    return seed
```

Verified against 6 known hashes:

| Input                       | Expected             | Computed             | Status |
| --------------------------- | -------------------- | -------------------- | ------ |
| `rwd_tint_0019`             | `0x74d228d09dc5dd8f` | `0x74d228d09dc5dd8f` | PASS   |
| `rwd_tint_0000`             | `0x74d228d09dc5dc86` | `0x74d228d09dc5dc86` | PASS   |
| `rwd_tint_s1_a_default`     | `0x3e474b60a9416aca` | `0x3e474b60a9416aca` | PASS   |
| `rwd_tint_s3_tint_a`        | `0xa11587a1254c9507` | `0xa11587a1254c9507` | PASS   |
| `serverdb`                  | `0x25e886012ced8064` | `0x25e886012ced8064` | PASS   |
| `NEVRProtobufJSONMessageV1` | `0xc6b3710cd9c4ef47` | `0xc6b3710cd9c4ef47` | PASS   |

### C++ Implementation Recommendation

**Use the existing hardcoded table** from `resource_override.cpp` (lines 36-83). This is the simplest, most reliable approach:

- Already verified correct in production code
- No need for constexpr generation complexity
- Copy the `CSYM_TABLE[256]` array and the `CSymbol64Hash()` function (lines 85-93) directly

For a cleaner standalone header, the constexpr generation from `csymbol64_hash.cpp` also works but adds complexity for no runtime benefit. The hardcoded table is 2KB — trivial.

### Bug: evrFileTools hash.go

The `evrFileTools/pkg/hash/hash.go` (lines 80-95) uses standard MSB-first CRC generation:

```go
crc := uint64(i) << 56
for range 8 {
    if crc>>63 != 0 { crc = (crc << 1) ^ poly } else { crc <<= 1 }
}
```

This produces `table[1] = 0x95ac9329ac4bc9b5` (wrong). The correct value is `0x2b5926535897936a`. The custom algorithm has a different bit-processing structure and the final `*2` shift. The nakama implementation (`core_hash_lookup.go`) has the correct algorithm.

---

## Tint Data Availability — Confirmed Sufficient

### TintEntry Structure

From `evrFileTools/pkg/tint/tint.go`:

- **Size**: 96 bytes (0x60)
- **Layout**:
  - `[0x00]` uint64 ResourceID (Symbol64 hash)
  - `[0x08]` Color 0 (Main 1) — 16 bytes RGBA float32
  - `[0x18]` Color 1 (Accent 1)
  - `[0x28]` Color 2 (Main 2)
  - `[0x38]` Color 3 (Accent 2)
  - `[0x48]` Color 4 (Body)
  - `[0x58]` 8 bytes reserved/padding

Each Color is 4x float32 (RGBA, 0.0-1.0).

### Custom Tint Feasibility

**Yes, arbitrary custom tints can be created.** The process:

1. Choose a name (e.g., `custom_tint_neon_pink`)
2. Compute its Symbol64 hash using `CSymbol64Hash("custom_tint_neon_pink")`
3. Define 5 arbitrary RGBA colors (Main1, Accent1, Main2, Accent2, Body)
4. Pack into TintEntry struct (96 bytes) using `TintEntry.ToBytes()`

The `KnownTints` map in `tint.go` is a reverse-lookup convenience only — it maps hashes back to names for display. It does NOT constrain what tints can be created. The game accepts any valid TintEntry with any ResourceID — there is no whitelist validation.

### KnownTints Bypass

For the PoC, KnownTints bypass is **not needed and not sufficient**:

- **Not needed**: We can compute new Symbol64 hashes for any custom tint name using the verified hash algorithm + table above
- **Not sufficient**: KnownTints only has ~47 existing tint hashes. For custom colors, we need to CREATE new TintEntry structs with new hashes, not reuse existing ones

The correct approach for PoC is:

1. Use the CSymbol64Hash function to compute a ResourceID for the custom tint name
2. Pack arbitrary 5-color palettes into TintEntry format
3. Inject via CDN asset loading (the main project goal)

### showtints Tool

`evrFileTools/cmd/showtints/` provides a CLI tool that scans extracted asset directories for 96-byte tint entry files. It supports `--css` output for web preview. Useful for inspecting existing tints but not needed for creating custom ones.

---

## Source Files Referenced

- `/home/andrew/src/evr-reconstruction/tools/hash_tool/csymbol64_hash.cpp` — Correct constexpr table generation + hash
- `/home/andrew/src/nakama/server/evr/core_hash_lookup.go` — Correct Go table generation
- `/home/andrew/src/nevr-runtime/src/gamepatches/resource_override.cpp:36-93` — Production C++ table + hash function
- `/home/andrew/src/evrFileTools/pkg/hash/hash.go:80-95` — BUGGY table generation (wrong algorithm)
- `/home/andrew/src/evrFileTools/pkg/tint/tint.go` — TintEntry struct, serialization, KnownTints map
- `/home/andrew/src/evrFileTools/cmd/showtints/main.go` — Tint inspection CLI tool
- `/home/andrew/src/nevr-runtime/extras/reference/core_hash.go` — Go hash reference (correct algorithm, requires precache parameter)
