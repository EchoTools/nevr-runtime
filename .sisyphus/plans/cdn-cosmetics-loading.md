# CDN Cosmetics Loading System

## TL;DR

> **Quick Summary**: Build a complete CDN-based cosmetics loading system for Echo VR — define manifest/package binary formats, create Go CLI tools to build them, implement C++ game hooks to download custom cosmetic assets from `https://cdn.echo.taxi/` (Cloudflare R2), cache in `%LOCALAPPDATA%`, and inject into the game's rendering pipeline via resource resolution hooks.
>
> **Deliverables**:
> - `src/common/symbol_hash.h` — constexpr C++ implementation of CSymbol64_Hash
> - `src/gamepatches/asset_cdn.h` + `asset_cdn.cpp` — AssetCDN module (manifest fetch, package download, local cache, resource injection)
> - `tools/cosmetics-manifest/` — Go CLI to build versioned manifest files
> - `tools/cosmetics-package/` — Go CLI to build asset package files
> - `docs/cosmetics-cdn-format.md` — Format specification document
> - Ghidra RE deliverables: validated hook targets, binary format understanding, unknown-SymbolId behavior documentation
>
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 5 waves
> **Critical Path**: Task 1 → Task 6 → Task 12 → Task 13

---

## Context

### Original Request
User wants to integrate cosmetics loading from a CDN. Needs manifest/package file format definitions, CLI tools to create them, and game client hooks to download, cache, and render custom cosmetic assets. These are NEW assets (not overwrites). Cached in `%LOCALAPPDATA%`. Hosted on Cloudflare R2 at `https://cdn.echo.taxi/`. Data files must be versioned and immediately auditable. CDN is never human-read.

### Interview Summary
**Key Discussions**:
- **Asset type**: New binary asset data — pre-processed GPU-ready (vertex/index buffers, DDS textures)
- **Format**: Custom format + deep hook — NOT game-native packages. We define our own container, hook deep enough to inject
- **Hook strategy**: Resource resolution level — `FUN_1404f37a0` @ RVA `0x004F37A0` resolves loadout data → CResourceID. This is our injection point
- **Slot scope**: All 20 cosmetic equip slots from day one
- **CLI tools**: Go on Linux (single-binary, easy cross-compile)
- **Update strategy**: On game startup — fetch manifest once, download missing/updated packages
- **CDN URL**: `https://cdn.echo.taxi/` with versioned, immediately auditable paths
- **Network replication**: SymbolIds already broadcast to other players. What they see without assets is UNKNOWN — RE task required
- **Integrity**: None needed — trust the R2 bucket
- **Test strategy**: No automated unit tests — agent-executed QA scenarios only

**Research Findings**:
- Mapped complete project structure: C++ DLLs injected into Echo VR via gamepatches.dll (deployed as dbgcore.dll)
- Existing hooking: MinHook/Detours via `PatchDetour()` — well-established pattern
- Commented-out `AssetCDN::Initialize()` at patches.cpp:1430-1433 and `asset_cdn_url` config at patches.cpp:760-767 — prior intent to build this
- `CSymbol64_Hash` @ 0x1400CE120 — case-insensitive, polynomial 0x95ac9329ac4bc9b5. 21,122 unique hashes captured. New SymbolIds CAN be computed for custom cosmetics
 Go hash implementation exists at `extras/reference/core_hash.go` (vendored from Nakama server)
- `CResourceID` = int64 SymbolId hash (lookup key for game assets)
- Existing mesh dump hooks (DISABLED) at AsyncResourceIOCallback (0x0FA16D0) and CGMeshListResource::DeserializeAndUpload (0x0547AB0)
- ECHOVR_STRUCT_MEMORY_MAP.md has complete LoadoutData structure with exact offsets
- sourcedb/ JSON files map the full cosmetic ecosystem

### Metis Review
**Identified Gaps** (addressed):
- **FUN_1404f37a0 must be validated via Ghidra**: Added as explicit RE task (Task 2) before any hook implementation
- **Vertical slice approach strongly recommended**: Plan restructured — Wave 2 proves injection works with ONE hardcoded asset before building full pipeline
- **jsoncpp not linked in gamepatches CMakeLists.txt**: Added as Task 5 prerequisite
- **No blocking HTTP in hooks**: Guardrail added — all HTTP in background thread, never in game thread
- **Game version detection needed**: Added to Task 13 — guard against wrong game version
- **Fallback paths required**: Guardrail — graceful degradation if CDN unreachable or asset missing

---

## Work Objectives

### Core Objective
Implement a complete pipeline: define formats → build tools → hook game → download assets → cache locally → inject into rendering. Validated via vertical slice (one slot, one asset) before full integration.

### Concrete Deliverables
1. `src/common/symbol_hash.h` — constexpr C++ CSymbol64_Hash (matching Go implementation)
2. `src/gamepatches/asset_cdn.h` + `asset_cdn.cpp` — AssetCDN module
3. `tools/cosmetics-manifest/main.go` — Go CLI tool for manifest generation
4. `tools/cosmetics-package/main.go` — Go CLI tool for package building
5. `docs/cosmetics-cdn-format.md` — Format specification (manifest JSON schema, package binary layout, URL scheme)
6. RE documentation: hook validation, binary format analysis, unknown-SymbolId behavior

### Definition of Done
 [ ] `symbol_hash.h` computes identical hashes to Go `extras/reference/core_hash.go` for all test vectors
- [ ] AssetCDN module downloads manifest from `https://cdn.echo.taxi/`, caches packages in `%LOCALAPPDATA%/EchoVR/cosmetics/`
- [ ] At least ONE custom cosmetic renders correctly on ONE slot in-game (vertical slice proof)
- [ ] Go CLI tools produce valid manifest and package files
- [ ] Format spec document is complete and matches implementation
- [ ] Project builds cleanly with `make build` (or equivalent CMake)

### Must Have
- constexpr C++ symbol hash matching Go implementation exactly
- Manifest format: versioned, binary-auditable, contains SymbolId → package mappings
- Package format: versioned, contains GPU-ready binary asset data
- Local cache in `%LOCALAPPDATA%/EchoVR/cosmetics/` (or similar user-local path)
- Background HTTP — never block game thread
- Graceful degradation — game functions normally if CDN unreachable
- All 20 cosmetic slot types supported in manifest schema (even if only 1 tested in vertical slice)
- Game version guard — detect and warn on unsupported game version

### Must NOT Have (Guardrails)
- **No audio, animation, or shader assets** — meshes and textures only in v1
- **No in-game UI** for cosmetic selection — server assigns via loadout
- **No hot-reload** — assets loaded at startup, require restart for updates
- **No multi-player visibility guarantee in v1** — document the unknown behavior, don't try to solve it yet
- **No cache eviction policy** — cache grows unbounded in v1
- **No retry logic** — single attempt per download, fail gracefully
- **No blocking HTTP in game hooks** — all network I/O on background thread
- **No human-readable CDN paths** — binary/hash-based, versioned and auditable
- **No assumptions about game internals without exact memory addresses** — all hooks validated via Ghidra RE first
- **No overwrites of existing game assets** — only NEW assets with NEW SymbolIds

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.
> Acceptance criteria requiring "user manually tests/confirms" are FORBIDDEN.

### Test Decision
- **Infrastructure exists**: YES (CMake build system)
- **Automated tests**: None (user decision — QA via agent-executed scenarios)
- **Framework**: N/A
- **Rationale**: Cross-compiled C++ DLL targeting game process — standard unit testing not practical. Go CLI tools could be tested but user opted for QA-only.

### QA Policy
Every task MUST include agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **C++ compilation**: Use Bash — `make build` or CMake commands, verify zero errors/warnings
- **Go CLI tools**: Use Bash — build, run with test inputs, validate output files
- **Binary format validation**: Use Bash — hexdump, file inspection, format compliance checks
- **RE tasks**: Use notghidra tools — decompile, disassemble, validate addresses
- **Hash verification**: Use Bash — run Go hash and C++ hash, compare outputs
- **Game integration**: Use Bash — verify DLL builds, hooks compile, no link errors

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Foundation — prerequisites + parallel RE):
├── Task 1: symbol_hash.h implementation [quick]
├── Task 2: RE — Validate FUN_1404f37a0 hook target [deep]
├── Task 3: RE — Analyze CGMeshListResource binary format [deep]
├── Task 4: RE — Investigate unknown SymbolId behavior [deep]
└── Task 5: Build system prep — jsoncpp linking + source scaffolding [quick]

Wave 2 (Vertical Slice — prove injection works):
├── Task 6: Hardcoded injection PoC [deep]
└── Task 7: Hash consistency verification — Go vs C++ [quick]

Wave 3 (Format Design + CDN Pipeline):
├── Task 8: Design manifest + package formats + URL scheme [unspecified-high]
├── Task 9: Go CLI — cosmetics-manifest tool [unspecified-high]
├── Task 10: Go CLI — cosmetics-package tool [unspecified-high]
└── Task 11: AssetCDN module — manifest fetch + download + cache [deep]

Wave 4 (Integration — full pipeline):
├── Task 12: Resource injection hook — generalize to all 20 slots [deep]
├── Task 13: AssetCDN initialization — startup integration [unspecified-high]
└── Task 14: Format specification document [writing]

Wave FINAL (Verification — 4 parallel reviews):
├── F1: Plan compliance audit [oracle]
├── F2: Code quality review [unspecified-high]
├── F3: Integration QA [unspecified-high]
└── F4: Scope fidelity check [deep]

Critical Path: Task 1 → Task 6 → Task 12 → Task 13
Parallel Speedup: ~60% faster than sequential
Max Concurrent: 5 (Wave 1)
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 6, 7 | 1 |
| 2 | — | 6 | 1 |
| 3 | — | 8 | 1 |
| 4 | — | (documentation only) | 1 |
| 5 | — | 6, 11 | 1 |
| 6 | 1, 2, 5 | 12 | 2 |
| 7 | 1 | 9, 10 | 2 |
| 8 | 3 | 9, 10, 11, 14 | 3 |
| 9 | 7, 8 | 13 | 3 |
| 10 | 7, 8 | 13 | 3 |
| 11 | 5, 8 | 12, 13 | 3 |
| 12 | 6, 11 | 13 | 4 |
| 13 | 9, 10, 11, 12 | F1-F4 | 4 |
| 14 | 8 | F1 | 4 |
| F1-F4 | 13, 14 | — | FINAL |

### Agent Dispatch Summary

| Wave | Tasks | Categories |
|------|-------|------------|
| 1 | 5 tasks | T1 → `quick`, T2-T4 → `deep`, T5 → `quick` |
| 2 | 2 tasks | T6 → `deep`, T7 → `quick` |
| 3 | 4 tasks | T8 → `unspecified-high`, T9-T10 → `unspecified-high`, T11 → `deep` |
| 4 | 3 tasks | T12 → `deep`, T13 → `unspecified-high`, T14 → `writing` |
| FINAL | 4 tasks | F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep` |

---

## TODOs

> Implementation + Verification = ONE Task. Never separate.
> EVERY task MUST have: Recommended Agent Profile + Parallelization info + QA Scenarios.

<!-- TASKS_START -->


### Wave 1 — Foundation (All 5 tasks start immediately, no dependencies)

- [ ] 1. **Implement constexpr CSymbol64_Hash in C++ header**

  **What to do**:
  - Create `src/common/symbol_hash.h` with a constexpr implementation of the CSymbol64_Hash algorithm
  - Algorithm: case-insensitive CRC64 with polynomial `0x95ac9329ac4bc9b5`, seed `0xFFFFFFFFFFFFFFFF`
  - Must handle null terminator, lowercase conversion, and the exact polynomial reduction loop
  - Port from the Go implementation at `extras/reference/core_hash.go` (vendored into this repo)
  - Provide both constexpr (compile-time) and runtime variants
  - Include a `SymbolHash(const char* str)` convenience function
  - Reference the existing plan `.sisyphus/plans/symbol-hash-implementation.md` for detailed specifications

  **Must NOT do**:
  - Do not modify any existing files — this is a NEW header only
  - Do not add unit test framework — verification is via QA scenario
  - Do not attempt to hook or patch anything — this is a pure utility

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single header file, well-defined algorithm, direct port from Go
  - **Skills**: []
    - No special skills needed — straightforward C++ implementation

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 2, 3, 4, 5)
  - **Blocks**: Tasks 6, 7
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `extras/reference/core_hash.go` — Go implementation to port. Contains the polynomial, seed, and case-insensitive logic. This is the AUTHORITATIVE reference for correctness. Vendored from the Nakama server repo.
  - `src/common/symbols.h` — Existing SymbolId type definition (`typedef INT64 SymbolId`) and predefined constants. New header must be compatible with this type.
  - `.sisyphus/plans/symbol-hash-implementation.md` — Detailed plan with test vectors, API design, and implementation notes. Follow this plan's specifications.

  **API/Type References**:
  - `src/common/echovr.h:SymbolId` — The `INT64` typedef that hash results must be compatible with

  **External References**:
  - `extras/docs/HASH_DISCOVERY_COMPLETE.md` — Documents the hash algorithm discovery: polynomial `0x95ac9329ac4bc9b5`, seed `0xFFFFFFFFFFFFFFFF`, case-insensitive, includes null terminator

  **WHY Each Reference Matters**:
  - `extras/reference/core_hash.go`: This is the ONLY verified-correct implementation. Port exactly, do not improvise.
  - `symbols.h`: Must return `SymbolId` (INT64) type for compatibility with rest of codebase.
  - `symbol-hash-implementation.md`: Contains test vectors like `rwd_tint_0019 → 0x74d228d09dc5dd8f` for verification.

  **Acceptance Criteria**:
  - [ ] File `src/common/symbol_hash.h` exists and compiles with MSVC/MinGW
  - [ ] Header is self-contained (no dependencies beyond standard library)
  - [ ] `constexpr` qualification allows compile-time hash computation

  **QA Scenarios:**

  ```
  Scenario: Hash function produces correct output for known test vectors
    Tool: Bash
    Preconditions: Project builds successfully
    Steps:
      1. Create a minimal C++ test program that #includes symbol_hash.h
      2. Compute hashes for: "rwd_tint_0019", "decal_default", "" (empty string)
      3. Print results as hex
      4. Compare against known values from extras/reference/core_hash.go (run Go program with same inputs)
    Expected Result: All hash values match exactly between C++ and Go
    Failure Indicators: Any hash mismatch, compilation error, or linker error
    Evidence: .sisyphus/evidence/task-1-hash-test-vectors.txt

  Scenario: constexpr compilation — hash used in static_assert
    Tool: Bash
    Preconditions: symbol_hash.h exists
    Steps:
      1. Create test program with: static_assert(SymbolHash("test") != 0, "hash failed");
      2. Compile with MSVC or MinGW
    Expected Result: Compiles without error (proves constexpr works at compile time)
    Failure Indicators: Compiler error about non-constant expression
    Evidence: .sisyphus/evidence/task-1-constexpr-verify.txt
  ```

  **Commit**: YES
  - Message: `feat(common): add constexpr CSymbol64_Hash implementation`
  - Files: `src/common/symbol_hash.h`
  - Pre-commit: compile test program

---

- [ ] 2. **RE — Validate FUN_1404f37a0 as hook target via Ghidra**

  **What to do**:
  - Use notghidra tools to decompile `FUN_1404f37a0` (RVA `0x004F37A0`, absolute `0x1404F37A0`)
  - Determine: function signature, parameters, return type
  - Confirm it resolves loadout_id → LoadoutData pointer
  - Map the CResourceID access pattern at LoadoutData+0x370 (primary at +0xB8, fallback at +0x80)
  - Identify the slot indexing mechanism (how slot ID maps to CResourceID array index)
  - Document the full call chain: `net_apply_loadout_items_to_player` → `FUN_1404f37a0` → LoadoutData → CResourceID
  - Determine if this function can be safely hooked (no inline, no tail-call, sufficient prologue bytes)
  - Save findings to `docs/ghidra/FUN_1404f37a0_analysis.md`

  **Must NOT do**:
  - Do not write any C++ hook code — this is analysis only
  - Do not assume behavior — document exactly what the decompilation shows
  - Do not modify any source files

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Requires careful RE analysis, Ghidra decompilation, and understanding of game internals
  - **Skills**: []
    - notghidra MCP tools are available by default

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 3, 4, 5)
  - **Blocks**: Task 6 (PoC depends on validated hook target)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `docs/ghidra/GHIDRA_STRUCT_ANALYSIS.md` — Existing analysis of `net_apply_loadout_items_to_player`. Contains the call chain and initial mapping of FUN_1404f37a0. Start here to understand what's already known.
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — Complete LoadoutData, EntrantData, LoadoutSlot structures with exact offsets. Use to cross-reference decompiled struct access patterns.
  - `src/gamepatches/patch_addresses.h` — All known RVAs. Check if FUN_1404f37a0 or related functions already have entries.

  **API/Type References**:
  - `src/common/echovr.h:LoadoutSlot` — The 21-field struct with SymbolId fields. Maps to what FUN_1404f37a0 processes.
  - `src/common/echovr.h:LoadoutEntry` — Parent struct containing LoadoutSlot at offset 0x30.

  **External References**:
  - `extras/docs/MESH_DUMP_HOOKS_README.md` — Documents AsyncResourceIOCallback and CGMeshListResource hooks. These are downstream of FUN_1404f37a0 in the resource loading chain.

  **WHY Each Reference Matters**:
  - `GHIDRA_STRUCT_ANALYSIS.md`: Starting point — contains initial findings about this exact function. Extend, don't duplicate.
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: Has the LoadoutData offset map. Use to verify decompiled struct field accesses match known offsets.
  - `patch_addresses.h`: Must check for conflicts. Hook address must not collide with existing patches.

  **Acceptance Criteria**:
  - [ ] `docs/ghidra/FUN_1404f37a0_analysis.md` exists with complete decompilation analysis
  - [ ] Function signature documented (params, return type, calling convention)
  - [ ] CResourceID access pattern confirmed or corrected with exact offsets
  - [ ] Hookability assessment: YES/NO with reasoning (prologue size, inline status)
  - [ ] Slot indexing mechanism documented

  **QA Scenarios:**

  ```
  Scenario: Decompilation produces readable output
    Tool: Bash (notghidra MCP)
    Preconditions: Echo VR binary imported into notghidra project. Binary location: `echovr/bin/win10/echovr.exe`
    Steps:
      1. Import echovr.exe binary from `echovr/bin/win10/echovr.exe` if not already imported
      2. Run analysis on the binary
      3. Decompile function at address 0x1404F37A0
      4. Verify decompiled output contains recognizable patterns (pointer arithmetic, array indexing)
    Expected Result: Decompiled C code showing function parameters, local variables, and return value
    Failure Indicators: Empty decompilation, "analysis failed", or unrecognizable output
    Evidence: .sisyphus/evidence/task-2-decompilation.txt

  Scenario: Cross-reference validation — callers match expected call chain
    Tool: Bash (notghidra MCP)
    Preconditions: Analysis complete
    Steps:
      1. Get xrefs TO 0x1404F37A0
      2. Verify net_apply_loadout_items_to_player (0x140154C00) is among callers
      3. Document any OTHER callers (may reveal additional hook opportunities or constraints)
    Expected Result: At least net_apply_loadout_items_to_player is a confirmed caller
    Failure Indicators: No xrefs found, or expected caller not present
    Evidence: .sisyphus/evidence/task-2-xrefs-validation.txt
  ```

  **Commit**: YES (grouped with Tasks 3-4)
  - Message: `docs(ghidra): RE findings — hook validation, binary format, unknown SymbolId`
  - Files: `docs/ghidra/FUN_1404f37a0_analysis.md`

---

- [ ] 3. **RE — Analyze CGMeshListResource::DeserializeAndUpload binary format**

  **What to do**:
  - Use notghidra tools to decompile `CGMeshListResource::DeserializeAndUpload` (RVA `0x0547AB0`, absolute `0x140547AB0`)
  - Determine the input buffer format: what binary data does this function expect?
  - Map the struct layout it parses: vertex buffer offset/size, index buffer offset/size, texture references
  - Document the GPU upload sequence: what DirectX calls are made, in what order
  - Identify the minimum viable binary payload we need to construct for custom assets
  - Cross-reference with AsyncResourceIOCallback (RVA `0x0FA16D0`) to understand how raw I/O data flows into this function
  - Save findings to `docs/ghidra/mesh_binary_format.md`

  **Must NOT do**:
  - Do not write any C++ code — analysis only
  - Do not attempt to create asset files — format understanding first
  - Do not reverse-engineer shader compilation — meshes and textures only

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Complex binary format RE requiring careful struct analysis and DirectX API knowledge
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 2, 4, 5)
  - **Blocks**: Task 8 (format design depends on understanding the binary format the game expects)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `extras/docs/MESH_DUMP_HOOKS_README.md` — Documents mesh dump hook addresses and parameter signatures. **NOTE**: This README references `dbghooks/mesh_dump_hooks.cpp` which does NOT exist — the actual hook code is integrated into `src/gamepatches/`. Use this file for RVA addresses and parameter signatures ONLY, not as a code reference. START HERE for address/signature info.
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — Struct layouts that may be referenced by the deserialization function.

  **External References**:
  - `extras/docs/HASH_HOOKS_TROUBLESHOOTING.md` — Contains RVA `0x0547AB0` for DeserializeAndUpload and `0x0FA16D0` for AsyncResourceIOCallback. Confirms addresses.

  **WHY Each Reference Matters**:
  - `MESH_DUMP_HOOKS_README.md`: Has the hook signatures — tells you what parameters DeserializeAndUpload receives (buffer pointer, size, context). Essential starting point.
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: The CGMeshListResource struct may be partially documented here.

  **Acceptance Criteria**:
  - [ ] `docs/ghidra/mesh_binary_format.md` exists
  - [ ] Input buffer format documented: header structure, vertex data layout, index data layout
  - [ ] Minimum viable payload identified: what fields are required vs optional
  - [ ] DirectX upload sequence documented (which D3D calls, what state)

  **QA Scenarios:**

  ```
  Scenario: Decompilation of DeserializeAndUpload reveals parseable structure
    Tool: Bash (notghidra MCP)
    Preconditions: Echo VR binary analyzed. Binary location: `echovr/bin/win10/echovr.exe`
    Steps:
      1. Decompile function at 0x140547AB0
      2. Identify buffer read operations (memcpy, pointer arithmetic with offsets)
      3. Map at least 3 distinct fields/sections from the input buffer
      4. Document the parsing sequence in mesh_binary_format.md
    Expected Result: At least vertex count, index count, and data offsets identified in decompilation
    Failure Indicators: Function is too complex to decompile, or no buffer parsing visible
    Evidence: .sisyphus/evidence/task-3-mesh-decompilation.txt
  ```

  **Commit**: YES (grouped with Tasks 2, 4)
  - Message: `docs(ghidra): RE findings — hook validation, binary format, unknown SymbolId`
  - Files: `docs/ghidra/mesh_binary_format.md`

---

- [ ] 4. **RE — Investigate unknown SymbolId behavior (what renders when asset missing?)**

  **What to do**:
  - Investigate what the game renders when it encounters a SymbolId that has no corresponding asset loaded
  - Trace the resource resolution path: SymbolId → CResourceID lookup → what happens on lookup failure?
  - Check: does the game crash? Show invisible model? Show default/fallback? Show T-pose?
  - Analyze `net_apply_loadout_items_to_player` (RVA `0x00154C00`) error handling paths
  - Check the fallback CResourceID array at LoadoutData+0x370+0x80 — is this the missing-asset fallback?
  - Document findings in `docs/ghidra/unknown_symbolid_behavior.md`
  - This is critical for understanding multi-player visibility: when Player A has custom cosmetics and Player B doesn't have the assets

  **Must NOT do**:
  - Do not try to fix or solve the multi-player visibility problem — document only
  - Do not write any C++ code
  - Do not assume behavior — only document what the decompilation reveals

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Requires tracing error/fallback paths through decompiled code
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 2, 3, 5)
  - **Blocks**: None directly (documentation-only, informs future work)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `docs/ghidra/GHIDRA_STRUCT_ANALYSIS.md` — Analysis of `net_apply_loadout_items_to_player`. Shows the primary + fallback CResourceID pattern at LoadoutData+0x370. The fallback array may be the missing-asset behavior.
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — LoadoutData struct offsets. The primary (offset +0xB8) and fallback (+0x80) CResourceID arrays are documented here.

  **WHY Each Reference Matters**:
  - `GHIDRA_STRUCT_ANALYSIS.md`: Shows the dual-array pattern — understanding which array is primary vs fallback is key to understanding missing-asset behavior.
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: Exact offsets needed to trace the code paths.

  **Acceptance Criteria**:
  - [ ] `docs/ghidra/unknown_symbolid_behavior.md` exists
  - [ ] Documented: what happens when CResourceID lookup returns -1/null/invalid
  - [ ] Documented: role of fallback CResourceID array
  - [ ] Documented: crash risk assessment (does unknown SymbolId crash the game?)

  **QA Scenarios:**

  ```
  Scenario: Error handling paths identified in decompilation
    Tool: Bash (notghidra MCP)
    Preconditions: Echo VR binary analyzed. Binary location: `echovr/bin/win10/echovr.exe`
    Steps:
      1. Decompile net_apply_loadout_items_to_player at 0x140154C00
      2. Search for conditional branches after CResourceID lookup (if resource == -1, if resource == NULL)
      3. Trace both the success path and the failure/fallback path
      4. Document what each path does (render default? skip? crash?)
    Expected Result: At least one error handling path identified and documented
    Failure Indicators: No conditional branches found (would mean crash on unknown SymbolId)
    Evidence: .sisyphus/evidence/task-4-unknown-symbolid.txt
  ```

  **Commit**: YES (grouped with Tasks 2, 3)
  - Message: `docs(ghidra): RE findings — hook validation, binary format, unknown SymbolId`
  - Files: `docs/ghidra/unknown_symbolid_behavior.md`

---

- [ ] 5. **Build system prep — add jsoncpp linking + AssetCDN source scaffolding**

  **What to do**:
  - Edit `src/gamepatches/CMakeLists.txt` to link jsoncpp (already a vcpkg dependency but not linked to gamepatches target)
  - Create empty `src/gamepatches/asset_cdn.h` with class stub: `namespace AssetCDN { void Initialize(); }`
  - Create empty `src/gamepatches/asset_cdn.cpp` with stub implementation that just logs "AssetCDN initialized"
  - Add new source files to CMakeLists.txt
  - Verify the project still builds cleanly after changes
  - Uncomment the existing `asset_cdn_url` config reading at `patches.cpp:760-767` (reads URL from config file)
  - Do NOT uncomment `AssetCDN::Initialize()` call yet (Task 13 will do this)

  **Must NOT do**:
  - Do not implement any actual CDN functionality — stubs only
  - Do not uncomment `AssetCDN::Initialize()` call at patches.cpp:1430-1433 (that's Task 13)
  - Do not add curl usage yet — that's Task 11

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Small CMake changes + empty source files. Straightforward build system task.
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 2, 3, 4)
  - **Blocks**: Tasks 6, 11
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `src/gamepatches/CMakeLists.txt` — Current build configuration. Look at how other libraries are linked (e.g., curl, minhook). Follow the same pattern for jsoncpp.
  - `src/gamepatches/patches.cpp:760-767` — Commented-out `asset_cdn_url` config reading. Uncomment this block. It reads from the config file.
  - `src/gamepatches/patches.cpp:1430-1433` — Commented-out `AssetCDN::Initialize()` call. Do NOT touch this yet.

  **API/Type References**:
  - `vcpkg.json` — Confirms jsoncpp is already a project dependency. No need to add it to vcpkg.

  **WHY Each Reference Matters**:
  - `CMakeLists.txt`: Must follow existing link patterns. Wrong linking will break the build.
  - `patches.cpp:760-767`: This is pre-existing code that reads the CDN URL. Uncommenting it gives us config support for free.
  - `vcpkg.json`: Confirms jsoncpp is available — no dependency installation needed.

  **Acceptance Criteria**:
  - [ ] `src/gamepatches/asset_cdn.h` exists with namespace and Initialize() declaration
  - [ ] `src/gamepatches/asset_cdn.cpp` exists with stub implementation
  - [ ] `src/gamepatches/CMakeLists.txt` links jsoncpp and includes new source files
  - [ ] `patches.cpp:760-767` uncommented (asset_cdn_url config reading)
  - [ ] `make build` succeeds with zero errors

  **QA Scenarios:**

  ```
  Scenario: Project builds cleanly with new files and jsoncpp linking
    Tool: Bash
    Preconditions: None
    Steps:
      1. Run `make build` (or equivalent CMake build command)
      2. Check for zero compilation errors and zero linker errors
      3. Verify gamepatches.dll is produced in build output directory
    Expected Result: Build succeeds, DLL produced, no warnings about jsoncpp
    Failure Indicators: Linker errors for jsoncpp symbols, missing header errors, DLL not produced
    Evidence: .sisyphus/evidence/task-5-build-output.txt

  Scenario: Stub files have correct structure
    Tool: Bash
    Preconditions: Files created
    Steps:
      1. Verify asset_cdn.h contains namespace AssetCDN and Initialize() declaration
      2. Verify asset_cdn.cpp includes asset_cdn.h and has Initialize() body with Log() call
      3. Verify CMakeLists.txt contains jsoncpp in target_link_libraries
    Expected Result: All three files contain expected content
    Failure Indicators: Missing declarations, wrong namespace, jsoncpp not linked
    Evidence: .sisyphus/evidence/task-5-stub-verification.txt
  ```

  **Commit**: YES
  - Message: `build(gamepatches): add jsoncpp linking and AssetCDN scaffolding`
  - Files: `src/gamepatches/CMakeLists.txt`, `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`, `src/gamepatches/patches.cpp`
  - Pre-commit: `make build`

### Wave 2 — Vertical Slice (Prove injection works before building full pipeline)

- [ ] 6. **Hardcoded injection PoC — hook ONE slot, inject ONE asset, confirm rendering**

  **What to do**:
  - Using findings from Task 2 (FUN_1404f37a0 validation) and Task 3 (mesh binary format), create a proof-of-concept hook
  - Hook the validated function (expected: FUN_1404f37a0 @ RVA 0x004F37A0) using the existing PatchDetour() pattern
  - In the hook: intercept ONE specific cosmetic slot (e.g., `banner` or `tint`) for the local player only
  - When the intercepted slot is accessed, replace the CResourceID with a hardcoded test SymbolId computed via symbol_hash.h (Task 1)
  - Create a minimal test asset binary blob (based on Task 3 findings) — even a colored cube is sufficient
  - Embed the test asset as a hardcoded byte array in the hook code (no file I/O yet)
  - The goal is to prove: custom SymbolId → custom CResourceID → custom mesh renders in-game
  - If Task 2 determines FUN_1404f37a0 is NOT hookable, adapt to the recommended alternative from Task 2's analysis
  - Write the hook in a new file: `src/gamepatches/cosmetics_hook.h` + `cosmetics_hook.cpp`

  **Must NOT do**:
  - Do not implement file I/O, network requests, or caching — hardcoded data only
  - Do not hook ALL slots — ONE slot only for proof-of-concept
  - Do not modify other players' cosmetics — local player only
  - Do not build a general-purpose injection system — that's Task 12
  - Do not skip Task 2/3 findings — if RE reveals different hook point or format, use THOSE findings

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Game hooking with binary data injection requires careful memory management and understanding of decompiled structures
  - **Skills**: []
    - notghidra tools available by default if further RE needed during implementation

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 7)
  - **Parallel Group**: Wave 2 (with Task 7)
  - **Blocks**: Task 12 (generalization depends on proven PoC)
  - **Blocked By**: Tasks 1 (symbol_hash.h), 2 (hook target validation), 5 (build scaffolding)

  **References**:

  **Pattern References**:
  - `src/gamepatches/patches.cpp` — All existing hooks follow PatchDetour() pattern. Search for `PatchDetour(` to see examples. The hook function signature must match the original function exactly.
  - `src/common/hooking.h` — MinHook/Detours abstraction layer. Provides `PatchDetour()`, `Unpatch()`. Use this — do not call MinHook directly.
  - `docs/ghidra/FUN_1404f37a0_analysis.md` (OUTPUT of Task 2) — Contains the validated function signature, parameters, and hookability assessment. THIS IS YOUR PRIMARY GUIDE for the hook implementation.
  - `docs/ghidra/mesh_binary_format.md` (OUTPUT of Task 3) — Contains the minimum viable binary payload format. Use this to construct the test asset byte array.

  **API/Type References**:
  - `src/common/echovr.h:LoadoutSlot` — The 21-field SymbolId struct. Identifies which field corresponds to which slot.
  - `src/common/echovr.h:SymbolId` — `INT64` typedef. Your computed hash must be this type.
  - `src/common/symbol_hash.h` (OUTPUT of Task 1) — Use `SymbolHash()` to compute the custom SymbolId at compile time.
  - `src/gamepatches/patch_addresses.h` — Add the new hook RVA here following the existing naming pattern.

  **WHY Each Reference Matters**:
  - `FUN_1404f37a0_analysis.md`: Without this, you don't know the function signature to hook. MUST read first.
  - `mesh_binary_format.md`: Without this, you don't know what binary data to inject. MUST read second.
  - `hooking.h`: Use the established pattern — deviation will cause conflicts with other hooks.
  - `patches.cpp`: See how other hooks are registered in `ApplyPatches()` — follow same structure.

  **Acceptance Criteria**:
  - [ ] `src/gamepatches/cosmetics_hook.h` and `cosmetics_hook.cpp` exist
  - [ ] Hook address added to `patch_addresses.h`
  - [ ] `make build` succeeds with the new hook code
  - [ ] Hook function signature matches FUN_1404f37a0 exactly (per Task 2 findings)
  - [ ] At least one hardcoded test SymbolId is computed via symbol_hash.h

  **QA Scenarios:**

  ```
  Scenario: PoC hook compiles and links cleanly
    Tool: Bash
    Preconditions: Tasks 1, 2, 3, 5 completed
    Steps:
      1. Verify cosmetics_hook.h includes symbol_hash.h and declares hook function
      2. Verify cosmetics_hook.cpp implements hook matching FUN_1404f37a0 signature from Task 2
      3. Run `make build`
      4. Verify gamepatches.dll is produced without errors
    Expected Result: Clean build, DLL produced, no linker errors
    Failure Indicators: Signature mismatch errors, undefined symbol errors, linker failures
    Evidence: .sisyphus/evidence/task-6-poc-build.txt

  Scenario: Hook function contains correct slot interception logic
    Tool: Bash
    Preconditions: cosmetics_hook.cpp exists
    Steps:
      1. Read cosmetics_hook.cpp
      2. Verify it checks for a specific slot ID (e.g., banner slot)
      3. Verify it replaces CResourceID with computed SymbolHash value
      4. Verify the original function is called for all non-intercepted slots (pass-through)
      5. Verify a test asset byte array is defined (even if minimal placeholder)
    Expected Result: Code contains slot check, hash computation, pass-through for other slots, and embedded test data
    Failure Indicators: Missing pass-through (would break all cosmetics), missing slot check, no test data
    Evidence: .sisyphus/evidence/task-6-poc-logic-review.txt
  ```

  **Commit**: YES
  - Message: `feat(gamepatches): hardcoded cosmetic injection PoC — single slot proof-of-concept`
  - Files: `src/gamepatches/cosmetics_hook.h`, `src/gamepatches/cosmetics_hook.cpp`, `src/gamepatches/patch_addresses.h`, `src/gamepatches/CMakeLists.txt`
  - Pre-commit: `make build`

---

- [ ] 7. **Hash consistency verification — Go vs C++ output comparison**

  **What to do**:
  - Create a verification script that computes hashes using BOTH the Go implementation (`extras/reference/core_hash.go`) and the C++ implementation (`src/common/symbol_hash.h` from Task 1)
  - Test with a comprehensive set of inputs:
    - Known cosmetic names from sourcedb (e.g., `rwd_tint_0019`, `rwd_decal_default`, `rwd_chassis_body_s11`)
    - Edge cases: empty string, single character, very long string, mixed case (`RWD_Tint_0019` vs `rwd_tint_0019`)
    - Custom cosmetic name patterns we'll use (e.g., `custom_tint_001`, `cdn_banner_test`)
  - Extract real cosmetic names from `echovr/sourcedb/rad15/json/r14/multiplayer/customization_models.json`
  - Compare outputs — every single hash must match exactly
  - This validates that the Go CLI tools (Tasks 9, 10) will produce SymbolIds compatible with the C++ game hooks

  **Must NOT do**:
  - Do not modify either hash implementation — this is read-only verification
  - Do not write a test framework — simple script comparison is sufficient
  - Do not test more than 100 inputs — diminishing returns

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Script-based comparison, no complex logic, well-defined pass/fail
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 6)
  - **Parallel Group**: Wave 2 (with Task 6)
  - **Blocks**: Tasks 9, 10 (Go CLI tools depend on proven hash consistency)
  - **Blocked By**: Task 1 (needs symbol_hash.h)

  **References**:

  **Pattern References**:
  - `extras/reference/core_hash.go` — Go hash implementation. Write a small Go main() that reads names from stdin and outputs `name\thash_hex`. This is the REFERENCE implementation.
  - `src/common/symbol_hash.h` (OUTPUT of Task 1) — C++ implementation to verify. Write a small C++ main() that does the same stdin → hash_hex output.

  **API/Type References**:
  - `echovr/sourcedb/rad15/json/r14/multiplayer/customization_models.json` — Contains real cosmetic names. Extract names with `jq` or similar to feed into both hash programs.

  **WHY Each Reference Matters**:
  - `extras/reference/core_hash.go`: The Go implementation is considered authoritative (used in production Nakama server). C++ must match it.
  - `customization_models.json`: Real-world cosmetic names ensure we test with actual game data, not synthetic inputs.

  **Acceptance Criteria**:
  - [ ] At least 50 real cosmetic names tested
  - [ ] At least 10 edge case inputs tested
  - [ ] 100% hash match between Go and C++ for all inputs
  - [ ] Results saved as evidence file with name + Go hash + C++ hash columns

  **QA Scenarios:**

  ```
  Scenario: All hashes match between Go and C++ implementations
    Tool: Bash
    Preconditions: Task 1 (symbol_hash.h) completed, Go installed
    Steps:
      1. Extract cosmetic names from customization_models.json using jq
      2. Write minimal Go program: reads names from stdin, outputs name<tab>hash per line
      3. Write minimal C++ program: reads names from stdin, outputs name<tab>hash per line
      4. Feed same name list to both programs
      5. diff the outputs — expect zero differences
    Expected Result: `diff` produces no output (identical hashes for all inputs)
    Failure Indicators: Any line differs between Go and C++ output
    Evidence: .sisyphus/evidence/task-7-hash-comparison.txt

  Scenario: Case insensitivity works correctly
    Tool: Bash
    Preconditions: Both hash programs built
    Steps:
      1. Hash "rwd_tint_0019" and "RWD_TINT_0019" and "Rwd_Tint_0019" with both programs
      2. All three inputs must produce the same hash value
      3. Both programs must agree on that value
    Expected Result: All 6 outputs (3 inputs × 2 programs) produce identical hash
    Failure Indicators: Any case variation produces different hash
    Evidence: .sisyphus/evidence/task-7-case-insensitive.txt
  ```

  **Commit**: YES
  - Message: `test(common): verify Go/C++ hash consistency across 50+ cosmetic names`
  - Files: (test scripts — can be in a temp directory, no permanent files needed)
  - Pre-commit: both hash programs compile

---

### Wave 3 — Format Design + CDN Pipeline (After Waves 1-2)

- [ ] 8. **Design manifest JSON format + package binary format + URL scheme**

  **What to do**:
  - Design the manifest JSON schema that maps SymbolIds to package files. Must include:
    - Manifest version number (integer, monotonically increasing)
    - Per-slot-type asset entries keyed by SymbolId (hex string)
    - Each entry: package filename, package version, file size, slot type
    - Timestamp of manifest generation
  - Design the package binary format:
    - Magic bytes header (e.g., `EVRP`) + format version (uint32)
    - SymbolId (int64) that this package provides
    - Slot type identifier (uint8)
    - Asset data type flags (mesh, texture, or both)
    - Mesh data section: vertex buffer (position, normal, UV, tangent), index buffer, bounding box
    - Texture data section: DDS format (pre-compressed, GPU-ready), mip count
    - All multi-byte values little-endian
  - Design the URL scheme for `https://cdn.echo.taxi/`:
    - Manifest URL: `https://cdn.echo.taxi/v{VERSION}/manifest.json`
    - Package URL: `https://cdn.echo.taxi/v{VERSION}/packages/{symbolid_hex}.evrp`
    - Version is the manifest version number — auditable by incrementing path
    - Old versions remain accessible (immutable, append-only)
  - Produce a design document ONLY (no code) — saved as `docs/cosmetics-cdn-format.md` (DRAFT)
  - This is the AUTHORITATIVE format reference for Tasks 9, 10, 11, 12

  **Must NOT do**:
  - Do not implement any code — design document only
  - Do not use protobuf or msgpack — keep it simple (JSON manifest, raw binary packages)
  - Do not include integrity hashes — trust the R2 bucket (user decision)
  - Do not include audio, animation, or shader sections in the package format
  - Do not design for hot-reload or delta updates — full packages only

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Format design requires understanding GPU data layout, binary serialization, and URL architecture — not just code writing
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (this is the first task in Wave 3, blocks the rest)
  - **Parallel Group**: Wave 3 (starts after Wave 1 Task 3 completes)
  - **Blocks**: Tasks 9, 10, 11, 14
  - **Blocked By**: Task 3 (RE mesh binary format — need to understand what data to put in packages)

  **References**:

  **Pattern References**:
  - `docs/ghidra/mesh_binary_format.md` — (Task 3 output) RE analysis of how the game stores mesh data. The package binary format must match what the game expects at the GPU upload level.
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — LoadoutData structure showing how CResourceID maps to cosmetic slots. Slot type identifiers in the package must align with these offsets.

  **API/Type References**:
  - `src/common/echovr.h:LoadoutSlot` — The 21 cosmetic fields (selectionmode through title). The manifest must support entries for all 20 equippable slot types.
  - `echovr/sourcedb/rad15/json/r14/multiplayer/customization_models.json` — Real cosmetic names and structure. Use as sample data for the manifest schema design.
  - `echovr/sourcedb/rad15/json/r14/multiplayer/equip_slots.json` — Slot type enumeration. Use for slot type identifiers in the format.

  **External References**:
  - DDS format specification — Package texture section should store DDS files directly (already GPU-ready, no runtime conversion needed)

  **WHY Each Reference Matters**:
  - `mesh_binary_format.md`: Without knowing what vertex attributes the game uses (position, normal, UV layout, tangent), we can't design a compatible package format.
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: Slot indexing must match game internals exactly.
  - `LoadoutSlot`: Enumerate ALL slot types so the manifest schema covers them all from day one.
  - `customization_models.json`: Provides real naming patterns and structure to validate our schema against.

  **Acceptance Criteria**:
  - [ ] `docs/cosmetics-cdn-format.md` exists with manifest JSON schema section
  - [ ] Document includes package binary format with byte-level layout diagram
  - [ ] Document includes URL scheme with concrete examples
  - [ ] Manifest schema covers all 20 cosmetic slot types
  - [ ] Package format includes magic bytes, version, SymbolId, slot type, mesh data, texture data sections
  - [ ] Format is versioned (both manifest and package have version fields)

  **QA Scenarios:**

  ```
  Scenario: Manifest JSON schema is valid and parseable
    Tool: Bash
    Preconditions: docs/cosmetics-cdn-format.md exists
    Steps:
      1. Extract the example manifest JSON from the design doc
      2. Pipe it through `python3 -m json.tool` to validate JSON syntax
      3. Verify it contains: version field, timestamp, at least one slot type with entries
      4. Verify each entry has: symbolid, package_file, package_version, file_size, slot_type
    Expected Result: Valid JSON, all required fields present
    Failure Indicators: JSON parse error, missing required fields
    Evidence: .sisyphus/evidence/task-8-manifest-schema-valid.txt

  Scenario: Package binary format has complete byte-level layout
    Tool: Bash
    Preconditions: docs/cosmetics-cdn-format.md exists
    Steps:
      1. Search document for magic bytes definition
      2. Verify byte offsets are specified for: magic (4 bytes), format_version (4 bytes), symbolid (8 bytes), slot_type (1 byte)
      3. Verify mesh data section and texture data section are both described with offsets
      4. Verify endianness is specified (little-endian)
    Expected Result: All sections have explicit byte offsets and sizes
    Failure Indicators: Vague descriptions without byte offsets, missing sections
    Evidence: .sisyphus/evidence/task-8-package-format-complete.txt
  ```

  **Commit**: YES
  - Message: `docs(cosmetics): design manifest JSON + package binary format + CDN URL scheme`
  - Files: `docs/cosmetics-cdn-format.md`
  - Pre-commit: N/A (documentation only)

---

- [ ] 9. **Go CLI tool — cosmetics-manifest generator**

  **What to do**:
  - Create `tools/cosmetics-manifest/main.go` with `go.mod`
  - CLI reads a directory of `.evrp` package files and generates a manifest JSON file
  - Usage: `cosmetics-manifest --packages-dir ./packages/ --version 42 --output manifest.json`
  - For each `.evrp` file:
    - Read the package header (magic bytes, SymbolId, slot type, format version)
    - Extract metadata without parsing full asset data
    - Add entry to manifest keyed by slot type → SymbolId
  - Include the Go CSymbol64_Hash function (copy from `extras/reference/core_hash.go`) for any name-to-SymbolId lookups
  - Manifest JSON must exactly match the schema from Task 8
  - Validate all packages have valid headers before generating manifest
  - Exit with non-zero code on any invalid package

  **Must NOT do**:
  - Do not parse or validate the full asset data inside packages — header only
  - Do not upload to R2 — this tool only generates the manifest file locally
  - Do not add compression — manifest is small JSON, packages are already binary
  - Do not add dependency on external Go libraries beyond standard library

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Go CLI with binary file parsing, must match format spec exactly
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 10, once Task 8 is done)
  - **Parallel Group**: Wave 3 (after Tasks 7 and 8 complete)
  - **Blocks**: Task 13
  - **Blocked By**: Task 7 (hash consistency — ensures Go hash is correct), Task 8 (format design)

  **References**:

  **Pattern References**:
  - `extras/reference/core_hash.go` — Go CSymbol64_Hash implementation. Copy this function into the CLI tool for SymbolId computation. This is the AUTHORITATIVE hash reference.
  - `docs/cosmetics-cdn-format.md` — (Task 8 output) Format specification. The manifest JSON this tool generates MUST match the schema defined here exactly.

  **API/Type References**:
  - `docs/cosmetics-cdn-format.md:Package Binary Format` — Header structure (magic, version, SymbolId, slot type) that this tool must parse from `.evrp` files.

  **External References**:
  - Go `encoding/binary` package — for reading little-endian binary headers from package files
  - Go `encoding/json` package — for generating the manifest JSON output

  **WHY Each Reference Matters**:
  - `extras/reference/core_hash.go`: The hash function must produce SymbolIds identical to the C++ implementation. Copy, don't reimplement.
  - `cosmetics-cdn-format.md`: This tool is a FORMAT CONSUMER — it must read packages and write manifests that exactly match the spec. Any deviation breaks the pipeline.

  **Acceptance Criteria**:
  - [ ] `tools/cosmetics-manifest/main.go` and `go.mod` exist
  - [ ] `go build` succeeds with no errors
  - [ ] Running with `--help` shows usage with --packages-dir, --version, --output flags
  - [ ] Generates valid JSON matching Task 8 schema

  **QA Scenarios:**

  ```
  Scenario: Build and run with test package directory
    Tool: Bash
    Preconditions: tools/cosmetics-manifest/ exists, Task 8 format doc available
    Steps:
      1. cd tools/cosmetics-manifest && go build -o cosmetics-manifest
      2. Create a temp directory with a synthetic .evrp file (write correct magic bytes + header using printf/dd)
      3. Run: ./cosmetics-manifest --packages-dir /tmp/test-packages/ --version 1 --output /tmp/manifest.json
      4. Validate /tmp/manifest.json with python3 -m json.tool
      5. Verify manifest contains version:1, timestamp, and an entry for the test package's SymbolId
    Expected Result: Valid manifest JSON with correct entries from test package
    Failure Indicators: Build failure, runtime panic, invalid JSON, missing entries
    Evidence: .sisyphus/evidence/task-9-manifest-generation.txt

  Scenario: Invalid package file is rejected
    Tool: Bash
    Preconditions: cosmetics-manifest binary built
    Steps:
      1. Create a temp directory with a file that has wrong magic bytes (e.g., echo "XXXX" > bad.evrp)
      2. Run: ./cosmetics-manifest --packages-dir /tmp/bad-packages/ --version 1 --output /tmp/manifest.json
      3. Check exit code
    Expected Result: Non-zero exit code, error message about invalid package header
    Failure Indicators: Exit code 0 (silent success on bad input)
    Evidence: .sisyphus/evidence/task-9-invalid-package-rejected.txt
  ```

  **Commit**: YES
  - Message: `feat(tools): add cosmetics-manifest Go CLI for generating CDN manifests`
  - Files: `tools/cosmetics-manifest/main.go`, `tools/cosmetics-manifest/go.mod`
  - Pre-commit: `cd tools/cosmetics-manifest && go build`

---

- [ ] 10. **Go CLI tool — cosmetics-package builder**

  **What to do**:
  - Create `tools/cosmetics-package/main.go` with `go.mod`
  - CLI takes raw asset files (mesh data + DDS texture) and wraps them into the `.evrp` binary package format
  - Usage: `cosmetics-package --name "custom_tint_001" --slot tint --mesh ./mesh.bin --texture ./texture.dds --output custom_tint_001.evrp`
  - Writes the package binary format defined in Task 8:
    - Magic bytes (`EVRP`), format version, SymbolId (computed from --name via CSymbol64_Hash), slot type
    - Mesh data section with vertex/index buffers
    - Texture data section with DDS data
  - Include the Go CSymbol64_Hash function (copy from `extras/reference/core_hash.go`) for name → SymbolId
  - Validate inputs exist and are non-empty before writing
  - Support `--mesh-only` and `--texture-only` flags for packages with partial assets
  - Print the computed SymbolId (hex) to stdout for verification

  **Must NOT do**:
  - Do not convert or process asset data — raw passthrough into the package container
  - Do not compress — assets are already GPU-ready, compression would require decompression on load
  - Do not upload to R2 — this tool only creates local `.evrp` files
  - Do not add dependency on external Go libraries beyond standard library
  - Do not validate the CONTENT of mesh/texture data — only that files exist and are non-empty

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Go CLI with binary file writing, must produce format-compliant output
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 9, once Task 8 is done)
  - **Parallel Group**: Wave 3 (after Tasks 7 and 8 complete)
  - **Blocks**: Task 13
  - **Blocked By**: Task 7 (hash consistency), Task 8 (format design)

  **References**:

  **Pattern References**:
  - `extras/reference/core_hash.go` — Go CSymbol64_Hash implementation. Copy for name → SymbolId computation.
  - `docs/cosmetics-cdn-format.md` — (Task 8 output) Package binary format specification. This tool PRODUCES the format — must match byte-for-byte.

  **API/Type References**:
  - `docs/cosmetics-cdn-format.md:Package Binary Format` — Byte-level layout: magic, version, SymbolId, slot_type, mesh section header, texture section header. This is the EXACT output format.

  **External References**:
  - Go `encoding/binary` package — for writing little-endian binary data
  - DDS file format — texture input is expected to be valid DDS (this tool does NOT validate DDS structure, just wraps it)

  **WHY Each Reference Matters**:
  - `extras/reference/core_hash.go`: SymbolId must be computed identically to C++ side. Use the same function.
  - `cosmetics-cdn-format.md`: This tool is a FORMAT PRODUCER — the C++ game hook (Task 12) will read what this tool writes. Any byte-level deviation breaks the pipeline.

  **Acceptance Criteria**:
  - [ ] `tools/cosmetics-package/main.go` and `go.mod` exist
  - [ ] `go build` succeeds with no errors
  - [ ] Running with `--help` shows usage with --name, --slot, --mesh, --texture, --output flags
  - [ ] Prints computed SymbolId hex to stdout
  - [ ] Produced `.evrp` file starts with correct magic bytes

  **QA Scenarios:**

  ```
  Scenario: Build and package a test asset
    Tool: Bash
    Preconditions: tools/cosmetics-package/ exists, Task 8 format doc available
    Steps:
      1. cd tools/cosmetics-package && go build -o cosmetics-package
      2. Create synthetic test data: dd if=/dev/urandom of=/tmp/test_mesh.bin bs=1024 count=4
      3. Create synthetic texture: dd if=/dev/urandom of=/tmp/test_texture.dds bs=1024 count=8
      4. Run: ./cosmetics-package --name "test_custom_tint" --slot tint --mesh /tmp/test_mesh.bin --texture /tmp/test_texture.dds --output /tmp/test_custom_tint.evrp
      5. Verify output file exists and starts with magic bytes: xxd -l 4 /tmp/test_custom_tint.evrp | grep -q "4556 5250" (EVRP)
      6. Check stdout contains the hex SymbolId for "test_custom_tint"
    Expected Result: .evrp file created with correct magic bytes, SymbolId printed to stdout
    Failure Indicators: Build failure, missing output file, wrong magic bytes, no SymbolId output
    Evidence: .sisyphus/evidence/task-10-package-creation.txt

  Scenario: Missing input file is rejected
    Tool: Bash
    Preconditions: cosmetics-package binary built
    Steps:
      1. Run: ./cosmetics-package --name "test" --slot tint --mesh /tmp/nonexistent.bin --texture /tmp/nonexistent.dds --output /tmp/out.evrp
      2. Check exit code
    Expected Result: Non-zero exit code, error message about missing input files
    Failure Indicators: Exit code 0 (silently creating empty/corrupt package)
    Evidence: .sisyphus/evidence/task-10-missing-input-rejected.txt
  ```

  **Commit**: YES
  - Message: `feat(tools): add cosmetics-package Go CLI for building .evrp asset packages`
  - Files: `tools/cosmetics-package/main.go`, `tools/cosmetics-package/go.mod`
  - Pre-commit: `cd tools/cosmetics-package && go build`

---

- [ ] 11. **AssetCDN module — manifest fetch + package download + local cache**

  **What to do**:
  - Create `src/gamepatches/asset_cdn.h` and `src/gamepatches/asset_cdn.cpp`
  - Implement the `AssetCDN` class with these responsibilities:
    1. **Manifest fetch**: HTTP GET `https://cdn.echo.taxi/v{VERSION}/manifest.json` using libcurl
       - Parse JSON response using jsoncpp
       - Store parsed manifest in memory (map of SymbolId → package info per slot type)
    2. **Package download**: For each manifest entry, check local cache first:
       - Cache directory: `%LOCALAPPDATA%\EchoVR\cosmetics\v{VERSION}\`
       - If cached file exists with correct size, skip download
       - If not cached, HTTP GET the package URL and save to cache directory
    3. **Local cache management**:
       - Create cache directory structure on first run
       - Store packages as `{symbolid_hex}.evrp` in versioned subdirectory
       - NO cache eviction — grows unbounded (v1 design decision)
    4. **Asset lookup API**:
       - `bool HasAsset(SymbolId id, uint8_t slotType)` — check if we have a cached package for this SymbolId+slot
       - `const AssetData* GetAsset(SymbolId id, uint8_t slotType)` — return pointer to loaded asset data (memory-mapped or loaded from cache)
       - `bool IsReady()` — whether manifest has been fetched and initial downloads complete
    5. **Threading**:
       - ALL HTTP operations (manifest fetch + package downloads) on a background thread (std::thread)
       - Main game thread only calls HasAsset/GetAsset (lock-free reads after initial load)
       - Use std::atomic<bool> for ready flag
       - Mutex only during initial asset map population, not during reads
    6. **Initialization**:
       - `static void Initialize(const std::string& cdnUrl)` — starts background thread
       - Called from patches.cpp during DLL initialization
       - Reads CDN URL from config (already parsed at patches.cpp:760-767)
    7. **Graceful degradation**:
       - If CDN unreachable: log warning, set ready=true with empty asset map
       - If individual package download fails: log warning, skip that asset, continue with others
       - Game MUST function normally when no custom assets are available
  - Integrate with the existing build system — add source files to `src/gamepatches/CMakeLists.txt`
  - Use the `Log()` macro from `common/logging.h` for all logging

  **Must NOT do**:
  - Do not block the game thread — ALL HTTP on background thread, no exceptions
  - Do not implement retry logic — single attempt per download (v1 guardrail)
  - Do not implement cache eviction — cache grows unbounded (v1 guardrail)
  - Do not validate package data beyond header magic bytes — trust CDN content
  - Do not implement hot-reload — assets loaded at startup only, require restart
  - Do not use raw WinHTTP/WinInet — use libcurl (already a vcpkg dependency)
  - Do not parse package asset data — just load raw bytes, the injection hook (Task 12) handles interpretation

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Multi-threaded C++ module with HTTP, file I/O, JSON parsing, cache management, and thread-safety considerations. Needs careful design.
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 9, 10, once Task 8 is done)
  - **Parallel Group**: Wave 3 (after Tasks 5 and 8 complete)
  - **Blocks**: Tasks 12, 13
  - **Blocked By**: Task 5 (build system prep — jsoncpp linking), Task 8 (format design — manifest schema + package format)

  **References**:

  **Pattern References**:
  - `src/gamepatches/patches.cpp:760-767` — Existing `asset_cdn_url` config reading (commented out). Uncomment and use this as the CDN URL source. Shows the config parsing pattern.
  - `src/gamepatches/patches.cpp:1430-1433` — Existing `AssetCDN::Initialize()` call (commented out). This is WHERE initialization should be called from.
  - `src/common/logging.h` — Use `Log(level, format, ...)` for all output. Follow existing logging patterns.
  - `src/common/hooking.h` — NOT needed for this module (no hooks), but shows the project's C++ style.

  **API/Type References**:
  - `docs/cosmetics-cdn-format.md` — (Task 8 output) Manifest JSON schema and package binary format. This module parses the manifest and reads package headers.
  - `src/common/echovr.h:SymbolId` — INT64 typedef. All SymbolId parameters and map keys use this type.

  **External References**:
  - libcurl — `curl_easy_init`, `curl_easy_setopt`, `curl_easy_perform`. Already linked via vcpkg. Use CURLOPT_WRITEFUNCTION for response data.
  - jsoncpp — `Json::Value`, `Json::Reader`. Must be linked in CMakeLists.txt (Task 5 added this).
  - Windows API — `SHGetKnownFolderPath(FOLDERID_LocalAppData)` or `getenv("LOCALAPPDATA")` for cache directory. `CreateDirectoryW` for directory creation.

  **WHY Each Reference Matters**:
  - `patches.cpp:760-767`: Don't reinvent config reading — the pattern is already there, just uncommented.
  - `patches.cpp:1430-1433`: The integration point is pre-planned — this exact line is where Initialize() gets called.
  - `cosmetics-cdn-format.md`: This module CONSUMES both the manifest (JSON parse) and packages (binary read). Format must match exactly.
  - libcurl/jsoncpp: These are ALREADY in vcpkg.json — don't add new dependencies.

  **Acceptance Criteria**:
  - [ ] `src/gamepatches/asset_cdn.h` and `asset_cdn.cpp` exist
  - [ ] `make build` succeeds with these new files included
  - [ ] AssetCDN class has Initialize(), HasAsset(), GetAsset(), IsReady() methods
  - [ ] All HTTP operations are on background thread (std::thread, not main thread)
  - [ ] Uses Log() for all output, not printf/cout

  **QA Scenarios:**

  ```
  Scenario: Module compiles and links correctly
    Tool: Bash
    Preconditions: Task 5 build system prep complete, source files written
    Steps:
      1. Run: make build (or cmake --build)
      2. Check build output for any errors or warnings in asset_cdn.cpp
      3. Verify the output DLL (gamepatches.dll / dbgcore.dll) is produced
    Expected Result: Clean build, DLL produced, no linker errors for curl/jsoncpp symbols
    Failure Indicators: Linker errors (unresolved curl_* or Json::*), compilation errors
    Evidence: .sisyphus/evidence/task-11-build-success.txt

  Scenario: Background thread design verification (code review)
    Tool: Bash
    Preconditions: asset_cdn.cpp exists
    Steps:
      1. grep -n "std::thread" src/gamepatches/asset_cdn.cpp — verify background thread creation
      2. grep -n "curl_easy_perform" src/gamepatches/asset_cdn.cpp — verify all curl calls exist
      3. Verify curl_easy_perform is NOT called from Initialize() directly but from thread function
      4. grep -n "std::atomic" src/gamepatches/asset_cdn.cpp — verify ready flag is atomic
      5. Verify HasAsset/GetAsset do NOT call curl or do file I/O
    Expected Result: All HTTP in thread function, atomic ready flag, HasAsset/GetAsset are pure lookups
    Failure Indicators: curl_easy_perform in Initialize() body, non-atomic ready flag, I/O in getter functions
    Evidence: .sisyphus/evidence/task-11-threading-review.txt
  ```

  **Commit**: YES
  - Message: `feat(gamepatches): add AssetCDN module — manifest fetch, package download, local cache`
  - Files: `src/gamepatches/asset_cdn.h`, `src/gamepatches/asset_cdn.cpp`, `src/gamepatches/CMakeLists.txt`
  - Pre-commit: `make build`

---

### Wave 4 — Integration + Documentation (after Wave 3)

- [ ] 12. **Resource injection hook — generalize PoC to all 20 cosmetic slots with cached CDN assets**

  **What to do**:
  - Extend the proof-of-concept hook from Task 6 (`cosmetics_hook.h` / `cosmetics_hook.cpp`) to handle ALL 20 cosmetic slot types
  - Build a **slot dispatch table** mapping each slot index (0–20) to its corresponding `LoadoutSlot` field:
    - Map the 21 fields: selectionmode, banner, booster, bracer, chassis, decal, decal_body, emissive, emote, secondemote, goal_fx, medal, pattern, pattern_body, pip, tag, tint, tint_alignment_a, tint_alignment_b, tint_body, title
    - Each entry maps slot index → SymbolId field offset within LoadoutSlot struct
  - Integrate with `AssetCDN` module (Task 11) to check for cached custom assets:
    1. When the hooked function `FUN_1404f37a0` is called with a loadout_id and slot:
       - Read the SymbolId from the player's LoadoutSlot for that slot index
       - Call `AssetCDN::HasAsset(symbolId, slotType)` to check if a custom asset exists
       - If YES: call `AssetCDN::GetAsset(symbolId, slotType)` and construct a custom CResourceID pointing to the cached asset data
       - If NO: fall through to the original `FUN_1404f37a0` for vanilla behavior
    2. CResourceID construction: Based on Task 2's RE findings on how CResourceID maps to actual asset data. The custom CResourceID must be accepted by the game's downstream resource loading pipeline.
  - **Graceful fallthrough is CRITICAL**: If AssetCDN is not ready (IsReady() == false), or if the SymbolId is not in the custom asset map, the hook MUST call the original function. Zero chance of crash from missing custom assets.
  - Add safety checks:
    - Validate slot index is within bounds (0–20)
    - Null-check all pointers before dereferencing
    - Log all custom asset injections at DEBUG level
    - Log all fallthroughs at TRACE level
  - Register the hook in the DLL initialization sequence (the actual registration is wired up in Task 13)

  **Must NOT do**:
  - Do not perform ANY HTTP or file I/O in the hook — it runs on the game thread. Only read from AssetCDN's in-memory cache.
  - Do not modify the player's LoadoutSlot data — read-only access to SymbolIds
  - Do not handle audio, animation, or shader assets — mesh/texture cosmetics ONLY (v1 guardrail)
  - Do not implement hot-reload — hook reads whatever was cached at startup
  - Do not hardcode SymbolIds (that was Task 6's PoC) — all IDs come from the player's actual loadout data
  - Do not add new hooking framework code — use existing `PatchDetour()` from `common/hooking.h`

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: This is the core hook logic — must handle 20+ code paths, integrate with two modules (hooking framework + AssetCDN), requires precise memory layout knowledge from RE tasks, and must be crash-proof. Needs deep reasoning about game thread safety.
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on PoC from Task 6 and AssetCDN from Task 11)
  - **Parallel Group**: Wave 4 (sequential after Wave 3)
  - **Blocks**: Task 13 (initialization wiring)
  - **Blocked By**: Task 6 (PoC hook — provides the base hook code to generalize), Task 11 (AssetCDN module — provides HasAsset/GetAsset API)

  **References**:

  **Pattern References**:
  - `src/gamepatches/cosmetics_hook.h` + `cosmetics_hook.cpp` — (Task 6 output) The PoC hook for a single hardcoded slot. This task generalizes it to all slots.
  - `src/gamepatches/patches.cpp` — Contains all existing `PatchDetour()` calls. Follow the same registration pattern.
  - `src/common/hooking.h` — `PatchDetour()` API for Detours/MinHook. Hook registration follows: declare original function pointer, write hook function, call PatchDetour.

  **API/Type References**:
  - `src/common/echovr.h:LoadoutSlot` — Struct with 21 SymbolId fields. Map each field to its slot index for the dispatch table.
  - `src/common/echovr.h:SymbolId` — INT64 typedef. All cosmetic IDs are this type.
  - `src/gamepatches/asset_cdn.h` — (Task 11 output) `AssetCDN::HasAsset(SymbolId, uint8_t)`, `AssetCDN::GetAsset(SymbolId, uint8_t)`, `AssetCDN::IsReady()`.
  - `docs/ghidra/FUN_1404f37a0_analysis.md` — (Task 2 output) RE analysis of the hook target. Documents function signature, parameters, return value, and CResourceID construction.
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — Complete LoadoutData and EntrantData struct layouts with exact offsets.

  **External References**:
  - None — all dependencies are internal.

  **WHY Each Reference Matters**:
  - `cosmetics_hook.*` (Task 6): This IS the code you're extending. The PoC has the hook function signature and original function pointer — generalize, don't rewrite.
  - `echovr.h:LoadoutSlot`: You need the exact field order to build the slot dispatch table. Field offsets determine how to index into the struct.
  - `asset_cdn.h`: The HasAsset/GetAsset API is your ONLY interface to cached assets. Do not reach into AssetCDN internals.
  - `FUN_1404f37a0_analysis.md`: Without this, you don't know what the hook target's parameters mean or what to return. Critical for correctness.
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: The loadout data comes from runtime memory at specific offsets. Wrong offsets = crash.

  **Acceptance Criteria**:
  - [ ] `cosmetics_hook.cpp` handles all 21 LoadoutSlot fields via dispatch table
  - [ ] `AssetCDN::HasAsset()` is called to check for custom assets before injection
  - [ ] Falls through to original function when no custom asset exists or AssetCDN not ready
  - [ ] No HTTP/file I/O in the hook function body
  - [ ] `make build` succeeds
  - [ ] All 21 slot indices are mapped in the dispatch table

  **QA Scenarios:**

  ```
  Scenario: Hook compiles and links with AssetCDN integration
    Tool: Bash
    Preconditions: Tasks 6 and 11 complete, source files written
    Steps:
      1. Run: make build
      2. Check build output for errors/warnings in cosmetics_hook.cpp
      3. Verify output DLL is produced
    Expected Result: Clean build, no linker errors referencing AssetCDN symbols
    Failure Indicators: Unresolved symbols for AssetCDN::HasAsset/GetAsset, compilation errors
    Evidence: .sisyphus/evidence/task-12-build-success.txt

  Scenario: Dispatch table completeness verification (code review)
    Tool: Bash
    Preconditions: cosmetics_hook.cpp exists with dispatch table
    Steps:
      1. grep -c "selectionmode\|banner\|booster\|bracer\|chassis\|decal\|decal_body\|emissive\|emote\|secondemote\|goal_fx\|medal\|pattern\|pattern_body\|pip\|tag\|tint\|tint_alignment_a\|tint_alignment_b\|tint_body\|title" src/gamepatches/cosmetics_hook.cpp
      2. Verify count >= 21 (all fields referenced)
      3. grep -n "HasAsset\|GetAsset\|IsReady" src/gamepatches/cosmetics_hook.cpp — verify AssetCDN integration
      4. grep -n "original" src/gamepatches/cosmetics_hook.cpp — verify fallthrough to original function exists
    Expected Result: All 21 slot field names present, AssetCDN calls present, original function fallthrough present
    Failure Indicators: Missing slot names, no AssetCDN integration, no fallthrough path
    Evidence: .sisyphus/evidence/task-12-dispatch-review.txt

  Scenario: No blocking I/O in hook path (code review)
    Tool: Bash
    Preconditions: cosmetics_hook.cpp exists
    Steps:
      1. grep -n "curl_\|fopen\|fread\|ReadFile\|CreateFile\|std::ifstream\|HTTP\|Download" src/gamepatches/cosmetics_hook.cpp
      2. Verify zero matches (no I/O in hook code)
    Expected Result: Zero matches — hook only reads from in-memory cache
    Failure Indicators: Any file I/O or HTTP call found in the hook function
    Evidence: .sisyphus/evidence/task-12-no-blocking-io.txt
  ```

  **Commit**: YES
  - Message: `feat(gamepatches): generalize cosmetics hook to all 20 slots with AssetCDN integration`
  - Files: `src/gamepatches/cosmetics_hook.h`, `src/gamepatches/cosmetics_hook.cpp`
  - Pre-commit: `make build`

- [ ] 13. **AssetCDN initialization — startup integration + cosmetics hook registration**

  **What to do**:
  - Modify `src/gamepatches/patches.cpp` to wire up the full CDN cosmetics pipeline at DLL startup:
    1. **Uncomment `asset_cdn_url` config reading** (patches.cpp:760-767):
       - This already reads the CDN URL from the game config file
       - Verify the parsed URL is stored and accessible
    2. **Uncomment `AssetCDN::Initialize()` call** (patches.cpp:1430-1433):
       - Pass the parsed `asset_cdn_url` string to `AssetCDN::Initialize()`
       - This triggers background manifest fetch and package download (Task 11)
    3. **Register the cosmetics injection hook**:
       - After AssetCDN::Initialize(), call the cosmetics hook registration function from Task 12
       - Use `PatchDetour()` to hook `FUN_1404f37a0` at RVA `0x004F37A0`
       - Add the hook RVA to `src/gamepatches/patch_addresses.h`
    4. **Add game version guard**:
       - Check game version is `34.4.631547.1` before enabling hooks
       - If wrong version: log warning with detected vs expected version, skip hook registration
       - Game must still function — just without custom cosmetics
    5. **Conditional initialization**:
       - Only call AssetCDN::Initialize() if `asset_cdn_url` is non-empty
       - Only register cosmetics hook if AssetCDN initialized successfully
       - Log at INFO level: "CDN cosmetics enabled: {url}" or "CDN cosmetics disabled: no URL configured"
    6. **Include ordering**:
       - Add `#include "asset_cdn.h"` and `#include "cosmetics_hook.h"` to patches.cpp
       - Place includes with existing game patches includes

  **Must NOT do**:
  - Do not modify AssetCDN module code — only wire it up
  - Do not modify cosmetics hook code — only register it
  - Do not add new config keys — use the existing `asset_cdn_url`
  - Do not add retry/reconnect logic — single initialization attempt
  - Do not block DLL startup on CDN fetch — AssetCDN::Initialize() is async (launches background thread)
  - Do not change existing hook registrations — add new ones alongside

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Modifying patches.cpp — the central integration point. Requires understanding of the existing initialization sequence, config parsing, and hook registration patterns. Medium complexity but high impact if wrong.
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on Tasks 9, 10, 11, 12 all being complete)
  - **Parallel Group**: Wave 4 (sequential — last implementation task before Final Verification)
  - **Blocks**: F1, F2, F3, F4 (all final verification tasks)
  - **Blocked By**: Task 9 (Go manifest CLI — must exist for end-to-end), Task 10 (Go package CLI — must exist for end-to-end), Task 11 (AssetCDN module — provides Initialize()), Task 12 (cosmetics hook — provides hook registration function)

  **References**:

  **Pattern References**:
  - `src/gamepatches/patches.cpp:760-767` — Existing `asset_cdn_url` config reading (commented out). UNCOMMENT THIS. Shows exactly how config values are parsed.
  - `src/gamepatches/patches.cpp:1430-1433` — Existing `AssetCDN::Initialize()` call (commented out). UNCOMMENT THIS. Shows exactly where in the init sequence CDN init was intended.
  - `src/gamepatches/patches.cpp` (full file) — Contains all existing `PatchDetour()` hook registrations. Follow the same pattern for the cosmetics hook. Note the initialization order.
  - `src/gamepatches/patch_addresses.h` — All existing RVA definitions. Add `FUN_1404f37a0` RVA here following the same naming convention.

  **API/Type References**:
  - `src/gamepatches/asset_cdn.h` — (Task 11 output) `AssetCDN::Initialize(const std::string& cdnUrl)`. The function to call.
  - `src/gamepatches/cosmetics_hook.h` — (Task 12 output) Hook registration function. Contains the `PatchDetour()` call for `FUN_1404f37a0`.
  - `src/common/hooking.h` — `PatchDetour()` API. Used by the cosmetics hook.

  **External References**:
  - None — all integration points are internal.

  **WHY Each Reference Matters**:
  - `patches.cpp:760-767`: This is the EXACT code to uncomment. Don't reinvent config reading.
  - `patches.cpp:1430-1433`: This is the EXACT insertion point. The prior developers planned for this.
  - `patches.cpp` (full): You need to understand the init sequence to place new registrations correctly. Wrong ordering = crash or race conditions.
  - `patch_addresses.h`: New RVA must follow the existing naming pattern. Missing entry = hook can't find target address.

  **Acceptance Criteria**:
  - [ ] `patches.cpp:760-767` is uncommented — `asset_cdn_url` config reading is active
  - [ ] `patches.cpp:1430-1433` is uncommented — `AssetCDN::Initialize()` call is active
  - [ ] Cosmetics hook registered via `PatchDetour()` after AssetCDN init
  - [ ] `FUN_1404f37a0` RVA added to `patch_addresses.h`
  - [ ] Game version check guards hook registration (version `34.4.631547.1`)
  - [ ] Conditional init: CDN only enabled if `asset_cdn_url` is non-empty
  - [ ] `make build` succeeds
  - [ ] No changes to existing hook registrations

  **QA Scenarios:**

  ```
  Scenario: Startup integration compiles and builds
    Tool: Bash
    Preconditions: Tasks 11 and 12 complete, patches.cpp modified
    Steps:
      1. Run: make build
      2. Check for any errors/warnings in patches.cpp compilation
      3. Verify output DLL is produced
    Expected Result: Clean build, DLL produced
    Failure Indicators: Compilation errors from new includes, linker errors from AssetCDN/cosmetics_hook symbols
    Evidence: .sisyphus/evidence/task-13-build-success.txt

  Scenario: Config reading and conditional init verification (code review)
    Tool: Bash
    Preconditions: patches.cpp modified
    Steps:
      1. grep -n "asset_cdn_url" src/gamepatches/patches.cpp — verify config reading is uncommented (no // prefix)
      2. grep -n "AssetCDN::Initialize" src/gamepatches/patches.cpp — verify init call is uncommented
      3. grep -n "34.4.631547.1\|game.*version" src/gamepatches/patches.cpp — verify version check exists
      4. grep -n "PatchDetour.*37a0\|FUN_1404f37a0\|cosmetics.*hook" src/gamepatches/patches.cpp — verify hook registration
      5. grep -n "FUN_1404f37a0\|COSMETICS_HOOK" src/gamepatches/patch_addresses.h — verify RVA entry
    Expected Result: All patterns found, no commented-out versions remaining for the CDN code
    Failure Indicators: Config reading still commented, missing version check, missing hook registration, missing RVA
    Evidence: .sisyphus/evidence/task-13-integration-review.txt

  Scenario: Conditional init — CDN disabled when URL empty (code review)
    Tool: Bash
    Preconditions: patches.cpp modified
    Steps:
      1. grep -A5 "asset_cdn_url" src/gamepatches/patches.cpp — find the conditional check
      2. Verify there is an if-check that skips Initialize() when URL is empty
      3. grep -n "CDN cosmetics disabled\|CDN cosmetics enabled\|no URL" src/gamepatches/patches.cpp — verify logging
    Expected Result: Conditional check present, both enabled/disabled log messages exist
    Failure Indicators: No conditional check — Initialize() called unconditionally with empty URL
    Evidence: .sisyphus/evidence/task-13-conditional-init.txt
  ```

  **Commit**: YES (grouped with Task 12)
  - Message: `feat(gamepatches): full resource injection hook + startup integration`
  - Files: `src/gamepatches/patches.cpp`, `src/gamepatches/patch_addresses.h`
  - Pre-commit: `make build`

- [ ] 14. **Format specification document — finalize `docs/cosmetics-cdn-format.md`**

  **What to do**:
  1. **Read the DRAFT** produced by Task 8 at `docs/cosmetics-cdn-format.md` — this contains the initial format spec written during the RE/format-design phase
  2. **Cross-reference against final implementations**:
     - Read `tools/cosmetics-manifest/main.go` (Task 9 output) — verify manifest JSON schema in the doc matches what the CLI actually produces
     - Read `tools/cosmetics-package/main.go` (Task 10 output) — verify package binary layout in the doc matches what the CLI actually produces
     - Read `src/gamepatches/asset_cdn.h` and `asset_cdn.cpp` (Task 11 output) — verify the C++ consumer's expectations match the documented format
     - Read `src/gamepatches/cosmetics_hook.h` and `cosmetics_hook.cpp` (Task 12 output) — verify hook behavior matches documented resource resolution
  3. **Finalize the following sections** (update DRAFT to match reality):
     - **Manifest JSON Schema**: Exact fields, types, example with real SymbolId hashes. Must match Go CLI output byte-for-byte.
     - **Package Binary Layout**: Header magic, version, offset table, chunk format, compression (Zstd). Must match Go CLI output and C++ reader.
     - **URL Structure**: `https://cdn.echo.taxi/v{N}/manifest.json`, `https://cdn.echo.taxi/v{N}/packages/{hash}.pkg`. Versioned, immediately auditable.
     - **Versioning & Rollback**: How manifest versions work, how to roll back (point to previous version directory), how clients detect updates.
     - **Client Cache Behavior**: `%LOCALAPPDATA%/EchoVR/CDN/` structure, file naming, staleness detection (ETag or manifest version comparison).
     - **CLI Tool Usage**: Complete usage examples for both `cosmetics-manifest` and `cosmetics-package` tools with flags, inputs, outputs.
     - **SymbolId Generation**: How new cosmetic SymbolIds are computed (CSymbol64_Hash with naming convention), reserved ranges if any.
     - **Error Handling**: What happens on download failure, corrupt package, version mismatch, unknown SymbolId on other clients.
  4. **Ensure the document is immediately auditable** per user requirement:
     - A developer should be able to read the doc, `curl` a manifest URL, and verify every field matches the spec
     - Include concrete examples with actual hex values, not placeholder descriptions
     - Include a "Quick Verification" section with copy-paste curl/jq commands
  5. **Add a version history table** at the bottom:
     - `| Version | Date | Changes |` format
     - Initial entry: `v1.0 | {date} | Initial specification`

  **Must NOT do**:
  - Do not invent format details — only document what was actually implemented in Tasks 8-12
  - Do not add features not in the implementation (no auth tokens, no signing, no differential updates)
  - Do not write tutorial-style prose — this is a technical specification, terse and precise
  - Do not duplicate code — reference source files, don't inline implementation details
  - Do not add diagrams or images — text-only, machine-parseable where possible

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: Primary output is a technical document. Requires reading multiple source files and synthesizing into a coherent spec. No code changes.
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: No browser interaction needed — pure document writing
    - `git-master`: No git operations — just file editing

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on Tasks 8-12 all being complete to cross-reference)
  - **Parallel Group**: Wave 4 (with Tasks 12, 13 — but sequentially after them)
  - **Blocks**: F1 (plan compliance audit references this doc)
  - **Blocked By**: Task 8 (produces the DRAFT), Task 9 (manifest CLI — must exist to cross-reference), Task 10 (package CLI — must exist to cross-reference), Task 11 (AssetCDN module — must exist to cross-reference), Task 12 (cosmetics hook — must exist to cross-reference)

  **References**:

  **Pattern References**:
  - `docs/ghidra/ECHOVR_STRUCT_MEMORY_MAP.md` — Example of how this project writes technical specs: terse, struct-focused, with exact offsets/sizes. Match this tone.
  - `extras/docs/HASH_DISCOVERY_COMPLETE.md` — Another reference spec in the project. Note the level of detail and concrete values.

  **API/Type References**:
  - `docs/cosmetics-cdn-format.md` — (Task 8 DRAFT output) The initial draft to be finalized. This is the file being edited.
  - `tools/cosmetics-manifest/main.go` — (Task 9 output) Go CLI for manifest generation. Cross-reference JSON output format.
  - `tools/cosmetics-package/main.go` — (Task 10 output) Go CLI for package building. Cross-reference binary layout.
  - `src/gamepatches/asset_cdn.h` — (Task 11 output) C++ manifest parser and download logic. Verify it expects what the doc says.
  - `src/gamepatches/cosmetics_hook.h` — (Task 12 output) C++ resource injection. Verify resource resolution matches doc.

  **External References**:
  - Cloudflare R2 docs: `https://developers.cloudflare.com/r2/` — For URL structure and caching behavior reference.

  **WHY Each Reference Matters**:
  - `ECHOVR_STRUCT_MEMORY_MAP.md`: Sets the tone and detail level. Your doc should feel like it belongs next to this one.
  - `HASH_DISCOVERY_COMPLETE.md`: Shows how concrete values (hex hashes, function addresses) are documented in this project.
  - Task 8 DRAFT: This is your starting point — don't start from scratch, EDIT the existing draft.
  - Task 9/10 Go CLIs: The doc MUST match their actual output. Read the code, run the tools, compare.
  - Task 11/12 C++ code: The doc MUST match what the consumer expects. Any mismatch = runtime failures.

  **Acceptance Criteria**:
  - [ ] `docs/cosmetics-cdn-format.md` exists and is finalized (not a draft)
  - [ ] Manifest JSON schema section matches `tools/cosmetics-manifest/` output exactly
  - [ ] Package binary layout section matches `tools/cosmetics-package/` output exactly
  - [ ] URL structure section shows versioned paths under `https://cdn.echo.taxi/`
  - [ ] Client cache section documents `%LOCALAPPDATA%/EchoVR/CDN/` structure
  - [ ] CLI usage section has complete examples for both tools
  - [ ] Quick Verification section has copy-paste curl/jq commands
  - [ ] Version history table present
  - [ ] No format details that contradict actual implementation

  **QA Scenarios:**

  ```
  Scenario: Document completeness — all required sections present
    Tool: Bash
    Preconditions: docs/cosmetics-cdn-format.md finalized
    Steps:
      1. grep -c "## Manifest" docs/cosmetics-cdn-format.md — manifest schema section exists
      2. grep -c "## Package" docs/cosmetics-cdn-format.md — package layout section exists
      3. grep -c "## URL" docs/cosmetics-cdn-format.md — URL structure section exists
      4. grep -c "## Cache\|## Client Cache" docs/cosmetics-cdn-format.md — cache section exists
      5. grep -c "## CLI\|## Tool" docs/cosmetics-cdn-format.md — CLI usage section exists
      6. grep -c "## Version History\|## Changelog" docs/cosmetics-cdn-format.md — version history exists
      7. grep -c "## Quick Verification\|## Verification" docs/cosmetics-cdn-format.md — verification section exists
    Expected Result: All grep counts >= 1 (every section present)
    Failure Indicators: Any section missing (grep returns 0)
    Evidence: .sisyphus/evidence/task-14-doc-completeness.txt

  Scenario: Cross-reference — manifest schema matches Go CLI output
    Tool: Bash
    Preconditions: Task 9 complete, docs/cosmetics-cdn-format.md finalized
    Steps:
      1. Read the manifest JSON schema from docs/cosmetics-cdn-format.md
      2. Read the JSON marshaling code in tools/cosmetics-manifest/main.go
      3. Compare field names, types, and structure — every field in the Go struct must appear in the doc
      4. Check for fields in the doc that don't exist in the Go code (phantom fields)
    Expected Result: 1:1 correspondence between documented schema and Go output
    Failure Indicators: Missing fields, extra fields, wrong types, wrong nesting
    Evidence: .sisyphus/evidence/task-14-manifest-crossref.txt

  Scenario: Cross-reference — package binary layout matches Go CLI output
    Tool: Bash
    Preconditions: Task 10 complete, docs/cosmetics-cdn-format.md finalized
    Steps:
      1. Read the package binary layout from docs/cosmetics-cdn-format.md
      2. Read the binary writing code in tools/cosmetics-package/main.go
      3. Compare magic bytes, header fields, offset calculations, chunk structure
      4. Verify documented byte offsets match the Go writer's actual Write() calls
    Expected Result: Documented layout matches Go implementation exactly
    Failure Indicators: Wrong magic bytes, wrong field sizes, wrong offset calculations
    Evidence: .sisyphus/evidence/task-14-package-crossref.txt

  Scenario: Auditability — Quick Verification commands work
    Tool: Bash
    Preconditions: docs/cosmetics-cdn-format.md finalized
    Steps:
      1. Extract all curl/jq commands from the Quick Verification section
      2. Verify each command is syntactically valid (curl --help check, jq --help check)
      3. Verify URL patterns use https://cdn.echo.taxi/ base
      4. Verify jq filters reference fields from the documented manifest schema
    Expected Result: All commands are syntactically valid and reference correct fields
    Failure Indicators: Broken curl syntax, jq filters referencing non-existent fields
    Evidence: .sisyphus/evidence/task-14-auditability.txt
  ```

  **Commit**: YES
  - Message: `docs: finalize cosmetics CDN format specification`
  - Files: `docs/cosmetics-cdn-format.md`
  - Pre-commit: `grep -c '## ' docs/cosmetics-cdn-format.md` (verify sections exist)

<!-- TASKS_END -->

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents run in PARALLEL. ALL must APPROVE. Rejection → fix → re-run.

- [ ] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in .sisyphus/evidence/. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [ ] F2. **Code Quality Review** — `unspecified-high`
  Run full build. Review all new/changed files for: `as any`/`@ts-ignore` (N/A for C++), empty catches, raw printf in prod (use Log()), commented-out code, unused includes. Check AI slop: excessive comments, over-abstraction, generic names. Verify symbol_hash.h is constexpr. Verify no blocking HTTP in game thread.
  Output: `Build [PASS/FAIL] | Files [N clean/N issues] | VERDICT`

- [ ] F3. **Integration QA** — `unspecified-high`
  Build entire project from clean. Verify: symbol_hash.h compiles and produces correct hashes. Go CLI tools build and produce valid output files. AssetCDN module compiles with no link errors. Manifest/package files pass format validation. Run every QA scenario from every task.
  Output: `Build [PASS/FAIL] | Scenarios [N/N pass] | VERDICT`

- [ ] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual code written. Verify 1:1 — everything in spec was built, nothing beyond spec. Check "Must NOT do" compliance. No audio/animation/shader code. No in-game UI. No hot-reload. No blocking HTTP. No cache eviction. No overwrites of existing assets.
  Output: `Tasks [N/N compliant] | Scope [CLEAN/N issues] | VERDICT`

---

## Commit Strategy

| After | Commit Message | Key Files |
|-------|---------------|-----------|
| Task 1 | `feat(common): add constexpr CSymbol64_Hash implementation` | `src/common/symbol_hash.h` |
| Tasks 2-4 | `docs(ghidra): RE findings — hook validation, binary format, unknown SymbolId` | `docs/ghidra/*.md` |
| Task 5 | `build(gamepatches): add jsoncpp linking and AssetCDN scaffolding` | `src/gamepatches/CMakeLists.txt`, `asset_cdn.h`, `asset_cdn.cpp` |
| Task 6 | `feat(gamepatches): PoC — hardcoded cosmetic injection on single slot` | `src/gamepatches/asset_cdn.cpp` |
| Task 7 | `test: verify Go/C++ hash consistency` | evidence files |
| Task 8 | `docs: cosmetics CDN format design (manifest + package + URL scheme)` | `docs/cosmetics-cdn-format.md` (draft) |
| Tasks 9-10 | `feat(tools): Go CLI tools for cosmetics manifest and package building` | `tools/cosmetics-manifest/`, `tools/cosmetics-package/` |
| Task 11 | `feat(gamepatches): AssetCDN module — manifest fetch, download, cache` | `src/gamepatches/asset_cdn.*` |
| Tasks 12-13 | `feat(gamepatches): full resource injection hook + startup integration` | `src/gamepatches/asset_cdn.*`, `patches.cpp` |
| Task 14 | `docs: finalize cosmetics CDN format specification` | `docs/cosmetics-cdn-format.md` |

---

## Success Criteria

### Verification Commands
```bash
# Build passes
make build  # Expected: zero errors, zero warnings

# symbol_hash.h produces correct hashes
# (Verified in Task 7 — Go vs C++ comparison)

# Go CLI tools build
cd tools/cosmetics-manifest && go build ./...  # Expected: binary produced
cd tools/cosmetics-package && go build ./...   # Expected: binary produced

# DLL contains AssetCDN symbols
nm build/*/bin/gamepatches.dll | grep -i assetcdn  # Expected: symbols present
```

### Final Checklist
- [ ] All "Must Have" items present and verified
- [ ] All "Must NOT Have" items absent from codebase
- [ ] Project builds cleanly
- [ ] At least ONE cosmetic renders in-game (vertical slice)
- [ ] Go CLI tools produce valid manifest/package files
- [ ] Format spec document complete
- [ ] All evidence files present in `.sisyphus/evidence/`
