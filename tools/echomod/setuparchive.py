#!/usr/bin/env python3
"""
setuparchive.py — Unified EchoVR archive builder

Creates both custom levels in a single repacked archive:
  - mpl_arena_b:       Clone of mpl_arena_a
  - mpl_arenacombat:   Clone of mpl_arena_a with combat sub-level loading

Usage:
    python setuparchive.py "C:\\Path\\To\\_data\\5932408047"
    python setuparchive.py "C:\\Path\\To\\_data\\5932408047" --skip-frisbee
    python setuparchive.py "C:\\Path\\To\\_data\\5932408047" --skip-combat

The data path should point to your game's archive folder, e.g.:
    C:\\Program Files\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena\\_data\\5932408047
"""

import sys
import argparse
import shutil
import json
import time
import logging
from pathlib import Path

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

logging.basicConfig(level=logging.INFO, format="%(message)s")
log = logging.getLogger("setuparchive")


def validate_data_path(p: Path) -> Path:
    """Validate the user-provided game data path."""
    if not p.exists():
        log.error("Path does not exist: %s", p)
        sys.exit(1)
    if not p.is_dir():
        log.error("Path is not a directory: %s", p)
        sys.exit(1)
    # Check for expected archive structure (rad15/ subdir, or manifests/packages)
    manifests = list(p.glob("**/manifests"))
    packages = list(p.glob("**/packages"))
    if not manifests and not packages:
        # Maybe user passed the parent _data dir
        subdirs = [d for d in p.iterdir() if d.is_dir() and d.name.isdigit()]
        if subdirs:
            log.warning("Path looks like the _data directory. Did you mean: %s?", subdirs[0])
            log.error("Please pass the archive folder directly (e.g., ...\\_data\\5932408047)")
            sys.exit(1)
        log.error("Path doesn't look like a game archive (no manifests/packages found): %s", p)
        sys.exit(1)
    return p


def configure_modules(data_path: Path, build_dir: Path, extract_dir: Path,
                      output_dir: Path, hash_lookup: Path):
    """Import and configure both pipeline modules with shared paths."""
    import clone_frisbee
    import clone_combat

    # Override path globals in both modules
    for mod in [clone_frisbee, clone_combat]:
        mod.ARCHIVE_DIR = data_path
        mod.CLEAN_EXTRACT_DIR = extract_dir
        mod.BUILD_DIR = build_dir
        mod.OUTPUT_DIR = output_dir
        mod.HASH_LOOKUP_PATH = hash_lookup

    return clone_frisbee, clone_combat


def main():
    parser = argparse.ArgumentParser(
        description="Build a patched EchoVR archive with custom levels.",
        epilog="Example: python setuparchive.py \"C:\\Program Files\\Oculus\\...\\5932408047\"",
    )
    parser.add_argument(
        "data_path",
        help="Path to the game archive folder (e.g., ...\\_data\\5932408047)",
    )
    parser.add_argument(
        "--skip-frisbee", action="store_true",
        help="Skip building mpl_arena_b",
    )
    parser.add_argument(
        "--skip-combat", action="store_true",
        help="Skip building mpl_arenacombat (combat arena level)",
    )
    parser.add_argument(
        "--no-cleanup", action="store_true",
        help="Keep the build directory after repacking (for debugging)",
    )
    parser.add_argument(
        "--output", type=str, default=None,
        help="Output directory for repacked archive (default: patched_output/)",
    )
    args = parser.parse_args()

    data_path = validate_data_path(Path(args.data_path))

    # Shared paths
    extract_dir = SCRIPT_DIR / "echovr_clean_extract"
    build_dir = SCRIPT_DIR / "echovr_build_setup"
    output_dir = Path(args.output) if args.output else SCRIPT_DIR / "patched_output"
    hash_lookup = SCRIPT_DIR / "hash_lookup.json"

    frisbee_mod, combat_mod = configure_modules(
        data_path, build_dir, extract_dir, output_dir, hash_lookup
    )

    start = time.time()

    log.info("=" * 60)
    log.info("  EchoVR Archive Setup")
    log.info("=" * 60)
    log.info("")
    log.info("  Data path:    %s", data_path)
    log.info("  Extract dir:  %s", extract_dir)
    log.info("  Build dir:    %s", build_dir)
    log.info("  Output dir:   %s", output_dir)
    log.info("  Frisbee:      %s", "SKIP" if args.skip_frisbee else "enabled")
    log.info("  Combat:       %s", "SKIP" if args.skip_combat else "enabled")
    log.info("")

    if args.skip_frisbee and args.skip_combat:
        log.error("Both --skip-frisbee and --skip-combat specified. Nothing to do.")
        sys.exit(1)

    # =======================================================================
    # Phase 0: Extract archive
    # =======================================================================
    log.info("=" * 60)
    log.info("PHASE 0: Extract archive")
    log.info("=" * 60)
    frisbee_mod.phase0_extract()

    # =======================================================================
    # Phase 1: Copy to shared build directory
    # =======================================================================
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 1: Copy to build directory")
    log.info("=" * 60)
    frisbee_mod.phase1_copy_to_build()

    # Find manifest directory (shared by both levels)
    mf_dirs = sorted(
        [d for d in build_dir.iterdir() if d.is_dir()],
        key=lambda d: sum(1 for _ in d.iterdir()),
        reverse=True,
    )
    if not mf_dirs:
        log.error("No manifest directories found in %s", build_dir)
        sys.exit(1)
    build_mf = mf_dirs[0]
    log.info("  Main manifest: %s", build_mf.name)

    # =======================================================================
    # Phase 2: Clone levels
    # =======================================================================
    if not args.skip_frisbee:
        log.info("")
        log.info("=" * 60)
        log.info("PHASE 2a: Clone level (mpl_arena_a -> mpl_arena_b)")
        log.info("=" * 60)
        frisbee_mod.phase2_clone_level()

    if not args.skip_combat:
        log.info("")
        log.info("=" * 60)
        log.info("PHASE 2b: Clone level (mpl_arena_a -> mpl_arenacombat)")
        log.info("=" * 60)
        combat_mod.phase2_clone_level()

    # =======================================================================
    # Phase 3: Level-specific modifications
    # =======================================================================
    if not args.skip_frisbee:
        log.info("")
        log.info("=" * 60)
        log.info("PHASE 3a: Clone in mpl_arena_b")
        log.info("=" * 60)
        frisbee_mod.phase3_clone_frisbee(build_mf)

    if not args.skip_combat:
        log.info("")
        log.info("=" * 60)
        log.info("PHASE 3b: Setup combat sub-level in mpl_arenacombat")
        log.info("=" * 60)
        combat_mod.phase3_transplant(build_mf)

    # =======================================================================
    # Phase 4: Repack archive
    # =======================================================================
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 4: Repack archive")
    log.info("=" * 60)
    frisbee_mod.phase4_repack()

    # =======================================================================
    # Phase 5: Update hash lookup
    # =======================================================================
    log.info("")
    log.info("=" * 60)
    log.info("PHASE 5: Update hash lookup")
    log.info("=" * 60)
    if not args.skip_frisbee:
        frisbee_mod.phase5_update_hash_lookup()
    if not args.skip_combat:
        combat_mod.phase5_update_hash_lookup()

    # =======================================================================
    # Cleanup
    # =======================================================================
    if not args.no_cleanup:
        log.info("")
        log.info("Cleaning up build directory...")
        frisbee_mod.cleanup()
    else:
        log.info("")
        log.info("Build directory kept at: %s", build_dir)

    elapsed = time.time() - start
    log.info("")
    log.info("=" * 60)
    log.info("  ALL DONE in %.1f seconds", elapsed)
    log.info("=" * 60)
    log.info("")
    log.info("  Output archive: %s", output_dir)
    log.info("")
    levels = []
    if not args.skip_frisbee:
        levels.append("mpl_arena_b")
    if not args.skip_combat:
        levels.append("mpl_arenacombat (combat arena)")
    log.info("  Levels built:")
    for lv in levels:
        log.info("    - %s", lv)
    log.info("")
    log.info("  To deploy: copy the output archive contents to your game's")
    log.info("  _data folder, replacing the original archive files.")
    log.info("")


if __name__ == "__main__":
    main()
