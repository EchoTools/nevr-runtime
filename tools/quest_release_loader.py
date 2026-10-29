#!/usr/bin/env python3
"""The Quest loader of a release APK is the release's own asset, not a second build of the same tag (#1135).

CI builds `libovrplatformloader.so` once per tag, attests it and attaches it to the release as
`nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so`. `just release-quest-apk` used to compile its own, so the asset and
the library inside the published APK were two builds with different build IDs. It now packages the asset:

  quest_release_loader.py fetch --tag vX.Y.Z --out <dir> [--repo OWNER/REPO]
      Download the tag's asset (a published release or a draft) into <dir>/libovrplatformloader.so and print its
      sha256. Refused unless the release has exactly one asset of that name, the downloaded size is the release's
      size, the downloaded bytes have the sha256 GitHub recorded for the asset, and `gh attestation verify` accepts
      the file for the repository.

  quest_release_loader.py verify-apk --apk <apk> --sha256 <hex>
      Refused unless the loader inside the APK has exactly that sha256.

`gh` is the GitHub CLI named by $GH_BIN (default `gh`). One JSON line per action goes to stderr; a refusal is one JSON
line and exit 1.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

DEFAULT_REPO = "EchoTools/nevr-runtime"
LOADER_IN_APK = "lib/arm64-v8a/libovrplatformloader.so"
LOADER_FILE = "libovrplatformloader.so"
TAG = re.compile(r"v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))")
DIGEST = re.compile(r"sha256:([0-9a-f]{64})")


class Refused(Exception):
    pass


def asset_name(version: str) -> str:
    """The name has one source, tools/release_asset_names.sh (release_quest_asset_name)."""
    return f"nevr-runtime-v{version}-quest-libovrplatformloader.so"


def _log(event: str, **fields) -> None:
    print(json.dumps({"tag": "NEVR.QUEST.LOADER", "event": event, **fields}, separators=(",", ":")), file=sys.stderr)


def _gh(*args: str) -> str:
    gh = os.environ.get("GH_BIN", "gh")
    try:
        done = subprocess.run([gh, *args], capture_output=True, text=True, check=False)
    except OSError as error:
        raise Refused(f"cannot run {gh}: {error.strerror}") from None
    if done.returncode != 0:
        raise Refused(f"`{gh} {' '.join(args[:2])}` exited {done.returncode}: {done.stderr.strip()}")
    return done.stdout


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(tag: str, out: Path, repo: str) -> str:
    match = TAG.fullmatch(tag)
    if match is None:
        raise Refused(f"not a release tag (vX.Y.Z): {tag}")
    name = asset_name(match.group(1))
    try:
        assets = json.loads(_gh("release", "view", tag, "--repo", repo, "--json", "assets")).get("assets", [])
    except json.JSONDecodeError as error:
        raise Refused(f"`gh release view` printed no JSON: {error}") from None
    named = [a for a in assets if a.get("name") == name]
    if len(named) != 1:
        raise Refused(f"release {tag} has {len(named)} asset(s) named {name} (need exactly 1)")
    found = DIGEST.fullmatch(str(named[0].get("digest", "")))
    if found is None:
        raise Refused(f"release {tag} records no sha256 digest for {name}")
    want, size = found.group(1), named[0].get("size")
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out, prefix=".download-") as scratch:
        _gh("release", "download", tag, "--repo", repo, "--pattern", name, "--dir", scratch)
        got = Path(scratch) / name
        if not got.is_file():
            raise Refused(f"`gh release download` left no {name}")
        if got.stat().st_size != size:
            raise Refused(f"{name} is {got.stat().st_size} bytes, the release says {size}")
        have = sha256_of(got)
        if have != want:
            raise Refused(f"{name} has sha256 {have}, the release recorded {want}")
        _gh("attestation", "verify", str(got), "-R", repo)
        target = out / LOADER_FILE
        got.replace(target)
    _log("fetched", release=tag, asset=name, sha256=want, size=size, path=str(target))
    return want


def verify_apk(apk: Path, want: str) -> None:
    if not re.fullmatch(r"[0-9a-f]{64}", want):
        raise Refused(f"--sha256 is not 64 lowercase hex digits: {want}")
    try:
        with zipfile.ZipFile(apk) as z:
            blob = z.read(LOADER_IN_APK)
    except KeyError:
        raise Refused(f"{apk.name} has no {LOADER_IN_APK}") from None
    have = hashlib.sha256(blob).hexdigest()
    if have != want:
        raise Refused(f"the loader in {apk.name} has sha256 {have}, the release asset has {want}")
    _log("apk_loader_matches_asset", apk=apk.name, sha256=have)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="quest_release_loader.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--tag", required=True)
    f.add_argument("--out", required=True, type=Path)
    f.add_argument("--repo", default=DEFAULT_REPO)
    v = sub.add_parser("verify-apk")
    v.add_argument("--apk", required=True, type=Path)
    v.add_argument("--sha256", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "fetch":
            print(fetch(args.tag, args.out, args.repo))
        else:
            verify_apk(args.apk, args.sha256)
    except (Refused, OSError, zipfile.BadZipFile) as error:
        _log("refused", command=args.command, reason=str(error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
