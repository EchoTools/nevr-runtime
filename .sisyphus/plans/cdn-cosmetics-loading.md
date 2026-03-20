# CDN Cosmetics Loading — Tint PoC

## TL;DR

> **Quick Summary**: Build a tint-first vertical slice of CDN cosmetics for Echo VR. Define file formats (`.evrp` package, JSON manifest), create Go CLI tools in a separate repo (`~/src/nevr-cdn-tools/`) using `evrFileTools` as a library, implement C++ game hooks in nevr-runtime to download tint assets from `https://cdn.echo.taxi/` (Cloudflare R2), cache in `%LOCALAPPDATA%/EchoVR/cosmetics/`, and inject custom tint data via the `Loadout_ResolveDataFromId` hook.
>
> **Deliverables**:
> - `docs/cosmetics-cdn-format.md` — Format specification (`.evrp` binary layout, manifest JSON schema, CDN URL scheme)
> - `~/src/nevr-cdn-tools/` — Go module (`github.com/EchoTools/nevr-cdn-tools`) with `pack-tint`, `build-manifest` CLI commands, using `evrFileTools` as dependency
> - `src/common/symbol_hash.h` — constexpr C++ implementation of CSymbol64_Hash
> - `src/gamepatches/asset_cdn.h` + `asset_cdn.cpp` — AssetCDN module (manifest fetch, tint download, local cache, tint injection)
> - One real custom tint uploaded to CDN and rendering in-game
>
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 5 waves
> **Critical Path**: Phase 0 gates → Task 4 (symbol_hash) → Task 8 (PoC hook) → Task 11 (AssetCDN module) → Task 13 (startup integration)

---

## Context

### Original Request
User wants CDN-based cosmetics loading for Echo VR. Custom cosmetic assets hosted on Cloudflare R2 at `https://cdn.echo.taxi/`, cached locally, injected into the game's rendering pipeline via function hooks. Starting with **tints** as the simplest proof-of-concept (96 bytes of pure color data — no mesh/texture complexity).

### Interview Summary
**Key Discussions**:
- **Start with tints**: 96-byte color data, simplest asset type, proves the full pipeline without mesh/texture complexity
- **Separate repo for Go tools**: `~/src/nevr-cdn-tools/` (module `github.com/EchoTools/nevr-cdn-tools`), NOT inside nevr-runtime
- **Use evrFileTools as Go library**: `github.com/EchoTools/evrFileTools` already has tint parsing, manifest/package building, ZSTD compression
- **Hook strategy**: `Loadout_ResolveDataFromId` @ RVA `0x004F37A0` resolves loadout data. Hook intercepts AFTER resolution to patch tint color data in-place
- **CDN URL**: `https://cdn.echo.taxi/` with versioned paths
- **Cache**: `%LOCALAPPDATA%/EchoVR/cosmetics/`
- **No blocking HTTP on game thread** — ever
- **Manual R2 upload for PoC** (wrangler CLI or dashboard)
- **No automated unit tests** — agent-executed QA scenarios only
- **No Sisyphus footer on commits**

**Research Findings**:
- Tint binary format is 96 bytes: `uint64 resourceID` + 5 × `RGBA float32×4` (main1, accent1, main2, accent2, body) + 8 reserved bytes
- 48 known tint hashes in `evrFileTools/pkg/tint/tint.go` (e.g., `0x74d228d09dc5dc86` → `rwd_tint_0000`)
- `evrFileTools` provides: tint read/write/serialize, manifest/package build, ZSTD archive format
- `core_hash.go` (Go hash) requires a `precache [0x100]uint64` lookup table NOT included in the file — must be extracted from game binary or bypassed via known hashes
- Existing commented-out `AssetCDN::Initialize()` at `patches.cpp:1430-1433` and `asset_cdn_url` config at `patches.cpp:760-767`
- Hook target confirmed via RE: `Loadout_ResolveDataFromId` @ `0x1404f37a0`, LoadoutData struct fully mapped (0x4A0 bytes)
- CosmeticArrays at LoadoutData+0x370, primary CResourceID at +0xB8, fallback at +0x80
- curl is in vcpkg.json but linkage in CMakeLists.txt needs verification
- jsoncpp is in vcpkg.json but not linked to gamepatches target

### Metis Review
**Identified Gaps** (addressed in Phase 0):
- **File formats are INVENTED, not existing** — `.evrp` doesn't exist anywhere. Must write spec BEFORE any code. Added as Phase 0 gate (Task 1)
- **Hook target not validated at runtime** — FUN_1404f37a0 needs RE confirmation of tint resolution path. Added as Phase 0 gate (Task 2)
- **Precache table missing** — `core_hash.go` needs `precache [0x100]uint64` lookup table. Must extract from binary OR use KnownTints map to bypass. Added as Phase 0 gate (Task 3)
- **Tint data stored in components, not files** — Tints are within CR15NetRewardItemCS component data. Need to confirm raw 96-byte entries can be obtained. Added as Phase 0 gate (Task 3)
- **curl/jsoncpp linkage unverified** — May not actually be linked in CMakeLists.txt. Added as Task 5
- **CResourceID struct layout unknown** — Just a uint64? Pointer? Complex object? Addressed in Task 2 RE validation

**Guardrails Applied**:
- Format Decision Gate: Written spec before ANY code
- Track A (Go tools) and Track B (C++ hooks) independently testable
- PoC "Done" must be agent-verifiable (not "game renders tint" — instead: tint bytes match, hook compiles, file downloads)
- No new C++ dependencies beyond what's in vcpkg.json
- Hook isolation — self-contained, disableable via PatchDetour pattern
- Cache directory hardcoded for PoC, no config system
- ONE game version targeted — document exact version

---

## Work Objectives

### Core Objective
Build a complete tint-loading pipeline: define formats → build Go CLI tools → pack real tint data → upload to CDN → hook game → download tint → cache locally → inject tint color data into game memory. Validated by verifying tint bytes in cache match source data.

### Concrete Deliverables
1. `docs/cosmetics-cdn-format.md` — Format spec: `.evrp` binary layout, manifest JSON schema, CDN URL scheme
2. `~/src/nevr-cdn-tools/` — Go repo with `pack-tint` and `build-manifest` commands
3. `src/common/symbol_hash.h` — constexpr CSymbol64_Hash
4. `src/gamepatches/asset_cdn.h` + `asset_cdn.cpp` — AssetCDN module (tint-only for PoC)
5. One real custom tint `.evrp` file uploaded to `https://cdn.echo.taxi/v1/packages/`
6. Manifest JSON at `https://cdn.echo.taxi/v1/manifest.json`

### Definition of Done
- [ ] Format spec exists and is internally consistent (`.evrp` matches what Go tools produce and C++ parses)
- [ ] `pack-tint` CLI takes tint color values → produces valid `.evrp` file
- [ ] `build-manifest` CLI takes `.evrp` files → produces valid `manifest.json`
- [ ] `symbol_hash.h` computes identical hashes to Go implementation for all known test vectors
- [ ] AssetCDN module downloads manifest from CDN, downloads `.evrp` packages, caches in `%LOCALAPPDATA%/EchoVR/cosmetics/`
- [ ] Project builds cleanly with `make build`
- [ ] At least ONE custom tint `.evrp` is uploaded to CDN and downloadable via curl

### Must Have
- Written format spec BEFORE any implementation (Phase 0 gate)
- constexpr C++ symbol hash matching Go implementation exactly
- `.evrp` package format with magic bytes, version, symbol ID, slot type, asset data
- JSON manifest mapping symbol IDs → package URLs + checksums
- Local cache in `%LOCALAPPDATA%/EchoVR/cosmetics/`
- Background HTTP — never block game thread
- Graceful degradation — game functions normally if CDN unreachable
- Tint injection — patching 96-byte tint color data in LoadoutData after resolution

### Must NOT Have (Guardrails)
- **No mesh, texture, DDS, or geometry assets** — TINTS ONLY in this PoC
- **No audio, animation, or shader assets**
- **No in-game UI** for cosmetic selection — server assigns via loadout
- **No hot-reload** — assets loaded at startup, require restart for updates
- **No retry logic** — single attempt per download, fail gracefully
- **No blocking HTTP in game hooks** — all network I/O on background thread
- **No cache eviction policy** — cache grows unbounded
- **No CI/CD pipeline for CDN uploads** — manual wrangler/dashboard upload
- **No multi-player visibility guarantees** — document unknown behavior, don't solve
- **No config system for cache path** — hardcoded for PoC
- **No new C++ dependencies beyond vcpkg.json** (curl, jsoncpp already declared)
- **No overwrites of existing game assets** — only NEW assets with NEW SymbolIds
- **No assumptions about game internals without RE validation** — all hooks validated via Ghidra first

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.
> Acceptance criteria requiring "user manually tests/confirms" are FORBIDDEN.

### Test Decision
- **Infrastructure exists**: YES (CMake build system for C++, Go toolchain for tools)
- **Automated tests**: None (user decision — QA via agent-executed scenarios)
- **Framework**: N/A
- **Rationale**: Cross-compiled C++ DLL targeting game process — standard unit testing not practical. Go CLI tools verified via QA scenarios with real data.

### QA Policy
Every task MUST include agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **C++ compilation**: Use Bash — `make build` or CMake commands, verify zero errors/warnings
- **Go CLI tools**: Use Bash — `go build`, `go run` with test inputs, validate output files
- **Binary format validation**: Use Bash — hexdump, file inspection, format compliance
- **RE tasks**: Use notghidra tools — decompile, disassemble, validate addresses
- **Hash verification**: Use Bash — run Go hash and C++ hash, compare outputs
- **CDN upload verification**: Use Bash — `curl` the CDN URL, verify file downloads correctly

---

## Execution Strategy

### Two-Track Architecture

```
Track A: Go CLI Tools (~/src/nevr-cdn-tools/)          Track B: C++ Game Hooks (nevr-runtime)
─────────────────────────────────────────────          ─────────────────────────────────────
  evrFileTools (library dep)                             Existing hooking infrastructure
  ├── pack-tint: colors → .evrp                         ├── symbol_hash.h
  ├── build-manifest: .evrp files → manifest.json       ├── asset_cdn.h/.cpp (fetch + cache)
  └── (future: pack-mesh, pack-texture, cdn-push)       └── cosmetics hook in patches.cpp

                    ┌──────────────┐
                    │  CDN (R2)    │
                    │  manifest +  │◄── Track A uploads
                    │  packages    │──► Track B downloads
                    └──────────────┘
```

Track A and Track B are **independently testable**. Track A produces files. Track B consumes files. The format spec is the contract between them.

### Parallel Execution Waves

```
Phase 0 (Gates — MUST complete before Wave 1):
├── Task 1: Write format spec (contract between Track A & Track B) [writing]
├── Task 2: RE — Validate hook target + tint resolution path [deep]
└── Task 3: RE — Extract precache table + verify tint data availability [deep]

Wave 1 (Foundation — all start after Phase 0):
├── Task 4: symbol_hash.h C++ implementation [quick]
├── Task 5: Build system prep — curl + jsoncpp linking [quick]
├── Task 6: nevr-cdn-tools repo scaffolding [quick]
└── Task 7: pack-tint CLI command [unspecified-high]

Wave 2 (CDN Pipeline + C++ Module):
├── Task 8: Hardcoded tint injection PoC (prove hook works) [deep]
├── Task 9: build-manifest CLI command [unspecified-high]
├── Task 10: Pack real tint + upload to CDN [quick]
└── Task 11: AssetCDN module — manifest fetch + download + cache [deep]

Wave 3 (Integration):
├── Task 12: Tint injection from cached .evrp files [deep]
└── Task 13: Startup integration — AssetCDN::Initialize() [unspecified-high]

Wave FINAL (Verification — 4 parallel reviews):
├── F1: Plan compliance audit [oracle]
├── F2: Code quality review [unspecified-high]
├── F3: Integration QA [unspecified-high]
└── F4: Scope fidelity check [deep]

Critical Path: T1 (spec) → T7 (pack-tint) → T10 (upload) → T11 (fetch) → T12 (inject) → T13 (startup)
Parallel Speedup: ~65% faster than sequential
Max Concurrent: 4 (Wave 1)
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 4, 5, 6, 7, 8, 9, 10, 11 | Phase 0 |
| 2 | — | 8 | Phase 0 |
| 3 | — | 4, 7, 8 | Phase 0 |
| 4 | 1, 3 | 8, 12 | 1 |
| 5 | 1 | 8, 11 | 1 |
| 6 | 1 | 7 | 1 |
| 7 | 1, 3, 6 | 9, 10 | 1 |
| 8 | 1, 2, 3, 4, 5 | 12 | 2 |
| 9 | 1, 7 | 10 | 2 |
| 10 | 7, 9 | 11 | 2 |
| 11 | 1, 5, 10 | 12, 13 | 2 |
| 12 | 4, 8, 11 | 13 | 3 |
| 13 | 11, 12 | F1-F4 | 3 |
| F1-F4 | 13 | — | FINAL |

### Agent Dispatch Summary

| Wave | Tasks | Categories |
|------|-------|------------|
| Phase 0 | 3 tasks | T1 → `writing`, T2-T3 → `deep` |
| 1 | 4 tasks | T4 → `quick`, T5 → `quick`, T6 → `quick`, T7 → `unspecified-high` |
| 2 | 4 tasks | T8 → `deep`, T9 → `unspecified-high`, T10 → `quick`, T11 → `deep` |
| 3 | 2 tasks | T12 → `deep`, T13 → `unspecified-high` |
| FINAL | 4 tasks | F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep` |

---

## TODOs

> Implementation + Verification = ONE Task. Never separate.
> EVERY task MUST have: Recommended Agent Profile + Parallelization info + QA Scenarios.
> **A task WITHOUT QA Scenarios is INCOMPLETE. No exceptions.**

<!-- TASKS_START -->

### Phase 0 — Gates (ALL must complete before Wave 1 begins)

- [ ] 1. **Write format specification — `.evrp` + manifest JSON + CDN URL scheme**

  **What to do**:
  - Create `docs/cosmetics-cdn-format.md` defining the complete contract between Track A (Go tools) and Track B (C++ hooks)
  - Define `.evrp` package binary layout:
    ```
    magic:          [4]byte  "EVRP"
    format_version: uint32   (little-endian, start at 1)
    symbol_id:      int64    (little-endian, CSymbol64_Hash of asset name)
    slot_type:      uint8    (cosmetic slot enum — 0x01 = tint for PoC)
    reserved:       [7]byte  (zero-filled, future use)
    data_length:    uint32   (little-endian, length of asset_data)
    asset_data:     []byte   (type-specific: for tints, 80 bytes = 5 × RGBA float32×4)
    ```
  - Note: tint asset_data is 80 bytes (5 colors × 16 bytes), NOT 96 bytes. The 96-byte TintEntry format includes the 8-byte resourceID prefix and 8-byte reserved suffix. The `.evrp` header already carries the symbol_id, so asset_data omits the resourceID. The reserved suffix is also omitted.
  - Define manifest JSON schema:
    ```json
    {
      "version": 1,
      "game_version": "34.4.631399.1",
      "packages": {
        "74d228d09dc5dc86": {
          "url": "packages/74d228d09dc5dc86.evrp",
          "sha256": "abc123...",
          "slot_type": "tint",
          "size": 108
        }
      }
    }
    ```
  - Define CDN URL scheme:
    - Manifest: `https://cdn.echo.taxi/v1/manifest.json`
    - Packages: `https://cdn.echo.taxi/v1/packages/{symbolid_hex}.evrp`
  - Document slot_type enum values (only `tint = 0x01` for PoC, reserve others)
  - Document byte order (all little-endian), alignment, and validation rules
  - Document relationship to evrFileTools formats: `.evrp` is a NEW format we are inventing. It is NOT the same as evrFileTools' archive format. evrFileTools is used to READ source tint data, which we then REPACKAGE into `.evrp`.

  **Must NOT do**:
  - Do not define mesh, texture, or geometry slot types — reserve numbers but don't spec
  - Do not implement anything — this is documentation only
  - Do not invent a streaming/chunked format — flat file is fine for PoC

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: Pure documentation task — format specification document
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 2, 3)
  - **Parallel Group**: Phase 0
  - **Blocks**: Tasks 4, 5, 6, 7, 8, 9, 10, 11 (everything depends on the format spec)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `evrFileTools/pkg/tint/tint.go` — TintEntry struct (96 bytes): 8-byte resourceID + 5×RGBA(16 bytes) + 8 reserved. Our `.evrp` asset_data is the MIDDLE 80 bytes (colors only, no resourceID prefix or reserved suffix)
  - `evrFileTools/pkg/manifest/` — EVR native manifest format for reference. Our JSON manifest is DIFFERENT — simpler, CDN-oriented, not the binary game format
  - `evrFileTools/pkg/archive/` — ZSTD archive format for reference. We are NOT using this format, but understanding it helps clarify what `.evrp` is NOT

  **External References**:
  - `extras/docs/HASH_DISCOVERY_COMPLETE.md` — Documents CSymbol64_Hash algorithm used for symbol_id field
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — LoadoutData struct with cosmetic slot offsets

  **WHY Each Reference Matters**:
  - `tint.go`: Defines the exact byte layout of tint color data. Our `.evrp` asset_data for tints must match the 80-byte color region (offsets 0x08-0x57 of a TintEntry)
  - `manifest/` and `archive/`: These are what `.evrp` is NOT. Understanding the native formats prevents confusion
  - `HASH_DISCOVERY_COMPLETE.md`: The symbol_id in `.evrp` headers uses this hash algorithm

  **Acceptance Criteria**:
  - [ ] `docs/cosmetics-cdn-format.md` exists
  - [ ] Document defines `.evrp` binary layout with exact byte offsets
  - [ ] Document defines manifest JSON schema with example
  - [ ] Document defines CDN URL scheme
  - [ ] Document specifies slot_type enum (at least tint = 0x01)
  - [ ] Document clarifies relationship to evrFileTools formats

  **QA Scenarios:**

  ```
  Scenario: Format spec is complete and internally consistent
    Tool: Bash
    Preconditions: docs/cosmetics-cdn-format.md exists
    Steps:
      1. Read the file and verify it contains sections for: .evrp layout, manifest schema, URL scheme, slot types
      2. Verify .evrp header size: 4 (magic) + 4 (version) + 8 (symbol_id) + 1 (slot_type) + 7 (reserved) + 4 (data_length) = 28 bytes
      3. Verify tint asset_data size is documented as 80 bytes (5 × 16)
      4. Verify manifest JSON example is valid JSON (pipe through jq)
      5. Verify URL scheme matches pattern https://cdn.echo.taxi/v{N}/...
    Expected Result: All sections present, sizes consistent, JSON valid
    Failure Indicators: Missing sections, size mismatches, invalid JSON example
    Evidence: .sisyphus/evidence/task-1-format-spec-review.txt
  ```

  **Commit**: YES
  - Message: `docs(cosmetics): add CDN format specification`
  - Files: `docs/cosmetics-cdn-format.md`
  - Pre-commit: n/a

---

- [ ] 2. **RE — Validate hook target and tint resolution path**

  **What to do**:
  - Use notghidra tools to decompile `Loadout_ResolveDataFromId` (RVA `0x004F37A0`, absolute `0x1404F37A0`)
  - Confirm: function signature, parameters, return type
  - Determine how tint data flows through this function:
    - Does it resolve a tint SymbolId → tint color data?
    - Where is tint color data stored after resolution? (LoadoutData+0x370 CosmeticArrays? Direct memory?)
    - What is the exact memory layout at the point where we can patch tint colors?
  - Map CResourceID struct: is it just a uint64 (SymbolId hash)? A pointer? A complex object?
  - Determine if hooking this function AFTER it returns gives us a writable pointer to tint color data
  - Document the exact hook strategy: pre-hook vs post-hook, what to read, what to write, at what offset
  - Check what happens if a SymbolId has no matching game asset (unknown SymbolId behavior)
  - Document the exact game version this analysis targets

  **Must NOT do**:
  - Do not write any C++ code — this is RE analysis only
  - Do not attempt to hook anything — just document the plan
  - Do not analyze mesh/texture resolution paths — tint path only

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Complex reverse engineering requiring binary analysis, decompilation, and cross-referencing multiple data structures
  - **Skills**: []
    - notghidra tools are available as built-in MCP tools, no skill loading needed

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 1, 3)
  - **Parallel Group**: Phase 0
  - **Blocks**: Task 8 (PoC hook needs validated hook target)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `src/gamepatches/patches.cpp:1430-1433` — Commented-out `AssetCDN::Initialize()` call. Shows where in startup flow CDN init was intended
  - `src/gamepatches/patches.cpp:760-767` — Commented-out `asset_cdn_url` config reading. Shows config pattern
  - `src/common/hooking.h` — `PatchDetour()` API for installing hooks. The validated hook will use this pattern
  - `src/common/echovr.h` — `SymbolId` typedef (INT64), `LoadoutSlot` enum, cosmetic struct definitions
  - `src/gamepatches/patch_addresses.h` — Where validated RVA should be added

  **API/Type References**:
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — LoadoutData (0x4A0 bytes), CosmeticArrays at +0x370, primary CResourceID at +0xB8, fallback at +0x80
  - `docs/ghidra/` — All existing RE documentation for cross-reference

  **External References**:
  - `evrFileTools/pkg/tint/tint.go` — TintEntry struct layout (96 bytes). Need to determine if game memory matches this layout

  **WHY Each Reference Matters**:
  - `patches.cpp`: Shows the prior developer's intent — AssetCDN was planned but never built. The hook location in startup flow is already chosen
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: Contains LoadoutData offsets needed to find tint color data after hook resolution
  - `hooking.h`: The hook will use PatchDetour — need to understand its calling convention requirements
  - `tint.go`: Need to verify game memory tint layout matches what evrFileTools expects

  **Acceptance Criteria**:
  - [ ] FUN_1404f37a0 decompiled and documented
  - [ ] CResourceID struct layout determined (uint64 vs pointer vs complex)
  - [ ] Tint resolution path traced: SymbolId → where tint color data lands in memory
  - [ ] Hook strategy documented: pre/post, read/write addresses, offset calculations
  - [ ] Unknown SymbolId behavior documented
  - [ ] Exact game version noted

  **QA Scenarios:**

  ```
  Scenario: Hook target function is decompilable and analyzable
    Tool: notghidra (MCP tools)
    Preconditions: echovr.exe binary imported into notghidra project
    Steps:
      1. Use notghidra_functions_decompile with address 0x004F37A0
      2. Verify decompiled output shows function parameters and return type
      3. Use notghidra_xrefs_to to find all callers of this function
      4. Use notghidra_xrefs_from to find all functions it calls
      5. Document findings in a structured format
    Expected Result: Function decompiles successfully, shows clear parameter types, callers identified
    Failure Indicators: Address invalid, function too large to decompile, no callers found
    Evidence: .sisyphus/evidence/task-2-hook-decompilation.md

  Scenario: CResourceID struct layout determination
    Tool: notghidra (MCP tools)
    Preconditions: FUN_1404f37a0 decompiled
    Steps:
      1. Find references to LoadoutData+0xB8 and LoadoutData+0x80 in decompiled code
      2. Trace how CResourceID values are used — passed by value or pointer?
      3. Check sizeof hints from stack allocation or memcpy calls
      4. Cross-reference with ECHOVR_STRUCT_MEMORY_MAP.md offsets
    Expected Result: CResourceID is identified as uint64 or struct with known layout
    Failure Indicators: Ambiguous usage, multiple possible interpretations
    Evidence: .sisyphus/evidence/task-2-cresourceid-layout.md
  ```

  **Commit**: NO (RE documentation only, recorded in evidence files)

---

- [ ] 3. **RE — Extract precache table + verify tint data availability**

  **What to do**:
  - **Precache table**: `core_hash.go` requires a `precache [0x100]uint64` lookup table as a parameter to `CoreHash()`. This table is NOT embedded in the source — it must be extracted from the game binary or generated.
    - Option A: Find the precache table in the game binary near `CSymbol64_Hash` @ `0x1400CE120`. It's likely a static 2048-byte array (256 × uint64) used as a CRC lookup table for polynomial `0x95ac9329ac4bc9b5`
    - Option B: Compute the CRC64 lookup table from the polynomial directly (standard CRC table generation algorithm). This is likely the correct approach — CRC lookup tables are deterministic from the polynomial
    - Option C: Bypass entirely — use `KnownTints` map from `evrFileTools/pkg/tint/tint.go` (48 pre-computed hashes) for the PoC, defer hash computation to later
    - Determine which option is viable and document
  - **Tint data availability**: Verify that raw tint color data (the 80 bytes of 5×RGBA) can be obtained from extracted game assets or constructed from known values
    - Check if `evrFileTools/cmd/showtints/` can extract real tint data from game files
    - Check if the `sourcedb/` JSON files contain tint color values
    - If neither: define synthetic test tint data (custom colors) for the PoC — we don't NEED real game tints, we need custom tints
  - **Recommendation**: For the PoC, we likely want CUSTOM tints (new colors not in the game), not copies of existing ones. So tint data "availability" means: can we define our own 5-color palette and pack it?

  **Must NOT do**:
  - Do not implement the hash function — just determine how to get the precache table
  - Do not extract mesh/texture data — tint data only
  - Do not modify any source files

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Binary analysis (precache table extraction) + data archaeology (tint data location)
  - **Skills**: []
    - notghidra tools available as built-in MCP

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 1, 2)
  - **Parallel Group**: Phase 0
  - **Blocks**: Tasks 4 (need precache table for hash impl), 7 (need tint data for pack-tint), 8 (need hash for PoC)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `extras/reference/core_hash.go` — Go hash implementation. Lines 12-38 show `CoreHash()` function requiring `precache [0x100]uint64` parameter. The polynomial is `0x95ac9329ac4bc9b5`
  - `evrFileTools/pkg/tint/tint.go` — `KnownTints` map with 48 pre-computed symbol → name mappings. If we can't compute hashes, we can use these known values for PoC
  - `evrFileTools/cmd/showtints/` — CLI tool that displays tint data. May show how to extract/read tint color values

  **API/Type References**:
  - `evrFileTools/pkg/tint/tint.go:TintEntry` — The 96-byte struct. Fields: ResourceID (uint64), Color0-Color4 (each RGBA float32×4), Reserved (8 bytes)

  **External References**:
  - CRC64 lookup table generation is a well-known algorithm: for each byte 0-255, compute 8 iterations of `if (bit0) { val = (val >> 1) ^ polynomial } else { val >>= 1 }`

  **WHY Each Reference Matters**:
  - `core_hash.go`: The precache table is the ONLY missing piece for computing new SymbolId hashes. Without it, we can only use pre-known hashes from KnownTints
  - `tint.go`: KnownTints provides a fallback if precache table extraction fails — we can map known hashes to names
  - `showtints/`: May reveal how to read real tint data from game files, giving us real color values to test with

  **Acceptance Criteria**:
  - [ ] Precache table situation resolved: extracted OR computed OR documented why bypassed
  - [ ] Tint data source identified: extracted real data OR synthetic test data defined
  - [ ] Documented recommendation for hash approach in PoC (compute vs known-hashes-only)
  - [ ] If precache table extracted: 256 uint64 values recorded/saved

  **QA Scenarios:**

  ```
  Scenario: Precache table resolution
    Tool: notghidra (MCP tools) + Bash
    Preconditions: echovr.exe binary imported into notghidra project
    Steps:
      1. Use notghidra_functions_disassemble at 0x000CE120 (CSymbol64_Hash)
      2. Look for data references to a 2048-byte region (256 × 8 bytes)
      3. If found: use notghidra_data_read to extract 2048 bytes
      4. If not found: implement CRC64 table generation from polynomial 0x95ac9329ac4bc9b5 and verify against known hash outputs
      5. Verify by computing hash of "rwd_tint_0000" and comparing to known value 0x74d228d09dc5dc86
    Expected Result: Either table extracted or computation method verified against known hashes
    Failure Indicators: Can't find table AND computed table produces wrong hashes
    Evidence: .sisyphus/evidence/task-3-precache-table.txt

  Scenario: Tint color data for PoC
    Tool: Bash
    Preconditions: evrFileTools available
    Steps:
      1. Check if showtints CLI can read tint data: cd ~/src/evrFileTools && go run ./cmd/showtints/ --help
      2. If game assets available: attempt to read real tint data
      3. If not: define synthetic test tint (5 distinct RGBA colors, e.g., bright red/green/blue/yellow/purple)
      4. Document the 80 bytes of color data that will be used for pack-tint testing
    Expected Result: 80 bytes of tint color data ready for use (real or synthetic)
    Failure Indicators: Cannot define valid tint color data
    Evidence: .sisyphus/evidence/task-3-tint-data-source.txt
  ```

  **Commit**: NO (RE analysis, recorded in evidence files)

---


### Wave 1 — Foundation (all start after Phase 0 completes)

- [ ] 4. **Implement constexpr CSymbol64_Hash in C++ header**

  **What to do**:
  - Create `src/common/symbol_hash.h` with a constexpr C++ implementation of CSymbol64_Hash
  - Algorithm: case-insensitive CRC64 with polynomial `0x95ac9329ac4bc9b5`, seed `0xFFFFFFFFFFFFFFFF`
  - Must handle null terminator, lowercase conversion, and the exact polynomial reduction loop
  - Port from Go implementation at `extras/reference/core_hash.go`
  - Use the precache table obtained from Task 3 (embed as constexpr array OR generate at compile time)
  - Provide both constexpr (compile-time) and runtime variants
  - Include `SymbolHash(const char* str)` convenience function returning `SymbolId` (INT64)
  - Reference `.sisyphus/plans/symbol-hash-implementation.md` for detailed specifications if it exists

  **Must NOT do**:
  - Do not modify any existing files — this is a NEW header only
  - Do not add unit test framework — verification is via QA scenario
  - Do not attempt to hook or patch anything — pure utility

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single header file, well-defined algorithm, direct port from Go
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 5, 6, 7)
  - **Parallel Group**: Wave 1
  - **Blocks**: Tasks 8, 12
  - **Blocked By**: Tasks 1 (format spec for context), 3 (precache table)

  **References**:

  **Pattern References**:
  - `extras/reference/core_hash.go` — AUTHORITATIVE reference. Contains polynomial, seed, case-insensitive logic. Port exactly.
  - `src/common/symbols.h` — Existing `SymbolId` type (`typedef INT64 SymbolId`). New header must be compatible.

  **API/Type References**:
  - `src/common/echovr.h:SymbolId` — The `INT64` typedef that hash results must be compatible with

  **External References**:
  - `extras/docs/HASH_DISCOVERY_COMPLETE.md` — Polynomial `0x95ac9329ac4bc9b5`, seed `0xFFFFFFFFFFFFFFFF`, case-insensitive, includes null terminator
  - Evidence from Task 3: `.sisyphus/evidence/task-3-precache-table.txt` — The 256-entry CRC lookup table

  **WHY Each Reference Matters**:
  - `core_hash.go`: ONLY verified-correct implementation. Port exactly, do not improvise
  - `symbols.h`: Must return `SymbolId` (INT64) for codebase compatibility
  - Task 3 evidence: Precache table is required input — either embed or compute

  **Acceptance Criteria**:
  - [ ] File `src/common/symbol_hash.h` exists and compiles with MinGW cross-compiler
  - [ ] Header is self-contained (no dependencies beyond standard library)
  - [ ] `constexpr` qualification allows compile-time hash computation
  - [ ] Hash of `"rwd_tint_0000"` equals `0x74d228d09dc5dc86`

  **QA Scenarios:**

  ```
  Scenario: Hash function produces correct output for known test vectors
    Tool: Bash
    Preconditions: Project builds successfully
    Steps:
      1. Create a minimal C++ test program that #includes symbol_hash.h
      2. Compute hashes for: "rwd_tint_0000", "rwd_tint_0019", "decal_default"
      3. Print results as hex
      4. Compare against known values: rwd_tint_0000=0x74d228d09dc5dc86, rwd_tint_0019=0x74d228d09dc5dd8f
    Expected Result: All hash values match known values exactly
    Failure Indicators: Any hash mismatch, compilation error
    Evidence: .sisyphus/evidence/task-4-hash-test-vectors.txt

  Scenario: constexpr compilation — hash used in static_assert
    Tool: Bash
    Preconditions: symbol_hash.h exists
    Steps:
      1. Create test program with: static_assert(SymbolHash("rwd_tint_0000") == 0x74d228d09dc5dc86, "hash mismatch");
      2. Compile with MinGW cross-compiler
    Expected Result: Compiles without error (proves constexpr works at compile time)
    Failure Indicators: Compiler error about non-constant expression
    Evidence: .sisyphus/evidence/task-4-constexpr-verify.txt
  ```

  **Commit**: YES
  - Message: `feat(common): add constexpr CSymbol64_Hash implementation`
  - Files: `src/common/symbol_hash.h`
  - Pre-commit: compile test program

---

- [ ] 5. **Build system prep — verify and link curl + jsoncpp to gamepatches**

  **What to do**:
  - Verify curl is actually linked (not just declared) in `src/gamepatches/CMakeLists.txt`
    - Look for `target_link_libraries(gamepatches ... CURL::libcurl ...)` or equivalent
    - If missing: add it
  - Verify jsoncpp is linked to gamepatches target
    - Look for `target_link_libraries(gamepatches ... jsoncpp_lib ...)` or `JsonCpp::JsonCpp`
    - If missing: add it
  - Create stub files `src/gamepatches/asset_cdn.h` and `src/gamepatches/asset_cdn.cpp` with:
    - Empty `AssetCDN` class/namespace with `Initialize()` and `Shutdown()` stubs
    - `#include <curl/curl.h>` and `#include <json/json.h>` to verify linkage at compile time
  - Add `asset_cdn.cpp` to CMakeLists.txt source list
  - Verify `make build` succeeds with curl and jsoncpp headers resolved

  **Must NOT do**:
  - Do not implement any CDN logic — stubs only
  - Do not add new vcpkg dependencies — curl and jsoncpp are already declared
  - Do not modify vcpkg.json

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Build system configuration + stub files, straightforward
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 4, 6, 7)
  - **Parallel Group**: Wave 1
  - **Blocks**: Tasks 8 (needs compilable asset_cdn stubs), 11 (needs curl/jsoncpp)
  - **Blocked By**: Task 1 (format spec for struct definitions)

  **References**:

  **Pattern References**:
  - `src/gamepatches/CMakeLists.txt` — Current build configuration. Check existing `target_link_libraries` calls for pattern to follow
  - `vcpkg.json` — Declares curl and jsoncpp as dependencies. Verify feature flags and version constraints
  - `src/gamepatches/patches.cpp:1430-1433` — Commented `AssetCDN::Initialize()` call. Shows expected include and call pattern
  - `src/gamepatches/patches.cpp:760-767` — Commented `asset_cdn_url` config. Shows config reading pattern

  **WHY Each Reference Matters**:
  - `CMakeLists.txt`: Need to verify what IS linked and add what's MISSING. Following existing patterns prevents build breakage
  - `vcpkg.json`: Confirms the dependencies exist in the dependency manifest — we just need to link them
  - `patches.cpp`: Shows prior developer's intended integration points. The stubs should match these expectations

  **Acceptance Criteria**:
  - [ ] `make build` succeeds with zero errors
  - [ ] `asset_cdn.h` and `asset_cdn.cpp` exist as stubs
  - [ ] `#include <curl/curl.h>` compiles without errors in asset_cdn.cpp
  - [ ] `#include <json/json.h>` compiles without errors in asset_cdn.cpp
  - [ ] asset_cdn.cpp is in CMakeLists.txt source list

  **QA Scenarios:**

  ```
  Scenario: Build succeeds with curl and jsoncpp linked
    Tool: Bash (isBackground: true)
    Preconditions: Source files exist
    Steps:
      1. Run: make build 2>&1 | tee /tmp/build-output.txt
      2. Check exit code is 0
      3. Grep build output for "curl/curl.h" errors — should be none
      4. Grep build output for "json/json.h" errors — should be none
      5. Verify asset_cdn.o (or equivalent) was produced
    Expected Result: Clean build, zero errors, zero relevant warnings
    Failure Indicators: Linker errors for curl/jsoncpp symbols, missing header errors
    Evidence: .sisyphus/evidence/task-5-build-output.txt
  ```

  **Commit**: YES
  - Message: `build(gamepatches): link curl and jsoncpp, add asset_cdn stubs`
  - Files: `src/gamepatches/CMakeLists.txt`, `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`
  - Pre-commit: `make build`

---

- [ ] 6. **Scaffold nevr-cdn-tools Go repository**

  **What to do**:
  - Create `~/src/nevr-cdn-tools/` directory
  - Initialize Go module: `go mod init github.com/EchoTools/nevr-cdn-tools`
  - Add evrFileTools as dependency: `go get github.com/EchoTools/evrFileTools`
  - Create directory structure:
    ```
    nevr-cdn-tools/
    ├── go.mod
    ├── go.sum
    ├── cmd/
    │   ├── pack-tint/
    │   │   └── main.go      (stub: prints "pack-tint: not yet implemented")
    │   └── build-manifest/
    │       └── main.go      (stub: prints "build-manifest: not yet implemented")
    └── internal/
        └── evrp/
            └── evrp.go      (stub: EVRP format constants from spec)
    ```
  - The `internal/evrp/evrp.go` should define the `.evrp` format constants from the spec (Task 1):
    - Magic bytes `"EVRP"`
    - Format version `1`
    - SlotType constants (`SlotTypeTint = 0x01`)
    - Header struct matching the spec
  - Initialize git repo: `git init`
  - Verify: `go build ./...` succeeds

  **Must NOT do**:
  - Do not implement pack-tint or build-manifest logic — stubs only
  - Do not add dependencies beyond evrFileTools
  - Do not set up CI/CD

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Repo scaffolding, go mod init, stub files
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 4, 5, 7 — but 7 depends on 6)
  - **Parallel Group**: Wave 1
  - **Blocks**: Task 7 (pack-tint needs repo structure)
  - **Blocked By**: Task 1 (format spec for evrp.go constants)

  **References**:

  **Pattern References**:
  - `~/src/evrFileTools/go.mod` — Module path is `github.com/EchoTools/evrFileTools`. This is the import path for the dependency
  - `~/src/evrFileTools/cmd/showtints/` — Example CLI command structure to follow
  - `~/src/evrFileTools/pkg/tint/tint.go` — Tint types that will be imported

  **WHY Each Reference Matters**:
  - `evrFileTools/go.mod`: Need exact module path for `go get`
  - `showtints/`: Pattern for how cmd/ directories are structured in evrFileTools — follow same convention

  **Acceptance Criteria**:
  - [ ] `~/src/nevr-cdn-tools/` directory exists
  - [ ] `go.mod` references `github.com/EchoTools/nevr-cdn-tools`
  - [ ] `go.sum` includes evrFileTools
  - [ ] `go build ./...` succeeds with zero errors
  - [ ] `go run ./cmd/pack-tint` prints stub message
  - [ ] `go run ./cmd/build-manifest` prints stub message

  **QA Scenarios:**

  ```
  Scenario: Go repo builds and stubs run
    Tool: Bash
    Preconditions: Go toolchain installed
    Steps:
      1. cd ~/src/nevr-cdn-tools && go build ./...
      2. Verify exit code 0
      3. go run ./cmd/pack-tint 2>&1 | grep -i "not yet implemented"
      4. go run ./cmd/build-manifest 2>&1 | grep -i "not yet implemented"
      5. grep "evrFileTools" go.mod
    Expected Result: Build succeeds, stubs print messages, evrFileTools in go.mod
    Failure Indicators: Build errors, missing dependency, stub doesn't run
    Evidence: .sisyphus/evidence/task-6-repo-scaffold.txt
  ```

  **Commit**: YES (in nevr-cdn-tools repo)
  - Message: `feat: scaffold nevr-cdn-tools repository with evrFileTools dependency`
  - Files: all files
  - Pre-commit: `go build ./...`

---

- [ ] 7. **Implement pack-tint CLI command**

  **What to do**:
  - Implement `cmd/pack-tint/main.go` in `~/src/nevr-cdn-tools/`
  - CLI interface:
    ```
    pack-tint --name <tint_name> --colors <color_spec> --output <path.evrp>
    
    # or with individual color flags:
    pack-tint --name "custom_tint_001" \
      --main1 "1.0,0.0,0.0,1.0" \
      --accent1 "0.0,1.0,0.0,1.0" \
      --main2 "0.0,0.0,1.0,1.0" \
      --accent2 "1.0,1.0,0.0,1.0" \
      --body "1.0,0.0,1.0,1.0" \
      --output custom_tint_001.evrp
    ```
  - Compute SymbolId hash from `--name` using CSymbol64_Hash (port from Go `core_hash.go` or use evrFileTools if it has hash function)
  - Note: If Task 3 determined the precache table approach, use that. If bypassing with known hashes, accept `--symbol-id 0x...` as override
  - Pack into `.evrp` format per spec (Task 1):
    - Write EVRP magic, version 1, symbol_id, slot_type=0x01 (tint), reserved zeros, data_length=80, 80 bytes of color data
  - Use `internal/evrp/evrp.go` for format constants and write helpers
  - Parse RGBA values from comma-separated float strings
  - Validate: all color components are 0.0-1.0 range

  **Must NOT do**:
  - Do not support any asset type other than tints
  - Do not add compression — raw bytes for PoC
  - Do not add encryption or signing
  - Do not read from game files — colors specified on command line

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Non-trivial CLI tool with binary format writing, hash computation, and evrFileTools integration
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on Task 6 for repo structure)
  - **Parallel Group**: Wave 1 (starts after Task 6, parallel with 4, 5)
  - **Blocks**: Tasks 9 (build-manifest needs .evrp files), 10 (upload needs .evrp files)
  - **Blocked By**: Tasks 1 (format spec), 3 (precache table / hash approach), 6 (repo structure)

  **References**:

  **Pattern References**:
  - `~/src/evrFileTools/cmd/showtints/main.go` — CLI command pattern in evrFileTools. Follow same flag parsing and error handling style
  - `~/src/evrFileTools/pkg/tint/tint.go` — TintEntry struct and Color struct. Import and use these types for color data representation
  - `~/src/nevr-cdn-tools/internal/evrp/evrp.go` — Format constants from Task 6. Use for magic bytes, version, slot types
  - `docs/cosmetics-cdn-format.md` — THE format spec. Follow exactly for .evrp binary layout

  **API/Type References**:
  - `evrFileTools/pkg/tint:TintEntry` — 96-byte struct. We write the 80-byte color portion (offsets 0x08-0x57)
  - `evrFileTools/pkg/tint:Color` — RGBA float32×4 struct. Use for parsing color inputs
  - Evidence from Task 3: `.sisyphus/evidence/task-3-precache-table.txt` — Hash computation approach

  **WHY Each Reference Matters**:
  - `showtints/main.go`: Establishes CLI conventions (flag names, error output, exit codes) for consistency
  - `tint.go`: Provides the Color struct we should use — don't reinvent
  - `evrp.go`: Single source of truth for format constants
  - `cosmetics-cdn-format.md`: The contract. If pack-tint output doesn't match spec, Track B will fail

  **Acceptance Criteria**:
  - [ ] `go run ./cmd/pack-tint --help` shows usage
  - [ ] Running with valid color flags produces a `.evrp` file
  - [ ] Output file starts with `EVRP` magic bytes
  - [ ] Output file is exactly 28 (header) + 80 (tint data) = 108 bytes
  - [ ] Symbol ID in output matches CSymbol64_Hash of the `--name` value
  - [ ] Hexdump of output matches spec layout

  **QA Scenarios:**

  ```
  Scenario: Pack a test tint and verify binary output
    Tool: Bash
    Preconditions: nevr-cdn-tools repo built, format spec available
    Steps:
      1. Run: go run ./cmd/pack-tint --name "custom_test_tint" --main1 "1.0,0.0,0.0,1.0" --accent1 "0.0,1.0,0.0,1.0" --main2 "0.0,0.0,1.0,1.0" --accent2 "1.0,1.0,0.0,1.0" --body "0.5,0.5,0.5,1.0" --output /tmp/test_tint.evrp
      2. Verify file size: stat -c %s /tmp/test_tint.evrp == 108
      3. Verify magic: hexdump -C -n 4 /tmp/test_tint.evrp | grep "EVRP"
      4. Verify version: hexdump -C -s 4 -n 4 /tmp/test_tint.evrp (should be 01 00 00 00)
      5. Verify slot_type at offset 16: hexdump -C -s 16 -n 1 /tmp/test_tint.evrp (should be 01)
      6. Verify first color (main1 = red = 1.0,0.0,0.0,1.0): hexdump -C -s 28 -n 16 /tmp/test_tint.evrp (should show 00 00 80 3f 00 00 00 00 00 00 00 00 00 00 80 3f)
    Expected Result: File is 108 bytes, magic EVRP, version 1, slot_type 1, color data correct
    Failure Indicators: Wrong file size, bad magic, incorrect color encoding
    Evidence: .sisyphus/evidence/task-7-pack-tint-output.txt

  Scenario: Invalid input handling
    Tool: Bash
    Preconditions: pack-tint built
    Steps:
      1. Run with missing --name: should error with clear message
      2. Run with out-of-range color (2.0): should error or clamp
      3. Run with malformed color string ("abc"): should error
    Expected Result: Clear error messages, non-zero exit codes
    Failure Indicators: Panic, cryptic error, silent failure
    Evidence: .sisyphus/evidence/task-7-pack-tint-errors.txt
  ```

  **Commit**: YES (in nevr-cdn-tools repo)
  - Message: `feat(pack-tint): implement tint packing to .evrp format`
  - Files: `cmd/pack-tint/main.go`, `internal/evrp/evrp.go` (updated with write helpers)
  - Pre-commit: `go build ./...`

---

### Wave 2 — CDN Pipeline + C++ Module (after Wave 1)

- [ ] 8. **Hardcoded tint injection PoC — prove the hook works**

  **What to do**:
  - Implement a minimal hook on `Loadout_ResolveDataFromId` (RVA from Task 2) using `PatchDetour()`
  - The hook should:
    1. Call the original function (let the game resolve normally)
    2. Check if the resolved loadout has a tint slot
    3. Overwrite the tint color data at the known offset with HARDCODED test colors (e.g., bright neon green)
    4. Log that the override happened: `Log(LOG_INFO, "[AssetCDN] Tint override applied for slot %d", slotIndex)`
  - This proves: the hook target is correct, the memory offset is writable, and the game renders the overridden tint
  - Use findings from Task 2 (RE validation) for exact offsets and hook strategy
  - Use `symbol_hash.h` from Task 4 if needed to identify tint SymbolIds
  - Implement in `asset_cdn.cpp` (from Task 5 stubs), keep isolated with a `#define CDN_TINT_POC_ENABLED`
  - Add the hook address to `patch_addresses.h`

  **Must NOT do**:
  - Do not download anything from CDN — hardcoded colors only
  - Do not read from cache — hardcoded colors only
  - Do not hook more than ONE function
  - Do not implement for all slots — one tint slot only
  - Do not add config reading

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Complex C++ hook implementation requiring precise memory manipulation, calling conventions, and game process injection knowledge
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on multiple Phase 0 + Wave 1 tasks)
  - **Parallel Group**: Wave 2
  - **Blocks**: Task 12 (generalized injection needs proven PoC)
  - **Blocked By**: Tasks 1 (format spec), 2 (RE validation), 3 (hash table), 4 (symbol_hash.h), 5 (build system)

  **References**:

  **Pattern References**:
  - `src/common/hooking.h` — `PatchDetour()` API. Follow the exact pattern used by existing hooks
  - `src/gamepatches/patches.cpp` — Existing hook installations (search for `PatchDetour` calls). Follow the same pattern for our new hook
  - `src/gamepatches/patch_addresses.h` — Where to add the new RVA constant
  - `src/gamepatches/asset_cdn.h` + `asset_cdn.cpp` — Stub files from Task 5. Implement the hook here

  **API/Type References**:
  - Evidence from Task 2: `.sisyphus/evidence/task-2-hook-decompilation.md` — Validated function signature, parameters, return type
  - Evidence from Task 2: `.sisyphus/evidence/task-2-cresourceid-layout.md` — CResourceID struct layout
  - `src/common/echovr.h` — LoadoutSlot enum, SymbolId type
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — LoadoutData offsets for tint color data

  **WHY Each Reference Matters**:
  - `hooking.h` + `patches.cpp`: Must follow EXACT existing hook installation pattern or DLL won't load
  - Task 2 evidence: Contains the validated hook strategy — DO NOT deviate from these findings
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: Memory offsets are the difference between working hook and crash

  **Acceptance Criteria**:
  - [ ] Hook compiles and links without errors (`make build`)
  - [ ] Hook address added to `patch_addresses.h`
  - [ ] Hook is gated behind `#define CDN_TINT_POC_ENABLED`
  - [ ] Log message emitted when tint override is applied
  - [ ] Build produces updated gamepatches.dll

  **QA Scenarios:**

  ```
  Scenario: PoC hook compiles and is wired correctly
    Tool: Bash (isBackground: true)
    Preconditions: Tasks 4, 5 complete (symbol_hash.h exists, asset_cdn stubs exist)
    Steps:
      1. Run: make build 2>&1 | tee /tmp/build-poc.txt
      2. Verify exit code 0
      3. Grep for asset_cdn in build output to confirm it compiled
      4. Verify gamepatches.dll was produced
      5. Grep asset_cdn.cpp for PatchDetour call to confirm hook is installed
      6. Grep asset_cdn.cpp for Log call to confirm logging exists
    Expected Result: Clean build, hook installed via PatchDetour, logging present
    Failure Indicators: Link errors, missing PatchDetour call, no Log statement
    Evidence: .sisyphus/evidence/task-8-poc-build.txt

  Scenario: Hook is properly isolated
    Tool: Bash
    Preconditions: asset_cdn.cpp written
    Steps:
      1. Verify #define CDN_TINT_POC_ENABLED exists
      2. Verify the hook installation is inside an #ifdef CDN_TINT_POC_ENABLED block
      3. Verify commenting out the #define results in no hook being installed (check with grep)
    Expected Result: Hook is fully gated behind preprocessor flag
    Failure Indicators: Hook code outside of ifdef, no define found
    Evidence: .sisyphus/evidence/task-8-poc-isolation.txt
  ```

  **Commit**: YES
  - Message: `feat(gamepatches): add hardcoded tint injection PoC hook`
  - Files: `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`, `src/gamepatches/patch_addresses.h`
  - Pre-commit: `make build`

---

- [ ] 9. **Implement build-manifest CLI command**

  **What to do**:
  - Implement `cmd/build-manifest/main.go` in `~/src/nevr-cdn-tools/`
  - CLI interface:
    ```
    build-manifest --input-dir <dir_of_evrp_files> --game-version <version> --output manifest.json
    ```
  - Scan input directory for `.evrp` files
  - For each `.evrp` file:
    - Read and validate header (check magic, version)
    - Extract symbol_id, slot_type
    - Compute SHA256 checksum of entire file
    - Get file size
  - Generate manifest JSON per spec (Task 1):
    ```json
    {
      "version": 1,
      "game_version": "34.4.631399.1",
      "packages": {
        "<symbolid_hex>": {
          "url": "packages/<symbolid_hex>.evrp",
          "sha256": "<sha256>",
          "slot_type": "<type_name>",
          "size": <file_size>
        }
      }
    }
    ```
  - Use `internal/evrp/evrp.go` for reading `.evrp` headers
  - Map slot_type byte → string name (0x01 → "tint")

  **Must NOT do**:
  - Do not upload to CDN — just generate the manifest file
  - Do not validate asset_data contents — just read the header
  - Do not handle anything other than `.evrp` files

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: File I/O, binary parsing, JSON generation, SHA256 computation
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 8, 10, 11)
  - **Parallel Group**: Wave 2
  - **Blocks**: Task 10 (needs manifest for CDN upload)
  - **Blocked By**: Tasks 1 (format spec), 7 (needs .evrp files to test with)

  **References**:

  **Pattern References**:
  - `docs/cosmetics-cdn-format.md` — Manifest JSON schema. Follow EXACTLY
  - `~/src/nevr-cdn-tools/internal/evrp/evrp.go` — Format constants and header struct from Task 6/7
  - `~/src/evrFileTools/cmd/showtints/main.go` — CLI pattern reference

  **WHY Each Reference Matters**:
  - `cosmetics-cdn-format.md`: The manifest schema is the contract with Track B. Any deviation breaks C++ parsing
  - `evrp.go`: Single source of truth for header parsing

  **Acceptance Criteria**:
  - [ ] `go run ./cmd/build-manifest --help` shows usage
  - [ ] Given a directory with `.evrp` files, produces valid `manifest.json`
  - [ ] Manifest JSON validates against the spec schema
  - [ ] SHA256 checksums are correct (verifiable with `sha256sum`)
  - [ ] Symbol IDs in manifest match those in `.evrp` headers

  **QA Scenarios:**

  ```
  Scenario: Build manifest from pack-tint output
    Tool: Bash
    Preconditions: pack-tint works (Task 7), at least one .evrp file exists
    Steps:
      1. Create temp dir: mkdir -p /tmp/cdn-test/packages
      2. Pack a test tint: go run ./cmd/pack-tint --name "test_tint" ... --output /tmp/cdn-test/packages/test.evrp
      3. Run: go run ./cmd/build-manifest --input-dir /tmp/cdn-test/packages --game-version "34.4.631399.1" --output /tmp/cdn-test/manifest.json
      4. Verify manifest.json is valid JSON: cat /tmp/cdn-test/manifest.json | jq .
      5. Verify manifest has "version": 1
      6. Verify manifest has "packages" object with at least one entry
      7. Verify SHA256 in manifest matches: sha256sum /tmp/cdn-test/packages/test.evrp
    Expected Result: Valid manifest JSON with correct checksums and package entries
    Failure Indicators: Invalid JSON, wrong checksums, missing fields
    Evidence: .sisyphus/evidence/task-9-build-manifest.txt
  ```

  **Commit**: YES (in nevr-cdn-tools repo)
  - Message: `feat(build-manifest): generate CDN manifest from .evrp packages`
  - Files: `cmd/build-manifest/main.go`, `internal/evrp/evrp.go` (updated with read helpers)
  - Pre-commit: `go build ./...`

---

- [ ] 10. **Pack real custom tint + upload to CDN**

  **What to do**:
  - Use `pack-tint` (Task 7) to create a real custom tint `.evrp` file with distinctive colors
  - Choose colors that are obviously different from any game tint (e.g., bright neon green body, purple accents)
  - Use `build-manifest` (Task 9) to generate the manifest
  - Upload to CDN manually:
    - Upload `.evrp` file to `https://cdn.echo.taxi/v1/packages/<symbolid_hex>.evrp`
    - Upload `manifest.json` to `https://cdn.echo.taxi/v1/manifest.json`
    - Use wrangler CLI (`wrangler r2 object put`) or R2 dashboard
  - Verify uploads via curl:
    - `curl -I https://cdn.echo.taxi/v1/manifest.json` → 200 OK
    - `curl -I https://cdn.echo.taxi/v1/packages/<symbolid_hex>.evrp` → 200 OK
  - Document the exact SymbolId and file paths used

  **Must NOT do**:
  - Do not automate uploads — manual for PoC
  - Do not create multiple tints — one is enough
  - Do not set up CDN routing rules or caching policies

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Run existing tools, manual upload, curl verification
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (sequential: pack → manifest → upload)
  - **Parallel Group**: Wave 2
  - **Blocks**: Task 11 (CDN must have content before fetch module is tested)
  - **Blocked By**: Tasks 7 (pack-tint), 9 (build-manifest)

  **References**:

  **Pattern References**:
  - Evidence from Task 3: `.sisyphus/evidence/task-3-tint-data-source.txt` — Color values to use
  - `docs/cosmetics-cdn-format.md` — CDN URL scheme for upload paths

  **WHY Each Reference Matters**:
  - Task 3 evidence: Defines the actual color data we're using for the PoC tint
  - Format spec: Defines exact URL paths for upload destinations

  **Acceptance Criteria**:
  - [ ] One `.evrp` file exists locally
  - [ ] `manifest.json` exists locally and references the .evrp file
  - [ ] `curl -s https://cdn.echo.taxi/v1/manifest.json | jq .` returns valid JSON
  - [ ] `curl -s -o /tmp/cdn-test.evrp https://cdn.echo.taxi/v1/packages/<id>.evrp && hexdump -C -n 4 /tmp/cdn-test.evrp` shows `EVRP`
  - [ ] Downloaded file matches local file: `diff <local> /tmp/cdn-test.evrp`

  **QA Scenarios:**

  ```
  Scenario: CDN content is accessible and correct
    Tool: Bash
    Preconditions: Files uploaded to R2
    Steps:
      1. curl -s -w "%{http_code}" https://cdn.echo.taxi/v1/manifest.json -o /tmp/cdn-manifest.json
      2. Verify HTTP 200
      3. Verify manifest is valid JSON: jq . /tmp/cdn-manifest.json
      4. Extract first package URL from manifest: jq -r '.packages | to_entries[0].value.url' /tmp/cdn-manifest.json
      5. Download package: curl -s -o /tmp/cdn-package.evrp https://cdn.echo.taxi/v1/<url_from_step_4>
      6. Verify magic bytes: hexdump -C -n 4 /tmp/cdn-package.evrp | grep EVRP
      7. Verify SHA256 matches manifest: sha256sum /tmp/cdn-package.evrp vs jq '.packages | to_entries[0].value.sha256' /tmp/cdn-manifest.json
    Expected Result: Manifest downloadable, package downloadable, checksums match
    Failure Indicators: HTTP errors, invalid JSON, checksum mismatch, wrong magic bytes
    Evidence: .sisyphus/evidence/task-10-cdn-verification.txt
  ```

  **Commit**: NO (CDN upload, no code changes)

---

- [ ] 11. **AssetCDN module — manifest fetch + package download + local cache**

  **What to do**:
  - Implement the core AssetCDN module in `asset_cdn.h` / `asset_cdn.cpp` (replacing Task 5 stubs):
  - **Manifest fetch**:
    - On `Initialize()`, spawn a background thread
    - Background thread uses libcurl to GET `https://cdn.echo.taxi/v1/manifest.json`
    - Parse JSON response with jsoncpp
    - Store parsed manifest in thread-safe data structure
  - **Package download**:
    - For each package in manifest, check if cached file exists at `%LOCALAPPDATA%\\EchoVR\\cosmetics\\packages\\<symbolid_hex>.evrp`
    - If not cached: download via curl to `.tmp` file, rename to final path on completion (atomic write)
    - If cached: verify file size matches manifest (skip SHA256 for PoC — too slow)
  - **Local cache**:
    - Create cache directory recursively if it doesn't exist: `%LOCALAPPDATA%\\EchoVR\\cosmetics\\packages\\`
    - Cache path is HARDCODED (no config system)
  - **State management**:
    - Track download state: `NOT_STARTED`, `DOWNLOADING`, `READY`, `FAILED`
    - Provide `IsReady()` — returns true when manifest parsed and all packages cached
    - Provide `GetTintData(SymbolId id)` — returns pointer to 80-byte tint color data from cached `.evrp` file, or nullptr if not found
  - **Error handling**:
    - If manifest fetch fails: log warning, set state to FAILED, game continues normally
    - If package download fails: log warning per package, skip that package
    - Never crash, never block game thread
  - **Async pattern**: Use `std::thread` + `curl_easy_perform` (not curl_multi). One thread for all downloads sequentially. Mutex for shared state.

  **Must NOT do**:
  - Do not block game thread — all HTTP on background thread
  - Do not implement retry logic — single attempt per URL
  - Do not implement cache eviction — cache grows forever
  - Do not validate SHA256 — trust the download (PoC only)
  - Do not implement hot-reload — download once at startup
  - Do not add config for CDN URL or cache path — hardcoded

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Multi-threaded C++ with curl, jsoncpp, file I/O, error handling, thread safety
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 8, 9)
  - **Parallel Group**: Wave 2
  - **Blocks**: Tasks 12 (tint injection needs cached data), 13 (startup needs module)
  - **Blocked By**: Tasks 1 (format spec for parsing), 5 (curl/jsoncpp linked), 10 (CDN has content to download)

  **References**:

  **Pattern References**:
  - `src/gamepatches/asset_cdn.h` + `asset_cdn.cpp` — Stub files from Task 5. Replace stubs with real implementation
  - `src/common/logging.h` — `Log(level, format, ...)` for all output. Use `LOG_INFO` for progress, `LOG_WARNING` for errors
  - `src/gamepatches/patches.cpp` — Existing thread patterns (search for `std::thread` or `CreateThread`). Follow same threading approach
  - `docs/cosmetics-cdn-format.md` — Manifest JSON schema and `.evrp` binary layout for parsing

  **API/Type References**:
  - `src/common/echovr.h:SymbolId` — INT64 type used as map key for `GetTintData()`
  - `vcpkg.json` — curl and jsoncpp version/feature constraints

  **External References**:
  - curl easy API: `curl_easy_init()`, `curl_easy_setopt()`, `curl_easy_perform()`, `curl_easy_cleanup()`
  - jsoncpp: `Json::Reader`, `Json::Value`, `reader.parse(string, root)`
  - Windows API: `SHGetFolderPathA(CSIDL_LOCAL_APPDATA)` or `std::getenv("LOCALAPPDATA")` for cache path
  - Windows API: `CreateDirectoryA()` or `std::filesystem::create_directories()` for cache dir creation

  **WHY Each Reference Matters**:
  - `asset_cdn.cpp` stubs: Starting point — expand, don't replace from scratch
  - `logging.h`: All output MUST use the logging framework, not printf/cout
  - `patches.cpp`: Threading patterns must match existing codebase conventions
  - Format spec: Parsing code must exactly match the documented JSON schema and binary layout

  **Acceptance Criteria**:
  - [ ] `make build` succeeds
  - [ ] `Initialize()` spawns background thread (grep for `std::thread`)
  - [ ] Background thread fetches manifest via curl (grep for `curl_easy`)
  - [ ] Manifest JSON parsed via jsoncpp
  - [ ] Packages downloaded to `%LOCALAPPDATA%\\EchoVR\\cosmetics\\packages\\`
  - [ ] Atomic write: download to `.tmp`, rename to final
  - [ ] Cache directory created recursively if missing
  - [ ] `GetTintData(SymbolId)` returns pointer to color data from `.evrp` file
  - [ ] All errors logged, never crash, never block game thread

  **QA Scenarios:**

  ```
  Scenario: Module compiles and API is complete
    Tool: Bash (isBackground: true)
    Preconditions: Tasks 5, 10 complete
    Steps:
      1. make build 2>&1 | tee /tmp/build-cdn.txt
      2. Verify exit code 0
      3. Grep asset_cdn.h for: Initialize, Shutdown, IsReady, GetTintData declarations
      4. Grep asset_cdn.cpp for: curl_easy, json, std::thread, CreateDirectory or create_directories
      5. Grep asset_cdn.cpp for: LOG_WARNING or LOG_INFO (logging present)
      6. Grep asset_cdn.cpp for: .tmp (atomic write pattern)
    Expected Result: Clean build, all API functions present, curl/json/thread usage, logging, atomic writes
    Failure Indicators: Missing API functions, no curl usage, no thread, no logging
    Evidence: .sisyphus/evidence/task-11-assetcdn-build.txt

  Scenario: No game-thread blocking
    Tool: Bash
    Preconditions: asset_cdn.cpp written
    Steps:
      1. Search for curl_easy_perform in asset_cdn.cpp
      2. Verify ALL curl_easy_perform calls are inside the background thread function (not in Initialize or GetTintData)
      3. Verify Initialize() returns immediately (just spawns thread)
      4. Verify GetTintData() only reads from local cache (no network calls)
    Expected Result: All network I/O is in the background thread function
    Failure Indicators: curl_easy_perform in Initialize or GetTintData, blocking calls on game thread
    Evidence: .sisyphus/evidence/task-11-no-blocking.txt
  ```

  **Commit**: YES
  - Message: `feat(gamepatches): implement AssetCDN manifest fetch and package cache`
  - Files: `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`
  - Pre-commit: `make build`

---

### Wave 3 — Integration (after Waves 1-2)

- [ ] 12. **Tint injection from cached .evrp files**

  **What to do**:
  - Evolve the hardcoded PoC hook (Task 8) to read tint data from the AssetCDN cache (Task 11)
  - Replace the hardcoded color values with a call to `AssetCDN::GetTintData(symbolId)`
  - Hook flow:
    1. Original function resolves loadout data
    2. Post-hook reads the resolved tint SymbolId
    3. Calls `AssetCDN::GetTintData(symbolId)` — returns 80-byte color data or nullptr
    4. If found: memcpy the 80 bytes over the resolved tint color region in LoadoutData
    5. If not found (nullptr): do nothing, let game use its own tint
  - Remove the `#define CDN_TINT_POC_ENABLED` gate — this is now the real implementation
  - Add guard: only apply override if `AssetCDN::IsReady()` returns true
  - Handle the tint slot specifically — check slot_type before applying
  - Log when a CDN tint override is applied vs when falling back to game tint

  **Must NOT do**:
  - Do not handle mesh/texture/geometry slots — tints only
  - Do not block if CDN isn't ready — fall through to game tint
  - Do not modify AssetCDN module — use its public API only
  - Do not handle multiple slots — tint slot only for PoC

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Precise memory manipulation, hook modification, integration between modules
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (with Task 13, but 13 also depends on 12)
  - **Blocks**: Task 13 (startup integration)
  - **Blocked By**: Tasks 4 (symbol_hash), 8 (PoC hook), 11 (AssetCDN module)

  **References**:

  **Pattern References**:
  - `src/gamepatches/asset_cdn.cpp` — PoC hook code from Task 8. Evolve this, don't rewrite from scratch
  - `src/gamepatches/asset_cdn.h` — AssetCDN public API: `IsReady()`, `GetTintData(SymbolId)`
  - Evidence from Task 2: `.sisyphus/evidence/task-2-hook-decompilation.md` — Memory offsets for tint color data

  **API/Type References**:
  - `src/common/echovr.h:SymbolId` — INT64 used to query GetTintData
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — LoadoutData offsets
  - `docs/cosmetics-cdn-format.md` — asset_data is 80 bytes for tints (5 colors × 16 bytes)

  **WHY Each Reference Matters**:
  - `asset_cdn.cpp` (Task 8): The PoC hook structure is already correct — just swap hardcoded values for cache lookup
  - `asset_cdn.h` API: GetTintData returns exactly what we need to memcpy

  **Acceptance Criteria**:
  - [ ] PoC `#define CDN_TINT_POC_ENABLED` removed
  - [ ] Hook calls `AssetCDN::GetTintData()` instead of using hardcoded colors
  - [ ] Nullptr check: if CDN has no tint for this SymbolId, game tint is preserved
  - [ ] IsReady() check: if CDN module not ready, skip all overrides
  - [ ] `make build` succeeds

  **QA Scenarios:**

  ```
  Scenario: Cache-based injection compiles and is wired correctly
    Tool: Bash (isBackground: true)
    Preconditions: Tasks 8, 11 complete
    Steps:
      1. make build 2>&1 | tee /tmp/build-inject.txt
      2. Verify exit code 0
      3. Grep asset_cdn.cpp for GetTintData — should be called in hook
      4. Grep asset_cdn.cpp for IsReady — should be checked before override
      5. Grep asset_cdn.cpp for nullptr — null check must exist
      6. Verify CDN_TINT_POC_ENABLED is NOT in the codebase (removed)
    Expected Result: Clean build, GetTintData used, IsReady checked, null-safe, PoC flag removed
    Failure Indicators: PoC flag still present, no null check, no IsReady guard
    Evidence: .sisyphus/evidence/task-12-injection-build.txt
  ```

  **Commit**: YES (combined with Task 13)
  - Message: `feat(gamepatches): integrate tint injection with AssetCDN pipeline`
  - Files: `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`
  - Pre-commit: `make build`

---

- [ ] 13. **Startup integration — wire AssetCDN::Initialize() into game startup**

  **What to do**:
  - Uncomment and update the `AssetCDN::Initialize()` call at `patches.cpp:1430-1433`
  - Uncomment and update the `asset_cdn_url` config reading at `patches.cpp:760-767` (or remove if we're hardcoding the URL)
  - Ensure `#include "asset_cdn.h"` is at the top of patches.cpp
  - Call `AssetCDN::Initialize()` at the right point in startup — after config is loaded, before game enters main loop
  - Call `AssetCDN::Shutdown()` at the appropriate shutdown point (thread join, curl cleanup)
  - Add game version guard:
    - Read game version from known location (or compare against hardcoded expected version)
    - If game version doesn't match, log warning and skip CDN initialization
    - Document the exact game version this targets
  - Integration test: build the full DLL, verify it would initialize AssetCDN at startup

  **Must NOT do**:
  - Do not add config system — CDN URL is hardcoded in asset_cdn.cpp
  - Do not add game version auto-detection — hardcoded version check
  - Do not add UI or user-visible status

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Integration task touching existing code (patches.cpp), needs careful placement
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (after Task 12)
  - **Blocks**: F1-F4 (final verification)
  - **Blocked By**: Tasks 11 (AssetCDN module), 12 (tint injection)

  **References**:

  **Pattern References**:
  - `src/gamepatches/patches.cpp:1430-1433` — Commented `AssetCDN::Initialize()` call. Uncomment and update
  - `src/gamepatches/patches.cpp:760-767` — Commented `asset_cdn_url` config. Decide: uncomment or remove
  - `src/gamepatches/patches.cpp` — Search for other `::Initialize()` calls to see the startup order pattern
  - `src/gamepatches/patches.cpp` — Search for shutdown/cleanup patterns

  **API/Type References**:
  - `src/gamepatches/asset_cdn.h` — Public API: `Initialize()`, `Shutdown()`

  **WHY Each Reference Matters**:
  - `patches.cpp:1430-1433`: This is the EXACT integration point — prior developer already chose where Initialize goes
  - Other `::Initialize()` calls: Shows the startup ordering convention — our init must fit in the sequence
  - Shutdown patterns: Must clean up curl and join background thread

  **Acceptance Criteria**:
  - [ ] `AssetCDN::Initialize()` called during startup (uncommented, updated)
  - [ ] `AssetCDN::Shutdown()` called during shutdown
  - [ ] `#include "asset_cdn.h"` in patches.cpp
  - [ ] Game version guard logs warning if version mismatch
  - [ ] `make build` succeeds with full integration

  **QA Scenarios:**

  ```
  Scenario: Startup integration compiles and is correctly placed
    Tool: Bash (isBackground: true)
    Preconditions: Tasks 11, 12 complete
    Steps:
      1. make build 2>&1 | tee /tmp/build-final.txt
      2. Verify exit code 0
      3. Grep patches.cpp for AssetCDN::Initialize — should NOT be commented out
      4. Grep patches.cpp for AssetCDN::Shutdown — should exist
      5. Grep patches.cpp for '#include "asset_cdn.h"' — should exist
      6. Verify the Initialize call is after config loading (check line ordering)
    Expected Result: Clean build, Initialize/Shutdown wired, include present
    Failure Indicators: Still commented, missing shutdown, include missing
    Evidence: .sisyphus/evidence/task-13-startup-integration.txt

  Scenario: Game version guard exists
    Tool: Bash
    Preconditions: patches.cpp or asset_cdn.cpp updated
    Steps:
      1. Grep for game version string (e.g., "34.4.631399.1") in asset_cdn.cpp or patches.cpp
      2. Verify there's a version comparison before Initialize proceeds
      3. Verify a LOG_WARNING is emitted if version doesn't match
    Expected Result: Version guard present with logging
    Failure Indicators: No version check, no warning on mismatch
    Evidence: .sisyphus/evidence/task-13-version-guard.txt
  ```

  **Commit**: YES (combined with Task 12)
  - Message: `feat(gamepatches): integrate tint injection with AssetCDN pipeline`
  - Files: `src/gamepatches/patches.cpp`, `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`
  - Pre-commit: `make build`

---

<!-- TASKS_END -->

---

## Final Verification Wave

> 4 review agents run in PARALLEL. ALL must APPROVE. Rejection → fix → re-run.

- [ ] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, curl endpoint, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in `.sisyphus/evidence/`. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [ ] F2. **Code Quality Review** — `unspecified-high`
  Run `make build` for C++ and `go build ./...` for Go tools. Review all changed files for: `as any`/`@ts-ignore` equivalents, empty catches, debug prints in prod, commented-out code, unused imports. Check AI slop: excessive comments, over-abstraction, generic variable names.
  Output: `Build [PASS/FAIL] | Lint [PASS/FAIL] | Files [N clean/N issues] | VERDICT`

- [ ] F3. **Integration QA** — `unspecified-high`
  Start from clean state. Execute EVERY QA scenario from EVERY task — follow exact steps, capture evidence. Test cross-task integration: pack-tint → build-manifest → curl CDN → verify cached file. Save to `.sisyphus/evidence/final-qa/`.
  Output: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | VERDICT`

- [ ] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff (git log/diff). Verify 1:1 — everything in spec was built (no missing), nothing beyond spec was built (no creep). Check "Must NOT do" compliance. Detect cross-task contamination: Task N touching Task M's files. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

| Group | Message | Files | Pre-commit |
|-------|---------|-------|------------|
| Phase 0 | `docs(cosmetics): add CDN format specification` | `docs/cosmetics-cdn-format.md` | n/a |
| Task 4 | `feat(common): add constexpr CSymbol64_Hash implementation` | `src/common/symbol_hash.h` | compile test |
| Task 5 | `build(gamepatches): link curl and jsoncpp dependencies` | `src/gamepatches/CMakeLists.txt` | `make build` |
| Tasks 6-7 | Committed in `~/src/nevr-cdn-tools/` repo separately | all files | `go build ./...` |
| Task 8 | `feat(gamepatches): add hardcoded tint injection PoC hook` | `asset_cdn.*`, `patches.cpp` | `make build` |
| Tasks 9-10 | Committed in `~/src/nevr-cdn-tools/` repo separately | manifest files | `go build ./...` |
| Task 11 | `feat(gamepatches): add AssetCDN manifest fetch and package cache` | `asset_cdn.*` | `make build` |
| Tasks 12-13 | `feat(gamepatches): integrate tint injection with AssetCDN pipeline` | `asset_cdn.*`, `patches.cpp` | `make build` |

---

## Success Criteria

### Verification Commands
```bash
# C++ builds cleanly
make build  # Expected: zero errors, zero warnings

# Go tools build cleanly
cd ~/src/nevr-cdn-tools && go build ./...  # Expected: zero errors

# Pack a test tint
cd ~/src/nevr-cdn-tools && go run ./cmd/pack-tint --help  # Expected: usage output

# Build manifest
cd ~/src/nevr-cdn-tools && go run ./cmd/build-manifest --help  # Expected: usage output

# CDN manifest is reachable
curl -s https://cdn.echo.taxi/v1/manifest.json | head -c 200  # Expected: valid JSON

# Cached files exist after game startup
ls %LOCALAPPDATA%/EchoVR/cosmetics/  # Expected: manifest.json + .evrp files
```

### Final Checklist
- [ ] Format spec written and internally consistent
- [ ] Go tools build, run, and produce valid output files
- [ ] C++ builds with zero errors/warnings
- [ ] symbol_hash.h produces correct hashes for all test vectors
- [ ] AssetCDN module fetches manifest and downloads packages
- [ ] At least one .evrp file on CDN and downloadable
- [ ] Hook compiles and is wired into startup via PatchDetour
- [ ] All "Must Have" present
- [ ] All "Must NOT Have" absent
