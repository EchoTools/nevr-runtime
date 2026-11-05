#!/usr/bin/env python3
"""Check everything a release needs, up front, before anything is built or published (#1997).

    just release-check

It prints one `release-check: PROBLEM: ...` line for each thing that is missing or wrong (it never stops at the first)
and exits 1, or prints `release-check: OK` and exits 0. It changes nothing in the clone: it reads the environment
(`just` loads `.env` with `set dotenv-load`), stats files, and runs `tools/release_preflight.py` (which appends one record
to its own log). It never prints a value: a line it passes through from the preflight has every URL's userinfo
redacted.

Paths are taken as written. `just` does not expand `~` in `.env`, and neither do the release recipes, so neither does
this check. The Quest release timer and the beta rollback (tools/quest_release_timer.py, tools/rollback_beta.py) read
QUEST_RELEASE_SIGNER_REPO; they are out of scope here, and #1997 removes them.

What it checks:
  1. this clone is exactly origin/main (tools/release_preflight.py, every one of its checks);
  2. every input in INPUTS is set and not empty;
  3. every input that names a file names an existing, readable file (the signer: an executable one, or a name on PATH);
  4. every program in PROGRAMS is on PATH.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from release_preflight import redact_url  # noqa: E402

# Every input a release reads, once each: (name, is it a file, what it is). `.env.example` lists the same names.
INPUTS: tuple[tuple[str, bool, str], ...] = (
    ("NEVR_QUEST_REFERENCE_APK", True, "the reference base APK the Quest release APK is repacked from"),
    ("NEVR_QUEST_KEYSTORE", True, "the Quest APK signing keystore"),
    ("NEVR_QUEST_KEYSTORE_PASS", False, "the Quest keystore's store passphrase"),
    ("R2_ENDPOINT", False, "the release host's S3 endpoint"),
    ("R2_BUCKET", False, "the release host's bucket"),
    ("R2_ACCESS_KEY_ID", False, "the release host's access key id"),
    ("R2_SECRET_ACCESS_KEY", False, "the release host's secret key"),
    ("NEVR_RCLONE_REMOTE", False, "the rclone remote name for the bucket"),
    ("RELEASE_PUBLIC_BASE", False, "the release host's public base URL"),
    ("RELEASE_SIGNER_BIN", True, "the release-signer executable"),
    ("RELEASE_SIGNER_DIR", False, "the release-signer checkout"),
    ("GH_TOKEN", False, "a GitHub token for the release repository"),
)

# The programs the release path runs (AGENTS.md, "Quest APK", plus the publish and the preflight).
PROGRAMS: tuple[str, ...] = (
    "git", "gh", "python3", "rclone", "unzip", "zip", "zipalign", "apksigner", "aapt2", "keytool", "patchelf",
    "readelf", "nm",
)


def problems(env: dict[str, str], root: pathlib.Path, run_preflight: bool = True) -> list[str]:
    """Every problem with the release's inputs, in a fixed order. Empty means ready."""
    out: list[str] = []
    if run_preflight:
        pre = subprocess.run([sys.executable, str(root / "tools" / "release_preflight.py")], cwd=root,
                             capture_output=True, text=True, env=env)
        if pre.returncode != 0:
            for line in (pre.stdout + pre.stderr).splitlines():
                if "PROBLEM" in line:
                    out.append(f"preflight: {redact_url(line.split('PROBLEM:', 1)[-1].strip())}")
            if not out:
                out.append(f"preflight: tools/release_preflight.py exited {pre.returncode}")
    for name, is_file, what in INPUTS:
        value = env.get(name, "")
        if not value:
            out.append(f"{name} is not set ({what}); add it to .env")
            continue
        if is_file:
            if name == "RELEASE_SIGNER_BIN":
                found = shutil.which(value, path=env.get("PATH"))
                if found is None:
                    out.append(f"{name} names no executable file and no program on PATH ({what})")
                continue
            path = pathlib.Path(value)
            if not path.is_file():
                out.append(f"{name} names no file ({what})")
            elif not os.access(path, os.R_OK):
                out.append(f"{name} names a file this user can't read ({what})")
    for program in PROGRAMS:
        if shutil.which(program, path=env.get("PATH")) is None:
            out.append(f"{program} is not on PATH")
    return out


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    found = problems(dict(os.environ), root)
    for problem in found:
        print(f"release-check: PROBLEM: {problem}")
    if found:
        print(f"release-check: {len(found)} problem(s); nothing was built or published")
        return 1
    print("release-check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
