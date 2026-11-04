#!/usr/bin/env python3
"""The packaging half of `just quest-store-build` (docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md).

The recipe builds with CMake and the repack recipe; this script holds what is left, which is bookkeeping:

  require <obb|apk|keystore> ...  the per-machine inputs exist: NEVR_STORE_OBB and NEVR_STORE_APK name files, and the signing
                                  keystore opens. A missing one is refused, naming the variable.
  check                           the cache check: exit 0 when build/quest-store holds the outputs of exactly these inputs
                                  (every output digest read again), exit 3 when it does not (the line says which input changed).
  collect                         after a repack: link the signed APK and the stock `_data` tree into build/quest-store, check the
                                  tree against tools/quest_stock_data.pin, and write store-build.json, the manifest of every
                                  input and output digest, which is what `check` and `quest_store_install.py` read.

Every step is one JSON line on standard output (tag NEVR.QUESTBUILD, docs/standards/logging.md); a refusal is a line with the
event `refused`, a `code` and a `detail`, and exit status 1. Nothing here prints a key or a passphrase.
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from typing import Callable, Dict, List, Optional, Tuple

TAG = "NEVR.QUESTBUILD"
SCHEMA = 1
REPO = pathlib.Path(__file__).resolve().parents[1]
MANIFEST_NAME = "store-build.json"
SIGNED_APK_NAME = "r15_nevr-sentinel_signed.apk"
REPACK_RECIPE = "android-repack-apk"
EXIT_REFUSED = 1
EXIT_MISS = 3


class Refused(Exception):
    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code = code
        self.detail = detail


def log(level: str, event: str, **fields: object) -> None:
    now = datetime.datetime.now(datetime.timezone.utc)
    record = {"schema": SCHEMA, "ts": now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z", "level": level, "tag": TAG, "event": event}
    record.update(fields)
    print(json.dumps(record, sort_keys=True), flush=True)


@dataclasses.dataclass(frozen=True)
class Layout:
    """Where the steps leave things, under one repository root (a test passes a scratch root)."""

    root: pathlib.Path

    @property
    def pins(self) -> pathlib.Path:
        return self.root / "tools" / "quest_stock_data.pin"

    @property
    def stock_root(self) -> pathlib.Path:
        return self.root / "build" / "quest-stock-data"

    @property
    def shim(self) -> pathlib.Path:
        return self.root / "build" / "android-arm64" / "sentinel" / "libovrplatformloader.so"

    @property
    def build_info(self) -> pathlib.Path:
        return self.root / "build" / "android-arm64" / "generated" / "nevr_build_info.h"

    @property
    def patcher(self) -> pathlib.Path:
        return self.root / "build" / "quest-patcher" / "quest_patch_libr15"

    @property
    def out(self) -> pathlib.Path:
        return self.root / "build" / "quest-store"

    @property
    def repack(self) -> pathlib.Path:
        return self.out / "repack"

    @property
    def repacked_apk(self) -> pathlib.Path:
        return self.repack / SIGNED_APK_NAME

    @property
    def manifest(self) -> pathlib.Path:
        return self.out / MANIFEST_NAME


# ── hashing and pins ────────────────────────────────────────────────────────────────────────────────────────────────────
def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(8 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def file_record(path: pathlib.Path) -> Dict[str, object]:
    return {"sha256": sha256_file(path), "bytes": path.stat().st_size}


HEX64 = re.compile(r"^[0-9a-f]{64}$")


def parse_pins(text: str) -> Tuple[str, int, List[Dict[str, object]]]:
    """The pin file (tools/quest_stock_data.pin), read by this script as the oracle of the C++ extractor's own reading."""
    obb_sha: Optional[str] = None
    obb_size: Optional[int] = None
    files: List[Dict[str, object]] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 3)
        if parts[0] == "obb_sha256" and len(parts) == 2 and HEX64.match(parts[1]):
            obb_sha = parts[1]
        elif parts[0] == "obb_size" and len(parts) == 2 and parts[1].isdigit():
            obb_size = int(parts[1])
        elif parts[0] == "file" and len(parts) == 4 and HEX64.match(parts[1]) and parts[2].isdigit():
            files.append({"sha256": parts[1], "bytes": int(parts[2]), "path": parts[3].strip()})
        else:
            raise Refused("pin_syntax", f"{line!r} is not a pin record (line {number})")
    if obb_sha is None or obb_size is None or not files:
        raise Refused("pin_incomplete", "the pins need obb_sha256, obb_size and at least one file")
    return obb_sha, obb_size, files


# ── the per-machine inputs ──────────────────────────────────────────────────────────────────────────────────────────────
INPUT_VARIABLES = {"obb": "NEVR_STORE_OBB", "apk": "NEVR_STORE_APK"}


def input_path(kind: str, environ: Dict[str, str]) -> pathlib.Path:
    variable = INPUT_VARIABLES[kind]
    value = environ.get(variable, "")
    if not value:
        raise Refused("input_unset", f"{variable} names no file: set it to the store {kind.upper()} of the build")
    path = pathlib.Path(value)
    if not path.is_file():
        raise Refused("input_not_a_file", f"{variable} is not a file: {value}")
    return path


def keystore_fingerprint(layout: Layout) -> str:
    """The signer's SHA-256 (lower case hex), from tools/quest_keystore.sh; the passphrase stays in the environment."""
    script = layout.root / "tools" / "quest_keystore.sh"
    done = subprocess.run([str(script), "fingerprint"], capture_output=True, text=True, timeout=120)
    fingerprint = done.stdout.strip().lower()
    if done.returncode != 0 or not HEX64.match(fingerprint):
        raise Refused("keystore_unusable", f"tools/quest_keystore.sh fingerprint exited {done.returncode}: {done.stderr.strip()[-300:]}")
    return fingerprint


def repack_recipe_text(layout: Layout) -> str:
    done = subprocess.run(["just", "--show", REPACK_RECIPE], cwd=layout.root, capture_output=True, text=True, timeout=120)
    if done.returncode != 0 or not done.stdout.strip():
        raise Refused("recipe_unreadable", f"just --show {REPACK_RECIPE} exited {done.returncode}: {done.stderr.strip()[-300:]}")
    return done.stdout


# ── the cache key ───────────────────────────────────────────────────────────────────────────────────────────────────────
def stock_data_dir(layout: Layout, obb_sha: str) -> pathlib.Path:
    directory = layout.stock_root / obb_sha
    if not (directory / "stock-data.json").is_file():
        raise Refused("stock_data_missing", f"build/quest-stock-data/{obb_sha} is not there: run `just quest-stock-data` first")
    return directory


def gather_inputs(layout: Layout, store_apk: pathlib.Path, recipe: str, signer: str) -> Dict[str, object]:
    """Everything the repack's output depends on, each as a digest. A change in any one changes the key."""
    for what, path in (("sentinel library", layout.shim), ("build info", layout.build_info), ("libr15 patcher", layout.patcher)):
        if not path.is_file():
            raise Refused("build_output_missing", f"the {what} is not built: {path.relative_to(layout.root)}")
    obb_sha, _, _ = parse_pins(layout.pins.read_text())
    stock = stock_data_dir(layout, obb_sha)
    tools: Dict[str, str] = {}
    for name in sorted(set(re.findall(r"tools/[A-Za-z0-9_./-]+", recipe))):
        path = layout.root / name
        if path.is_file():
            tools[name] = sha256_file(path)
    return {
        "store_apk": {"name": store_apk.name, **file_record(store_apk)},
        "stock_data": {"obb_sha256": obb_sha, "manifest_sha256": sha256_file(stock / "stock-data.json")},
        "sentinel_library": sha256_file(layout.shim),
        "build_info": sha256_file(layout.build_info),
        "libr15_patcher": sha256_file(layout.patcher),
        "repack_recipe": hashlib.sha256(recipe.encode()).hexdigest(),
        "repack_tools": tools,
        "signing_key": signer,
    }


def cache_key(inputs: Dict[str, object]) -> str:
    return hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def changed_inputs(old: Dict[str, object], new: Dict[str, object]) -> List[str]:
    names = []
    for name in sorted(set(old) | set(new)):
        if old.get(name) != new.get(name):
            names.append(name)
    return names


# ── check ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
def tree_files(base: pathlib.Path) -> Dict[str, pathlib.Path]:
    found = {}
    for path in sorted(base.rglob("*")):
        if path.is_symlink() or path.is_file():
            found[path.relative_to(base.parent).as_posix()] = path
    return found


def verify_outputs(layout: Layout, manifest: Dict[str, object]) -> Optional[str]:
    """None when every output the manifest names is there and still has its digest, else the first fault."""
    outputs = manifest["outputs"]
    apk = layout.out / outputs["apk"]["path"]
    if not apk.is_file():
        return f"{outputs['apk']['path']} is missing"
    if file_record(apk) != {"sha256": outputs["apk"]["sha256"], "bytes": outputs["apk"]["bytes"]}:
        return f"{outputs['apk']['path']} has changed"
    expected = {entry["path"]: entry for entry in outputs["data"]}
    present = tree_files(layout.out / "_data")
    for path in sorted(set(present) - set(expected)):
        return f"{path} is not in the manifest"
    for path, entry in sorted(expected.items()):
        if path not in present:
            return f"{path} is missing"
        if file_record(present[path]) != {"sha256": entry["sha256"], "bytes": entry["bytes"]}:
            return f"{path} has changed"
    return None


def check(layout: Layout, store_apk: pathlib.Path, recipe: str, signer: str) -> int:
    started = time.monotonic()
    inputs = gather_inputs(layout, store_apk, recipe, signer)
    key = cache_key(inputs)
    reason: Optional[str] = None
    old: Optional[Dict[str, object]] = None
    if not layout.manifest.is_file():
        reason = "no store-build.json: nothing was built yet"
    else:
        try:
            old = json.loads(layout.manifest.read_text())
            if old.get("key") != key:
                reason = "inputs changed: " + ", ".join(changed_inputs(old.get("inputs", {}), inputs))
            else:
                reason = verify_outputs(layout, old)
        except (OSError, ValueError, KeyError, TypeError) as error:
            reason = f"store-build.json cannot be used: {error}"
    ms = round((time.monotonic() - started) * 1000, 1)
    if reason is None:
        log("info", "cache_hit", key=key, apk=old["outputs"]["apk"]["sha256"], data_files=len(old["outputs"]["data"]), ms=ms)
        return 0
    if layout.manifest.exists():
        layout.manifest.unlink()
        log("info", "manifest_removed", reason="the outputs it describes are not trusted")
    log("info", "cache_miss", key=key, reason=reason, ms=ms)
    return EXIT_MISS


# ── collect ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
def link_or_copy(source: pathlib.Path, destination: pathlib.Path) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
        return "hardlink"
    except OSError:
        shutil.copyfile(source, destination)
        return "copy"


def collect(layout: Layout, store_apk: pathlib.Path, recipe: str, signer: str) -> int:
    started = time.monotonic()
    if not layout.repacked_apk.is_file():
        raise Refused("repack_missing", f"the repack left no {layout.repacked_apk.relative_to(layout.root)}")
    inputs = gather_inputs(layout, store_apk, recipe, signer)
    key = cache_key(inputs)
    obb_sha, _, pinned = parse_pins(layout.pins.read_text())
    source_data = stock_data_dir(layout, obb_sha) / "_data"

    if layout.manifest.exists():
        layout.manifest.unlink()
    apk_out = layout.out / SIGNED_APK_NAME
    if apk_out.exists():
        apk_out.unlink()
    method = link_or_copy(layout.repacked_apk, apk_out)
    data_out = layout.out / "_data"
    if data_out.exists():
        shutil.rmtree(data_out)
    methods = set()
    for path in sorted(source_data.rglob("*")):
        if path.is_file() and not path.is_symlink():
            methods.add(link_or_copy(path, data_out / path.relative_to(source_data)))
    log("info", "outputs_placed", apk=method, data=sorted(methods))

    present = tree_files(data_out)
    data_records = []
    expected = {entry["path"]: entry for entry in pinned}
    for path in sorted(set(present) - set(expected)):
        raise Refused("pin_unlisted_file", f"build/quest-store/{path} is not a pinned file")
    for path, entry in sorted(expected.items()):
        if path not in present:
            raise Refused("pin_missing_file", f"the pins list {path} and build/quest-store does not hold it")
        record = file_record(present[path])
        if record["bytes"] != entry["bytes"]:
            raise Refused("pin_size_mismatch", f"{path} is {record['bytes']} bytes; the pins say {entry['bytes']}")
        if record["sha256"] != entry["sha256"]:
            raise Refused("pin_digest_mismatch", f"{path} has SHA-256 {record['sha256']}; the pins say {entry['sha256']}")
        data_records.append({"path": path, **record})
    log("info", "data_matches_pins", files=len(data_records), obb_sha256=obb_sha)

    apk_record = file_record(apk_out)
    manifest = {
        "schema": SCHEMA,
        "tag": TAG,
        "key": key,
        "inputs": inputs,
        "outputs": {"apk": {"path": SIGNED_APK_NAME, **apk_record}, "data": data_records},
        "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    layout.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    log("info", "store_build_ready", key=key, apk=SIGNED_APK_NAME, apk_sha256=apk_record["sha256"], apk_bytes=apk_record["bytes"],
        data_files=len(data_records), data_bytes=sum(r["bytes"] for r in data_records), manifest=f"build/quest-store/{MANIFEST_NAME}",
        ms=round((time.monotonic() - started) * 1000, 1))
    return 0


# ── command line ────────────────────────────────────────────────────────────────────────────────────────────────────────
def require(layout: Layout, kinds: List[str], environ: Dict[str, str], keystore: Callable[[Layout], str] = keystore_fingerprint) -> int:
    for kind in kinds:
        if kind == "keystore":
            fingerprint = keystore(layout)
            log("info", "input_ok", input="keystore", signer_sha256=fingerprint)
        elif kind in INPUT_VARIABLES:
            path = input_path(kind, environ)
            log("info", "input_ok", input=kind, variable=INPUT_VARIABLES[kind], name=path.name, bytes=path.stat().st_size)
        else:
            raise Refused("usage", f"unknown input '{kind}': obb, apk or keystore")
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=pathlib.Path, default=REPO, help="repository root (tests pass a scratch tree)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("require").add_argument("kinds", nargs="+")
    sub.add_parser("check")
    sub.add_parser("collect")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exit_:
        return 2 if exit_.code else 0
    layout = Layout(args.root.resolve())
    try:
        if args.command == "require":
            return require(layout, args.kinds, dict(os.environ))
        store_apk = input_path("apk", dict(os.environ))
        recipe = repack_recipe_text(layout)
        signer = keystore_fingerprint(layout)
        return (check if args.command == "check" else collect)(layout, store_apk, recipe, signer)
    except Refused as refusal:
        log("error", "refused", code=refusal.code, detail=refusal.detail)
        return EXIT_REFUSED
    except (OSError, subprocess.SubprocessError) as error:
        log("error", "refused", code="failed", detail=f"{type(error).__name__}: {error}")
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
