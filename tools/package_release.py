#!/usr/bin/env python3
"""Assemble and gate a Windows release package: the sealed zip, SHA256SUMS and RELEASE-NOTES.md.

A release is a version (plain semver, taken from the tag) plus GitHub's pre-release flag. Nothing in a
file depends on that flag, so promoting a release changes no byte: this tool has no flag input.

  package_release.py tree  --version X.Y.Z --commit <sha40> --out DIR --dll BugSplat64.dll --pc-header H
      CI: gate the DLL and write the package tree (what the signing step signs) + release.json
  package_release.py seal  --tree DIR --out DIR [--require-signed]
      CI: zip the (signed or unsigned) tree with a SHA256SUMS regenerated from the files as they are now,
      write the top-level SHA256SUMS and RELEASE-NOTES.md. The release carries no Quest APK.
  package_release.py stamp --dll BugSplat64.dll [--version X.Y.Z] [--commit <sha40>]
      CI: assert the DLL's one identity literal agrees with what the workflow computed
  package_release.py --commit <sha40> --out DIR --dll ... --apk ... --pc-header ... --quest-header ...
                     --quest-build-info ...        (`just package-dev`, a LOCAL build)
      the full gate for a local development build (its version is X.Y.(Z+1)-dev.N+sha, never a release),
      then the same tree + seal, with the tester Quest APK. Never attached to a release.

The identity literal (src/core/build_identity.cpp) is the one string binding version and commit:
    NEVR-BUILD <version> <commit40>        (NUL-bounded, exactly one per DLL)
It does not build anything and never tags or uploads. It refuses (exit 1, nothing written) unless the
DLL embeds exactly config/public-defaults.env (tools/check_embedded_defaults.py) and its identity literal
carries the expected version and the build's commit.
"""

import argparse
import hashlib
import json
import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_embedded_defaults as defaults_check  # noqa: E402

PACKAGE_DIR = REPO / "tools" / "package-release"
SENTINEL_IN_APK = "lib/arm64-v8a/libovrplatformloader.so"
REQUIRED_FEATURES = ("redirect", "bridge", "login", "social")

# The one identity literal: NUL-bounded "NEVR-BUILD <version> <commit40>".
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")
RELEASE_VERSION = re.compile(r"^\d+\.\d+\.\d+$")
DEV_VERSION = re.compile(r"^\d+\.\d+\.\d+-dev\.\d+\+[0-9a-f]{7,40}$")


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


def identity_in(blob: bytes, label: str) -> tuple:
    """(version, commit40) of the binary's single identity literal; none or several is a refusal."""
    found = IDENTITY.findall(blob)
    if len(found) != 1:
        raise GateFailure(f"{label}: expected exactly one 'NEVR-BUILD <version> <commit40>' literal, found {len(found)}")
    version, commit = found[0]
    return version.decode("ascii"), commit.decode("ascii")


def expected_identity_problems(label: str, blob: bytes, version, commit: str) -> list:
    """`version` None: any development version (a local build); else exactly that release version."""
    try:
        got_version, got_commit = identity_in(blob, label)
    except GateFailure as error:
        return [str(error)]
    problems = []
    if version is None:
        if not DEV_VERSION.match(got_version):
            problems.append(f"{label}: version '{got_version}' is not a development version "
                            f"(<x.y.z>-dev.<n>+<sha>): a local build is never a release")
    elif got_version != version:
        problems.append(f"{label}: version '{got_version}' is not the expected '{version}'")
    if commit and got_commit != commit:
        problems.append(f"{label}: commit {got_commit[:12]} is not the checked-out commit {commit[:12]}")
    return problems


def package_base(version: str, commit: str) -> str:
    """The file-name stem. A release is nevr-runtime-v<X.Y.Z>; a development build is
    nevr-runtime-v<X.Y.Z-dev.N>-<sha7> (the build metadata dropped, the commit kept)."""
    if RELEASE_VERSION.match(version):
        return f"nevr-runtime-v{version}"
    match = re.match(r"^(\d+\.\d+\.\d+-dev\.\d+)\+", version)
    if not match:
        raise GateFailure(f"version '{version}' is neither X.Y.Z nor X.Y.Z-dev.N+sha")
    return f"nevr-runtime-v{match.group(1)}-{commit[:7]}"


def render(template: Path, values: dict, apk: bool = False) -> str:
    """Fill @KEY@ placeholders; a line starting with @IF_APK@ / @IF_NOAPK@ is kept (tag removed) only when
    the package has / has no Quest APK. Nothing here reads a release flag."""
    out = []
    for line in template.read_text(encoding="utf-8").splitlines(keepends=True):
        if line.startswith("@IF_APK@"):
            if apk:
                out.append(line[len("@IF_APK@"):])
        elif line.startswith("@IF_NOAPK@"):
            if not apk:
                out.append(line[len("@IF_NOAPK@"):])
        else:
            out.append(line)
    text = "".join(out)
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    return text


def pe_has_certificate_table(path: Path) -> bool:
    """True when the PE file carries an Authenticode certificate table (a signed file); not a validation."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe:pe + 4] != b"PE\0\0":
        return False
    optional = pe + 24
    magic = int.from_bytes(data[optional:optional + 2], "little")
    directories = optional + (112 if magic == 0x20B else 96)
    entry = directories + 8 * 4  # IMAGE_DIRECTORY_ENTRY_SECURITY
    size = int.from_bytes(data[entry + 4:entry + 8], "little")
    return size > 0


def ps1_has_signature_block(path: Path) -> bool:
    """True when a PowerShell script ends in an Authenticode signature block; not a validation."""
    return b"# SIG # Begin signature block" in path.read_bytes()


def windows_problems(args, dll: Path) -> list:
    """The Windows half of the gate: embedded defaults, then the identity literal (version and commit)."""
    problems = [f"windows: {p}" for p in defaults_check.check(Path(args.defaults), Path(args.pc_header), [dll])]
    problems += expected_identity_problems("windows", dll.read_bytes(), args.version, args.commit)
    return problems


def gate(args, work: Path) -> dict:
    """The full local gate (`just package-dev`): Windows and Quest, development version only."""
    problems = windows_problems(args, Path(args.dll))
    with zipfile.ZipFile(args.apk) as apk:
        try:
            sentinel = work / "libovrplatformloader.so"
            sentinel.write_bytes(apk.read(SENTINEL_IN_APK))
        except KeyError:
            raise GateFailure(f"the APK has no {SENTINEL_IN_APK}")
    problems += [f"quest: {p}" for p in defaults_check.check(Path(args.defaults), Path(args.quest_header), [sentinel])]
    info = read_build_info(Path(args.quest_build_info))
    features = set(filter(None, info["kDefaultFeatures"].split(",")))
    for feature in REQUIRED_FEATURES:
        if feature not in features:
            problems.append(f"quest: feature {feature} is not on by default: the APK would need a nevr-quest.json")
    version, _ = identity_in(Path(args.dll).read_bytes(), "windows") if not problems else (None, None)
    if version is not None:
        if info["kVersion"] != version:
            problems.append(f"quest: build info version '{info['kVersion']}' is not the DLL's '{version}'")
        if version.encode() not in sentinel.read_bytes():
            problems.append(f"quest: version {version} is not in the sentinel library")
        if args.commit[:7].encode() not in sentinel.read_bytes():
            problems.append(f"quest: commit {args.commit[:7]} is not in the sentinel library")
    if problems:
        raise GateFailure("\n".join(problems))
    return info


def stamp_check(dll: Path, version, commit: str = "") -> str:
    """The CI assertion after the build. `version` set: the identity literal must be exactly that release
    version; unset: it must be a development version. `commit` (when given): the checked-out commit."""
    problems = expected_identity_problems(dll.name, dll.read_bytes(), version, commit)
    if problems:
        raise GateFailure("\n".join(problems))
    found_version, found_commit = identity_in(dll.read_bytes(), dll.name)
    return f"NEVR-BUILD {found_version} {found_commit}"


def check_apk_version(apk: Path, version: str, commit: str) -> None:
    """A tester APK (local package only) must carry the package's own version and commit."""
    try:
        with zipfile.ZipFile(apk) as z:
            blob = z.read(SENTINEL_IN_APK)
    except KeyError:
        raise GateFailure(f"{apk.name} has no {SENTINEL_IN_APK}")
    if version.encode() not in blob:
        raise GateFailure(f"{apk.name}: its sentinel library does not carry version '{version}'")
    if commit.encode()[:7] not in blob:
        raise GateFailure(f"{apk.name}: commit {commit[:7]} is not in its sentinel library")


def write_tree(args, out: Path, version: str) -> Path:
    """The package tree: what the signing step signs. SHA256SUMS is written after, by seal()."""
    base = package_base(version, args.commit)
    tree = out / f"{base}-windows"
    if tree.exists():
        raise GateFailure(f"{tree} exists; not overwritten")
    tree.mkdir(parents=True)
    values = {"VERSION": version, "COMMIT": args.commit,
              "ZIP": f"{base}-windows.zip", "APK": f"{base}-quest.apk"}
    shutil.copyfile(args.dll, tree / "BugSplat64.dll")
    shutil.copyfile(PACKAGE_DIR / "install.ps1", tree / "install.ps1")
    shutil.copyfile(PACKAGE_DIR / "uninstall.ps1", tree / "uninstall.ps1")
    (tree / "README.txt").write_text(render(PACKAGE_DIR / "README.template.txt", values), encoding="utf-8")
    (out / "release.json").write_text(json.dumps({"version": version, "commit": args.commit,
                                                  "tree": tree.name}, indent=1) + "\n", encoding="utf-8")
    return tree


# The signing statement is the same text whether or not a signer has run: it states how to check, never
# which state this copy is in, so a signer replacing the signed entries never has to edit it.
SIGNING_TEXT = """Signing

This package is built by CI from a tag; the build itself signs nothing. A separate signing step may sign
BugSplat64.dll, install.ps1 and uninstall.ps1 afterwards and replace this zip under the same name. This
text is the same either way: it says how to check, not which state your copy is in.

A file is signed when it carries a signature:
  BugSplat64.dll              an Authenticode signature
                              (PowerShell: Get-AuthenticodeSignature .\\BugSplat64.dll)
  install.ps1, uninstall.ps1  an Authenticode signature block at the end of the script
                              (PowerShell: Get-AuthenticodeSignature .\\install.ps1)
Status "Valid" means signed; "NotSigned" means not signed.

SHA256SUMS in this zip lists the SHA-256 of every other file in it. Windows Defender or SmartScreen may
warn about a file that is not signed.
"""


def seal(tree: Path, out: Path, apk: Path = None, require_signed: bool = False) -> list:
    """Zip `tree` with a SHA256SUMS regenerated from the files as they are NOW, copy the APK if there is one
    (a local tester package only), write the top-level SHA256SUMS and the release notes. Writes only into
    `out`. The output depends on the tree and its release.json alone: no release flag exists here."""
    info = json.loads((tree.parent / "release.json").read_text(encoding="utf-8"))
    commit, version = info["commit"], info["version"]
    if apk is not None:  # before anything is written: a foreign APK never rides a package
        check_apk_version(apk, version, commit)
    files = sorted(p for p in tree.iterdir() if p.is_file() and p.name != "SHA256SUMS")
    if "BugSplat64.dll" not in {p.name for p in files}:
        raise GateFailure(f"{tree} has no BugSplat64.dll")
    if require_signed:
        unsigned = [p.name for p in files if p.suffix.lower() in (".dll", ".exe") and not pe_has_certificate_table(p)]
        if unsigned:
            raise GateFailure("not signed (no certificate table): " + ", ".join(unsigned))
        unsigned_scripts = [p.name for p in files if p.suffix.lower() == ".ps1" and not ps1_has_signature_block(p)]
        if unsigned_scripts:
            raise GateFailure("not signed (no signature block): " + ", ".join(unsigned_scripts))
    out.mkdir(parents=True, exist_ok=True)
    base = package_base(version, commit)
    zip_path = out / f"{base}-windows.zip"
    apk_path = out / f"{base}-quest.apk"
    notes_path = out / "RELEASE-NOTES.md"
    sums_path = out / "SHA256SUMS"
    for target in (zip_path, apk_path if apk else None, notes_path, sums_path):
        if target is not None and target.exists():
            raise GateFailure(f"{target} exists; not overwritten")
    signing_text = SIGNING_TEXT.encode("utf-8")
    inner = "".join(f"{sha256(p)}  {p.name}\n" for p in files)
    inner += f"{hashlib.sha256(signing_text).hexdigest()}  SIGNING.txt\n"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in [("SIGNING.txt", signing_text)] + [(p.name, p.read_bytes()) for p in files]:
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            z.writestr(entry, data)
        entry = zipfile.ZipInfo("SHA256SUMS", date_time=(2026, 1, 1, 0, 0, 0))
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = 0o644 << 16
        z.writestr(entry, inner.encode("ascii"))
    lines = [f"{sha256(zip_path)}  {zip_path.name}\n"]
    written = [zip_path]
    if apk is not None:
        shutil.copyfile(apk, apk_path)
        lines.append(f"{sha256(apk_path)}  {apk_path.name}\n")
        written.append(apk_path)
    sums_path.write_text("".join(lines), encoding="ascii")
    values = {"VERSION": version, "COMMIT": commit, "ZIP": zip_path.name, "APK": apk_path.name}
    notes_path.write_text(render(PACKAGE_DIR / "RELEASE-NOTES.template.md", values, apk=apk is not None),
                          encoding="utf-8")
    return written + [sums_path, notes_path]


def run_all(args) -> list:
    """`just package-dev`: the full gate for a LOCAL build (development version), then the same tree and
    seal the CI uses. Never a release: args.version is None here."""
    with tempfile.TemporaryDirectory(prefix="package-release-") as tmp:
        info = gate(args, Path(tmp))
        stage = Path(tmp) / "stage"
        stage.mkdir()
        tree = write_tree(args, stage, info["kVersion"])
        return seal(tree, Path(args.out), apk=Path(args.apk))


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    mode = argv.pop(0) if argv and argv[0] in ("tree", "seal", "stamp") else "all"
    parser = argparse.ArgumentParser(prog=f"package_release.py {mode}".strip())
    if mode == "seal":
        parser.add_argument("--tree", required=True, type=Path)
        parser.add_argument("--out", required=True, type=Path)
        parser.add_argument("--require-signed", action="store_true")
    elif mode == "stamp":
        parser.add_argument("--dll", required=True, type=Path)
        parser.add_argument("--version", default="")
        parser.add_argument("--commit", default="")
    else:
        if mode == "tree":
            parser.add_argument("--version", required=True)
        parser.add_argument("--commit", required=True)
        parser.add_argument("--out", required=True)
        parser.add_argument("--dll", required=True)
        parser.add_argument("--pc-header", required=True)
        parser.add_argument("--defaults", default=str(REPO / "config" / "public-defaults.env"))
        if mode == "all":
            parser.add_argument("--apk", required=True)
            parser.add_argument("--quest-header", required=True)
            parser.add_argument("--quest-build-info", required=True)
    args = parser.parse_args(argv)
    if mode == "all":
        args.version = None
    try:
        if mode == "stamp":
            if args.version and not RELEASE_VERSION.match(args.version):
                raise GateFailure(f"--version '{args.version}' is not X.Y.Z")
            print(f"stamped identity: {stamp_check(args.dll, args.version or None, args.commit)}")
            return 0
        if mode == "seal":
            written = seal(args.tree, args.out, require_signed=args.require_signed)
        elif mode == "tree":
            if not RELEASE_VERSION.match(args.version):
                raise GateFailure(f"--version '{args.version}' is not X.Y.Z")
            problems = windows_problems(args, Path(args.dll))
            if problems:
                raise GateFailure("\n".join(problems))
            written = [write_tree(args, Path(args.out), args.version)]
        else:
            written = run_all(args)
    except (GateFailure, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"package_release: FAIL:\n{error}", file=sys.stderr)
        return 1
    for path in written:
        print(f"{sha256(path) if path.is_file() else '-' * 64}  {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
