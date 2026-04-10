"""Actor cloning operation: clone an actor in a level with a new position."""

from __future__ import annotations
import os
from pathlib import Path

from ..radhash import rad_hash
from ..resources.actor_data import ActorDataResource
from ..resources.transform_cr import TransformCR, TransformEntry, ENTRY_SIZE as TRANSFORM_ENTRY_SIZE
from ..resources.scene import CGSceneResource
from ..resources.generic_cr import GenericCR


def clone_actor(
    extracted_dir: str,
    level_name: str,
    source_actor_hash: int,
    new_actor_name: str,
    position: tuple[float, float, float],
    rotation: tuple[float, float, float, float] | None = None,
) -> dict[str, bytes]:
    """Clone an actor in a level, returning modified resource files.

    Args:
        extracted_dir: Path to echovr_extracted_named/
        level_name: e.g., "mpl_arena_a"
        source_actor_hash: CRC-64 hash of the source actor to clone
        new_actor_name: Name for the new actor (hashed for the new actor hash)
        position: (x, y, z) world position for the clone
        rotation: (x, y, z, w) quaternion rotation, or None to copy source

    Returns:
        Dict of {type_dir/level_name.bin: modified_bytes} for all changed files.
    """
    base = Path(extracted_dir)
    new_hash = rad_hash(new_actor_name)
    modified_files: dict[str, bytes] = {}

    def read_resource(type_dir: str) -> bytes:
        path = base / type_dir / f"{level_name}.bin"
        with open(path, "rb") as f:
            return f.read()

    def resource_path(type_dir: str) -> str:
        return f"{type_dir}/{level_name}.bin"

    # 1. Modify CActorDataResource
    actor_data = ActorDataResource.from_bytes(read_resource("CActorDataResourceWin10"))
    source_idx = actor_data.find_actor_index(source_actor_hash)
    if source_idx is None:
        raise ValueError(f"Source actor 0x{source_actor_hash:016x} not found in CActorDataResource")

    new_idx = actor_data.clone_actor(source_idx, new_hash, new_hash, position)
    modified_files[resource_path("CActorDataResourceWin10")] = actor_data.to_bytes()

    # 2. Modify CGSceneResource — insert into entity lookup table
    scene_data = read_resource("CGSceneResourceWin10")
    scene = CGSceneResource.from_bytes(scene_data)
    scene.insert_entity(new_hash)
    modified_files[resource_path("CGSceneResourceWin10")] = scene.to_bytes()

    # 3. Modify CTransformCR
    transform = TransformCR.from_bytes(read_resource("CTransformCRWin10"))
    source_transforms = transform.find_by_entity(source_actor_hash)
    for src_entry in source_transforms:
        new_entry = src_entry.clone(new_hash, position=position, rotation=rotation)
        transform.add_entry(new_entry)
    modified_files[resource_path("CTransformCRWin10")] = transform.to_bytes()

    # 4. Modify each CR component file where the source actor exists
    component_types = actor_data.get_component_types_for_actor(source_idx)
    # Map component type hashes to directory names
    cr_dirs = _find_cr_directories(base, level_name)

    for type_hash, type_dir in cr_dirs.items():
        if type_dir in ("CActorDataResourceWin10", "CGSceneResourceWin10", "CTransformCRWin10"):
            continue  # Already handled

        try:
            cr_data = read_resource(type_dir)
        except FileNotFoundError:
            continue

        cr = GenericCR.from_bytes(cr_data)
        source_entries = cr.find_entries_by_actor(source_actor_hash)
        if not source_entries:
            continue

        for src_entry in source_entries:
            new_entry = src_entry.clone(new_hash)
            cr.add_entry(new_entry)

        modified_files[resource_path(type_dir)] = cr.to_bytes()

    return modified_files


def _find_cr_directories(base: Path, level_name: str) -> dict[int, str]:
    """Build a mapping of type_hash -> directory_name for all CR types that
    have the given level resource."""
    from ..radhash import rad_hash as rh
    result = {}
    for d in base.iterdir():
        if d.is_dir() and (d / f"{level_name}.bin").exists():
            type_name = d.name
            type_hash = rh(type_name)
            result[type_hash] = type_name
    return result


def clone_actor_by_name(
    extracted_dir: str,
    level_name: str,
    source_actor_name: str,
    new_actor_name: str,
    position: tuple[float, float, float],
    rotation: tuple[float, float, float, float] | None = None,
) -> dict[str, bytes]:
    """Convenience wrapper that hashes the source actor name."""
    source_hash = rad_hash(source_actor_name)
    return clone_actor(extracted_dir, level_name, source_hash, new_actor_name,
                       position, rotation)


def write_modified_files(modified: dict[str, bytes], output_dir: str) -> list[str]:
    """Write modified resource files to an output directory."""
    out = Path(output_dir)
    written = []
    for rel_path, data in modified.items():
        full_path = out / rel_path
        full_path.parent.mkdir(parents=True, exist_ok=True)
        with open(full_path, "wb") as f:
            f.write(data)
        written.append(str(full_path))
    return written
