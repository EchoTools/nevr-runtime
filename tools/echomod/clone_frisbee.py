#!/usr/bin/env python3
"""
Clone mpl_arena_a -> mpl_arena_b
Uses rad_archive_tool.py for extract/repack (known-good archive handling).
"""

import sys
import os
import struct
import shutil
import subprocess
import json
import logging
from pathlib import Path

# Add parent to path for echomod imports
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
BUILD_DIR = SCRIPT_DIR / "echovr_build"
OUTPUT_DIR = SCRIPT_DIR / "patched_output"
HASH_LOOKUP_PATH = SCRIPT_DIR / "hash_lookup.json"

SOURCE_LEVEL = "mpl_arena_a"
CLONED_LEVEL = "mpl_arena_b"

SOURCE_ACTOR_HASH = 0x91A4A5864D973E76  # game_frisbee
NEW_ACTOR_NAME = "game_frisbee_clone"
NEW_POSITION = (1.0, 0.0, 0.0)
SOURCE_POSITION = (-1.0, 0.0, 0.0)  # reposition original frisbee

# Child actor (bubble/shield) — parented to frisbee
CHILD_ACTOR_HASH = 0x03147FD8F518E997  # frisbee child at index 121
CHILD_CLONE_NAME = "game_frisbee_clone_child_0"  # hash must have bit63=1 for sorted table lookup

# Child CR types to skip (handled elsewhere or not needed for child)
CHILD_SKIP_TYPES = {
    "CActorDataResourceWin10",
    "CTransformCRWin10",
    "CGSceneResourceWin10",
    "CInstanceModelCRWin10",  # 10-array format, handled by dedicated cloner
}

# Diagnostic flags
SKIP_FRISBEE_CLONE = False
BUBBLE_ONLY_TEST = False

# Level hashes (little-endian 8-byte)
SOURCE_LEVEL_HASH = 0x576ED3F8428EBC4B  # rad_hash("mpl_arena_a")
CLONED_LEVEL_HASH = 0x576ED3F8428EBC48  # rad_hash("mpl_arena_b")

SOURCE_LEVEL_HASH_LE = struct.pack("<Q", SOURCE_LEVEL_HASH)
CLONED_LEVEL_HASH_LE = struct.pack("<Q", CLONED_LEVEL_HASH)

# Model duplication: give clone its own model resources
ORIG_MODEL_HASHES = [0xE988F213C7363E88, 0x34918B2B464D6B58]
NEW_MODEL_HASHES = [rad_hash("prp_frisbee_clone_model_a"),
                    rad_hash("prp_frisbee_clone_model_b")]
MODEL_HASH_MAP = dict(zip(ORIG_MODEL_HASHES, NEW_MODEL_HASHES))

# Model sub-resource types (7 named + 2 hash-only)
MODEL_SUB_TYPES = [
    "CGSceneResourceWin10",
    "CPhysicsResourceWin10",
    "CGTextureStreamingResourceWin10",
    "CGLightMapResourceWin10",
    "CSkeletonResourceWin10",
    "CGMeshListResourceWin10",
    "CGVisibilityResourceWin10",
    "0x7B9A4C9FD03F37EB",   # zero-size placeholder
    "0xE642BFB1ABCF76DF",   # GPU buffer
]

# Types whose pairs to add to CArchiveResource (exclude zero-size and GPU)
ARCHIVE_SUB_TYPES = [
    "CGSceneResourceWin10",
    "CPhysicsResourceWin10",
    "CGTextureStreamingResourceWin10",
    "CGLightMapResourceWin10",
    "CSkeletonResourceWin10",
    "CGMeshListResourceWin10",
    "CGVisibilityResourceWin10",
]

# CR types handled by dedicated parsers (skip in generic scan)
DEDICATED_CR_TYPES = {
    "CActorDataResourceWin10",
    "CGSceneResourceWin10",
    "CTransformCRWin10",
    "CModelCRWin10",
    "CAnimationCRWin10",
    "CComponentLODCRWin10",
    "CScriptCRWin10",
    "CParticleEffectCRWin10",
    "CR15SyncGrabCRWin10",
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run_tool(args: list[str]) -> None:
    """Run rad_archive_tool.py as a subprocess."""
    cmd = [sys.executable, str(SCRIPT_DIR / "rad_archive_tool.py")] + args
    log.info("  Running: %s", " ".join(cmd))
    result = subprocess.run(cmd, check=True)
    if result.returncode != 0:
        raise RuntimeError(f"rad_archive_tool.py failed with return code {result.returncode}")


def find_type_dir(build_mf: Path, type_name: str) -> Path | None:
    """Find the type directory by resolved name or hash fallback."""
    # Try resolved name first
    candidate = build_mf / type_name
    if candidate.is_dir():
        return candidate
    # Try hash-based name
    type_hash = rad_hash(type_name)
    candidate = build_mf / f"0x{type_hash:016X}"
    if candidate.is_dir():
        return candidate
    return None


def read_level_resource(build_mf: Path, type_name: str) -> bytes:
    """Read mpl_arena_b.bin from the given type directory."""
    type_dir = find_type_dir(build_mf, type_name)
    if type_dir is None:
        raise FileNotFoundError(f"Type directory not found: {type_name}")
    path = type_dir / f"{CLONED_LEVEL}.bin"
    if not path.is_file():
        raise FileNotFoundError(f"Resource not found: {path}")
    return path.read_bytes()


def write_level_resource(build_mf: Path, type_name: str, data: bytes) -> None:
    """Write mpl_arena_b.bin to the given type directory."""
    type_dir = find_type_dir(build_mf, type_name)
    if type_dir is None:
        raise FileNotFoundError(f"Type directory not found: {type_name}")
    path = type_dir / f"{CLONED_LEVEL}.bin"
    path.write_bytes(data)
    log.info("    Wrote %s (%d bytes)", path.name, len(data))


# ---------------------------------------------------------------------------
# Phase 0: Fresh Extract
# ---------------------------------------------------------------------------

def phase0_extract():
    """Extract original game archive to a clean directory."""
    log.info("=" * 60)
    log.info("PHASE 0: Fresh Extract")
    log.info("=" * 60)

    if not ARCHIVE_DIR.is_dir():
        raise FileNotFoundError(f"Archive directory not found: {ARCHIVE_DIR}")

    if CLEAN_EXTRACT_DIR.exists():
        log.info("  Clean extract already exists at %s, skipping.", CLEAN_EXTRACT_DIR)
        return

    run_tool(["extract", str(ARCHIVE_DIR), str(CLEAN_EXTRACT_DIR)])
    log.info("  Extracted to %s", CLEAN_EXTRACT_DIR)


# ---------------------------------------------------------------------------
# Phase 1: Copy to build directory
# ---------------------------------------------------------------------------

def phase1_copy_to_build():
    """Copy clean extract to build directory."""
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 1: Copy to build directory")
    log.info("=" * 60)

    if BUILD_DIR.exists():
        log.info("  Removing existing build directory...")
        shutil.rmtree(BUILD_DIR)

    log.info("  Copying %s -> %s", CLEAN_EXTRACT_DIR, BUILD_DIR)
    shutil.copytree(str(CLEAN_EXTRACT_DIR), str(BUILD_DIR))
    log.info("  Done.")


# ---------------------------------------------------------------------------
# Phase 2: Clone level (mpl_arena_a -> mpl_arena_b)
# ---------------------------------------------------------------------------

def phase2_clone_level():
    """Clone mpl_arena_a -> mpl_arena_b in all type directories."""
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 2: Clone level (%s -> %s)", SOURCE_LEVEL, CLONED_LEVEL)
    log.info("=" * 60)

    # Find all manifest subdirectories in the build (pick the largest for main ops)
    mf_dirs = sorted(
        [d for d in BUILD_DIR.iterdir() if d.is_dir()],
        key=lambda d: sum(1 for _ in d.iterdir()),
        reverse=True,
    )
    if not mf_dirs:
        raise RuntimeError(f"No manifest directories found in {BUILD_DIR}")
    log.info("  Main manifest: %s (%d entries)", mf_dirs[0].name,
             sum(1 for _ in mf_dirs[0].iterdir()))

    copied_count = 0
    replaced_count = 0

    for mf_dir in mf_dirs:
        # Walk type subdirectories within each manifest dir
        for type_dir in sorted(mf_dir.iterdir()):
            if not type_dir.is_dir() or type_dir.name.startswith("_"):
                continue

            src_file = type_dir / f"{SOURCE_LEVEL}.bin"
            if not src_file.is_file():
                continue

            dst_file = type_dir / f"{CLONED_LEVEL}.bin"
            shutil.copy2(str(src_file), str(dst_file))
            copied_count += 1

    log.info("  Copied %d mpl_arena_a.bin -> mpl_arena_b.bin files", copied_count)

    # Binary search-replace the level hash in all mpl_arena_b.bin files
    log.info("  Replacing level hash 0x%016X -> 0x%016X in mpl_arena_b.bin files...",
             SOURCE_LEVEL_HASH, CLONED_LEVEL_HASH)

    for mf_dir in mf_dirs:
        for type_dir in sorted(mf_dir.iterdir()):
            if not type_dir.is_dir() or type_dir.name.startswith("_"):
                continue

            arena_b = type_dir / f"{CLONED_LEVEL}.bin"
            if not arena_b.is_file():
                continue

            data = arena_b.read_bytes()
            if SOURCE_LEVEL_HASH_LE not in data:
                continue

            count = data.count(SOURCE_LEVEL_HASH_LE)
            new_data = data.replace(SOURCE_LEVEL_HASH_LE, CLONED_LEVEL_HASH_LE)
            arena_b.write_bytes(new_data)
            replaced_count += 1
            log.info("    %s: %d occurrences replaced", type_dir.name, count)

    log.info("  Level hash replaced in %d files", replaced_count)

    # Return one of the manifest dirs (there's typically one)
    return mf_dirs[0]


# ---------------------------------------------------------------------------
# Phase 3: Clone frisbee actor in mpl_arena_b
# ---------------------------------------------------------------------------

def phase3_clone_frisbee(build_mf: Path):
    """Clone the frisbee actor inside mpl_arena_b."""
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 3: Clone frisbee actor in %s", CLONED_LEVEL)
    log.info("=" * 60)

    new_actor_hash = rad_hash(NEW_ACTOR_NAME)
    log.info("  Source actor: 0x%016X", SOURCE_ACTOR_HASH)
    log.info("  New actor '%s': 0x%016X", NEW_ACTOR_NAME, new_actor_hash)
    log.info("  New position: %s", NEW_POSITION)

    source_hash_bytes = struct.pack("<Q", SOURCE_ACTOR_HASH)

    # --- Step 1: CActorDataResource ---
    log.info("")
    log.info("  --- CActorDataResource ---")
    ad_data = read_level_resource(build_mf, "CActorDataResourceWin10")
    ad = ActorDataResource.from_bytes(ad_data)
    source_idx = ad.find_actor_index(SOURCE_ACTOR_HASH)
    if source_idx is None:
        raise ValueError("Source actor not found in CActorDataResource")
    log.info("    Source actor index: %d", source_idx)
    log.info("    Actor count before: %d", ad.actor_count)

    new_idx = ad.clone_actor(source_idx, new_actor_hash, new_actor_hash, NEW_POSITION)
    log.info("    New actor index: %d", new_idx)
    log.info("    Actor count after: %d", ad.actor_count)

    # Reposition original frisbee
    ad.transforms[source_idx].position_x = SOURCE_POSITION[0]
    ad.transforms[source_idx].position_y = SOURCE_POSITION[1]
    ad.transforms[source_idx].position_z = SOURCE_POSITION[2]
    ad.transforms[source_idx].scale_x = 4
    ad.transforms[source_idx].scale_y = 4
    ad.transforms[source_idx].scale_z = 4
    log.info("    Moved original frisbee to %s", SOURCE_POSITION)

    ad_output = ad.to_bytes()
    log.info("    Size: %d -> %d", len(ad_data), len(ad_output))
    write_level_resource(build_mf, "CActorDataResourceWin10", ad_output)

    # --- Step 2: CTransformCR (frisbee entries — shared tcr for child too) ---
    log.info("")
    log.info("  --- CTransformCR ---")
    tcr_data = read_level_resource(build_mf, "CTransformCRWin10")
    tcr = TransformCR.from_bytes(tcr_data)
    tcr_count_before = len(tcr.entries)
    source_transforms = tcr.find_by_entity(SOURCE_ACTOR_HASH)
    log.info("    Source transform entries: %d", len(source_transforms))
    log.info("    Entry count before: %d", tcr_count_before)

    num_frisbee_entries = len(source_transforms)

    for src in source_transforms:
        # Reposition original frisbee transform entries
        src.position_x = SOURCE_POSITION[0]
        src.position_y = SOURCE_POSITION[1]
        src.position_z = SOURCE_POSITION[2]
        src.scale_x = 4
        src.scale_y = 4
        src.scale_z = 4

        cloned = src.clone(new_actor_hash, position=NEW_POSITION)
        tcr.add_entry(cloned)

    log.info("    Repositioned %d original frisbee transforms to %s", len(source_transforms), SOURCE_POSITION)
    log.info("    Frisbee entries appended: %d (total now %d)",
             num_frisbee_entries, len(tcr.entries))

    # --- Step 3: Clone child actor (bubble) — appends to shared tcr ---
    log.info("")
    log.info("  --- Child actor (bubble) ---")
    num_child_entries = clone_child_actor(
        build_mf, CHILD_ACTOR_HASH, CHILD_CLONE_NAME,
        new_actor_hash, tcr, skip_crs=True,  # CRs deferred until after frisbee CRs
    )

    total_new_entries = len(tcr.entries) - tcr_count_before
    log.info("    Total new CTransformCR entries: %d (frisbee=%d, child=%d)",
             total_new_entries, num_frisbee_entries, num_child_entries)

    # --- Step 3b: Write CTransformCR + BSP expansion ---
    # Pure append, no swapping. BSP expansion covers the new indices.
    gap = len(tcr.entries) - tcr_count_before
    log.info("    BSP expansion gap: %d", gap)

    # Write final CTransformCR with all entries
    tcr_output = tcr.to_bytes()
    log.info("    CTransformCR final size: %d -> %d", len(tcr_data), len(tcr_output))
    write_level_resource(build_mf, "CTransformCRWin10", tcr_output)

    # --- Step 4: CModelCR (with model hash duplication) ---
    log.info("")
    log.info("  --- CModelCR ---")
    model_data = read_level_resource(build_mf, "CModelCRWin10")
    model_output = clone_model_entries(
        model_data, SOURCE_ACTOR_HASH, new_actor_hash,
        model_hash_map=MODEL_HASH_MAP,
    )
    log.info("    Size: %d -> %d", len(model_data), len(model_output))
    write_level_resource(build_mf, "CModelCRWin10", model_output)

    # --- Step 4b: Duplicate model sub-resources ---
    log.info("")
    log.info("  --- Model Resource Duplication ---")
    for old_hash, new_hash in MODEL_HASH_MAP.items():
        for type_name in MODEL_SUB_TYPES:
            # Find source file in clean extract (not build, since build may lack them)
            src_dir = None
            for mf in CLEAN_EXTRACT_DIR.iterdir():
                if not mf.is_dir():
                    continue
                for d in [mf / type_name, mf / f"0x{rad_hash(type_name):016X}" if not type_name.startswith("0x") else mf / type_name]:
                    src = d / f"0x{old_hash:016X}.bin"
                    if src.is_file():
                        src_dir = d
                        break
                if src_dir:
                    break

            if not src_dir:
                continue

            src_file = src_dir / f"0x{old_hash:016X}.bin"
            src_data = src_file.read_bytes()

            # Patch CSkeletonResource self-references
            if type_name == "CSkeletonResourceWin10" and len(src_data) >= 8:
                patched = bytearray(src_data)
                old_le = struct.pack("<Q", old_hash)
                new_le = struct.pack("<Q", new_hash)
                for patch_off in [0x0000, 0x0660]:
                    if patch_off + 8 <= len(patched):
                        if patched[patch_off:patch_off + 8] == old_le:
                            patched[patch_off:patch_off + 8] = new_le
                src_data = bytes(patched)

            # Note: CGSceneResource "initially visible" flag at comp byte+12 bit 0
            # was tested but didn't fix visibility. The frisbee's visibility is
            # controlled by game code, not by this flag.

            # Write to build directory under new hash
            dst_dir = find_type_dir(build_mf, type_name)
            if dst_dir is None:
                # Type dir uses hash name
                type_hash_name = type_name if type_name.startswith("0x") else f"0x{rad_hash(type_name):016X}"
                dst_dir = build_mf / type_hash_name
                if not dst_dir.is_dir():
                    dst_dir = build_mf / type_name
            if dst_dir.is_dir():
                dst_file = dst_dir / f"0x{new_hash:016X}.bin"
                dst_file.write_bytes(src_data)
                log.info("    %s/0x%016X.bin -> 0x%016X.bin (%d bytes)",
                         dst_dir.name, old_hash, new_hash, len(src_data))

    # --- Step 4c: Update CArchiveResource for mpl_arena_b ---
    log.info("")
    log.info("  --- CArchiveResource (add model pairs) ---")
    try:
        car_type = "carchiveresourceWin10"
        car_dir = find_type_dir(build_mf, car_type)
        if car_dir is None:
            car_dir = find_type_dir(build_mf, "CArchiveResourceWin10")
        if car_dir:
            car_path = car_dir / f"{CLONED_LEVEL}.bin"
            if car_path.is_file():
                car_data = bytearray(car_path.read_bytes())
                # CArchiveResource format:
                # u32 flat_dep_count + flat_deps
                # u32 pair_count + pairs (16B each: type_hash, resource_hash)
                # u32 type_dir_count + type_dirs
                flat_count = struct.unpack_from("<I", car_data, 0)[0]
                pair_off = 4 + flat_count * 8
                pair_count = struct.unpack_from("<I", car_data, pair_off)[0]
                pair_data_off = pair_off + 4
                pair_data_end = pair_data_off + pair_count * 16

                # Build new pairs to append
                new_pairs = bytearray()
                new_pair_count = 0
                for new_hash in NEW_MODEL_HASHES:
                    for type_name in ARCHIVE_SUB_TYPES:
                        type_hash = rad_hash(type_name)
                        new_pairs += struct.pack("<QQ", type_hash, new_hash)
                        new_pair_count += 1

                # Insert new pairs at end of pair data (do NOT re-sort)
                car_data[pair_off:pair_off + 4] = struct.pack("<I", pair_count + new_pair_count)
                car_data[pair_data_end:pair_data_end] = new_pairs
                car_path.write_bytes(bytes(car_data))
                log.info("    Added %d new pairs (%d total)", new_pair_count, pair_count + new_pair_count)
    except Exception as e:
        log.warning("    CArchiveResource update failed: %s", e)

    # --- Step 5: CAnimationCR ---
    log.info("")
    log.info("  --- CAnimationCR ---")
    anim_data = read_level_resource(build_mf, "CAnimationCRWin10")
    anim_output = clone_animation_entries(anim_data, SOURCE_ACTOR_HASH, new_actor_hash)
    log.info("    Size: %d -> %d", len(anim_data), len(anim_output))
    write_level_resource(build_mf, "CAnimationCRWin10", anim_output)

    # --- Step 6: Compound CRs (clone entries + inline data) ---
    compound_types = [
        "CComponentLODCRWin10",
        "CScriptCRWin10",
        "CParticleEffectCRWin10",
    ]

    for cr_type in compound_types:
        log.info("")
        log.info("  --- %s ---", cr_type)
        try:
            cr_data = read_level_resource(build_mf, cr_type)
        except FileNotFoundError:
            log.info("    Not found, skipping.")
            continue
        cr_output = clone_compound_cr(
            cr_data, SOURCE_ACTOR_HASH, new_actor_hash,
            search_field="actor_hash", copy_inline=True,
        )
        # Fix unpatched source hash refs in the CLONED portion only.
        # clone_compound_cr appends cloned entries + inline data at the end.
        # Only patch the new bytes to avoid corrupting original entries.
        orig_size = len(cr_data)
        if len(cr_output) > orig_size:
            new_portion = bytearray(cr_output[orig_size:])
            # Patch source actor hash → clone actor hash
            patched_count = new_portion.count(source_hash_bytes)
            if patched_count > 0:
                new_portion = new_portion.replace(
                    source_hash_bytes, struct.pack("<Q", new_actor_hash))
                log.info("    Patched %d source actor refs in cloned data",
                         patched_count)
            # Patch bubble actor hash → clone bubble hash
            # The CScriptCR inline data has a per-instance "bubble" actor
            # variable (SScriptCD+225) that references the child actor.
            child_hash_le = struct.pack("<Q", CHILD_ACTOR_HASH)
            child_clone_hash = rad_hash(CHILD_CLONE_NAME)
            child_clone_le = struct.pack("<Q", child_clone_hash)
            bubble_count = new_portion.count(child_hash_le)
            if bubble_count > 0:
                new_portion = new_portion.replace(child_hash_le, child_clone_le)
                log.info("    Patched %d bubble actor refs in cloned data",
                         bubble_count)
            cr_output = bytes(cr_output[:orig_size]) + bytes(new_portion)
        log.info("    Size: %d -> %d", len(cr_data), len(cr_output))
        write_level_resource(build_mf, cr_type, cr_output)

    # --- Step 6b: CR15SyncGrabCR (dedicated IDA-verified cloner) ---
    log.info("")
    log.info("  --- CR15SyncGrabCRWin10 ---")
    try:
        sg_data = read_level_resource(build_mf, "CR15SyncGrabCRWin10")
        sg_output = clone_syncgrab_cr(sg_data, SOURCE_ACTOR_HASH, new_actor_hash)
        log.info("    Size: %d -> %d", len(sg_data), len(sg_output))
        write_level_resource(build_mf, "CR15SyncGrabCRWin10", sg_output)
    except FileNotFoundError:
        log.info("    Not found, skipping.")
    except Exception as e:
        log.warning("    SyncGrab clone failed: %s", e)

    # --- Step 7: All other CR types with source actor hash ---
    log.info("")
    log.info("  --- Scanning remaining CR types ---")

    for type_dir in sorted(build_mf.iterdir()):
        if not type_dir.is_dir() or type_dir.name.startswith("_"):
            continue

        # Skip types already handled
        if type_dir.name in DEDICATED_CR_TYPES:
            continue
        # Also check hash-based names
        skip = False
        for ded in DEDICATED_CR_TYPES:
            ded_hash = rad_hash(ded)
            if type_dir.name == f"0x{ded_hash:016X}":
                skip = True
                break
        if skip:
            continue

        arena_b = type_dir / f"{CLONED_LEVEL}.bin"
        if not arena_b.is_file():
            continue

        data = arena_b.read_bytes()
        if source_hash_bytes not in data:
            continue

        # Parse as generic CR
        try:
            cr = GenericCR.from_bytes(data)
        except Exception as e:
            log.warning("    %s: parse failed (%s), skipping", type_dir.name, e)
            continue

        # Skip compound CRs with trailing data
        if cr.tail:
            log.warning("    %s: compound CR with %dB tail (skipping — unsafe to clone)",
                        type_dir.name, len(cr.tail))
            continue

        source_entries = cr.find_entries_by_actor(SOURCE_ACTOR_HASH)
        if not source_entries:
            log.info("    %s: hash in raw bytes but not as CR entry, skipping", type_dir.name)
            continue

        for src in source_entries:
            cr.add_entry(src.clone(new_actor_hash, node_index=0xFFFF))

        cr_output = cr.to_bytes()
        arena_b.write_bytes(cr_output)
        log.info("    %s: %d entries cloned, %d -> %d",
                 type_dir.name, len(source_entries), len(data), len(cr_output))

    # --- Step 8: Clone child CRs (AFTER frisbee CRs to avoid double compound clone) ---
    log.info("")
    log.info("  --- Child actor CRs (deferred) ---")
    clone_child_actor(
        build_mf, CHILD_ACTOR_HASH, CHILD_CLONE_NAME,
        new_actor_hash, crs_only=True,
    )


def clone_child_actor(build_mf: Path, source_hash: int, new_name: str,
                      parent_clone_hash: int, tcr: TransformCR = None,
                      skip_crs: bool = False, crs_only: bool = False) -> int:
    """Clone a child actor (like the frisbee bubble).

    Args:
        build_mf: Path to the build manifest directory.
        source_hash: Hash of the original child actor.
        new_name: Name for the cloned child actor.
        parent_clone_hash: Hash of the cloned frisbee (parent).
        tcr: Shared CTransformCR object (appended to, written later by caller).

    Returns:
        Number of CTransformCR entries added for the child.
    """
    new_hash = rad_hash(new_name)
    source_bytes = struct.pack("<Q", source_hash)
    new_le = struct.pack("<Q", new_hash)
    parent_source_bytes = struct.pack("<Q", SOURCE_ACTOR_HASH)
    parent_clone_bytes = struct.pack("<Q", parent_clone_hash)

    log.info("")
    log.info("  === Cloning child actor: %s ===", new_name)
    log.info("  Source: 0x%016X -> Clone: 0x%016X", source_hash, new_hash)

    # --- Steps A+B: CActorDataResource + CTransformCR (skip if crs_only) ---
    child_tcr_count = 0
    if not crs_only:
        ad_data = read_level_resource(build_mf, "CActorDataResourceWin10")
        ad = ActorDataResource.from_bytes(ad_data)
        source_idx = ad.find_actor_index(source_hash)
        if source_idx is None:
            log.warning("  Child actor not found in CActorDataResource, skipping.")
            return 0
        parent_idx = ad.find_actor_index(parent_clone_hash)
        if parent_idx is None:
            parent_idx = ad.parents[source_idx] if source_idx < len(ad.parents) else 0xFFFF
        new_idx = ad.clone_actor(source_idx, new_hash, 0xFFFFFFFFFFFFFFFF)
        if new_idx < len(ad.parents):
            ad.parents[new_idx] = parent_idx
        log.info("  CActorDataResource: cloned index %d -> %d (parent=%d)",
                 source_idx, new_idx, parent_idx)
        write_level_resource(build_mf, "CActorDataResourceWin10", ad.to_bytes())

        # CTransformCR — append child entries
        if tcr is not None:
            source_transforms = tcr.find_by_entity(source_hash)
            for src in source_transforms:
                cloned = src.clone(new_hash)
                tail = bytearray(cloned.tail_data)
                pos = 0
                while True:
                    found = tail.find(parent_source_bytes, pos)
                    if found < 0:
                        break
                    tail[found:found + 8] = parent_clone_bytes
                    pos = found + 8
                cloned.tail_data = bytes(tail)
                tcr.add_entry(cloned)
                child_tcr_count += 1
            log.info("  CTransformCR: %d child entries appended (parent patched)", child_tcr_count)

    # --- Step C: Scan ALL CRs for child hash ---
    # ONLY run if skip_crs is False (called after frisbee CRs to avoid double modification)
    if skip_crs:
        log.info("  Child CRs deferred (will run after frisbee CRs)")
        return child_tcr_count
    # --- Step C0: CInstanceModelCR (dedicated cloner, 10 parallel arrays) ---
    try:
        im_data = read_level_resource(build_mf, "CInstanceModelCRWin10")
        im_output = clone_instancemodel_cr(im_data, source_hash, new_hash)
        if len(im_output) > len(im_data):
            write_level_resource(build_mf, "CInstanceModelCRWin10", im_output)
            log.info("  CInstanceModelCR: cloned, %d -> %d", len(im_data), len(im_output))
        else:
            log.info("  CInstanceModelCR: no matching entries found")
    except FileNotFoundError:
        log.info("  CInstanceModelCR: not found, skipping")
    except Exception as e:
        log.warning("  CInstanceModelCR: clone failed (%s)", e)

    cloned_types = 0
    for type_dir in sorted(build_mf.iterdir()):
        if not type_dir.is_dir() or type_dir.name.startswith("_"):
            continue

        # Skip types in the dedicated/excluded set
        dir_name = type_dir.name
        if dir_name in CHILD_SKIP_TYPES:
            continue
        skip = False
        for skip_type in CHILD_SKIP_TYPES:
            if dir_name == f"0x{rad_hash(skip_type):016X}":
                skip = True
                break
        if skip:
            continue

        arena_b = type_dir / f"{CLONED_LEVEL}.bin"
        if not arena_b.is_file():
            continue

        data = arena_b.read_bytes()
        if source_bytes not in data:
            continue

        # Try generic CR parse first
        try:
            cr = GenericCR.from_bytes(data)
        except Exception:
            continue

        source_entries = cr.find_entries_by_actor(source_hash)

        if cr.tail or not source_entries:
            # Compound CR (has tail or hash is in inline data, not flat entries)
            try:
                cr_output = clone_compound_cr(
                    data, source_hash, new_hash,
                    search_field="actor_hash", copy_inline=True,
                )
                if len(cr_output) > len(data):
                    # Patch source hash -> clone hash in cloned portion only
                    orig_size = len(data)
                    new_portion = bytearray(cr_output[orig_size:])
                    if source_bytes in new_portion:
                        new_portion = bytes(new_portion).replace(source_bytes, new_le)
                        cr_output = cr_output[:orig_size] + new_portion
                    arena_b.write_bytes(cr_output)
                    cloned_types += 1
                    log.info("  %s: compound cloned, %d -> %d",
                             dir_name, len(data), len(cr_output))
            except Exception as e:
                log.warning("  %s: compound clone failed (%s)", dir_name, e)
            continue

        # Simple CR — clone entries
        for src in source_entries:
            cr.add_entry(src.clone(new_hash, node_index=0xFFFF))
        arena_b.write_bytes(cr.to_bytes())
        cloned_types += 1
        log.info("  %s: %d entries cloned", dir_name, len(source_entries))

    log.info("  Child actor cloned across %d CR types", cloned_types)
    return child_tcr_count


# ---------------------------------------------------------------------------
# Phase 4: Repack
# ---------------------------------------------------------------------------

def phase3_bubble_only(build_mf: Path):
    """Test: clone ONLY the bubble actor, no frisbee. Full independent clone."""
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 3: BUBBLE-ONLY TEST")
    log.info("=" * 60)

    source_hash = CHILD_ACTOR_HASH  # 0x03147FD8F518E997
    new_hash = rad_hash(CHILD_CLONE_NAME)  # must have bit63=1
    source_le = struct.pack("<Q", source_hash)
    new_le = struct.pack("<Q", new_hash)

    log.info("  Source: 0x%016X -> Clone: 0x%016X", source_hash, new_hash)

    # --- CActorDataResource ---
    ad_data = read_level_resource(build_mf, "CActorDataResourceWin10")
    ad = ActorDataResource.from_bytes(ad_data)
    source_idx = ad.find_actor_index(source_hash)
    if source_idx is None:
        log.error("  Bubble actor not found!")
        return
    # Clone as independent actor, keep original parent
    new_idx = ad.clone_actor(source_idx, new_hash, 0xFFFFFFFFFFFFFFFF)
    log.info("  CActorDataResource: %d -> %d (parent=%d)", source_idx, new_idx,
             ad.parents[new_idx] if new_idx < len(ad.parents) else -1)
    write_level_resource(build_mf, "CActorDataResourceWin10", ad.to_bytes())

    # --- CTransformCR ---
    tcr_data = read_level_resource(build_mf, "CTransformCRWin10")
    tcr = TransformCR.from_bytes(tcr_data)
    tcr_before = len(tcr.entries)
    for src in tcr.find_by_entity(source_hash):
        tcr.add_entry(src.clone(new_hash))
    tcr_added = len(tcr.entries) - tcr_before
    log.info("  CTransformCR: %d entries appended", tcr_added)
    write_level_resource(build_mf, "CTransformCRWin10", tcr.to_bytes())

    # --- BSP expansion ---
    if tcr_added > 0:
        scene_data = read_level_resource(build_mf, "CGSceneResourceWin10")
        scene = CGSceneResource.from_bytes(scene_data)
        scene.expand_for_clones(tcr_added)
        write_level_resource(build_mf, "CGSceneResourceWin10", scene.to_bytes())
        log.info("  BSP expanded by %d", tcr_added)

    # --- ALL other CRs (skip CActorData, CTransformCR, CGSceneResource) ---
    skip = {"CActorDataResourceWin10", "CTransformCRWin10", "CGSceneResourceWin10"}
    cloned = 0

    for type_dir in sorted(build_mf.iterdir()):
        if not type_dir.is_dir() or type_dir.name.startswith("_"):
            continue
        dir_name = type_dir.name
        if dir_name in skip:
            continue
        # Also check hash-based names
        if any(dir_name == f"0x{rad_hash(s):016X}" for s in skip):
            continue

        arena_b = type_dir / f"{CLONED_LEVEL}.bin"
        if not arena_b.is_file():
            continue

        data = arena_b.read_bytes()
        if source_le not in data:
            continue

        # Try GenericCR
        try:
            cr = GenericCR.from_bytes(data)
            entries = cr.find_entries_by_actor(source_hash)
            if entries and not cr.tail:
                for e in entries:
                    cr.add_entry(e.clone(new_hash, node_index=0xFFFF))
                arena_b.write_bytes(cr.to_bytes())
                cloned += 1
                log.info("  %s: %d generic entries cloned", dir_name, len(entries))
                continue
        except Exception:
            pass

        # Try compound clone
        try:
            cr_output = clone_compound_cr(data, source_hash, new_hash,
                                          search_field="actor_hash", copy_inline=True)
            if len(cr_output) > len(data):
                # Patch remaining source refs in cloned portion
                orig_size = len(data)
                new_portion = cr_output[orig_size:]
                if source_le in new_portion:
                    new_portion = new_portion.replace(source_le, new_le)
                    cr_output = cr_output[:orig_size] + new_portion
                arena_b.write_bytes(cr_output)
                cloned += 1
                log.info("  %s: compound cloned, %d -> %d", dir_name, len(data), len(cr_output))
                continue
        except Exception as e:
            pass

        # If hash is present but not cloneable, log it
        log.info("  %s: has hash but NOT cloned (hash at non-standard position?)", dir_name)

    log.info("  Bubble cloned across %d CR types", cloned)


def phase4_repack():
    """Repack the build directory into a game archive."""
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
    """Add mpl_arena_b to hash_lookup.json if not present."""
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 5: Update hash_lookup.json")
    log.info("=" * 60)

    hash_key = f"0x{CLONED_LEVEL_HASH:016X}"

    if HASH_LOOKUP_PATH.is_file():
        with open(HASH_LOOKUP_PATH, "r") as f:
            lookup = json.load(f)
    else:
        lookup = {}

    if hash_key in lookup:
        log.info("  %s: '%s' already present.", hash_key, lookup[hash_key])
    else:
        lookup[hash_key] = CLONED_LEVEL
        with open(HASH_LOOKUP_PATH, "w") as f:
            json.dump(lookup, f, indent=2, sort_keys=True)
        log.info("  Added %s: '%s'", hash_key, CLONED_LEVEL)


# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

def cleanup():
    """Remove the build directory to avoid leaving multiple large copies."""
    log.info("")
    log.info("=" * 60)
    log.info("Cleanup")
    log.info("=" * 60)

    if BUILD_DIR.exists():
        log.info("  Removing build directory %s ...", BUILD_DIR)
        shutil.rmtree(BUILD_DIR)
        log.info("  Done.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    log.info("Clone mpl_arena_a -> mpl_arena_b")
    log.info("Script directory: %s", SCRIPT_DIR)

    # Verify cloned level hash matches expectation
    computed = rad_hash(CLONED_LEVEL)
    assert computed == CLONED_LEVEL_HASH, (
        f"rad_hash('{CLONED_LEVEL}') = 0x{computed:016X}, "
        f"expected 0x{CLONED_LEVEL_HASH:016X}"
    )

    phase0_extract()
    phase1_copy_to_build()
    build_mf = phase2_clone_level()
    if BUBBLE_ONLY_TEST:
        phase3_bubble_only(build_mf)
    elif not SKIP_FRISBEE_CLONE:
        phase3_clone_frisbee(build_mf)
    else:
        log.info("\n  SKIP_FRISBEE_CLONE is set — skipping Phase 3")
    phase4_repack()
    phase5_update_hash_lookup()
    cleanup()

    log.info("")
    log.info("=" * 60)
    log.info("ALL DONE. Output at: %s", OUTPUT_DIR)
    log.info("=" * 60)


if __name__ == "__main__":
    main()
