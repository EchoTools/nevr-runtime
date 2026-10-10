#!/usr/bin/env python3
"""Assemble a release candidate: the Windows zip and the Quest APK, with the no-config gate.

usage: package_rc.py --n <N> --commit <sha> --out <dir> --dll <BugSplat64.dll> --apk <signed.apk>
                     --pc-header <generated/nevr_builtin_defaults.h> --quest-header <generated/nevr_builtin_defaults.h>
                     --quest-build-info <generated/nevr_build_info.h> [--defaults config/public-defaults.env]

It does not build anything. It refuses (exit 1, nothing written) unless:
  - the DLL and the APK's sentinel library each embed exactly the committed public defaults
    (tools/check_embedded_defaults.py), so neither needs a config file to log in;
  - the Quest build turns login and social on by default (nevr_build_info.h kDefaultFeatures);
  - the version string `<major>.<minor>.<patch>-rc.<N>+...` and the commit sha are in the DLL and in
    the APK's sentinel library.
It then writes <out>/nevr-runtime-v4.0.0-rc.<N>-windows.zip, ...-quest.apk, SHA256SUMS and RELEASE-NOTES.md.
Publishing is not its job: it never tags or uploads.
"""

import argparse
import hashlib
import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_embedded_defaults as defaults_check  # noqa: E402

PACKAGE_DIR = REPO / "tools" / "package-rc"
SENTINEL_IN_APK = "lib/arm64-v8a/libovrplatformloader.so"
REQUIRED_FEATURES = ("redirect", "bridge", "login", "social")


class GateFailure(Exception):
    pass


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_build_info(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    out = {}
    for name in ("kVersion", "kCommit", "kDefaultFeatures"):
        match = re.search(rf'\b{name}\s*=\s*"([^"]*)";', text)
        if not match:
            raise GateFailure(f"{path.name}: {name} not found")
        out[name] = match.group(1)
    return out


def check_version(label: str, version: str, n: int, commit: str) -> None:
    """`<major>.<minor>.<patch>-rc.<n>+<tweak>.<abbreviated commit>`: the label and the commit are in it."""
    match = re.match(rf"^\d+\.\d+\.\d+-rc\.{n}\+(?:\d+\.)?([0-9a-f]{{7,40}})$", version)
    if not match:
        raise GateFailure(f"{label}: version '{version}' is not <x.y.z>-rc.{n}+<...>.<commit>")
    if not commit.startswith(match.group(1)):
        raise GateFailure(f"{label}: version '{version}' names commit {match.group(1)}, not {commit[:12]}")


def gate(args, work: Path) -> dict:
    problems = []
    defaults = Path(args.defaults)
    # The Windows build embeds exactly the committed file.
    problems += [f"windows: {p}" for p in defaults_check.check(defaults, Path(args.pc_header), [Path(args.dll)])]
    # The Quest build embeds it too, in the sentinel library inside the APK.
    with zipfile.ZipFile(args.apk) as apk:
        try:
            sentinel = work / "libovrplatformloader.so"
            sentinel.write_bytes(apk.read(SENTINEL_IN_APK))
        except KeyError:
            raise GateFailure(f"the APK has no {SENTINEL_IN_APK}")
    problems += [f"quest: {p}" for p in defaults_check.check(defaults, Path(args.quest_header), [sentinel])]
    # No config file may be needed to log in on Quest: the build turns the login features on itself.
    info = read_build_info(Path(args.quest_build_info))
    features = set(filter(None, info["kDefaultFeatures"].split(",")))
    for feature in REQUIRED_FEATURES:
        if feature not in features:
            problems.append(f"quest: feature {feature} is not on by default: the APK would need a nevr-quest.json")
    # The version string and commit are in both artifacts.
    for label, blob, version in (("windows", Path(args.dll).read_bytes(), None),
                                 ("quest", sentinel.read_bytes(), info["kVersion"])):
        marker = f"-rc.{args.n}+".encode()
        if marker not in blob:
            problems.append(f"{label}: no '-rc.{args.n}+' version string in the binary")
        if args.commit.encode()[:7] not in blob:
            problems.append(f"{label}: commit {args.commit[:7]} is not in the binary")
    if info["kVersion"]:
        try:
            check_version("quest build info", info["kVersion"], args.n, args.commit)
        except GateFailure as error:
            problems.append(str(error))
    if problems:
        raise GateFailure("\n".join(problems))
    return info


def render(template: Path, values: dict) -> str:
    text = template.read_text(encoding="utf-8")
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    return text


def assemble(args, info: dict) -> list:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    base = f"nevr-runtime-v4.0.0-rc.{args.n}"
    zip_path = out / f"{base}-windows.zip"
    apk_path = out / f"{base}-quest.apk"
    version = info["kVersion"]
    values = {"VERSION": version, "RC": f"rc.{args.n}", "COMMIT": args.commit,
              "ZIP": zip_path.name, "APK": apk_path.name}
    for target in (zip_path, apk_path):
        if target.exists():
            raise GateFailure(f"{target} exists; not overwritten")
    members = {
        "BugSplat64.dll": Path(args.dll).read_bytes(),
        "install.ps1": (PACKAGE_DIR / "install.ps1").read_bytes(),
        "uninstall.ps1": (PACKAGE_DIR / "uninstall.ps1").read_bytes(),
        "README.txt": render(PACKAGE_DIR / "README.template.txt", values).encode("utf-8"),
    }
    sums = "".join(f"{hashlib.sha256(members['BugSplat64.dll']).hexdigest()}  BugSplat64.dll\n"
                   f"{hashlib.sha256(members['install.ps1']).hexdigest()}  install.ps1\n"
                   f"{hashlib.sha256(members['uninstall.ps1']).hexdigest()}  uninstall.ps1\n"
                   f"{hashlib.sha256(members['README.txt']).hexdigest()}  README.txt\n")
    members["SHA256SUMS"] = sums.encode("ascii")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in ("BugSplat64.dll", "install.ps1", "uninstall.ps1", "README.txt", "SHA256SUMS"):
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            z.writestr(entry, members[name])
    shutil.copyfile(args.apk, apk_path)
    (out / "SHA256SUMS").write_text(f"{sha256(zip_path)}  {zip_path.name}\n{sha256(apk_path)}  {apk_path.name}\n",
                                    encoding="ascii")
    (out / "RELEASE-NOTES.md").write_text(render(PACKAGE_DIR / "RELEASE-NOTES.template.md", values), encoding="utf-8")
    return [zip_path, apk_path, out / "SHA256SUMS", out / "RELEASE-NOTES.md"]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--n", type=int, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--apk", required=True)
    parser.add_argument("--pc-header", required=True)
    parser.add_argument("--quest-header", required=True)
    parser.add_argument("--quest-build-info", required=True)
    parser.add_argument("--defaults", default=str(REPO / "config" / "public-defaults.env"))
    args = parser.parse_args(argv)
    if args.n < 1:
        print("package_rc: FAIL: N must be a positive integer", file=sys.stderr)
        return 1
    try:
        with tempfile.TemporaryDirectory(prefix="package-rc-") as tmp:
            info = gate(args, Path(tmp))
            written = assemble(args, info)
    except (GateFailure, OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"package_rc: FAIL:\n{error}", file=sys.stderr)
        return 1
    for path in written:
        print(f"{sha256(path)}  {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
