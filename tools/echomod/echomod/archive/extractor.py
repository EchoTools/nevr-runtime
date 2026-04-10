"""Extract resources from Echo VR archives."""

from __future__ import annotations
import json
from pathlib import Path

from .manifest import Manifest
from .package import PackageReader


def extract_archive(
    game_data_dir: str,
    output_dir: str,
    hash_names_path: str | None = None,
    manifest_name: str | None = None,
) -> int:
    """Extract all resources from a game archive.

    Args:
        game_data_dir: Path containing manifests/ and packages/ dirs
        output_dir: Where to write extracted files
        hash_names_path: Optional path to hash_names.json for name resolution
        manifest_name: Manifest filename (auto-detected if None)

    Returns:
        Number of resources extracted.
    """
    base = Path(game_data_dir)
    manifests_dir = base / "manifests"
    packages_dir = base / "packages"

    # Auto-detect manifest
    if manifest_name is None:
        manifests = [f for f in manifests_dir.iterdir() if f.is_file()]
        # Pick the largest manifest (primary, not patch)
        manifests.sort(key=lambda f: f.stat().st_size, reverse=True)
        if not manifests:
            raise FileNotFoundError(f"No manifest files found in {manifests_dir}")
        manifest_name = manifests[0].name

    # Load manifest
    manifest = Manifest.from_file(str(manifests_dir / manifest_name))

    # Load hash names for resolution
    hash_db = {}
    if hash_names_path:
        with open(hash_names_path) as f:
            raw = json.load(f)
        for k, v in raw.items():
            try:
                hash_db[int(k, 16)] = v
            except (ValueError, TypeError):
                pass

    # Create package reader
    reader = PackageReader(manifest, str(packages_dir), manifest_name)

    # Extract
    count = reader.extract_all(output_dir, hash_db=hash_db if hash_db else None)
    return count
