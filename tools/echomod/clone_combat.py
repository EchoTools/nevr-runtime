#!/usr/bin/env python3
"""
Create mpl_arenacombat: merge mpl_arena_a (base arena) with combat actors
from mpl_combat_combustion (guns, bullets, explosions, etc.).

Pipeline:
  Phase 0: Extract archive
  Phase 1: Copy to build directory
  Phase 2: Clone mpl_arena_a -> mpl_arenacombat (file copy + level hash patch)
  Phase 3: Transplant combat actors from mpl_combat_combustion into mpl_arenacombat
  Phase 4: Repack archive
  Phase 5: Update hash_lookup.json
"""

import sys
import os
import struct
import shutil
import subprocess
import json
import logging
from pathlib import Path
from collections import defaultdict

sys.path.insert(0, str(Path(__file__).parent))

from echomod.radhash import rad_hash
from echomod.resources.actor_data import ActorDataResource
from echomod.resources.transform_cr import TransformCR
from echomod.resources.scene import CGSceneResource
from echomod.resources.generic_cr import GenericCR
from echomod.resources.model_cr import clone_model_entries
from echomod.resources.animation_cr import clone_animation_entries
from echomod.resources.compound_cr import clone_compound_cr
from echomod.resources.syncgrab_cr import clone_syncgrab_cr
from echomod.resources.instancemodel_cr import clone_instancemodel_cr

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(level=logging.INFO, format="%(message)s")
log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).parent
ARCHIVE_DIR = SCRIPT_DIR / "5932408047"
CLEAN_EXTRACT_DIR = SCRIPT_DIR / "echovr_clean_extract"
BUILD_DIR = SCRIPT_DIR / "echovr_build_combat"
OUTPUT_DIR = SCRIPT_DIR / "patched_output"
HASH_LOOKUP_PATH = SCRIPT_DIR / "hash_lookup.json"

# Source levels
ARENA_LEVEL = "mpl_arena_a"
COMBAT_LEVEL = "0xCB9977F7FC2B4526"  # mpl_lobby_b_combat (lobby combat sub-level with guns)
COMBAT_CSR_LEVEL = COMBAT_LEVEL
# Target level
TARGET_LEVEL = "mpl_arenacombat"

ARENA_LEVEL_HASH = rad_hash(ARENA_LEVEL)
# COMBAT_LEVEL is already a hex hash string — parse it directly, don't re-hash
COMBAT_LEVEL_HASH = int(COMBAT_LEVEL, 16) if COMBAT_LEVEL.startswith("0x") else rad_hash(COMBAT_LEVEL)
TARGET_LEVEL_HASH = rad_hash(TARGET_LEVEL)

ARENA_LEVEL_HASH_LE = struct.pack("<Q", ARENA_LEVEL_HASH)
TARGET_LEVEL_HASH_LE = struct.pack("<Q", TARGET_LEVEL_HASH)
COMBAT_LEVEL_HASH_LE = struct.pack("<Q", COMBAT_LEVEL_HASH)

log.info("Level hashes:")
log.info("  Arena:  0x%016X (%s)", ARENA_LEVEL_HASH, ARENA_LEVEL)
log.info("  Combat: 0x%016X (%s)", COMBAT_LEVEL_HASH, COMBAT_LEVEL)
log.info("  Target: 0x%016X (%s)", TARGET_LEVEL_HASH, TARGET_LEVEL)

# CR types that need dedicated parsers (not generic merge)
DEDICATED_CR_TYPES = {
    "CActorDataResourceWin10",
    "CTransformCRWin10",
    "CGSceneResourceWin10",
    "CModelCRWin10",
    "CAnimationCRWin10",
    "CScriptCRWin10",
    "CComponentLODCRWin10",
    "CParticleEffectCRWin10",
    "CR15SyncGrabCRWin10",
    "CInstanceModelCRWin10",
    "CR15NetBitFieldCRWin10",  # Multi-array, handled explicitly
    "carchiveresourceWin10",
    "CArchiveResourceWin10",
    # Non-CR types that shouldn't be merged:
    "CGameLevelResourceWin10",
    "CGameLevelInfoResourceWin10",
    "CComponentSpaceResourceWin10",
    "CBVHResourceWin10",
    "CGFSEffectsResourceWin10",
    "CGStaticInstanceResourceWin10",
    "CGReflectionProbeResourceWin10",
    "COccluderMeshResourceWin10",
    "CLevelAABBCRWin10",
    "CListCRWin10",
    "CStaticInstanceModelCRWin10",
    "CStaticLODRegionTargetCRWin10",
    "CStaticRaycastCRWin10",
    "CMaterialTypeCRWin10",
    "CMaterialTypesBVHResourceWin10",
    "CActorLODCRWin10",
    "CActorRegionLODCRWin10",
    "CComponentRegionLODCRWin10",
    "CDynamicLODRegionTargetCRWin10",
    # LOD zone CRs reference OBB shapes only in combat CGSceneResource —
    # transplanting them causes "cannot find OBB" spam.
    "CComponentLODZoneCRWin10",
    # Level-infrastructure CRs that conflict when transplanted from combat.
    # These reference level-specific scene graph data from the combat sub-level.
}

# Compound CRs (entry_size -> known inline descriptor offsets)
COMPOUND_CR_ENTRY_SIZES = {
    720: "CScriptCRWin10",
    96: "CComponentLODCRWin10",
    152: "CParticleEffectCRWin10",
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run_tool(args: list[str]) -> None:
    cmd = [sys.executable, str(SCRIPT_DIR / "rad_archive_tool.py")] + args
    log.info("  Running: %s", " ".join(cmd))
    subprocess.run(cmd, check=True)


def find_type_dir(mf_dir: Path, type_name: str) -> Path | None:
    candidate = mf_dir / type_name
    if candidate.is_dir():
        return candidate
    type_hash = rad_hash(type_name)
    candidate = mf_dir / f"0x{type_hash:016X}"
    if candidate.is_dir():
        return candidate
    return None


def read_resource(mf_dir: Path, type_name: str, level_name: str) -> bytes | None:
    type_dir = find_type_dir(mf_dir, type_name)
    if type_dir is None:
        return None
    path = type_dir / f"{level_name}.bin"
    if not path.is_file():
        return None
    return path.read_bytes()


def write_resource(mf_dir: Path, type_name: str, level_name: str, data: bytes) -> None:
    type_dir = find_type_dir(mf_dir, type_name)
    if type_dir is None:
        # Create the directory
        type_dir = mf_dir / type_name
        type_dir.mkdir(exist_ok=True)
    path = type_dir / f"{level_name}.bin"
    path.write_bytes(data)
    log.info("    Wrote %s/%s (%d bytes)", type_dir.name, path.name, len(data))


def get_mf_dir(build_dir: Path) -> Path:
    """Get the main manifest directory."""
    mf_dirs = sorted(
        [d for d in build_dir.iterdir() if d.is_dir()],
        key=lambda d: sum(1 for _ in d.iterdir()),
        reverse=True,
    )
    if not mf_dirs:
        raise RuntimeError(f"No manifest directories found in {build_dir}")
    return mf_dirs[0]


# ---------------------------------------------------------------------------
# Simple CR merge: concatenate entries from two files
# ---------------------------------------------------------------------------

def merge_simple_cr(arena_data: bytes, combat_data: bytes,
                    combat_actor_hashes: set[int]) -> bytes:
    """Merge entries from combat file into arena file (simple CR format).

    Only includes combat entries whose actor_hash is in combat_actor_hashes.
    """
    if len(arena_data) < 56 or len(combat_data) < 56:
        return arena_data

    # Parse arena descriptor
    a_dbs = struct.unpack_from('<Q', arena_data, 8)[0]
    a_cap = struct.unpack_from('<Q', arena_data, 40)[0]
    a_count = struct.unpack_from('<Q', arena_data, 48)[0]
    if a_cap == 0:
        return arena_data
    a_esize = a_dbs // a_cap

    # Parse combat descriptor
    c_dbs = struct.unpack_from('<Q', combat_data, 8)[0]
    c_cap = struct.unpack_from('<Q', combat_data, 40)[0]
    c_count = struct.unpack_from('<Q', combat_data, 48)[0]
    if c_cap == 0:
        return arena_data
    c_esize = c_dbs // c_cap

    if a_esize != c_esize:
        log.warning("      Entry size mismatch: arena=%d combat=%d", a_esize, c_esize)
        return arena_data

    # Check it's actually simple (no tail data)
    a_expected_end = 56 + a_count * a_esize
    if a_expected_end != len(arena_data):
        return arena_data  # Not simple, bail

    c_expected_end = 56 + c_count * c_esize
    if c_expected_end != len(combat_data):
        return arena_data  # Combat is compound, bail

    # Collect combat entries for the actors we want
    new_entries = bytearray()
    added = 0
    for i in range(c_count):
        off = 56 + i * c_esize
        actor_hash = struct.unpack_from('<Q', combat_data, off + 8)[0]
        if actor_hash in combat_actor_hashes:
            entry = bytearray(combat_data[off:off + c_esize])
            # Reset node_index to 0xFFFF (unresolved)
            struct.pack_into('<H', entry, 16, 0xFFFF)
            new_entries.extend(entry)
            added += 1

    if added == 0:
        return arena_data

    # Build merged file
    new_count = a_count + added
    result = bytearray(arena_data[:56])  # Copy header
    struct.pack_into('<Q', result, 8, new_count * a_esize)   # dbs
    struct.pack_into('<Q', result, 40, new_count)             # capacity
    struct.pack_into('<Q', result, 48, new_count)             # count
    result.extend(arena_data[56:a_expected_end])              # Arena entries
    result.extend(new_entries)                                 # Combat entries

    return bytes(result)


def create_simple_cr(combat_data: bytes, combat_actor_hashes: set[int]) -> bytes:
    """Create a new simple CR file with only entries for specified actors."""
    if len(combat_data) < 56:
        return combat_data

    c_dbs = struct.unpack_from('<Q', combat_data, 8)[0]
    c_cap = struct.unpack_from('<Q', combat_data, 40)[0]
    c_count = struct.unpack_from('<Q', combat_data, 48)[0]
    if c_cap == 0 or c_count == 0:
        return combat_data
    c_esize = c_dbs // c_cap

    # Collect matching entries
    entries = bytearray()
    added = 0
    for i in range(c_count):
        off = 56 + i * c_esize
        actor_hash = struct.unpack_from('<Q', combat_data, off + 8)[0]
        if actor_hash in combat_actor_hashes:
            entry = bytearray(combat_data[off:off + c_esize])
            struct.pack_into('<H', entry, 16, 0xFFFF)
            entries.extend(entry)
            added += 1

    if added == 0:
        # Return empty descriptor
        result = bytearray(56)
        struct.pack_into('<I', result, 28, 1)  # flags = 1
        return bytes(result)

    result = bytearray(combat_data[:56])
    struct.pack_into('<Q', result, 8, added * c_esize)
    struct.pack_into('<Q', result, 40, added)
    struct.pack_into('<Q', result, 48, added)
    result.extend(entries)
    return bytes(result)


# ---------------------------------------------------------------------------
# Compound CR merge: concatenate flat entries + inline data
# ---------------------------------------------------------------------------

def merge_compound_cr(arena_data: bytes, combat_data: bytes,
                      combat_actor_hashes: set[int]) -> bytes:
    """Merge compound CR entries from combat into arena.

    Strategy: extract combat entries whose actor_hash matches, then append
    them to the arena file using the same flat+inline layout.
    Only merges CRs with KNOWN inline descriptor offsets to avoid corruption.
    """
    if len(arena_data) < 56 or len(combat_data) < 56:
        return arena_data

    # Parse descriptors
    a_dbs = struct.unpack_from('<Q', arena_data, 8)[0]
    a_cap = struct.unpack_from('<Q', arena_data, 40)[0]
    a_count = struct.unpack_from('<Q', arena_data, 48)[0]
    if a_cap == 0:
        return arena_data
    a_esize = a_dbs // a_cap

    c_dbs = struct.unpack_from('<Q', combat_data, 8)[0]
    c_cap = struct.unpack_from('<Q', combat_data, 40)[0]
    c_count = struct.unpack_from('<Q', combat_data, 48)[0]
    if c_cap == 0 or c_count == 0:
        return arena_data
    c_esize = c_dbs // c_cap

    if a_esize != c_esize:
        log.warning("      Compound entry size mismatch: %d vs %d", a_esize, c_esize)
        return arena_data

    # Only merge CRs with KNOWN inline descriptor offsets
    KNOWN_OFFSETS = {
        720: [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664],
        88: [32],      # CR15NetBalanceSettingsCR (+32, CSet<CAssetName>)
        96: [32, 40],  # CComponentLODCR (+40) and CR15NetCaptureVolumeCR (+32)
        152: [96],
        24: [],
    }
    if c_esize not in KNOWN_OFFSETS:
        log.warning("      Unknown compound esize=%d, skipping merge (would corrupt)", c_esize)
        return arena_data
    desc_offsets = KNOWN_OFFSETS[c_esize]

    c_flat_start = 56
    c_flat_end = 56 + c_count * c_esize

    # Compute per-entry inline sizes for combat
    c_inline_pos = []
    pos = c_flat_end
    for i in range(c_count):
        start = pos
        eoff = c_flat_start + i * c_esize
        for d in desc_offsets:
            if eoff + d + 56 <= c_flat_end:
                dbs_val = struct.unpack_from('<Q', combat_data, eoff + d + 8)[0]
                cnt_val = struct.unpack_from('<Q', combat_data, eoff + d + 48)[0]
                if dbs_val > 0 and cnt_val > 0:
                    pos += dbs_val
        c_inline_pos.append((start, pos))

    # Find combat entries to transplant
    transplant_entries = bytearray()
    transplant_inline = bytearray()
    added = 0

    for i in range(c_count):
        eoff = c_flat_start + i * c_esize
        actor_hash = struct.unpack_from('<Q', combat_data, eoff + 8)[0]
        if actor_hash in combat_actor_hashes:
            entry = bytearray(combat_data[eoff:eoff + c_esize])
            struct.pack_into('<H', entry, 16, 0xFFFF)
            transplant_entries.extend(entry)
            if c_inline_pos:
                istart, iend = c_inline_pos[i]
                transplant_inline.extend(combat_data[istart:iend])
            added += 1

    if added == 0:
        return arena_data

    # Build merged file
    a_flat_end = 56 + a_count * a_esize
    a_inline = arena_data[a_flat_end:]

    new_count = a_count + added
    result = bytearray(arena_data[:56])
    struct.pack_into('<Q', result, 8, new_count * a_esize)
    struct.pack_into('<Q', result, 40, new_count)
    struct.pack_into('<Q', result, 48, new_count)

    result.extend(arena_data[56:a_flat_end])   # Arena flat entries
    result.extend(transplant_entries)           # Combat flat entries
    result.extend(a_inline)                    # Arena inline data
    result.extend(transplant_inline)           # Combat inline data

    return bytes(result)


def create_compound_cr(combat_data: bytes, combat_actor_hashes: set[int]) -> bytes:
    """Create a compound CR file with only combat actors' entries.
    Only handles CRs with KNOWN inline descriptor offsets.
    """
    if len(combat_data) < 56:
        return combat_data

    c_dbs = struct.unpack_from('<Q', combat_data, 8)[0]
    c_cap = struct.unpack_from('<Q', combat_data, 40)[0]
    c_count = struct.unpack_from('<Q', combat_data, 48)[0]
    if c_cap == 0 or c_count == 0:
        return combat_data
    c_esize = c_dbs // c_cap

    KNOWN_OFFSETS = {
        720: [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664],
        88: [32],      # CR15NetBalanceSettingsCR (+32, CSet<CAssetName>)
        96: [32, 40],  # CComponentLODCR (+40) and CR15NetCaptureVolumeCR (+32)
        152: [96],
        24: [],
    }
    if c_esize not in KNOWN_OFFSETS:
        log.warning("      Unknown compound esize=%d, skipping create", c_esize)
        return b''  # Return empty to signal skip
    desc_offsets = KNOWN_OFFSETS[c_esize]

    c_flat_start = 56
    c_flat_end = 56 + c_count * c_esize

    # Compute inline positions
    c_inline_pos = []
    pos = c_flat_end
    for i in range(c_count):
        start = pos
        eoff = c_flat_start + i * c_esize
        for d in desc_offsets:
            if eoff + d + 56 <= c_flat_end:
                dbs_val = struct.unpack_from('<Q', combat_data, eoff + d + 8)[0]
                cnt_val = struct.unpack_from('<Q', combat_data, eoff + d + 48)[0]
                if dbs_val > 0 and cnt_val > 0:
                    pos += dbs_val
        c_inline_pos.append((start, pos))

    entries_data = bytearray()
    inline_data = bytearray()
    added = 0

    for i in range(c_count):
        eoff = c_flat_start + i * c_esize
        actor_hash = struct.unpack_from('<Q', combat_data, eoff + 8)[0]
        if actor_hash in combat_actor_hashes:
            entry = bytearray(combat_data[eoff:eoff + c_esize])
            struct.pack_into('<H', entry, 16, 0xFFFF)
            entries_data.extend(entry)
            if c_inline_pos:
                istart, iend = c_inline_pos[i]
                inline_data.extend(combat_data[istart:iend])
            added += 1

    if added == 0:
        result = bytearray(56)
        struct.pack_into('<I', result, 28, 1)
        return bytes(result)

    result = bytearray(combat_data[:56])
    struct.pack_into('<Q', result, 8, added * c_esize)
    struct.pack_into('<Q', result, 40, added)
    struct.pack_into('<Q', result, 48, added)
    result.extend(entries_data)
    result.extend(inline_data)
    return bytes(result)


# ---------------------------------------------------------------------------
# Sublevel filtering: strip to combat-essential actors only
# ---------------------------------------------------------------------------

def _filter_sublevel_to_combat(build_mf: Path, sublevel_name: str,
                               preserve_actor_hashes: set[int] | None = None):
    """Disable non-combat sublevel systems: visibility, LOD zones, collision.

    Three-pronged approach:
      1. Clear ALL bits[0] in ActorData -> all actors start invisible
         (except actors in preserve_actor_hashes — equipment stations etc.)
      2. Set DisableComponentOnInit on ALL LOD CR entries -> prevents LOD zones
         from re-enabling actors based on camera distance
         (except entries for preserved actors)
      3. Replace collision/lighting resources with minimal stubs

    All CRs and scene graph stay intact — only flags are modified, preserving
    CompactPool sizes and BSP tree indices.
    """
    from echomod.resources.actor_data import ActorDataResource

    if preserve_actor_hashes is None:
        preserve_actor_hashes = set()

    log.info("")
    log.info("  --- Filter sublevel: visibility + LOD + collision ---")

    # ── 1. Clear ALL visibility bits ────────────────────────────────────
    # ALL bits cleared — no exceptions. Selective visibility causes BSP/scene
    # graph to render nearby lobby geometry. Equipment station scripts handle
    # their own visibility at runtime (like combat weapons do).
    ad_data = read_resource(build_mf, "CActorDataResourceWin10", sublevel_name)
    ad = ActorDataResource.from_bytes(ad_data)
    orig_vis = sum(bin(w).count('1') for w in ad.bits[0])
    log.info("    Actors: %d, originally visible: %d", ad.actor_count, orig_vis)

    for b_idx in range(len(ad.bits[0])):
        ad.bits[0][b_idx] = 0

    ad_path = build_mf / "CActorDataResourceWin10" / f"{sublevel_name}.bin"
    ad_path.write_bytes(ad.to_bytes())
    log.info("    [VIS] Cleared %d visibility bits -> all actors start invisible", orig_vis)

    # ── 2. Disable ALL LOD + visual CR entries ────────────────────────
    # DisableComponentOnInit (bit 0 at entry +0x18) tells the CS to skip
    # creating the component. This handles:
    #   - LOD zones that re-enable actors overriding bits[0]
    #   - Decals rendered independently of actor visibility
    #   - Billboards (glowing transparent zones)
    #   - Canvas UI elements
    #   - Texture overrides/streaming from lobby geometry
    # Particle effects (CParticleEffectCR) are NOT disabled — needed for
    # combat gun/grenade effects at runtime.
    DISABLE_CR_TYPES = [
        # LOD system CRs
        "CActorLODCRWin10",             # 48B entries
        "CComponentLODCRWin10",          # 96B entries (compound)
        "CComponentRegionLODCRWin10",    # 88B entries
        "CLODRegionCRWin10",             # 200B entries (compound)
        "CStaticLODRegionTargetCRWin10", # 32B entries
        "CDynamicLODRegionTargetCRWin10",# 32B entries
        "CActorRegionLODCRWin10",        # 32B entries
        "CComponentLODZoneCRWin10",      # 144B entries (compound)
        # Visual CRs — lobby decals, lights, billboards
        # NOT disabling CModel/CInstanceModel/CStaticInstanceModel/CSound
        # because combat guns/grenades need those at runtime.
        "CDecalCRWin10",                 # decal projections ("SKIRMISH" text etc)
        "CBillboardCRWin10",             # billboard sprites / glow zones
        "CCanvasUICRWin10",              # UI canvases (preserved actors exempted)
        "CTextureOverrideCRWin10",       # texture overrides
        "CTextureStreamingCRWin10",      # texture streaming triggers
        "COcclusionCullCRWin10",         # occlusion culling (lobby)
        "CFrustumCullCRWin10",           # frustum culling (lobby)
        # NOTE: CStaticInstanceModelCR has 24-byte entries with NO component_flags
        # field at +0x18. Writing DisableComponentOnInit there corrupts the data.
        # Static instance geometry is handled by stubbing CGMeshListResource and
        # CGVisibilityResource instead (empty draw list = nothing to render).
    ]
    cr_disabled = 0
    cr_skipped_preserved = 0
    cr_types_hit = 0
    for cr_type in DISABLE_CR_TYPES:
        cr_path = build_mf / cr_type / f"{sublevel_name}.bin"
        if not cr_path.is_file():
            continue
        data = bytearray(cr_path.read_bytes())
        if len(data) < 56:
            continue
        dbs = struct.unpack_from('<Q', data, 8)[0]
        cap = struct.unpack_from('<Q', data, 40)[0]
        count = struct.unpack_from('<Q', data, 48)[0]
        if cap == 0 or count == 0:
            continue
        esize = dbs // cap
        # Only preserve equipment actors for visual CRs (canvas, decals, etc.)
        # LOD CRs must ALWAYS be disabled — active LOD zones re-enable lobby actors.
        PRESERVE_ALLOWED_CRS = {
            "CCanvasUICRWin10", "CDecalCRWin10", "CBillboardCRWin10",
            "CTextureOverrideCRWin10", "CTextureStreamingCRWin10",
            "COcclusionCullCRWin10", "CFrustumCullCRWin10",
        }
        can_preserve = cr_type in PRESERVE_ALLOWED_CRS

        type_count = 0
        for i in range(count):
            off = 56 + i * esize
            # Skip entries for preserved actors (only for visual CRs, not LOD)
            if can_preserve and off + 16 <= len(data) and preserve_actor_hashes:
                actor_hash = struct.unpack_from('<Q', data, off + 8)[0]
                if actor_hash in preserve_actor_hashes:
                    cr_skipped_preserved += 1
                    continue
            flags_off = off + 0x18
            if flags_off + 4 <= len(data):
                flags = struct.unpack_from('<I', data, flags_off)[0]
                flags |= 0x01  # DisableComponentOnInit
                struct.pack_into('<I', data, flags_off, flags)
                type_count += 1
        cr_path.write_bytes(bytes(data))
        cr_disabled += type_count
        cr_types_hit += 1
    log.info("    [VIS] Disabled %d CR entries across %d types (LOD + visual), "
             "preserved %d entries for equipment stations",
             cr_disabled, cr_types_hit, cr_skipped_preserved)

    # ── 3. Replace collision resources with minimal stubs ────────────
    # CPhysicsResourceInstance creates collision bodies INDEPENDENTLY of
    # CPhysicsCR (bypasses DisableComponentOnInit). The only way to prevent
    # lobby collision is to replace CPhysicsResource with an empty stub
    # that has zero bodies. Deleting the file entirely causes the sublevel
    # to silently unload ("Cannot find resource").
    #
    # Revault-verified call chain:
    #   CLevelResource::LoadLevel -> CPhysicsResourceInstance::Activate (0x141050CB0)
    #     -> SetupBodies (0x1410517C0) -> CPhZone::CreateBody (0x14068D9E0)
    #   SetupBodies reads body count from resource data. Zero bodies = no collision.
    #
    # Stub formats verified from 540+ real game files (models with no physics):
    #   CPhysicsResource: 72B (bounds + zeros, zero bodies)
    #   CBVHResource:     80B (empty BVH tree, zero nodes)
    #   CMaterialTypesBVHResource: 64B (empty material BVH)
    # CArchiveResource entries are KEPT (files still exist, just empty).

    # Minimal stub data — copied from real game files with zero bodies/nodes
    PHYS_STUB = bytes.fromhex(
        "0000000000000000"   # +0x00: zeros
        "0000964300009643"   # +0x08: bounds (300.0, 300.0)
        "0000964300004040"   # +0x10: bounds (300.0, 3.0)
        "0000003f00000000"   # +0x18: (0.5, 0)
        "0000000000000000"   # +0x20: zeros
        "0000000000000000"   # +0x28: zeros
        "0000000000000000"   # +0x30: zeros
        "0000000000000000"   # +0x38: zeros
        "0000000000000000"   # +0x40: zeros
    )
    BVH_STUB = bytes.fromhex(
        "0000000000000000"   # 80 bytes: empty BVH
        "0000000000000000"
        "0000000000000000"
        "0100000000000000"
        "0000000000000000"
        "0000000000000000"
        "0000000000000000"
        "0100000000000000"
        "0000000000000000"
        "0000000000000000"
    )
    MATBVH_STUB = bytes.fromhex(
        "0000000000000000"   # 64 bytes: empty material BVH
        "0000000000000000"
        "0000000000000000"
        "0100000020000000"
        "0000000000000000"
        "0000000000000000"
        "0000000000000000"
        "0000000000000000"
    )

    # Lighting resource stubs — kill lobby lights, reflections, occluders
    # from the sublevel so only arena lighting is active.
    LIGHTMAP_STUB = bytes.fromhex(
        "00000000000000000000000000000000"   # 28 bytes: empty lightmap
        "000000000400000000000000"
    )
    REFLPROBE_STUB = bytes.fromhex(       # 344 bytes: exact copy of mnu_master.bin
        "00000000000000000000000000000000"
        "00000000000000000000000001000000"
        "20000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000010000002000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000001000000"
        "20000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000010000002000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000001000000"
        "20000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000010000002000000000000000"
        "00000000000000000000000000000000"
        "000000003b000000"
    )
    OCCLUDER_STUB = bytes.fromhex(
        "0000000000000000000000000000000000000000000000000000000001000000"   # 56B
        "200000000000000000000000000000000000000000000000"
    )

    # Scene-graph rendering resources — these render static geometry
    # (walls, barriers, text panels like "SKIRMISH") directly through
    # the renderer pipeline, bypassing per-actor visibility (bits[0]).
    # Stubbing removes all lobby static geometry from the scene graph.
    # Combat gun/grenade models use prefabs loaded at runtime, not these.
    STATIC_INST_STUB = bytes.fromhex(  # 376B: empty static instance resource
        "00000000000000000000000000000000"
        "00000000000000000000000001000000"
        "20000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000100000020000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "01000000200000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000010000002000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000100000020000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000001000000"
        "20000000000000000000000000000000"
        "00000000000000000000000000000000"
        "00000000000000000000000000000000"
        "01000000200000000000000000000000"
        "0000000000000000"
    )
    VISIBILITY_STUB = bytes.fromhex(   # 92B: empty visibility resource
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0300000003000000000000000300000000000000030000000000000003000000"
        "00000000030000000000000003000000000000000000000000000000"
    )
    MESHLIST_STUB = bytes.fromhex(     # 56B: empty mesh list
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000"
    )

    RESOURCE_STUBS = [
        # Collision — stub physics to prevent lobby collision bodies from
        # interfering with arena gameplay. Ordnance detonation is trigger-based
        # (player pulls hand trigger), not collision-based.
        ("CPhysicsResourceWin10",          PHYS_STUB,      "collision mesh"),
        ("CBVHResourceWin10",              BVH_STUB,       "raycast BVH"),
        ("CMaterialTypesBVHResourceWin10", MATBVH_STUB,    "material BVH"),
        # Lighting
        ("CGLightMapResourceWin10",        LIGHTMAP_STUB,  "lightmaps"),
        ("CGReflectionProbeResourceWin10", REFLPROBE_STUB, "reflection probes"),
        ("COccluderMeshResourceWin10",     OCCLUDER_STUB,  "occluder mesh"),
        # Scene graph rendering — empty mesh list = nothing to draw
        ("CGVisibilityResourceWin10",      VISIBILITY_STUB,  "visibility regions"),
        ("CGMeshListResourceWin10",        MESHLIST_STUB,    "mesh draw list"),
    ]
    for res_type, stub_data, desc in RESOURCE_STUBS:
        res_path = build_mf / res_type / f"{sublevel_name}.bin"
        if res_path.is_file():
            orig_sz = res_path.stat().st_size
            res_path.write_bytes(stub_data)
            log.info("    [STUB] %s -> %dB stub (was %d bytes, %s)",
                     res_type, len(stub_data), orig_sz, desc)

    # 3c: Disable CStaticRaycastCR entries (uses BVH for raycast collision)
    raycast_path = build_mf / "CStaticRaycastCRWin10" / f"{sublevel_name}.bin"
    if raycast_path.is_file():
        data = bytearray(raycast_path.read_bytes())
        if len(data) >= 56:
            dbs = struct.unpack_from('<Q', data, 8)[0]
            cap = struct.unpack_from('<Q', data, 40)[0]
            count = struct.unpack_from('<Q', data, 48)[0]
            if cap > 0 and count > 0:
                esize = dbs // cap
                for i in range(count):
                    flags_off = 56 + i * esize + 0x18
                    if flags_off + 4 <= len(data):
                        flags = struct.unpack_from('<I', data, flags_off)[0]
                        flags |= 0x01  # DisableComponentOnInit
                        struct.pack_into('<I', data, flags_off, flags)
                raycast_path.write_bytes(bytes(data))
                log.info("    [PHYS] Disabled %d CStaticRaycastCR entries", count)

    # 3d: Strip lighting/scene-set data from sublevel CGSceneResource
    # The sublevel's sec10 (1476 lighting entries) merges with the arena's
    # lighting at load and darkens the scene.  sec11 (CMemBlock) and sec25
    # (scene sets) also contribute reflection/environment data.
    # Zeroing counts makes the sublevel contribute no lighting influence.
    scene_path = build_mf / "CGSceneResourceWin10" / f"{sublevel_name}.bin"
    if scene_path.is_file():
        from echomod.resources.scene import CGSceneResource as SceneRes
        scene = SceneRes.from_bytes(scene_path.read_bytes())
        orig_sec10 = len(scene.sec10_raw)
        orig_sec11 = len(scene.sec11_raw)
        orig_sec25 = len(scene.sec25_raw)
        ZERO_COUNT = struct.pack('<I', 0)
        scene.sec10_raw = ZERO_COUNT   # zero lighting entries
        scene.sec11_raw = ZERO_COUNT   # zero CMemBlock bytes
        scene.sec25_raw = ZERO_COUNT   # zero scene set sub-objects
        scene_path.write_bytes(scene.to_bytes())
        log.info("    [SCENE] Stripped CGSceneResource lighting: "
                 "sec10 %d->4B, sec11 %d->4B, sec25 %d->4B",
                 orig_sec10, orig_sec11, orig_sec25)

    log.info("    Combat sublevel filtering complete")


# ---------------------------------------------------------------------------
# Phase 0: Extract
# ---------------------------------------------------------------------------

def phase0_extract():
    log.info("=" * 60)
    log.info("PHASE 0: Fresh Extract")
    log.info("=" * 60)
    if not ARCHIVE_DIR.is_dir():
        raise FileNotFoundError(f"Archive directory not found: {ARCHIVE_DIR}")
    if CLEAN_EXTRACT_DIR.exists():
        log.info("  Clean extract already exists, skipping.")
        return
    run_tool(["extract", str(ARCHIVE_DIR), str(CLEAN_EXTRACT_DIR)])


# ---------------------------------------------------------------------------
# Phase 1: Copy to build directory
# ---------------------------------------------------------------------------

def phase1_copy_to_build():
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 1: Copy to build directory")
    log.info("=" * 60)
    if BUILD_DIR.exists():
        log.info("  Removing existing build directory...")
        shutil.rmtree(BUILD_DIR)
    log.info("  Copying %s -> %s", CLEAN_EXTRACT_DIR, BUILD_DIR)
    shutil.copytree(str(CLEAN_EXTRACT_DIR), str(BUILD_DIR))


# ---------------------------------------------------------------------------
# Phase 2: Clone level (arena_a -> arenacombat)
# ---------------------------------------------------------------------------

def phase2_clone_level() -> Path:
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 2: Clone level (%s -> %s)", ARENA_LEVEL, TARGET_LEVEL)
    log.info("=" * 60)

    build_mf = get_mf_dir(BUILD_DIR)
    log.info("  Manifest dir: %s", build_mf.name)

    # Walk all manifest dirs
    mf_dirs = [d for d in BUILD_DIR.iterdir() if d.is_dir()]
    copied = 0
    patched = 0

    for mf_dir in mf_dirs:
        for type_dir in sorted(mf_dir.iterdir()):
            if not type_dir.is_dir():
                continue
            src = type_dir / f"{ARENA_LEVEL}.bin"
            if not src.is_file():
                continue
            dst = type_dir / f"{TARGET_LEVEL}.bin"
            shutil.copy2(str(src), str(dst))
            copied += 1

    log.info("  Copied %d files (%s.bin -> %s.bin)", copied, ARENA_LEVEL, TARGET_LEVEL)

    # Replace arena level hash with target level hash
    for mf_dir in mf_dirs:
        for type_dir in sorted(mf_dir.iterdir()):
            if not type_dir.is_dir():
                continue
            f = type_dir / f"{TARGET_LEVEL}.bin"
            if not f.is_file():
                continue
            data = f.read_bytes()
            if ARENA_LEVEL_HASH_LE in data:
                count = data.count(ARENA_LEVEL_HASH_LE)
                data = data.replace(ARENA_LEVEL_HASH_LE, TARGET_LEVEL_HASH_LE)
                f.write_bytes(data)
                patched += 1

    log.info("  Level hash patched in %d files", patched)
    return build_mf


# ---------------------------------------------------------------------------
# Phase 3: Transplant combat actors
# ---------------------------------------------------------------------------

def phase3_transplant(build_mf: Path):
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 3: Setup combat sub-level loading")
    log.info("=" * 60)

    # Combat actors are NO LONGER transplanted into the arena level.
    # Instead, the combat sub-level (0xCB9977F7FC2B4526) loads as a
    # separate gamespace via StreamingScript, providing all weapon
    # actors/CRs in their own complete gamespace.
    #
    # Skip directly to step 3n (sub-level loading setup).

    # --- 3n. Add combat sub-level loading script ---
    _setup_sublevel_loading(build_mf)
    return  # Skip ALL old transplant steps below


def _setup_sublevel_loading(build_mf: Path):
    """Clone combat sub-level, offset it, enlarge trigger, and load it."""
    import math
    from echomod.resources.actor_data import ActorDataResource, ActorDataHeaderEntry
    from echomod.resources.transform_cr import TransformCR

    # Equipment actor constants
    EQUIP_TERMINAL_ROOT = 0x2D65EBA7F674D94A
    EQUIP_STATION_MARKERS = [
        0xD62267852D1D5767, 0xD62267852D1D5764, 0xD62267852D1D5765,
        0xD62267852D1D5762, 0xD62267852D1D5763,
    ]
    EQUIP_SECONDARY_ANCHORS = [
        0x79254C5124F7F07B, 0x79254C5124F7F074,
        0xD3E5A4BFCEF2E705, 0x57070D74FC0BE1AE, 0x57070D74FC0BE1AF,
    ]

    CUSTOM_SUBLEVEL = "mpl_arena_combat"
    CUSTOM_SUBLEVEL_HASH = rad_hash(CUSTOM_SUBLEVEL)  # 0x813EDECF5228A2BA
    ORIGINAL_COMBAT_HASH = int(COMBAT_LEVEL, 16) if COMBAT_LEVEL.startswith("0x") else rad_hash(COMBAT_LEVEL)
    ORIGINAL_COMBAT_HASH_LE = struct.pack("<Q", ORIGINAL_COMBAT_HASH)
    CUSTOM_SUBLEVEL_HASH_LE = struct.pack("<Q", CUSTOM_SUBLEVEL_HASH)
    # Y offset is applied at RUNTIME by swaptoggle.dll hook on CNode3D::AddTranslation.
    # This offsets the ENTIRE sub-level including physics, scene graph, everything.
    # We still set the trigger to (0,0,0) in ActorData so it's at arena center
    # BEFORE the runtime offset moves it down.
    TRIGGER_ACTOR_HASH = 0x26B939FCAC9FBFF8  # Combat zone trigger

    log.info("")
    log.info("  --- Clone combat sub-level as %s (0x%016X) ---", CUSTOM_SUBLEVEL, CUSTOM_SUBLEVEL_HASH)

    # 1: Clone all combat sub-level files with new hash
    cloned_files = 0
    for type_dir in sorted(build_mf.iterdir()):
        if not type_dir.is_dir():
            continue
        src = type_dir / f"{COMBAT_LEVEL}.bin"
        if not src.is_file():
            continue
        dst = type_dir / f"{CUSTOM_SUBLEVEL}.bin"
        data = src.read_bytes()
        # Replace combat hash with our custom hash
        if ORIGINAL_COMBAT_HASH_LE in data:
            data = data.replace(ORIGINAL_COMBAT_HASH_LE, CUSTOM_SUBLEVEL_HASH_LE)
        dst.write_bytes(data)
        cloned_files += 1
    log.info("    Cloned %d files (%s -> %s)", cloned_files, COMBAT_LEVEL, CUSTOM_SUBLEVEL)

    # Offset ALL sublevel actors by Y=-100 to push leftover lobby geometry
    # underground. Equipment station actors are set to absolute positions
    # AFTER this step so they end up at arena level.
    Y_OFFSET = -100.0

    # 2a: Offset CTransformCR entries
    tcr_path = build_mf / "CTransformCRWin10" / f"{CUSTOM_SUBLEVEL}.bin"
    if tcr_path.is_file():
        tcr = TransformCR.from_bytes(tcr_path.read_bytes())
        for entry in tcr.entries:
            entry.position_y += Y_OFFSET
        tcr_path.write_bytes(tcr.to_bytes())
        log.info("    Offset %d CTransformCR entries by Y=%+.0f", len(tcr.entries), Y_OFFSET)

    # 2b: Offset ActorData transforms + move trigger to arena center
    ad_path = build_mf / "CActorDataResourceWin10" / f"{CUSTOM_SUBLEVEL}.bin"
    if ad_path.is_file():
        ad_tmp = ActorDataResource.from_bytes(ad_path.read_bytes())
        for xform in ad_tmp.transforms:
            xform.position_y += Y_OFFSET
        if TRIGGER_ACTOR_HASH in ad_tmp.nodeids:
            t_idx = ad_tmp.nodeids.index(TRIGGER_ACTOR_HASH)
            ad_tmp.transforms[t_idx].position_x = 0.0
            ad_tmp.transforms[t_idx].position_y = 0.0
            ad_tmp.transforms[t_idx].position_z = 0.0
            log.info("    Moved trigger actor to arena center (0, 0, 0)")
        ad_path.write_bytes(ad_tmp.to_bytes())
        log.info("    Offset %d ActorData transforms by Y=%+.0f", ad_tmp.actor_count, Y_OFFSET)

    # 4: Trigger volume — keep original 0.7m radius.
    # Enlarging to 200m breaks the combat zone behavior (events fire at wrong
    # locations). The combatpatch.dll handles combat activation via direct
    # event injection (EVT_ENTERED_COMBAT_LAND), so the trigger volume is
    # not needed for combat mode activation.
    log.info("    Trigger volume: keeping original 0.7m radius (combatpatch handles activation)")

    # Patch arena's CGSceneResource sec12 — add sub-level position offset.
    # This tells the engine to place the sub-level at (0, -100, 0).
    # Combined with the data offset, lobby geometry ends up at Y≈-200.
    log.info("    Patching arena CGSceneResource with sub-level offset...")
    scene_data = read_resource(build_mf, "CGSceneResourceWin10", TARGET_LEVEL)
    if scene_data:
        from echomod.resources.scene import CGSceneResource as SceneRes
        scene = SceneRes.from_bytes(scene_data)
        sec12 = bytearray(scene.sec12_raw)

        # Parse Shape 0 hull1
        pos = 0
        h1_count = struct.unpack_from('<I', sec12, pos)[0]
        h1_start = 4
        h1_end = h1_start + h1_count * 16
        status_pos = h1_end
        insert_idx = h1_count
        for i in range(h1_count):
            key = struct.unpack_from('<Q', sec12, h1_start + i * 16)[0]
            if key > CUSTOM_SUBLEVEL_HASH:
                insert_idx = i
                break

        pos = status_pos + 4
        h2_count = struct.unpack_from('<I', sec12, pos)[0]
        h2_start = pos + 4
        h2_end = h2_start + h2_count * 16
        pos = h2_end + 4
        d_count = struct.unpack_from('<I', sec12, pos)[0]
        d_start = pos + 4
        d_end = d_start + d_count * 12

        new_h1_entry = struct.pack('<QQ', CUSTOM_SUBLEVEL_HASH, d_count)
        insert_byte = h1_start + insert_idx * 16
        sec12[insert_byte:insert_byte] = new_h1_entry
        struct.pack_into('<I', sec12, 0, h1_count + 1)

        d_count_pos = d_start - 4 + 16
        d_new_end = d_end + 16

        new_data = struct.pack('<fff', 0.0, -100.0, 0.0)
        sec12[d_new_end:d_new_end] = new_data
        struct.pack_into('<I', sec12, d_count_pos, d_count + 1)

        scene.sec12_raw = bytes(sec12)
        new_scene_data = scene.to_bytes()
        write_resource(build_mf, "CGSceneResourceWin10", TARGET_LEVEL, new_scene_data)
        log.info("    Added offset (0, -100, 0) for sub-level 0x%016X",
                 CUSTOM_SUBLEVEL_HASH)

    # --- Equipment station setup ---
    # Equipment terminal 3D meshes are baked into static instance geometry
    # (no separate CModelCR). We preserve the interaction components (CCanvasUI,
    # ButtonInteract, CScript, BoundingSphere) and reposition them to arena
    # spawn areas. The buttons/UI will be floating (no terminal mesh) but
    # functional for loadout selection.
    #
    # Physical terminals found in lobby sub-level:
    #   - Tree root 0x2D65EBA7F674D94A (idx 109) + 7 children at (~41, 0.7, -24)
    #   - Standalone 0x313F7510E3EBE2A1 (idx 241) at (24.1, 1.5, -1.2)
    #   - Standalone 0xC540795C64704D0B (idx 243) at (13.1, 1.5, -20.3)

    equip_preserve = set()

    ad_data_sl = read_resource(build_mf, "CActorDataResourceWin10", CUSTOM_SUBLEVEL)
    ad_sl = ActorDataResource.from_bytes(ad_data_sl)

    # Build child map for the sublevel
    sl_children = {}
    for i in range(ad_sl.actor_count):
        p = ad_sl.parents[i]
        if p != 0xFFFF and p < ad_sl.actor_count:
            sl_children.setdefault(p, []).append(i)

    def _add_tree_hashes(root_idx):
        equip_preserve.add(ad_sl.nodeids[root_idx])
        for c in sl_children.get(root_idx, []):
            _add_tree_hashes(c)

    # Equipment system actor layout (from CScriptCR inline data analysis):
    #
    # Terminal tree: root 0x2D65EBA7F674D94A (idx 109) + 7 children
    #   Scripts: root logic, canvas UI, button interaction, etc.
    #
    # Position anchor actors referenced by the root terminal script
    # (0xB6690EAEAEAA7F70). The script reads THESE actors' positions to
    # determine where to activate the equipment UI:
    #   117: 0xD62267852D1D5767  (station marker 1)
    #   118: 0xD62267852D1D5764  (station marker 2)
    #   119: 0xD62267852D1D5765  (station marker 3)
    #   120: 0xD62267852D1D5762  (station marker 4)
    #   121: 0xD62267852D1D5763  (station marker 5)
    # Plus secondary anchors:
    #   124: 0x79254C5124F7F07B, 125: 0x79254C5124F7F074
    #   198: 0xD3E5A4BFCEF2E705, 199: 0x57070D74FC0BE1AE, 200: 0x57070D74FC0BE1AF
    #
    # sec12 offset adds (0, -100, 0) to sublevel origin, so Y=103 -> world Y=3.

    # Target positions: 5 station markers at spawn 1 (Z=-73),
    # spread X=-2 to X=2, facing toward center (0,0,0).
    # Rotation quaternion: facing +Z = identity (0,0,0,1)
    MARKER_TARGET_POSITIONS = [
        (-2.0, 0.0, -72.9),
        (-1.0, 0.0, -72.9),
        ( 0.0, 0.0, -72.9),
        ( 1.0, 0.0, -72.9),
        ( 2.0, 0.0, -72.9),
    ]
    MARKER_ROTATION = (0.0, 0.0, 0.0, 1.0)  # face +Z (toward center)

    # Secondary anchors at spawn 2 (Z=+73), facing toward center (0,0,0).
    # Rotation quaternion: facing -Z = 180° around Y = (0,1,0,0)
    SECONDARY_TARGET_POSITIONS = [
        (-2.0, 0.0, 72.9),
        (-1.0, 0.0, 72.9),
        ( 0.0, 0.0, 72.9),
        ( 1.0, 0.0, 72.9),
        ( 2.0, 0.0, 72.9),
    ]
    SECONDARY_ROTATION = (0.0, 1.0, 0.0, 0.0)  # face -Z (toward center)

    # Child local-space offsets (computed from lobby data by un-rotating
    # world offsets by root quaternion). These are in the terminal's local frame.
    EQUIP_CHILD_LOCAL_OFFSETS = {
        0xC7C81AF14364688D: ( 0.000,  0.000,  0.001),  # 110 - canvas (at root)
        0xF22E48B85B5C6ED0: ( 0.000, -0.158,  0.021),  # 111 - visual (below)
        0x5656B0445A3A7615: (-0.141, -0.047,  0.001),  # 112 - button left
        0xA6AB529E1D7B11A9: ( 0.000, -0.047,  0.001),  # 113 - button center
        0x76EA947D5A97FB2D: ( 0.141, -0.047,  0.001),  # 114 - button right
        0x03AD2361D4803399: ( 0.000,  0.175, -0.017),  # 115 - above
        0x9B11548AD2BD9F2C: (-0.168,  0.114,  0.008),  # 116 - button upper-left
    }

    # Terminal root + children -> spawn 1 center, facing +Z
    EQUIP_POSITIONS = {
        EQUIP_TERMINAL_ROOT: (0.0, 0.0, -72.9),
    }
    EQUIP_ROTATION = MARKER_ROTATION

    # Preserve and reposition station marker actors (script position anchors)
    all_equip_actors = set(EQUIP_STATION_MARKERS + EQUIP_SECONDARY_ANCHORS)
    for i, marker_hash in enumerate(EQUIP_STATION_MARKERS):
        if marker_hash in ad_sl.nodeids:
            idx = ad_sl.nodeids.index(marker_hash)
            equip_preserve.add(marker_hash)
            if i < len(MARKER_TARGET_POSITIONS):
                pos = MARKER_TARGET_POSITIONS[i]
                ad_sl.transforms[idx].position_x = pos[0]
                ad_sl.transforms[idx].position_y = pos[1]
                ad_sl.transforms[idx].position_z = pos[2]
                log.info("    Station marker %d (0x%016X) -> (%.0f, %.0f, %.0f)",
                         i, marker_hash, pos[0], pos[1], pos[2])
    for ai, anchor_hash in enumerate(EQUIP_SECONDARY_ANCHORS):
        if anchor_hash in ad_sl.nodeids:
            idx = ad_sl.nodeids.index(anchor_hash)
            equip_preserve.add(anchor_hash)
            if ai < len(SECONDARY_TARGET_POSITIONS):
                pos = SECONDARY_TARGET_POSITIONS[ai]
                ad_sl.transforms[idx].position_x = pos[0]
                ad_sl.transforms[idx].position_y = pos[1]
                ad_sl.transforms[idx].position_z = pos[2]
                log.info("    Secondary anchor %d (0x%016X) -> (%.0f, %.0f, %.1f)",
                         ai, anchor_hash, pos[0], pos[1], pos[2])

    # Preserve and reposition terminal actors
    equip_moved = 0
    for actor_hash, target_pos in EQUIP_POSITIONS.items():
        if actor_hash not in ad_sl.nodeids:
            continue
        idx = ad_sl.nodeids.index(actor_hash)
        _add_tree_hashes(idx)

        children = sl_children.get(idx, [])

        def _quat_rotate_vec(qx, qy, qz, qw, vx, vy, vz):
            """Rotate vector (vx,vy,vz) by quaternion (qx,qy,qz,qw)."""
            cx = qy * vz - qz * vy
            cy = qz * vx - qx * vz
            cz = qx * vy - qy * vx
            return (vx + 2.0 * (qw * cx + qy * cz - qz * cy),
                    vy + 2.0 * (qw * cy + qz * cx - qx * cz),
                    vz + 2.0 * (qw * cz + qx * cy - qy * cx))

        # Place root at target
        ad_sl.transforms[idx].position_x = target_pos[0]
        ad_sl.transforms[idx].position_y = target_pos[1]
        ad_sl.transforms[idx].position_z = target_pos[2]

        # Place children using local-space offsets rotated by target rotation
        rot = EQUIP_ROTATION
        for ci in children:
            ch = ad_sl.nodeids[ci]
            local_off = EQUIP_CHILD_LOCAL_OFFSETS.get(ch, (0.0, 0.0, 0.0))
            wx, wy, wz = _quat_rotate_vec(rot[0], rot[1], rot[2], rot[3],
                                            local_off[0], local_off[1], local_off[2])
            ad_sl.transforms[ci].position_x = target_pos[0] + wx
            ad_sl.transforms[ci].position_y = target_pos[1] + wy
            ad_sl.transforms[ci].position_z = target_pos[2] + wz

        equip_moved += 1
        log.info("    Terminal 0x%016X -> (%.0f, %.0f, %.0f) [%d children]",
                 actor_hash, target_pos[0], target_pos[1], target_pos[2], len(children))

    # Write updated ActorData (always write - markers were moved even if terminal wasn't)
    if True:
        sl_ad_path = build_mf / "CActorDataResourceWin10" / f"{CUSTOM_SUBLEVEL}.bin"
        sl_ad_path.write_bytes(ad_sl.to_bytes())

        # Also update CTransformCR for moved actors
        tcr_path_sl = build_mf / "CTransformCRWin10" / f"{CUSTOM_SUBLEVEL}.bin"
        if tcr_path_sl.is_file():
            tcr_sl = TransformCR.from_bytes(tcr_path_sl.read_bytes())
            # Build actor_hash -> (position, rotation) map
            tcr_targets = {}  # hash -> ((x,y,z), (rx,ry,rz,rw))
            # Terminal tree — use already-computed positions from ActorData
            for actor_hash, target_pos in EQUIP_POSITIONS.items():
                if actor_hash not in ad_sl.nodeids:
                    continue
                aidx = ad_sl.nodeids.index(actor_hash)
                tcr_targets[actor_hash] = (target_pos, EQUIP_ROTATION)
                for ci in sl_children.get(aidx, []):
                    ch = ad_sl.nodeids[ci]
                    # Positions already computed above with rotated local offsets
                    cpos = (ad_sl.transforms[ci].position_x,
                            ad_sl.transforms[ci].position_y,
                            ad_sl.transforms[ci].position_z)
                    tcr_targets[ch] = (cpos, EQUIP_ROTATION)
            # Station markers (spawn 1, face +Z)
            for ah in EQUIP_STATION_MARKERS:
                if ah in ad_sl.nodeids:
                    idx = ad_sl.nodeids.index(ah)
                    pos = (ad_sl.transforms[idx].position_x,
                           ad_sl.transforms[idx].position_y,
                           ad_sl.transforms[idx].position_z)
                    tcr_targets[ah] = (pos, MARKER_ROTATION)
            # Secondary anchors (spawn 2, face -Z)
            for ah in EQUIP_SECONDARY_ANCHORS:
                if ah in ad_sl.nodeids:
                    idx = ad_sl.nodeids.index(ah)
                    pos = (ad_sl.transforms[idx].position_x,
                           ad_sl.transforms[idx].position_y,
                           ad_sl.transforms[idx].position_z)
                    tcr_targets[ah] = (pos, SECONDARY_ROTATION)
            for entry in tcr_sl.entries:
                if entry.entity_hash in tcr_targets:
                    pos, rot = tcr_targets[entry.entity_hash]
                    entry.position_x = pos[0]
                    entry.position_y = pos[1]
                    entry.position_z = pos[2]
                    entry.rotation_x = rot[0]
                    entry.rotation_y = rot[1]
                    entry.rotation_z = rot[2]
                    entry.rotation_w = rot[3]
            tcr_path_sl.write_bytes(tcr_sl.to_bytes())
            log.info("    Updated CTransformCR for %d equipment actors", len(tcr_targets))

    # Preserve all UI panel actors (x > 55 — customization screens).
    # Originally at x=62-70, y=15-19. After step 2b Y-100 offset, they're at
    # y=-85 to -81 in sublevel data. Check X only (not affected by Y offset).
    # The equipment script teleports these panels to the terminal at runtime.
    for i in range(ad_sl.actor_count):
        t = ad_sl.transforms[i]
        if t.position_x > 55.0:
            _add_tree_hashes(i)

    # --- Restore Y for UI panel actors (undo the -100 offset) ---
    # Terminal tree, markers, anchors were set to absolute positions above.
    # UI panels (~1088 actors) still have Y-100 applied. Restore them.
    explicitly_positioned = set(EQUIP_STATION_MARKERS + EQUIP_SECONDARY_ANCHORS)
    for ah in EQUIP_POSITIONS:
        explicitly_positioned.add(ah)
        if ah in ad_sl.nodeids:
            for ci in sl_children.get(ad_sl.nodeids.index(ah), []):
                explicitly_positioned.add(ad_sl.nodeids[ci])

    ui_restored = 0
    for i in range(ad_sl.actor_count):
        h = ad_sl.nodeids[i]
        if h in equip_preserve and h not in explicitly_positioned:
            ad_sl.transforms[i].position_y -= Y_OFFSET  # +100
            ui_restored += 1

    # Write final ActorData with restored UI positions
    sl_ad_path2 = build_mf / "CActorDataResourceWin10" / f"{CUSTOM_SUBLEVEL}.bin"
    sl_ad_path2.write_bytes(ad_sl.to_bytes())

    # Restore CTransformCR for UI panel actors
    tcr_path_sl2 = build_mf / "CTransformCRWin10" / f"{CUSTOM_SUBLEVEL}.bin"
    if tcr_path_sl2.is_file():
        tcr_sl2 = TransformCR.from_bytes(tcr_path_sl2.read_bytes())
        for entry in tcr_sl2.entries:
            if entry.entity_hash in equip_preserve and entry.entity_hash not in explicitly_positioned:
                entry.position_y -= Y_OFFSET  # +100
        tcr_path_sl2.write_bytes(tcr_sl2.to_bytes())

    log.info("    Restored Y for %d UI panel actors", ui_restored)
    log.info("    Equipment system: %d actors preserved, %d terminals repositioned",
             len(equip_preserve), equip_moved)

    # --- Disable all sublevel actors via visibility bits ---
    # Equipment station interaction actors are preserved — their visibility bits
    # stay set and their CRs (CCanvasUI, ButtonInteract) are not disabled.
    _filter_sublevel_to_combat(build_mf, CUSTOM_SUBLEVEL,
                               preserve_actor_hashes=equip_preserve)

    # 5: Update hash_lookup for the custom level
    import json
    if HASH_LOOKUP_PATH.is_file():
        with open(HASH_LOOKUP_PATH, "r") as f:
            lookup = json.load(f)
        lookup[f"0x{CUSTOM_SUBLEVEL_HASH:016X}"] = CUSTOM_SUBLEVEL
        with open(HASH_LOOKUP_PATH, "w") as f:
            json.dump(lookup, f, indent=2, sort_keys=True)

    log.info("")
    log.info("  --- Add sub-level loading script ---")

    LEVEL_LOADER_ACTOR = 0xA284EC13052FEA81
    ORIGINAL_STREAMING_SCRIPT = 0xF808C072BB49DA47  # original lobby script
    # Use the ORIGINAL script hash — scriptpatch.dll patches it in memory at runtime
    LEVEL_LOADER_SCRIPT = ORIGINAL_STREAMING_SCRIPT
    GENERIC_SCRIPT_TYPE = 0x6AEA99EEB2BAB6C4

    # 1: Add the level-loader actor to CActorDataResource
    target_ad_data = read_resource(build_mf, "CActorDataResourceWin10", TARGET_LEVEL)
    target_ad = ActorDataResource.from_bytes(target_ad_data)
    if LEVEL_LOADER_ACTOR not in set(target_ad.nodeids):
        ll_idx = target_ad.actor_count
        target_ad.nodeids.append(LEVEL_LOADER_ACTOR)
        target_ad.names.append(LEVEL_LOADER_ACTOR)
        target_ad.prefabids.append(0)
        target_ad.transforms.append(target_ad.transforms[0])
        target_ad.numpooled.append(0)
        target_ad.parents.append(0xFFFF)
        new_qword_count = math.ceil(target_ad.actor_count / 64)
        for b in range(5):
            while len(target_ad.bits[b]) < new_qword_count:
                target_ad.bits[b].append(0)
        target_ad.headers.append(ActorDataHeaderEntry(
            type_hash=LEVEL_LOADER_ACTOR, count=ll_idx))
        target_ad.headers.sort(key=lambda h: struct.unpack('<q', struct.pack('<Q', h.type_hash))[0])
        write_resource(build_mf, "CActorDataResourceWin10", TARGET_LEVEL, target_ad.to_bytes())
        log.info("    Injected level-loader actor at index %d", ll_idx)

    # 2: Add CScriptCR entry for the level-loading StreamingScript
    script_cr_data = read_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL)
    if script_cr_data and len(script_cr_data) >= 56:
        s_dbs = struct.unpack_from('<Q', script_cr_data, 8)[0]
        s_cap = struct.unpack_from('<Q', script_cr_data, 40)[0]
        s_count = struct.unpack_from('<Q', script_cr_data, 48)[0]
        s_esize = s_dbs // s_cap if s_cap > 0 else 720
        s_flat_end = 56 + s_count * s_esize
        new_entry = bytearray(s_esize)
        struct.pack_into('<Q', new_entry, 0, GENERIC_SCRIPT_TYPE)
        struct.pack_into('<Q', new_entry, 8, LEVEL_LOADER_ACTOR)
        struct.pack_into('<I', new_entry, 16, 0xFFFFFFFF)
        struct.pack_into('<Q', new_entry, 32, LEVEL_LOADER_SCRIPT)
        for d in [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664]:
            struct.pack_into('<I', new_entry, d + 28, 1)
        existing_inline = script_cr_data[s_flat_end:]
        new_count = s_count + 1
        result = bytearray(script_cr_data[:56])
        struct.pack_into('<Q', result, 8, new_count * s_esize)
        struct.pack_into('<Q', result, 40, new_count)
        struct.pack_into('<Q', result, 48, new_count)
        result.extend(script_cr_data[56:s_flat_end])
        result.extend(new_entry)
        result.extend(existing_inline)
        write_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL, bytes(result))
        log.info("    Added level-loading script entry (%d -> %d)", s_count, new_count)

    # 3: Add StreamingScript to CArchiveResource
    ss_type_hash = rad_hash("StreamingScriptWin10")
    car_dir = find_type_dir(build_mf, "carchiveresourceWin10")
    if car_dir is None:
        car_dir = find_type_dir(build_mf, "CArchiveResourceWin10")
    if car_dir:
        arena_car = car_dir / f"{TARGET_LEVEL}.bin"
        if arena_car.is_file():
            car_data = bytearray(arena_car.read_bytes())
            a_flat = struct.unpack_from('<I', car_data, 0)[0]
            a_pair_off = 4 + a_flat * 8
            a_pair_count = struct.unpack_from('<I', car_data, a_pair_off)[0]
            a_pair_end = a_pair_off + 4 + a_pair_count * 16
            new_pair = struct.pack('<QQ', ss_type_hash, LEVEL_LOADER_SCRIPT)
            struct.pack_into('<I', car_data, a_pair_off, a_pair_count + 1)
            car_data[a_pair_end:a_pair_end] = new_pair
            arena_car.write_bytes(bytes(car_data))
            log.info("    Added StreamingScript pair to CArchiveResource")

    # 4: StreamingScript DLL patching is handled at RUNTIME by scriptpatch.dll.
    # It hooks the engine's DLL loader and patches f808c072bb49da47.dll in memory
    # when mpl_arenacombat is the active level. No on-disk DLL modifications needed.
    # The CScriptCR entry (step 2) and CArchiveResource pair (step 3) above
    # reference the ORIGINAL script hash so the engine finds the existing resource.
    log.info("    StreamingScript DLL patching deferred to scriptpatch.dll (runtime)")

    # --- 5: Combat spawn points ---
    # The arena (cloned from disc) has 25 CR15SpawnPointCR entries with disc IDs.
    # Combat maps normally use IDs 200-204 (0xC8-0xCC), but these conflict with
    # a global combat level (0x08A1AF9E108DEF0B) that also registers IDs 200-204.
    # Adding duplicate IDs causes "Multiple R14SpawnPoints are using ID" fatal error.
    #
    # The global combat level already provides combat spawn point definitions.
    # The arena's disc spawn points (IDs 0-5, near Z=±70 goal areas and Z=±10
    # center) remain as-is. Combat respawn should use the global level's spawn
    # points or the disc points depending on the active game mode.
    #
    # TODO: If players still respawn at death location, investigate:
    #   - Whether the global level's IDs 200-204 reference actors in THIS level
    #   - Whether combat respawn uses CR15SpawnPointCS or CRxRespawnTargetCS
    #   - Whether a DLL hook is needed to redirect respawn to team areas
    log.info("")
    log.info("  --- Combat spawn points: using disc spawn points (IDs 0-5) ---")
    log.info("    Global combat level provides IDs 200-204 separately")
    log.info("    Arena disc spawn points at Z=±70 (goals) and Z=±10 (center)")

    return  # Done — sub-level provides all weapon data

    # ===== DEAD CODE BELOW (old transplant steps, kept for reference) =====
    # NOTE: DO NOT transplant weapon CRs into arena — they reference actors
    # that only exist in the combat sub-level gamespace. The sub-level
    # provides everything. The slot+0x20 NULL issue needs to be fixed
    # differently (via script or timing adjustment).
    if False:  # DISABLED — CRs reference actors not in arena gamespace
        pass
        # 4: Transplant critical weapon CRs into arena gamespace.
    # The combat sub-level provides actors in its own gamespace, but the
    # magazine/bullet CS initialization needs local CR data to create
    # sub-objects before the cross-gamespace stomp happens.
    log.info("    Transplanting critical weapon CRs into arena...")
    WEAPON_CRS_TO_COPY = [
        "CR15NetGunCRWin10", "CR15NetGun2CRWin10",
        "CR15NetBulletCRWin10", "CR15NetMagazineCRWin10",
        "CR15NetExplosionCRWin10", "CR15NetDamageableCRWin10",
        "CR15NetPooledActorCRWin10", "CR15NetAimAssistCRWin10",
        "CR15NetAutoTargetCRWin10", "CR15CollisionCRWin10",
        "CR15NetSensorTargetCRWin10", "CR15NetFollowPlayerCRWin10",
        "CR15DampenerCRWin10", "CR15NetBitFieldCRWin10",
        "CR15NetDebugDrawCRWin10", "CR15NetDynamicCoverCRWin10",
        "CR15NetGhostLODCRWin10",
    ]
    copied_crs = 0
    for cr_name in WEAPON_CRS_TO_COPY:
        # Find combat sub-level CR
        cr_data = None
        for mf in [build_mf] + [d for d in CLEAN_EXTRACT_DIR.iterdir() if d.is_dir()]:
            td = find_type_dir(mf, cr_name)
            if td:
                f = td / f"{COMBAT_LEVEL}.bin"
                if f.is_file():
                    cr_data = f.read_bytes()
                    break
        if cr_data is None or len(cr_data) < 56:
            continue
        # Check if arena already has this CR
        existing = read_resource(build_mf, cr_name, TARGET_LEVEL)
        if existing and len(existing) > 56:
            continue  # Arena already has it, don't overwrite
        # Copy combat CR as the arena level's CR
        write_resource(build_mf, cr_name, TARGET_LEVEL, cr_data)
        copied_crs += 1
    log.info("    Copied %d weapon CRs to arena level", copied_crs)

    # Also merge combat CS types into CComponentSpaceResource
    arena_csr = read_resource(build_mf, "CComponentSpaceResourceWin10", TARGET_LEVEL)
    csr_path = CLEAN_EXTRACT_DIR / build_mf.name / "CComponentSpaceResourceWin10" / f"{COMBAT_LEVEL}.bin"
    combat_csr = csr_path.read_bytes() if csr_path.is_file() else None
    if arena_csr and combat_csr:
        a_count = struct.unpack_from('<Q', arena_csr, 56)[0]
        a_cs_set = set()
        for i in range(a_count):
            a_cs_set.add(struct.unpack_from('<Q', arena_csr, 64 + i * 16)[0])
        c_count = struct.unpack_from('<Q', combat_csr, 56)[0]
        wanted = set()
        for i in range(c_count):
            cs_hash = struct.unpack_from('<Q', combat_csr, 64 + i * 16)[0]
            if cs_hash not in a_cs_set:
                wanted.add(cs_hash)
        if wanted:
            all_entries = []
            for i in range(a_count):
                off = 64 + i * 16
                all_entries.append((struct.unpack_from('<Q', arena_csr, off)[0],
                                    struct.unpack_from('<Q', arena_csr, off + 8)[0]))
            for cs_hash in wanted:
                all_entries.append((cs_hash, TARGET_LEVEL_HASH))
            all_entries.sort(key=lambda e: struct.unpack('<q', struct.pack('<Q', e[0]))[0])
            new_count = len(all_entries)
            result = bytearray(64)
            struct.pack_into('<Q', result, 0, TARGET_LEVEL_HASH)
            struct.pack_into('<Q', result, 8 + 8, new_count * 16)
            struct.pack_into('<I', result, 8 + 28, 1)
            struct.pack_into('<Q', result, 8 + 40, new_count)
            struct.pack_into('<Q', result, 8 + 48, new_count)
            for cs_h, lvl_h in all_entries:
                result += struct.pack('<QQ', cs_h, lvl_h)
            write_resource(build_mf, "CComponentSpaceResourceWin10", TARGET_LEVEL, bytes(result))
            log.info("    Added %d combat CS types to arena CSR", len(wanted))

    # Add CArchiveResource pairs for the copied CRs
    car_dir2 = find_type_dir(build_mf, "carchiveresourceWin10")
    if car_dir2 is None:
        car_dir2 = find_type_dir(build_mf, "CArchiveResourceWin10")
    if car_dir2:
        arena_car2 = car_dir2 / f"{TARGET_LEVEL}.bin"
        if arena_car2.is_file():
            car_data2 = bytearray(arena_car2.read_bytes())
            a_flat2 = struct.unpack_from('<I', car_data2, 0)[0]
            a_pair_off2 = 4 + a_flat2 * 8
            a_pair_count2 = struct.unpack_from('<I', car_data2, a_pair_off2)[0]
            a_pair_end2 = a_pair_off2 + 4 + a_pair_count2 * 16
            existing_pairs = set()
            for i in range(a_pair_count2):
                off = a_pair_off2 + 4 + i * 16
                existing_pairs.add((struct.unpack_from('<Q', car_data2, off)[0],
                                     struct.unpack_from('<Q', car_data2, off + 8)[0]))
            new_pairs = bytearray()
            added = 0
            for cr_name in WEAPON_CRS_TO_COPY:
                cr_type_hash = rad_hash(cr_name)
                pair = (cr_type_hash, TARGET_LEVEL_HASH)
                if pair not in existing_pairs:
                    new_pairs += struct.pack('<QQ', cr_type_hash, TARGET_LEVEL_HASH)
                    added += 1
                    existing_pairs.add(pair)
            if added:
                struct.pack_into('<I', car_data2, a_pair_off2, a_pair_count2 + added)
                car_data2[a_pair_end2:a_pair_end2] = new_pairs
                arena_car2.write_bytes(bytes(car_data2))
                log.info("    Added %d weapon CR pairs to CArchiveResource", added)

    arena_ad_data = read_resource(build_mf, "CActorDataResourceWin10", TARGET_LEVEL)
    arena_ad = ActorDataResource.from_bytes(arena_ad_data)
    arena_actor_hashes = set(arena_ad.nodeids)
    log.info("    Arena actors: %d", arena_ad.actor_count)

    combat_ad_data = read_resource(build_mf, "CActorDataResourceWin10", COMBAT_LEVEL)
    if combat_ad_data is None:
        # Try from clean extract
        for mf in CLEAN_EXTRACT_DIR.iterdir():
            if not mf.is_dir():
                continue
            td = mf / "CActorDataResourceWin10"
            f = td / f"{COMBAT_LEVEL}.bin"
            if f.is_file():
                combat_ad_data = f.read_bytes()
                break
    if combat_ad_data is None:
        raise FileNotFoundError(f"Cannot find CActorDataResource for {COMBAT_LEVEL}")

    combat_ad = ActorDataResource.from_bytes(combat_ad_data)
    log.info("    Combat actors: %d", combat_ad.actor_count)

    # Helper for finding actor descendants
    def get_descendants(ad, root_idx):
        result = {root_idx}
        queue = [root_idx]
        while queue:
            parent = queue.pop(0)
            for i in range(len(ad.parents)):
                if ad.parents[i] == parent and i not in result:
                    result.add(i)
                    queue.append(i)
        return result

    # Transplant gun trees + ALL actors that any transplanted CR references.
    # The weapon system resolves component handles at runtime for actors that
    # may not be in the gun tree (decal targets, effect actors, etc.).
    shared_hashes = arena_actor_hashes & set(combat_ad.nodeids)
    log.info("    Shared actors: %d", len(shared_hashes))

    # Step 1: Gun root actors from CR15NetGunCR
    netgun_data = _get_combat_resource(build_mf, "CR15NetGunCRWin10")
    ng_count = struct.unpack_from('<Q', netgun_data, 48)[0]
    gun_root_indices = set()
    for i in range(ng_count):
        ahash = struct.unpack_from('<Q', netgun_data, 56 + i * 160 + 8)[0]
        try: gun_root_indices.add(combat_ad.nodeids.index(ahash))
        except ValueError: pass
    log.info("    Gun root actors: %d", len(gun_root_indices))

    # Step 2: Gun trees (roots + all descendants)
    needed_actors = set()
    for gi in gun_root_indices:
        needed_actors |= get_descendants(combat_ad, gi)

    # Step 3: Actors from weapon-related CRs
    for cr_name in ["CR15NetBulletCRWin10", "CR15NetExplosionCRWin10",
                     "CR15NetDamageableCRWin10", "CR15NetMagazineCRWin10",
                     "CR15CollisionCRWin10", "CR15NetAimAssistCRWin10",
                     "CR15NetSensorTargetCRWin10", "CR15NetFollowPlayerCRWin10",
                     "CR15NetAutoTargetCRWin10", "CR15DampenerCRWin10",
                     "CR15NetBalanceSettingsCRWin10", "CR15NetPooledActorCRWin10",
                     "CR15TeamCRWin10", "CR15NetActorCRWin10"]:
        cr_data = _get_combat_resource_optional(build_mf, cr_name)
        if cr_data is None or len(cr_data) < 56: continue
        cr_dbs = struct.unpack_from('<Q', cr_data, 8)[0]
        cr_cap = struct.unpack_from('<Q', cr_data, 40)[0]
        cr_cnt = struct.unpack_from('<Q', cr_data, 48)[0]
        if cr_cap == 0 or cr_cnt == 0: continue
        cr_esize = cr_dbs // cr_cap
        for j in range(cr_cnt):
            ahash = struct.unpack_from('<Q', cr_data, 56 + j * cr_esize + 8)[0]
            if ahash < 0x1000: continue
            try:
                idx = combat_ad.nodeids.index(ahash)
                needed_actors |= get_descendants(combat_ad, idx)
            except ValueError: pass

    # Step 3b: Muzzle/bullet cross-refs from NetGunCR
    for j in range(ng_count):
        for field_off in [80, 104]:
            bh = struct.unpack_from('<Q', netgun_data, 56 + j * 160 + field_off)[0]
            if bh > 0x1000:
                try:
                    idx = combat_ad.nodeids.index(bh)
                    needed_actors |= get_descendants(combat_ad, idx)
                except ValueError: pass

    # Step 4: Weapon SUPPORT actors (runtime CRI cross-refs).
    # These are referenced via script variables and per-slot stored handles,
    # not via standard CR actor_hash fields. Identified by weapontrace comparison.
    weapon_support_hashes = [
        0xD25D927E39480CB5, 0x491AA01F9195AE13,  # combat[41,42] decal targets
        0x9D893CF453319F78, 0xA5B88E61F02396B6,  # combat[586,587] script + parent
        0xE40247042081AE54,                        # combat[588] script
        0x8D45FB7AC36CC066,                        # combat[1128] parent tree
        0xD27A32A33FFD9890, 0xA899AA8550458928,   # combat[1131,1132] canvas+script
    ]
    # Also add script-referenced actors
    weapon_support_hashes += [
        0x06E83268F2A9A59E, 0x20B43793F784684A, 0x3EA06A2A3FC7A708,
        0x40007F42226AF08E, 0x53237BD3DBED2048, 0x8EB98EEE06D00FAE,
        0x94E1458813BB6C52, 0x98A2D33873CD03C2, 0xA5553A0A329B52DC,
        0xC7CAAD94FC0D2A62, 0xE3504495695FE85C, 0xED48432C9FED3914,
        0xFD44192CA11C271E, 0xA27D07192C299D3E, 0x26B939FCAC9FBFF8,
        0x8A133114A604FF08, 0x0671C5E4E1797488,
    ]
    for h in weapon_support_hashes:
        try:
            idx = combat_ad.nodeids.index(h)
            needed_actors |= get_descendants(combat_ad, idx)
        except ValueError: pass
    log.info("    Total needed actors (guns+support+scripts): %d", len(needed_actors))

    combat_nodeid_set = set(combat_ad.nodeids)

    # Remove shared actors from transplant set (zero shared in practice)
    transplant_indices = sorted(i for i in needed_actors
                                if combat_ad.nodeids[i] not in shared_hashes)
    log.info("    Total actors to transplant: %d", len(transplant_indices))

    # Use ORIGINAL combat actor hashes — no remapping needed.
    # Weapon system (gear tables → EntityLookup) requires original hashes to
    # resolve weapon entities. Zero conflicts with arena confirmed.
    combat_actor_hashes = set(combat_ad.nodeids[ci] for ci in transplant_indices)
    combat_old_hashes = combat_actor_hashes  # Same set — no remap
    log.info("    Using %d original combat hashes (no remap)", len(combat_actor_hashes))

    # --- 3b. Merge CActorDataResource ---
    log.info("")
    log.info("  --- Step 3b: Merge CActorDataResource ---")

    arena_count = arena_ad.actor_count
    combat_to_merged = {}
    for seq, ci in enumerate(transplant_indices):
        combat_to_merged[ci] = arena_count + seq
    for ci in range(combat_ad.actor_count):
        if combat_ad.nodeids[ci] in shared_hashes:
            combat_to_merged[ci] = arena_ad.nodeids.index(combat_ad.nodeids[ci])

    # Append actors with original combat hashes
    for ci in transplant_indices:
        orig_hash = combat_ad.nodeids[ci]
        arena_ad.nodeids.append(orig_hash)
        arena_ad.names.append(orig_hash)
        arena_ad.prefabids.append(combat_ad.prefabids[ci])
        arena_ad.transforms.append(combat_ad.transforms[ci])
        arena_ad.numpooled.append(combat_ad.numpooled[ci])

        old_parent = combat_ad.parents[ci]
        if old_parent == 0xFFFF:
            arena_ad.parents.append(0xFFFF)
        elif old_parent in combat_to_merged:
            arena_ad.parents.append(combat_to_merged[old_parent])
        else:
            arena_ad.parents.append(0xFFFF)

    # Expand bit arrays
    import math
    new_total = arena_ad.actor_count
    new_qword_count = math.ceil(new_total / 64)
    for b in range(5):
        while len(arena_ad.bits[b]) < new_qword_count:
            arena_ad.bits[b].append(0)
        for ci in transplant_indices:
            new_idx = combat_to_merged[ci]
            old_qw_idx = ci // 64
            old_bit_pos = ci % 64
            new_qw_idx = new_idx // 64
            new_bit_pos = new_idx % 64
            if old_qw_idx < len(combat_ad.bits[b]):
                bit_val = (combat_ad.bits[b][old_qw_idx] >> old_bit_pos) & 1
                if bit_val:
                    arena_ad.bits[b][new_qw_idx] |= (1 << new_bit_pos)

    # Headers: each entry is (entity_hash, actor_index) — the CRI lookup table.
    # Every actor MUST have a header entry or the engine can't find it.
    from echomod.resources.actor_data import ActorDataHeaderEntry, ActorDataComponentEntry
    from echomod.descriptors import RadArrayDescriptor56
    new_headers = []
    for ci in transplant_indices:
        orig_hash = combat_ad.nodeids[ci]
        new_idx = combat_to_merged[ci]
        new_headers.append(ActorDataHeaderEntry(type_hash=orig_hash, count=new_idx))

    # Also add ALIAS headers from the combat level.
    # These are extra header entries (hash NOT in nodeids) that scripts use
    # to find actors by component/alias hashes. Each maps to an actor index.
    combat_nodeid_set = set(combat_ad.nodeids)
    alias_count = 0
    for h in combat_ad.headers:
        if h.type_hash not in combat_nodeid_set:
            # This is an alias. Remap the actor index to our merged level.
            if h.count in combat_to_merged:
                new_idx = combat_to_merged[h.count]
                new_headers.append(ActorDataHeaderEntry(type_hash=h.type_hash, count=new_idx))
                alias_count += 1

    arena_ad.headers.extend(new_headers)
    arena_ad.headers.sort(key=lambda h: struct.unpack('<q', struct.pack('<Q', h.type_hash))[0])
    log.info("    Added %d header entries (%d actors + %d aliases, %d total)",
             len(new_headers), len(new_headers) - alias_count, alias_count,
             len(arena_ad.headers))

    # Component membership: merge combat component types so CS systems can find actors.
    # Map combat component types → remapped actor indices
    combat_comp_map = {}
    for comp in combat_ad.components:
        remapped = [combat_to_merged[ai] for ai in comp.actor_indices if ai in combat_to_merged]
        if remapped:
            combat_comp_map[comp.type_hash] = remapped

    arena_comp_map = {comp.type_hash: comp for comp in arena_ad.components}
    added_comp = 0
    for type_hash, new_indices in combat_comp_map.items():
        if type_hash in arena_comp_map:
            arena_comp_map[type_hash].actor_indices.extend(new_indices)
        else:
            arena_ad.components.append(ActorDataComponentEntry(
                type_hash=type_hash,
                indices_descriptor=RadArrayDescriptor56(),
                actor_indices=new_indices,
            ))
            added_comp += 1
    log.info("    Merged component membership: %d existing updated, %d new types added",
             len(combat_comp_map) - added_comp, added_comp)

    # --- Inject player_combat actor ---
    # The CActorJugglerCS in r14_glb_global_mp expects player_combat to exist
    # in the gamespace for SwapPlayerByName to succeed. Without it, the player
    # stays on the general body actor (no weapon expression graph).
    # player_combat has zero static CRs — the body swap script configures it
    # at runtime with the combat model/animations/expression graph.
    # NOTE: player_combat (0xF1E70ACD0DDE0312) already exists in r14_glb_global_mp
    # with ALL weapon CRs (MagazinePouch, Damageable, Equipment, etc.). No injection needed.
    # The CActorJuggler maps name hash player_combat → actor 0xF1E70ACD0DDE0312.
    # In echo_combat_private mode, AddPlayerToLobby selects player_combat automatically.

    ad_output = arena_ad.to_bytes()
    log.info("    Merged actor count: %d", arena_ad.actor_count)
    log.info("    Size: %d -> %d", len(arena_ad_data), len(ad_output))
    write_resource(build_mf, "CActorDataResourceWin10", TARGET_LEVEL, ad_output)

    # --- 3c. Merge CTransformCR ---
    log.info("")
    log.info("  --- Step 3c: Merge CTransformCR ---")

    arena_tcr_data = read_resource(build_mf, "CTransformCRWin10", TARGET_LEVEL)
    arena_tcr = TransformCR.from_bytes(arena_tcr_data)
    arena_tcr_count = len(arena_tcr.entries)

    combat_tcr_data = _get_combat_resource(build_mf, "CTransformCRWin10")
    combat_tcr = TransformCR.from_bytes(combat_tcr_data)

    # Transplant combat transform entries with original hashes (no remap)
    transplanted_tcr = 0
    for entry in combat_tcr.entries:
        if entry.entity_hash in combat_old_hashes:
            cloned = entry.clone(entry.entity_hash)
            arena_tcr.add_entry(cloned)
            transplanted_tcr += 1

    tcr_gap = len(arena_tcr.entries) - arena_tcr_count
    log.info("    Transplanted %d CTransformCR entries (gap=%d)", transplanted_tcr, tcr_gap)
    write_resource(build_mf, "CTransformCRWin10", TARGET_LEVEL, arena_tcr.to_bytes())

    # --- 3d. Expand CGSceneResource BSP ---
    log.info("")
    log.info("  --- Step 3d: Expand CGSceneResource BSP ---")

    if tcr_gap > 0:
        scene_data = read_resource(build_mf, "CGSceneResourceWin10", TARGET_LEVEL)
        scene = CGSceneResource.from_bytes(scene_data)
        scene.expand_for_clones(tcr_gap)
        scene_output = scene.to_bytes()
        log.info("    BSP expanded by %d entries (%d -> %d bytes)",
                 tcr_gap, len(scene_data), len(scene_output))
        write_resource(build_mf, "CGSceneResourceWin10", TARGET_LEVEL, scene_output)

    # --- 3e. Merge CModelCR ---
    log.info("")
    log.info("  --- Step 3e: Merge CModelCR ---")
    # NOTE: All merge functions use combat_old_hashes to find entries in
    # combat CR files. The hash remap (old→new) is done in step 3k globally.
    _merge_model_cr(build_mf, combat_old_hashes)

    # --- 3f. Merge CAnimationCR ---
    log.info("")
    log.info("  --- Step 3f: Merge CAnimationCR ---")
    _merge_animation_cr(build_mf, combat_old_hashes)

    # --- 3g. Merge compound CRs ---
    # Skip CComponentLODCR — lobby combat entries have null actor refs to sibling levels
    for cr_type in ["CScriptCRWin10", "CParticleEffectCRWin10"]:
        log.info("")
        log.info("  --- Step 3g: Merge %s ---", cr_type)
        _merge_or_create_compound_cr(build_mf, cr_type, combat_old_hashes)

    # --- 3g2. Strip arena body-swap detection script ---
    # The arena level has a script (0x12220D647AD3F5D3) on actor 0x10AEFC7855E68C55
    # that detects "combat player in arena" and forces an arena body swap. In turbo
    # mode (echo_combat_private + arena level), this would override the combat body
    # and remove the weapon expression graph. We strip the script entry so the
    # combat body (with weapon attachment nodes) persists.
    log.info("")
    log.info("  --- Step 3g2: Strip arena body-swap detection script ---")
    STRIP_SCRIPT_ACTOR = 0x10AEFC7855E68C55
    script_cr_data = read_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL)
    if script_cr_data and len(script_cr_data) >= 56:
        s_dbs = struct.unpack_from('<Q', script_cr_data, 8)[0]
        s_cap = struct.unpack_from('<Q', script_cr_data, 40)[0]
        s_count = struct.unpack_from('<Q', script_cr_data, 48)[0]
        if s_cap > 0 and s_count > 0:
            s_esize = s_dbs // s_cap
            s_flat_end = 56 + s_count * s_esize
            # Don't strip ANY arena scripts — they're all needed for gameplay.
            strip_indices = []
            if False:  # Disabled — keeping all arena scripts
                    strip_indices.append(i)
            if strip_indices:
                # Rebuild without stripped entries — keep flat entries + inline data
                # For compound CRs (720B entries with inline), we need to track inline
                kept_flat = bytearray()
                kept_inline = bytearray()
                inline_start = s_flat_end
                # Compute per-entry inline sizes
                KNOWN_DESC_OFFSETS = [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664]
                inline_pos = []
                pos = s_flat_end
                for i in range(s_count):
                    start = pos
                    eoff = 56 + i * s_esize
                    for d in KNOWN_DESC_OFFSETS:
                        if eoff + d + 56 <= s_flat_end:
                            dbs_val = struct.unpack_from('<Q', script_cr_data, eoff + d + 8)[0]
                            cnt_val = struct.unpack_from('<Q', script_cr_data, eoff + d + 48)[0]
                            if dbs_val > 0 and cnt_val > 0:
                                pos += dbs_val
                    inline_pos.append((start, pos))

                kept_count = 0
                for i in range(s_count):
                    if i in strip_indices:
                        continue
                    off = 56 + i * s_esize
                    kept_flat.extend(script_cr_data[off:off + s_esize])
                    if i < len(inline_pos):
                        istart, iend = inline_pos[i]
                        kept_inline.extend(script_cr_data[istart:iend])
                    kept_count += 1

                # Build new file
                new_header = bytearray(script_cr_data[:56])
                struct.pack_into('<Q', new_header, 8, kept_count * s_esize)
                struct.pack_into('<Q', new_header, 40, kept_count)
                struct.pack_into('<Q', new_header, 48, kept_count)
                new_data = bytes(new_header) + bytes(kept_flat) + bytes(kept_inline)
                write_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL, new_data)
                log.info("    Stripped %d body-swap script entries (%d -> %d entries, %d -> %d bytes)",
                         len(strip_indices), s_count, kept_count, len(script_cr_data), len(new_data))
            else:
                log.info("    Body-swap script not found in CScriptCR (already absent)")

    # --- 3g3. Combat swap script injection SKIPPED ---
    # Not needed: echo_combat_private selects player_combat (0xF1E70ACD0DDE0312)
    # automatically. With both arena swap scripts stripped, the player stays on
    # the combat body which has ALL weapon CRs.
    log.info("")
    log.info("  --- Step 3g3: Skipped (echo_combat_private handles body selection) ---")
    if False:
        COMBAT_MAP_LEVEL = "mpl_combat_combustion"
        COMBAT_SWAP_ACTOR = 0xA8ADFF5786155095
        combat_map_script = None
        for mf in [build_mf] + [d for d in CLEAN_EXTRACT_DIR.iterdir() if d.is_dir()]:
            td = mf / "CScriptCRWin10"
            f = td / f"{COMBAT_MAP_LEVEL}.bin"
            if f.is_file():
                combat_map_script = f.read_bytes()
                break
    if False and combat_map_script and len(combat_map_script) >= 56:
        # Read current target script CR (just modified above)
        target_script_data = read_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL)
        if target_script_data:
            ts_dbs = struct.unpack_from('<Q', target_script_data, 8)[0]
            ts_cap = struct.unpack_from('<Q', target_script_data, 40)[0]
            ts_count = struct.unpack_from('<Q', target_script_data, 48)[0]
            ts_esize = ts_dbs // ts_cap if ts_cap > 0 else 720
            # Parse combat map scripts
            cm_dbs = struct.unpack_from('<Q', combat_map_script, 8)[0]
            cm_cap = struct.unpack_from('<Q', combat_map_script, 40)[0]
            cm_count = struct.unpack_from('<Q', combat_map_script, 48)[0]
            cm_esize = cm_dbs // cm_cap if cm_cap > 0 else 720
            if ts_esize == cm_esize:
                cm_flat_end = 56 + cm_count * cm_esize
                # Find entries for the swap actor and collect their inline data
                KNOWN_DESC_OFFSETS = [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664]
                cm_inline_pos = []
                pos = cm_flat_end
                for i in range(cm_count):
                    start = pos
                    eoff = 56 + i * cm_esize
                    for d in KNOWN_DESC_OFFSETS:
                        if eoff + d + 56 <= cm_flat_end:
                            dbs_val = struct.unpack_from('<Q', combat_map_script, eoff + d + 8)[0]
                            cnt_val = struct.unpack_from('<Q', combat_map_script, eoff + d + 48)[0]
                            if dbs_val > 0 and cnt_val > 0:
                                pos += dbs_val
                    cm_inline_pos.append((start, pos))
                # Collect swap actor entries + inline
                swap_flat = bytearray()
                swap_inline = bytearray()
                swap_added = 0
                for i in range(cm_count):
                    off = 56 + i * cm_esize
                    actor = struct.unpack_from('<Q', combat_map_script, off + 8)[0]
                    if actor == COMBAT_SWAP_ACTOR:
                        swap_flat.extend(combat_map_script[off:off + cm_esize])
                        if i < len(cm_inline_pos):
                            istart, iend = cm_inline_pos[i]
                            swap_inline.extend(combat_map_script[istart:iend])
                        swap_added += 1
                if swap_added > 0:
                    # Append to current target script CR
                    ts_flat_end = 56 + ts_count * ts_esize
                    existing_inline = target_script_data[ts_flat_end:]
                    new_count = ts_count + swap_added
                    result = bytearray(target_script_data[:56])
                    struct.pack_into('<Q', result, 8, new_count * ts_esize)
                    struct.pack_into('<Q', result, 40, new_count)
                    struct.pack_into('<Q', result, 48, new_count)
                    result.extend(target_script_data[56:ts_flat_end])  # existing flat
                    result.extend(swap_flat)                           # new flat
                    result.extend(existing_inline)                     # existing inline
                    result.extend(swap_inline)                         # new inline
                    write_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL, bytes(result))
                    log.info("    Injected %d combat swap scripts from %s (%d -> %d entries)",
                             swap_added, COMBAT_MAP_LEVEL, ts_count, new_count)
                else:
                    log.info("    No swap actor scripts found in %s", COMBAT_MAP_LEVEL)
            else:
                log.info("    Entry size mismatch: target=%d combat_map=%d", ts_esize, cm_esize)
    else:
        log.info("    Combat map CScriptCR not found")

    # --- 3h. Merge CR15SyncGrabCR ---
    log.info("")
    log.info("  --- Step 3h: Merge CR15SyncGrabCR ---")
    # Skip — SyncGrab has deeply nested CTableXT inline data that the
    # generic multi-array merger can't handle. Arena version preserved.
    log.info("    Skipped (complex nested inline — arena version preserved)")

    # --- 3i. Merge CInstanceModelCR ---
    log.info("")
    log.info("  --- Step 3i: Merge CInstanceModelCR ---")
    _merge_instancemodel_cr(build_mf, combat_old_hashes)

    # --- 3i2. Multi-array / combat-only CRs (copy directly) ---
    log.info("")
    log.info("  --- Step 3i2: Copy multi-array combat CRs ---")
    # Create empty placeholder CRs for CS types that need a CR but
    # whose actors are non-gun (posters, sticky surfaces, etc.)
    # Empty placeholders for CRs whose CS types exist but we don't need the data
    for empty_cr in ["CR15NetDynamicPosterCRWin10", "CR15StickySurfaceCRWin10",
                      "CR15PlatformCRWin10"]:
        empty = bytearray(56)
        struct.pack_into('<I', empty, 28, 1)  # flags = 1
        write_resource(build_mf, empty_cr, TARGET_LEVEL, bytes(empty))
        log.info("    %s: empty placeholder (0 entries)", empty_cr)

    for copy_cr in ["CR15NetBitFieldCRWin10", "CR15TriggerCRWin10"]:
        cr_data = _get_combat_resource_optional(build_mf, copy_cr)
        if cr_data:
            write_resource(build_mf, copy_cr, TARGET_LEVEL, cr_data)
            log.info("    %s: copied (%d bytes)", copy_cr, len(cr_data))

    # --- 3i3. Generate CR15NetGun2CR to force CS creation ---
    # CR15NetGun2CR doesn't ship in any level, but the CS MUST exist in the game
    # space for the balance settings callback to find it and populate it.
    # Creating the CR file forces CreateComponentSystems to instantiate the CS.
    # Structure from Quest binary (SR15NetGun2CD::SProperties, 96B entries):
    #   [32B base] [24B muzzletransform] [24B bulletinstance]
    #   [4B damage] [4B rateoffire] [4B projvel] [4B spread]
    # Values are initial defaults — balance settings overrides them at runtime.
    log.info("")
    log.info("  --- Step 3i3: Generate CR15NetGun2CR ---")
    netgun_cr = _get_combat_resource_optional(build_mf, "CR15NetGunCRWin10")
    if netgun_cr and len(netgun_cr) >= 56:
        ng_count = struct.unpack_from('<Q', netgun_cr, 48)[0]
        ng2_entries = bytearray()
        for i in range(ng_count):
            src = netgun_cr[56 + i * 160 : 56 + (i + 1) * 160]
            dst = bytearray(96)
            dst[0:32] = src[0:32]           # Base header
            dst[32:56] = src[80:104]         # muzzletransform from NetGunCR +80
            dst[56:80] = src[104:128]        # bulletinstance from NetGunCR +104
            # Default values (overridden by mp_weapon_settings at runtime):
            struct.pack_into('<f', dst, 80, 9.0)    # damage
            struct.pack_into('<f', dst, 84, 8.0)    # rateoffire
            struct.pack_into('<f', dst, 88, 80.0)   # projvel
            dst[92:96] = src[128:132]                # spread from NetGunCR +128
            ng2_entries.extend(dst)

        ng2_header = bytearray(56)
        struct.pack_into('<Q', ng2_header, 8, ng_count * 96)
        struct.pack_into('<I', ng2_header, 28, 1)
        struct.pack_into('<Q', ng2_header, 40, ng_count)
        struct.pack_into('<Q', ng2_header, 48, ng_count)
        write_resource(build_mf, "CR15NetGun2CRWin10", TARGET_LEVEL,
                       bytes(ng2_header) + bytes(ng2_entries))
        log.info("    Generated CR15NetGun2CR: %d entries, %d bytes",
                 ng_count, len(ng2_header) + len(ng2_entries))

    # --- 3i5. Merge CComponentSpaceResource (CS type registry) ---
    # This resource lists which CS types to create for the level.
    # Format: [8B level_hash] [56B RadArrayDescriptor56] [count × 16B entries]
    # Each entry: (cs_hash: u64, level_hash: u64)
    # Combat has 23 CS types not in arena (incl. NetGun2CS).
    log.info("")
    log.info("  --- Step 3i5: Merge CComponentSpaceResource ---")
    arena_csr = read_resource(build_mf, "CComponentSpaceResourceWin10", TARGET_LEVEL)
    # Read CComponentSpaceResource from the SHARED combat level (has the canonical CS types)
    csr_path = CLEAN_EXTRACT_DIR / build_mf.name / "CComponentSpaceResourceWin10" / f"{COMBAT_CSR_LEVEL}.bin"
    combat_csr = csr_path.read_bytes() if csr_path.is_file() else None
    if arena_csr and combat_csr:
        # Parse arena entries
        a_count = struct.unpack_from('<Q', arena_csr, 56)[0]
        a_cs_set = set()
        for i in range(a_count):
            a_cs_set.add(struct.unpack_from('<Q', arena_csr, 64 + i * 16)[0])

        # Add only combat CS types that we have CR data for.
        # Adding CS types without factories/CRs causes crashes.
        # Start with the key ones we've verified.
        # The NetGun2CR we generate provides data for 0x7A804A0A9B2F5386.
        # CComponentLODZoneCS excluded — its CR references actors only in combat
        # levels, causing "Missing actor" fatal errors if included verbatim.
        EXCLUDED_CS_HASHES = {0xC5BC8612962D79DA}

        wanted_cs_hashes = set()
        c_count = struct.unpack_from('<Q', combat_csr, 56)[0]
        for i in range(c_count):
            cs_hash = struct.unpack_from('<Q', combat_csr, 64 + i * 16)[0]
            if cs_hash not in a_cs_set and cs_hash not in EXCLUDED_CS_HASHES:
                wanted_cs_hashes.add(cs_hash)

        # Add all combat-only CS types. MUST maintain signed-int64 sort order.
        new_entries = bytearray()
        added = 0
        for cs_hash in sorted(wanted_cs_hashes):
            new_entries += struct.pack('<QQ', cs_hash, TARGET_LEVEL_HASH)
            added += 1

        if added > 0:
            # Collect all entries (existing + new), then sort by signed int64
            all_entries = []
            for i in range(a_count):
                off = 64 + i * 16
                cs_h = struct.unpack_from('<Q', arena_csr, off)[0]
                lvl_h = struct.unpack_from('<Q', arena_csr, off + 8)[0]
                all_entries.append((cs_h, lvl_h))
            for cs_hash in sorted(wanted_cs_hashes):
                all_entries.append((cs_hash, TARGET_LEVEL_HASH))

            # Sort by signed int64 of cs_hash
            all_entries.sort(key=lambda e: struct.unpack('<q', struct.pack('<Q', e[0]))[0])

            new_count = len(all_entries)
            # Build new file: [8B level_hash] [56B descriptor] [entries]
            result = bytearray(64)  # header: 8B level + 56B descriptor
            struct.pack_into('<Q', result, 0, TARGET_LEVEL_HASH)
            # Descriptor at +8
            struct.pack_into('<Q', result, 8 + 8, new_count * 16)   # dbs
            struct.pack_into('<I', result, 8 + 28, 1)                # flags
            struct.pack_into('<Q', result, 8 + 40, new_count)       # capacity
            struct.pack_into('<Q', result, 8 + 48, new_count)       # count
            for cs_h, lvl_h in all_entries:
                result += struct.pack('<QQ', cs_h, lvl_h)
            write_resource(build_mf, "CComponentSpaceResourceWin10", TARGET_LEVEL, bytes(result))
            log.info("    Added %d combat CS types (%d total, sorted)", added, new_count)
        else:
            log.info("    No new CS types to add")

    # --- 3i6. Create empty stubs for globally-required CR types ---
    # The global MP CSR (r14_glb_global_mp) declares CComponentLODZoneCS, so the
    # engine expects CComponentLODZoneCR to exist for EVERY level. Combat entries
    # reference actors not in our level, so we provide an empty stub instead.
    log.info("")
    log.info("  --- Step 3i6: Create empty CR stubs ---")
    STUB_CRS = ["CComponentLODZoneCRWin10"]
    for cr_name in STUB_CRS:
        td = find_type_dir(build_mf, cr_name)
        if td and (td / f"{TARGET_LEVEL}.bin").is_file():
            log.info("    %s: already exists, skipping", cr_name)
            continue
        # Build empty CR: 56 bytes (header only, zero entries)
        empty_cr = bytearray(56)
        td = build_mf / cr_name
        td.mkdir(exist_ok=True)
        (td / f"{TARGET_LEVEL}.bin").write_bytes(bytes(empty_cr))
        log.info("    %s: created empty stub (56 bytes)", cr_name)

    # --- 3j. Merge/create all remaining CR types ---
    # NOTE: Use combat_old_hashes for matching in combat CR files!
    log.info("")
    log.info("  --- Step 3j: Merge remaining CR types ---")
    _merge_remaining_crs(build_mf, combat_old_hashes)

    # --- 3k. Hash remapping REMOVED ---
    # Original combat hashes are preserved to maintain weapon system compatibility.
    # Gear tables → EntityLookup requires original hashes. Zero arena conflicts.
    log.info("")
    log.info("  --- Step 3k: Hash remapping SKIPPED (using original hashes) ---")

    # --- 3l. Merge CArchiveResource (add combat resource pairs) ---
    log.info("")
    log.info("  --- Step 3l: Merge CArchiveResource ---")
    _merge_archive_resource(build_mf)

    # --- 3m. Copy combat model sub-resources to build directory ---
    log.info("")
    log.info("  --- Step 3m: Copy combat model sub-resources ---")
    _copy_combat_model_resources(build_mf)

    # --- 3n. Add combat sub-level loading script ---
    # The combat sub-level (0xCB9977F7FC2B4526) needs to load as a separate
    # gamespace alongside our arena level. This is how the lobby does it:
    # a StreamingScript (0xF808C072BB49DA47) calls CLoadLevelNode to
    # dynamically load the combat sub-level.
    log.info("")
    log.info("  --- Step 3n: Add combat sub-level loading script ---")

    LEVEL_LOADER_ACTOR = 0xA284EC13052FEA81
    ORIGINAL_STREAMING_SCRIPT = 0xF808C072BB49DA47  # original lobby script (untouched)
    LEVEL_LOADER_SCRIPT = 0xF07DC2D7407C2546       # rad_hash("combat_streaming_script")
    GENERIC_SCRIPT_TYPE = 0x6AEA99EEB2BAB6C4

    # 3n-1: Add the level-loader actor to CActorDataResource
    target_ad_data = read_resource(build_mf, "CActorDataResourceWin10", TARGET_LEVEL)
    target_ad = ActorDataResource.from_bytes(target_ad_data)
    if LEVEL_LOADER_ACTOR not in set(target_ad.nodeids):
        ll_idx = target_ad.actor_count
        target_ad.nodeids.append(LEVEL_LOADER_ACTOR)
        target_ad.names.append(LEVEL_LOADER_ACTOR)
        target_ad.prefabids.append(0)
        target_ad.transforms.append(target_ad.transforms[0])
        target_ad.numpooled.append(0)
        target_ad.parents.append(0xFFFF)

        import math
        new_qword_count = math.ceil(target_ad.actor_count / 64)
        for b in range(5):
            while len(target_ad.bits[b]) < new_qword_count:
                target_ad.bits[b].append(0)

        target_ad.headers.append(ActorDataHeaderEntry(
            type_hash=LEVEL_LOADER_ACTOR, count=ll_idx))
        target_ad.headers.sort(key=lambda h: struct.unpack('<q', struct.pack('<Q', h.type_hash))[0])

        write_resource(build_mf, "CActorDataResourceWin10", TARGET_LEVEL, target_ad.to_bytes())
        log.info("    Injected level-loader actor (0x%016X) at index %d", LEVEL_LOADER_ACTOR, ll_idx)

    # 3n-2: Add CScriptCR entry for the level-loading StreamingScript
    script_cr_data = read_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL)
    if script_cr_data and len(script_cr_data) >= 56:
        s_dbs = struct.unpack_from('<Q', script_cr_data, 8)[0]
        s_cap = struct.unpack_from('<Q', script_cr_data, 40)[0]
        s_count = struct.unpack_from('<Q', script_cr_data, 48)[0]
        s_esize = s_dbs // s_cap if s_cap > 0 else 720
        s_flat_end = 56 + s_count * s_esize

        # Create a new 720-byte entry (mostly zeroed)
        new_entry = bytearray(s_esize)
        struct.pack_into('<Q', new_entry, 0, GENERIC_SCRIPT_TYPE)   # type hash
        struct.pack_into('<Q', new_entry, 8, LEVEL_LOADER_ACTOR)    # actor hash
        struct.pack_into('<I', new_entry, 16, 0xFFFFFFFF)           # node_index + flags
        struct.pack_into('<Q', new_entry, 32, LEVEL_LOADER_SCRIPT)  # script resource hash
        # Set all inline descriptors to empty (flags=1, count=0)
        for d in [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664]:
            struct.pack_into('<I', new_entry, d + 28, 1)  # flags = 1

        # Append to flat entries (before inline data)
        existing_inline = script_cr_data[s_flat_end:]
        new_count = s_count + 1
        result = bytearray(script_cr_data[:56])
        struct.pack_into('<Q', result, 8, new_count * s_esize)
        struct.pack_into('<Q', result, 40, new_count)
        struct.pack_into('<Q', result, 48, new_count)
        result.extend(script_cr_data[56:s_flat_end])  # existing flat entries
        result.extend(new_entry)                       # new entry
        result.extend(existing_inline)                 # existing inline data

        write_resource(build_mf, "CScriptCRWin10", TARGET_LEVEL, bytes(result))
        log.info("    Added level-loading script entry (%d -> %d entries)", s_count, new_count)

    # 3n-3: Add StreamingScript and CArchiveResource references
    # The StreamingScript resource must be in the archive for loading
    ss_type_hash = rad_hash("StreamingScriptWin10")
    car_dir = find_type_dir(build_mf, "carchiveresourceWin10")
    if car_dir is None:
        car_dir = find_type_dir(build_mf, "CArchiveResourceWin10")
    if car_dir:
        arena_car = car_dir / f"{TARGET_LEVEL}.bin"
        if arena_car.is_file():
            car_data = bytearray(arena_car.read_bytes())
            a_flat = struct.unpack_from('<I', car_data, 0)[0]
            a_pair_off = 4 + a_flat * 8
            a_pair_count = struct.unpack_from('<I', car_data, a_pair_off)[0]
            a_pair_end = a_pair_off + 4 + a_pair_count * 16

            # Add pair for StreamingScript resource
            new_pair = struct.pack('<QQ', ss_type_hash, LEVEL_LOADER_SCRIPT)
            struct.pack_into('<I', car_data, a_pair_off, a_pair_count + 1)
            car_data[a_pair_end:a_pair_end] = new_pair
            arena_car.write_bytes(bytes(car_data))
            log.info("    Added StreamingScript pair to CArchiveResource")

    # --- 3o. Strip combat sub-level of visual/audio/scene data ---
    # The combat sub-level has its own scene graph, lighting, static meshes,
    # sounds, reflection probes, etc. designed for the LOBBY space. Loading
    # these alongside the arena causes darkness, wrong lighting, and missing
    # sounds. Remove everything except weapon-essential resources.
    log.info("")
    log.info("  --- Step 3o: Strip combat sub-level visual/audio data ---")
    COMBAT_SUB = "0xCB9977F7FC2B4526"
    # Don't strip ANY resources — the sub-level fails to load without them.
    # The darkness was from the LOBBY level loading alongside (now fixed).
    STRIP_VISUAL_CRS = set()  # Empty — keep everything
    # Also strip by hash for unknown type directories
    strip_hashes = {rad_hash(n) for n in STRIP_VISUAL_CRS}

    stripped = 0
    for mf_dir in BUILD_DIR.iterdir():
        if not mf_dir.is_dir():
            continue
        for type_dir in mf_dir.iterdir():
            if not type_dir.is_dir():
                continue
            combat_file = type_dir / f"{COMBAT_SUB}.bin"
            if not combat_file.is_file():
                continue
            dir_name = type_dir.name
            should_strip = dir_name in STRIP_VISUAL_CRS
            if not should_strip and dir_name.startswith("0x"):
                try:
                    if int(dir_name[2:], 16) in strip_hashes:
                        should_strip = True
                except ValueError:
                    pass
            # Keep unknown hash-named resources — they may contain gun meshes/textures
            if should_strip:
                combat_file.unlink()
                stripped += 1
    log.info("    Stripped %d visual/audio/scene resources from combat sub-level", stripped)


def _get_combat_resource(build_mf: Path, type_name: str) -> bytes:
    """Get combat level resource from build or clean extract."""
    data = read_resource(build_mf, type_name, COMBAT_LEVEL)
    if data is not None:
        return data
    # Fall back to clean extract
    for mf in CLEAN_EXTRACT_DIR.iterdir():
        if not mf.is_dir():
            continue
        td = find_type_dir(mf, type_name)
        if td:
            f = td / f"{COMBAT_LEVEL}.bin"
            if f.is_file():
                return f.read_bytes()
    raise FileNotFoundError(f"Combat resource not found: {type_name}/{COMBAT_LEVEL}")


def _remap_combat_hashes(data: bytes, orig_size: int,
                         hash_pairs: list[tuple[bytes, bytes]]) -> bytes:
    """Remap old combat hashes to new hashes in the appended portion of a merged CR.

    Only patches bytes AFTER orig_size to avoid corrupting arena entries.
    """
    if len(data) <= orig_size:
        return data
    new_portion = bytearray(data[orig_size:])
    for old_le, new_le in hash_pairs:
        new_portion = new_portion.replace(old_le, new_le)
    return data[:orig_size] + bytes(new_portion)


def _get_combat_resource_optional(build_mf: Path, type_name: str) -> bytes | None:
    """Get combat level resource, return None if not found."""
    try:
        return _get_combat_resource(build_mf, type_name)
    except FileNotFoundError:
        return None


def _merge_model_cr(build_mf: Path, combat_hashes: set[int]):
    """Merge CModelCR entries from combat into arena.

    CModelCR layout:
      [360B header: 6 descriptors at 0x00,0x38,0x78,0xB0,0xF0,0x130
       + 3 u64 scalars at 0x70,0xE8,0x128]
      [base entries 24B × count]
      [bf0 8B × bf0_count]
      [scene set 264B × count flat + per-entry inline]
      [bf1 8B × bf1_count]
      [bf2 8B × bf2_count]
      [model entries 296B × count flat + per-entry inline]
    """
    arena_data = read_resource(build_mf, "CModelCRWin10", TARGET_LEVEL)
    combat_data = _get_combat_resource(build_mf, "CModelCRWin10")

    def parse_model_cr(data):
        """Parse CModelCR sections, returns dict with all components."""
        count = struct.unpack_from('<Q', data, 0x30)[0]
        bf0_count = struct.unpack_from('<Q', data, 0x38 + 0x30)[0]
        bf1_count = struct.unpack_from('<Q', data, 0xB0 + 0x30)[0]
        bf2_count = struct.unpack_from('<Q', data, 0xF0 + 0x30)[0]

        pos = 360
        base = data[pos:pos + count * 24]; pos += count * 24
        bf0 = data[pos:pos + bf0_count * 8]; pos += bf0_count * 8

        # Scene set flat entries
        ss_flat = data[pos:pos + count * 264]
        ss_flat_start = pos
        pos += count * 264

        # Scene set inline
        ss_inline_pos = []
        for i in range(count):
            start = pos
            eoff = ss_flat_start + i * 264
            for doff in [40, 96, 152, 208]:
                dbs = struct.unpack_from('<Q', data, eoff + doff + 8)[0]
                cnt = struct.unpack_from('<Q', data, eoff + doff + 48)[0]
                if dbs > 0 and cnt > 0:
                    pos += dbs
                    if doff == 208:
                        sub_start = pos - dbs
                        for j in range(cnt):
                            for sdo in [8, 64]:
                                abs_off = sub_start + j * 120 + sdo
                                if abs_off + 56 <= len(data):
                                    sdbs = struct.unpack_from('<Q', data, abs_off + 8)[0]
                                    if sdbs > 0:
                                        pos += sdbs
            ss_inline_pos.append((start, pos))

        bf1 = data[pos:pos + bf1_count * 8]; pos += bf1_count * 8
        bf2 = data[pos:pos + bf2_count * 8]; pos += bf2_count * 8

        # Model flat entries
        m_flat = data[pos:pos + count * 296]
        m_flat_start = pos
        pos += count * 296

        # Model inline
        m_inline_pos = []
        for i in range(count):
            start = pos
            eoff = m_flat_start + i * 296
            for doff in [72, 128, 184, 240]:
                dbs = struct.unpack_from('<Q', data, eoff + doff + 8)[0]
                cnt = struct.unpack_from('<Q', data, eoff + doff + 48)[0]
                if dbs > 0 and cnt > 0:
                    pos += dbs
                    if doff == 240:
                        sub_start = pos - dbs
                        for j in range(cnt):
                            for sdo in [8, 64]:
                                abs_off = sub_start + j * 120 + sdo
                                if abs_off + 56 <= len(data):
                                    sdbs = struct.unpack_from('<Q', data, abs_off + 8)[0]
                                    if sdbs > 0:
                                        pos += sdbs
            m_inline_pos.append((start, pos))

        tail = data[pos:]
        return {
            'header': data[:360], 'count': count,
            'base': base, 'bf0': bf0, 'bf0_count': bf0_count,
            'ss_flat': ss_flat, 'ss_inline_pos': ss_inline_pos,
            'bf1': bf1, 'bf1_count': bf1_count,
            'bf2': bf2, 'bf2_count': bf2_count,
            'm_flat': m_flat, 'm_inline_pos': m_inline_pos,
            'tail': tail, 'raw': data,
        }

    a = parse_model_cr(arena_data)
    c = parse_model_cr(combat_data)

    # Find combat entries to transplant
    c_indices = []
    for i in range(c['count']):
        ahash = struct.unpack_from('<Q', c['base'], i * 24 + 8)[0]
        if ahash in combat_hashes:
            c_indices.append(i)

    if not c_indices:
        log.info("    No combat model entries to merge")
        return

    new_count = a['count'] + len(c_indices)
    log.info("    CModelCR: merging %d combat entries into %d arena entries -> %d",
             len(c_indices), a['count'], new_count)

    # Build merged header
    hdr = bytearray(a['header'])
    def upd(off, cnt, esz):
        struct.pack_into('<Q', hdr, off + 8, cnt * esz)
        struct.pack_into('<Q', hdr, off + 40, cnt)
        struct.pack_into('<Q', hdr, off + 48, cnt)

    upd(0x00, new_count, 24)       # base
    upd(0x78, new_count, 264)      # scene set
    upd(0x130, new_count, 296)     # model
    struct.pack_into('<Q', hdr, 0x70, new_count)   # scalar
    struct.pack_into('<Q', hdr, 0xE8, new_count)
    struct.pack_into('<Q', hdr, 0x128, new_count)

    result = bytearray(hdr)

    # Base entries
    result.extend(a['base'])
    for ci in c_indices:
        entry = bytearray(c['base'][ci * 24:(ci + 1) * 24])
        struct.pack_into('<H', entry, 16, 0xFFFF)
        result.extend(entry)

    # bf0 (keep arena's)
    result.extend(a['bf0'])

    # Scene set flat
    result.extend(a['ss_flat'])
    for ci in c_indices:
        result.extend(c['ss_flat'][ci * 264:(ci + 1) * 264])

    # Scene set inline: arena then combat
    for i in range(a['count']):
        s, e = a['ss_inline_pos'][i]
        result.extend(arena_data[s:e])
    for ci in c_indices:
        s, e = c['ss_inline_pos'][ci]
        result.extend(combat_data[s:e])

    # bf1 + bf2
    result.extend(a['bf1'])
    result.extend(a['bf2'])

    # Model flat
    result.extend(a['m_flat'])
    for ci in c_indices:
        result.extend(c['m_flat'][ci * 296:(ci + 1) * 296])

    # Model inline: arena then combat
    for i in range(a['count']):
        s, e = a['m_inline_pos'][i]
        result.extend(arena_data[s:e])
    for ci in c_indices:
        s, e = c['m_inline_pos'][ci]
        result.extend(combat_data[s:e])

    # Trailing
    result.extend(a['tail'])

    log.info("    Size: %d -> %d", len(arena_data), len(result))
    write_resource(build_mf, "CModelCRWin10", TARGET_LEVEL, bytes(result))


def _merge_animation_cr(build_mf: Path, combat_hashes: set[int]):
    """Merge CAnimationCR entries.

    CAnimationCR layout:
      [56B desc1: base 24B] [56B desc2: bf 8B] [8B parentCount]
      [56B desc3: arr3 176B] [56B desc4: arr4 8B] [56B desc5: arr5 216B]
      --- header = 288B ---
      [base 24B × count]  (actor_hash at +8 is type_hash, search key)
      [bf 8B × bf_count]
      [arr3 176B × count]
      [arr4 8B × count]
      [arr5 216B × count]
    """
    arena_data = read_resource(build_mf, "CAnimationCRWin10", TARGET_LEVEL)
    combat_data = _get_combat_resource(build_mf, "CAnimationCRWin10")

    HEADER_SIZE = 288
    DESC_OFFSETS = [0, 56, 120, 176, 232]
    ENTRY_SIZES = [24, 8, 176, 8, 216]

    # Arr3 (176B) has inline descriptors at +48 and +104
    # Arr5 (216B) has inline descriptors at +80 and +136
    ARR3_DESC_OFFSETS = [48, 104]
    ARR5_DESC_OFFSETS = [80, 136]

    def compute_inline(data, flat_start, count, entry_size, desc_offsets):
        """Compute per-entry inline data positions for a compound array."""
        flat_end = flat_start + count * entry_size
        positions = []
        pos = flat_end
        for i in range(count):
            start = pos
            eoff = flat_start + i * entry_size
            for doff in desc_offsets:
                abs_off = eoff + doff
                if abs_off + 56 > flat_end:
                    continue
                dbs = struct.unpack_from('<Q', data, abs_off + 8)[0]
                cap = struct.unpack_from('<Q', data, abs_off + 40)[0]
                cnt = struct.unpack_from('<Q', data, abs_off + 48)[0]
                if cnt > 0 and cap > 0:
                    esize = dbs // cap
                    pos += cnt * esize
            positions.append((start, pos))
        return positions, pos  # positions list + end offset

    def parse_anim(data):
        count = struct.unpack_from('<Q', data, DESC_OFFSETS[0] + 48)[0]
        bf_count = struct.unpack_from('<Q', data, DESC_OFFSETS[1] + 48)[0]
        pos = HEADER_SIZE
        base = data[pos:pos + count * 24]; pos += count * 24
        bf = data[pos:pos + bf_count * 8]; pos += bf_count * 8

        arr3_flat_start = pos
        arr3_flat = data[pos:pos + count * 176]; pos += count * 176
        arr3_inline_pos, pos = compute_inline(data, arr3_flat_start, count, 176, ARR3_DESC_OFFSETS)

        arr4 = data[pos:pos + count * 8]; pos += count * 8

        arr5_flat_start = pos
        arr5_flat = data[pos:pos + count * 216]; pos += count * 216
        arr5_inline_pos, pos = compute_inline(data, arr5_flat_start, count, 216, ARR5_DESC_OFFSETS)

        tail = data[pos:]
        return {'count': count, 'bf_count': bf_count,
                'base': base, 'bf': bf,
                'arr3_flat': arr3_flat, 'arr3_inline_pos': arr3_inline_pos,
                'arr3_flat_start': arr3_flat_start,
                'arr4': arr4,
                'arr5_flat': arr5_flat, 'arr5_inline_pos': arr5_inline_pos,
                'arr5_flat_start': arr5_flat_start,
                'tail': tail, 'raw': data}

    a = parse_anim(arena_data)
    c = parse_anim(combat_data)

    # Find combat entries (actor_hash at base+8 is the type_hash / search key)
    c_indices = []
    for i in range(c['count']):
        ahash = struct.unpack_from('<Q', c['base'], i * 24 + 8)[0]
        if ahash in combat_hashes:
            c_indices.append(i)

    if not c_indices:
        log.info("    No combat animation entries to merge")
        return

    new_count = a['count'] + len(c_indices)
    log.info("    CAnimationCR: %d + %d -> %d", a['count'], len(c_indices), new_count)

    # Bitfield must be ceil(new_count/64) qwords
    import math
    new_bf_count = math.ceil(new_count / 64)

    # Build header
    hdr = bytearray(arena_data[:HEADER_SIZE])
    def upd(off, cnt, esz):
        struct.pack_into('<Q', hdr, off + 8, cnt * esz)
        struct.pack_into('<Q', hdr, off + 40, cnt)
        struct.pack_into('<Q', hdr, off + 48, cnt)

    upd(DESC_OFFSETS[0], new_count, 24)
    upd(DESC_OFFSETS[1], new_bf_count, 8)  # bitfield expanded
    struct.pack_into('<Q', hdr, 112, new_count)  # parentCount
    upd(DESC_OFFSETS[2], new_count, 176)
    upd(DESC_OFFSETS[3], new_count, 8)
    upd(DESC_OFFSETS[4], new_count, 216)

    result = bytearray(hdr)
    # base
    result.extend(a['base'])
    for ci in c_indices:
        result.extend(c['base'][ci * 24:(ci + 1) * 24])
    # bf — expand to new_bf_count qwords, merging arena + combat bits
    merged_bf = bytearray(new_bf_count * 8)
    merged_bf[:len(a['bf'])] = a['bf']
    for seq, ci in enumerate(c_indices):
        new_idx = a['count'] + seq
        old_qw = ci // 64
        old_bit = ci % 64
        new_qw = new_idx // 64
        new_bit = new_idx % 64
        if old_qw * 8 + 8 <= len(c['bf']):
            val = struct.unpack_from('<Q', c['bf'], old_qw * 8)[0]
            if (val >> old_bit) & 1:
                cur = struct.unpack_from('<Q', merged_bf, new_qw * 8)[0]
                struct.pack_into('<Q', merged_bf, new_qw * 8, cur | (1 << new_bit))
    result.extend(merged_bf)
    # arr3 flat + inline (compound: descriptors at +48, +104 within 176B entries)
    result.extend(a['arr3_flat'])
    for ci in c_indices:
        result.extend(c['arr3_flat'][ci * 176:(ci + 1) * 176])
    # arr3 inline: arena entries then combat entries
    for i in range(a['count']):
        s, e = a['arr3_inline_pos'][i]
        result.extend(a['raw'][s:e])
    for ci in c_indices:
        s, e = c['arr3_inline_pos'][ci]
        result.extend(c['raw'][s:e])
    # arr4
    result.extend(a['arr4'])
    for ci in c_indices:
        result.extend(c['arr4'][ci * 8:(ci + 1) * 8])
    # arr5 flat + inline (compound: descriptors at +80, +136 within 216B entries)
    result.extend(a['arr5_flat'])
    for ci in c_indices:
        result.extend(c['arr5_flat'][ci * 216:(ci + 1) * 216])
    # arr5 inline: arena entries then combat entries
    for i in range(a['count']):
        s, e = a['arr5_inline_pos'][i]
        result.extend(a['raw'][s:e])
    for ci in c_indices:
        s, e = c['arr5_inline_pos'][ci]
        result.extend(c['raw'][s:e])
    result.extend(a['tail'])

    log.info("    Size: %d -> %d", len(arena_data), len(result))
    write_resource(build_mf, "CAnimationCRWin10", TARGET_LEVEL, bytes(result))


def _merge_multi_array_cr(arena_data: bytes, combat_data: bytes,
                          combat_hashes: set[int],
                          header_size: int, num_descs: int,
                          cr_name: str) -> bytes | None:
    """Generic multi-array CR merge.

    Multi-array CRs have a fixed header of N descriptors (+ optional scalars),
    followed by N data arrays in sequence with alignment.

    Strategy: for each array, if it's per-actor (count == desc0.count), find
    and extract combat entries at matching positions, then append them.
    """
    if len(arena_data) < header_size or len(combat_data) < header_size:
        return None

    # Parse descriptors from both files
    # Known scalar positions for specific CR types (offset within header where
    # a u64 scalar sits between descriptors instead of another descriptor)
    KNOWN_SCALAR_OFFSETS = {
        568: [0x070],   # CInstanceModelCR: scalar at 0x070 between desc 1 and desc 2
        288: [112],     # CAnimationCR: scalar at 112 between desc 1 and desc 2
    }
    scalar_offsets = set(KNOWN_SCALAR_OFFSETS.get(header_size, []))

    def parse_descs(data, hdr_size):
        descs = []
        pos = 0
        while pos + 56 <= hdr_size:
            if pos in scalar_offsets:
                # Known scalar position — skip 8 bytes
                pos += 8
                continue
            dbs = struct.unpack_from('<Q', data, pos + 8)[0]
            cap = struct.unpack_from('<Q', data, pos + 40)[0]
            count = struct.unpack_from('<Q', data, pos + 48)[0]
            descs.append({'offset': pos, 'dbs': dbs, 'cap': cap, 'count': count})
            pos += 56
        return descs

    a_descs = parse_descs(arena_data, header_size)
    c_descs = parse_descs(combat_data, header_size)

    if not a_descs or not c_descs:
        return None

    # Primary count = first descriptor count
    a_primary = a_descs[0]['count']
    c_primary = c_descs[0]['count']

    if c_primary == 0:
        return None

    # Find actor hash field: at +8 in the first entry (24B entries typically)
    c_esize0 = c_descs[0]['dbs'] // c_descs[0]['cap'] if c_descs[0]['cap'] > 0 else 0
    if c_esize0 == 0:
        return None

    # Locate first array data start (after header)
    # Align to entry size
    def align(pos, alignment):
        alignment = min(alignment, 8)
        if alignment <= 1:
            return pos
        return (pos + alignment - 1) & ~(alignment - 1)

    # Compute data positions for each array
    def compute_array_positions(data, descs, hdr_size):
        positions = []
        pos = hdr_size
        for d in descs:
            if d['count'] == 0 or d['cap'] == 0:
                positions.append((pos, pos, 0, 0))
                continue
            esize = d['dbs'] // d['cap']
            data_size = d['count'] * esize
            aligned_pos = align(pos, esize)
            positions.append((aligned_pos, aligned_pos + data_size, esize, d['count']))
            pos = aligned_pos + data_size
        return positions

    a_positions = compute_array_positions(arena_data, a_descs, header_size)
    c_positions = compute_array_positions(combat_data, c_descs, header_size)

    # Find which combat entries (in array 0) match our actor hashes
    c_arr0_start, c_arr0_end, c_arr0_esize, c_arr0_count = c_positions[0]
    matching_indices = []
    for i in range(c_arr0_count):
        off = c_arr0_start + i * c_arr0_esize
        actor_hash = struct.unpack_from('<Q', combat_data, off + 8)[0]
        if actor_hash in combat_hashes:
            matching_indices.append(i)

    if not matching_indices:
        return None

    log.info("    %s: merging %d combat entries into arena (%d existing)",
             cr_name, len(matching_indices), a_primary)

    # Build merged arrays
    new_header = bytearray(arena_data[:header_size])
    merged_arrays = bytearray()

    for arr_idx, (a_pos_tuple, c_pos_tuple) in enumerate(zip(a_positions, c_positions)):
        a_start, a_end, a_esize, a_count = a_pos_tuple
        c_start, c_end, c_esize, c_count = c_pos_tuple

        is_per_actor = (c_count == c_primary and a_count == a_primary)

        if is_per_actor and a_esize == c_esize and a_esize > 0:
            # Align
            if a_esize > 0:
                padding = align(len(new_header) + len(merged_arrays), a_esize) - (len(new_header) + len(merged_arrays))
                merged_arrays.extend(b'\x00' * padding)

            # Copy arena entries
            merged_arrays.extend(arena_data[a_start:a_end])

            # Append matching combat entries
            for ci in matching_indices:
                off = c_start + ci * c_esize
                entry = bytearray(combat_data[off:off + c_esize])
                # Reset node_index if present (offset +16, u16)
                if c_esize >= 20:
                    struct.pack_into('<H', entry, 16, 0xFFFF)
                merged_arrays.extend(entry)

            new_count = a_count + len(matching_indices)
            # Update descriptor in header
            desc_off = a_descs[arr_idx]['offset']
            struct.pack_into('<Q', new_header, desc_off + 8, new_count * a_esize)
            struct.pack_into('<Q', new_header, desc_off + 40, new_count)
            struct.pack_into('<Q', new_header, desc_off + 48, new_count)
        else:
            # Non-per-actor array or size mismatch: keep arena data as-is
            if a_esize > 0:
                padding = align(len(new_header) + len(merged_arrays), a_esize) - (len(new_header) + len(merged_arrays))
                merged_arrays.extend(b'\x00' * padding)
            merged_arrays.extend(arena_data[a_start:a_end])

    # Handle any scalars in the header (like u64 at specific offsets)
    # The u64 scalar at 0x070 in CInstanceModelCR / 0x70 in CAnimationCR
    # represents per-actor count. Update if present.
    # For CAnimationCR: scalar at offset 112 (between desc 2 and desc 3)
    # For CInstanceModelCR: scalar at offset 0x070

    result = bytes(new_header) + bytes(merged_arrays)
    return result


def _merge_syncgrab_cr(build_mf: Path, combat_hashes: set[int]):
    """Merge CR15SyncGrabCR."""
    arena_data = read_resource(build_mf, "CR15SyncGrabCRWin10", TARGET_LEVEL)
    combat_data = _get_combat_resource_optional(build_mf, "CR15SyncGrabCRWin10")
    if combat_data is None:
        log.info("    No combat SyncGrab data")
        return

    # SyncGrab is complex multi-array. For now, do a simple entry-level merge
    # using the same multi-array approach.
    result = _merge_multi_array_cr(arena_data, combat_data, combat_hashes,
                                   header_size=288, num_descs=5,
                                   cr_name="CR15SyncGrabCR")
    if result and len(result) > len(arena_data):
        write_resource(build_mf, "CR15SyncGrabCRWin10", TARGET_LEVEL, result)


def _merge_instancemodel_cr(build_mf: Path, combat_hashes: set[int]):
    """Merge CInstanceModelCR."""
    arena_data = read_resource(build_mf, "CInstanceModelCRWin10", TARGET_LEVEL)
    combat_data = _get_combat_resource_optional(build_mf, "CInstanceModelCRWin10")
    if combat_data is None or arena_data is None:
        log.info("    No instance model data to merge")
        return

    result = _merge_multi_array_cr(arena_data, combat_data, combat_hashes,
                                   header_size=568, num_descs=10,
                                   cr_name="CInstanceModelCR")
    if result and len(result) > len(arena_data):
        write_resource(build_mf, "CInstanceModelCRWin10", TARGET_LEVEL, result)


def _merge_or_create_compound_cr(build_mf: Path, cr_type: str,
                                  combat_hashes: set[int]):
    """Merge a compound CR type, or create it if arena doesn't have it."""
    arena_data = read_resource(build_mf, cr_type, TARGET_LEVEL)
    combat_data = _get_combat_resource_optional(build_mf, cr_type)

    if combat_data is None:
        log.info("    No combat data for %s", cr_type)
        return

    if arena_data is not None:
        result = merge_compound_cr(arena_data, combat_data, combat_hashes)
        if len(result) > len(arena_data):
            write_resource(build_mf, cr_type, TARGET_LEVEL, result)
            log.info("    Merged: %d -> %d bytes", len(arena_data), len(result))
        else:
            log.info("    No matching combat entries")
    else:
        # Arena doesn't have this CR type — create from combat data
        result = create_compound_cr(combat_data, combat_hashes)
        if len(result) > 56:
            write_resource(build_mf, cr_type, TARGET_LEVEL, result)
            log.info("    Created new: %d bytes", len(result))


def _merge_remaining_crs(build_mf: Path, combat_hashes: set[int]):
    """Scan all CR types in the combat level and merge/create as needed."""
    # Find all type directories that have combat level data
    combat_cr_dirs = set()
    for mf in [build_mf] + [d for d in CLEAN_EXTRACT_DIR.iterdir() if d.is_dir()]:
        for td in mf.iterdir():
            if not td.is_dir():
                continue
            if (td / f"{COMBAT_LEVEL}.bin").is_file():
                combat_cr_dirs.add(td.name)

    # Also check the build manifest
    for td in build_mf.iterdir():
        if not td.is_dir():
            continue
        if (td / f"{COMBAT_LEVEL}.bin").is_file():
            combat_cr_dirs.add(td.name)

    processed = 0
    skipped = 0

    for dir_name in sorted(combat_cr_dirs):
        # Skip types handled by dedicated mergers
        if dir_name in DEDICATED_CR_TYPES:
            continue
        # Check hash-based names
        skip = False
        for ded in DEDICATED_CR_TYPES:
            if dir_name == f"0x{rad_hash(ded):016X}":
                skip = True
                break
        if skip:
            continue

        # Get combat data
        combat_data = None
        for mf in [build_mf] + [d for d in CLEAN_EXTRACT_DIR.iterdir() if d.is_dir()]:
            td = mf / dir_name
            f = td / f"{COMBAT_LEVEL}.bin"
            if f.is_file():
                combat_data = f.read_bytes()
                break

        if combat_data is None or len(combat_data) < 56:
            continue

        # Check if any combat actor hash is in this file
        has_match = False
        for h in combat_hashes:
            if struct.pack('<Q', h) in combat_data:
                has_match = True
                break

        if not has_match:
            continue

        # Determine format
        c_dbs = struct.unpack_from('<Q', combat_data, 8)[0]
        c_cap = struct.unpack_from('<Q', combat_data, 40)[0]
        c_count = struct.unpack_from('<Q', combat_data, 48)[0]
        if c_cap == 0 or c_count == 0:
            continue
        c_esize = c_dbs // c_cap
        c_expected_end = 56 + c_count * c_esize
        is_simple = (c_expected_end == len(combat_data))
        is_compound = (c_expected_end < len(combat_data))

        # Get arena data
        arena_data = None
        td = find_type_dir(build_mf, dir_name)
        if td:
            af = td / f"{TARGET_LEVEL}.bin"
            if af.is_file():
                arena_data = af.read_bytes()

        if is_simple:
            if arena_data is not None:
                result = merge_simple_cr(arena_data, combat_data, combat_hashes)
                if len(result) > len(arena_data):
                    td = find_type_dir(build_mf, dir_name) or (build_mf / dir_name)
                    (td / f"{TARGET_LEVEL}.bin").write_bytes(result)
                    processed += 1
                    log.info("    %s: merged (simple), %d -> %d", dir_name,
                             len(arena_data), len(result))
            else:
                result = create_simple_cr(combat_data, combat_hashes)
                if len(result) > 56:
                    td = build_mf / dir_name
                    td.mkdir(exist_ok=True)
                    (td / f"{TARGET_LEVEL}.bin").write_bytes(result)
                    processed += 1
                    log.info("    %s: created (simple), %d bytes", dir_name, len(result))
        elif is_compound:
            if arena_data is not None:
                result = merge_compound_cr(arena_data, combat_data, combat_hashes)
                if len(result) > len(arena_data):
                    td = find_type_dir(build_mf, dir_name) or (build_mf / dir_name)
                    (td / f"{TARGET_LEVEL}.bin").write_bytes(result)
                    processed += 1
                    log.info("    %s: merged (compound), %d -> %d", dir_name,
                             len(arena_data), len(result))
            else:
                result = create_compound_cr(combat_data, combat_hashes)
                if len(result) > 56:
                    td = build_mf / dir_name
                    td.mkdir(exist_ok=True)
                    (td / f"{TARGET_LEVEL}.bin").write_bytes(result)
                    processed += 1
                    log.info("    %s: created (compound), %d bytes", dir_name, len(result))
        else:
            skipped += 1

    log.info("    Processed %d remaining CR types (%d skipped)", processed, skipped)


def _merge_archive_resource(build_mf: Path):
    """Merge CArchiveResource: add combat resource pairs to arena."""
    car_dir = find_type_dir(build_mf, "carchiveresourceWin10")
    if car_dir is None:
        car_dir = find_type_dir(build_mf, "CArchiveResourceWin10")
    if car_dir is None:
        log.warning("    CArchiveResource directory not found")
        return

    arena_car = car_dir / f"{TARGET_LEVEL}.bin"
    if not arena_car.is_file():
        log.warning("    Target CArchiveResource not found")
        return

    # Get combat archive resource
    combat_car_data = None
    for mf in [build_mf] + [d for d in CLEAN_EXTRACT_DIR.iterdir() if d.is_dir()]:
        td = find_type_dir(mf, "carchiveresourceWin10")
        if td is None:
            td = find_type_dir(mf, "CArchiveResourceWin10")
        if td:
            f = td / f"{COMBAT_LEVEL}.bin"
            if f.is_file():
                combat_car_data = f.read_bytes()
                break

    if combat_car_data is None:
        log.warning("    Combat CArchiveResource not found")
        return

    arena_data = bytearray(arena_car.read_bytes())

    # Parse arena pairs
    a_flat = struct.unpack_from('<I', arena_data, 0)[0]
    a_pair_off = 4 + a_flat * 8
    a_pair_count = struct.unpack_from('<I', arena_data, a_pair_off)[0]
    a_pair_data_off = a_pair_off + 4
    a_pair_data_end = a_pair_data_off + a_pair_count * 16

    arena_pairs = set()
    for i in range(a_pair_count):
        off = a_pair_data_off + i * 16
        th = struct.unpack_from('<Q', arena_data, off)[0]
        rh = struct.unpack_from('<Q', arena_data, off + 8)[0]
        arena_pairs.add((th, rh))

    # Parse combat pairs
    c_flat = struct.unpack_from('<I', combat_car_data, 0)[0]
    c_pair_off = 4 + c_flat * 8
    c_pair_count = struct.unpack_from('<I', combat_car_data, c_pair_off)[0]
    c_pair_data_off = c_pair_off + 4

    # Build set of type hashes that actually have a target level file in the build
    existing_target_types = set()
    for td in build_mf.iterdir():
        if not td.is_dir():
            continue
        if (td / f"{TARGET_LEVEL}.bin").is_file():
            # Map directory name to type hash
            if td.name.startswith("0x"):
                try:
                    existing_target_types.add(int(td.name[2:], 16))
                except ValueError:
                    pass
            else:
                existing_target_types.add(rad_hash(td.name))

    # Find combat pairs not in arena — only add if resource actually exists
    new_pairs = bytearray()
    new_count = 0
    skipped_missing = 0
    for i in range(c_pair_count):
        off = c_pair_data_off + i * 16
        th = struct.unpack_from('<Q', combat_car_data, off)[0]
        rh = struct.unpack_from('<Q', combat_car_data, off + 8)[0]
        # Replace combat level hash with target level hash in resource names
        if rh == COMBAT_LEVEL_HASH:
            rh = TARGET_LEVEL_HASH
            # Only add if we actually have this type's file in the build
            if th not in existing_target_types:
                skipped_missing += 1
                continue
        if (th, rh) not in arena_pairs:
            new_pairs += struct.pack('<QQ', th, rh)
            new_count += 1
            arena_pairs.add((th, rh))
    if skipped_missing:
        log.info("    Skipped %d pairs for missing target-level resources", skipped_missing)

    if new_count == 0:
        log.info("    No new resource pairs to add")
        return

    # Insert new pairs
    struct.pack_into('<I', arena_data, a_pair_off, a_pair_count + new_count)
    arena_data[a_pair_data_end:a_pair_data_end] = new_pairs
    arena_car.write_bytes(bytes(arena_data))
    log.info("    Added %d new resource pairs (%d total)",
             new_count, a_pair_count + new_count)


def _copy_combat_model_resources(build_mf: Path):
    """Copy model sub-resources (scenes, physics, meshes, etc.) from combat level."""
    # Parse combat CArchiveResource to find model resource hashes
    combat_car_data = None
    for mf in [build_mf] + [d for d in CLEAN_EXTRACT_DIR.iterdir() if d.is_dir()]:
        td = find_type_dir(mf, "carchiveresourceWin10")
        if td is None:
            td = find_type_dir(mf, "CArchiveResourceWin10")
        if td:
            f = td / f"{COMBAT_LEVEL}.bin"
            if f.is_file():
                combat_car_data = f.read_bytes()
                break

    if combat_car_data is None:
        return

    c_flat = struct.unpack_from('<I', combat_car_data, 0)[0]
    c_pair_off = 4 + c_flat * 8
    c_pair_count = struct.unpack_from('<I', combat_car_data, c_pair_off)[0]
    c_pair_data_off = c_pair_off + 4

    # Collect all non-level resource hashes from combat
    combat_resource_hashes = set()
    for i in range(c_pair_count):
        off = c_pair_data_off + i * 16
        rh = struct.unpack_from('<Q', combat_car_data, off + 8)[0]
        if rh != COMBAT_LEVEL_HASH:
            combat_resource_hashes.add(rh)

    # Find which of these don't already exist in build
    copied = 0
    for mf in CLEAN_EXTRACT_DIR.iterdir():
        if not mf.is_dir():
            continue
        for td in mf.iterdir():
            if not td.is_dir():
                continue
            for f in td.iterdir():
                if not f.is_file() or not f.name.startswith("0x"):
                    continue
                try:
                    fhash = int(f.stem, 16)
                except ValueError:
                    continue
                if fhash not in combat_resource_hashes:
                    continue
                # Copy to build if not already there
                build_td = build_mf / td.name
                if not build_td.is_dir():
                    build_td.mkdir(exist_ok=True)
                dst = build_td / f.name
                if not dst.exists():
                    shutil.copy2(str(f), str(dst))
                    copied += 1

    log.info("    Copied %d combat model sub-resources to build", copied)


# ---------------------------------------------------------------------------
# Phase 4: Repack
# ---------------------------------------------------------------------------

def phase4_repack():
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 4: Repack")
    log.info("=" * 60)

    if OUTPUT_DIR.exists():
        log.info("  Removing existing output directory...")
        shutil.rmtree(OUTPUT_DIR)

    run_tool(["repack", str(BUILD_DIR), str(OUTPUT_DIR)])
    log.info("  Repacked to %s", OUTPUT_DIR)


# ---------------------------------------------------------------------------
# Phase 5: Update hash_lookup.json
# ---------------------------------------------------------------------------

def phase5_update_hash_lookup():
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 5: Update hash_lookup.json")
    log.info("=" * 60)

    hash_key = f"0x{TARGET_LEVEL_HASH:016X}"

    if HASH_LOOKUP_PATH.is_file():
        with open(HASH_LOOKUP_PATH, "r") as f:
            lookup = json.load(f)
    else:
        lookup = {}

    if hash_key in lookup:
        log.info("  %s already present.", hash_key)
    else:
        lookup[hash_key] = TARGET_LEVEL
        with open(HASH_LOOKUP_PATH, "w") as f:
            json.dump(lookup, f, indent=2, sort_keys=True)
        log.info("  Added %s: '%s'", hash_key, TARGET_LEVEL)


# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

def cleanup():
    log.info("")
    log.info("=" * 60)
    log.info("Cleanup")
    log.info("=" * 60)
    if BUILD_DIR.exists():
        log.info("  Removing build directory...")
        shutil.rmtree(BUILD_DIR)
        log.info("  Done.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    log.info("=" * 60)
    log.info("  Create mpl_arenacombat: Arena + Combat Guns")
    log.info("=" * 60)

    phase0_extract()
    phase1_copy_to_build()
    build_mf = phase2_clone_level()
    phase3_transplant(build_mf)
    phase4_repack()
    phase5_update_hash_lookup()
    cleanup()

    log.info("")
    log.info("=" * 60)
    log.info("  DONE! Output at: %s", OUTPUT_DIR)
    log.info("=" * 60)


if __name__ == "__main__":
    main()
