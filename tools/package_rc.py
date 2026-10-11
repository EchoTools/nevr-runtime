#!/usr/bin/env python3
"""Assemble a package: the Windows zip and the Quest APK, with the no-config gate.

usage: package_rc.py --commit <sha> --out <dir> --dll <BugSplat64.dll> --apk <signed.apk>
                     --pc-header <generated/nevr_builtin_defaults.h> --quest-header <generated/nevr_builtin_defaults.h>
                     --quest-build-info <generated/nevr_build_info.h> [--defaults config/public-defaults.env]

Only a CI build of a v<x.y.z>-rc.<N> tag may carry a release-candidate stamp (cmake/nevr_rc_label.cmake),
so the two paths are separate:
  package_rc.py ...        `just package-dev`, a LOCAL build: the binaries must carry `-dev+<tweak>.<sha>`;
                           one stamped -rc.<N> is refused; the output is nevr-runtime-v<x.y.z>-dev-<sha>-*
  package_rc.py tree ...   CI: gate the DLL (`-rc.<N>+` required) and write the unsigned package tree + rc.json
  package_rc.py seal ...   CI: zip the (signed) tree with a SHA256SUMS regenerated from the files as they are
                           now; a given APK must carry `-rc.<N>+` (else refused); without one the candidate
                           is zip-only and says so; write the top-level SHA256SUMS and the release notes
  package_rc.py stamp ...  CI: assert the DLL's stamped version agrees with the label the workflow computed

It does not build anything. It refuses (exit 1, nothing written) unless:
  - the DLL and the APK's sentinel library each embed exactly config/public-defaults.env
    (tools/check_embedded_defaults.py), so neither needs a config file to log in;
  - the Quest build turns login and social on by default (nevr_build_info.h kDefaultFeatures);
  - the version string `<major>.<minor>.<patch>-dev|-rc.<N>+...` and the commit sha are in the DLL and in
    the APK's sentinel library.
Publishing is not its job: it never tags or uploads.
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

PACKAGE_DIR = REPO / "tools" / "package-rc"
RC_STAMP = re.compile(rb"\d+\.\d+\.\d+-rc\.\d+\+")
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


def stamp_token(n) -> str:
    """The pre-release part of the version string: -rc.<n> for a candidate, -dev for a local build (n None)."""
    return "-dev" if n is None else f"-rc.{n}"


def kind_text(n) -> str:
    return "development build, NOT a release candidate" if n is None else f"release candidate rc.{n}"


def check_version(label: str, version: str, n, commit: str) -> None:
    """`<major>.<minor>.<patch>-rc.<n>|-dev+<tweak>.<abbreviated commit>`: the stamp and the commit are in it."""
    token = stamp_token(n)
    match = re.match(rf"^\d+\.\d+\.\d+{re.escape(token)}\+(?:\d+\.)?([0-9a-f]{{7,40}})$", version)
    if not match:
        raise GateFailure(f"{label}: version '{version}' is not <x.y.z>{token}+<...>.<commit>")
    if not commit.startswith(match.group(1)):
        raise GateFailure(f"{label}: version '{version}' names commit {match.group(1)}, not {commit[:12]}")


def gate(args, work: Path) -> dict:
    problems = []
    defaults = Path(args.defaults)
    # The Windows build embeds exactly the defaults file.
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
        marker = f"{stamp_token(args.n)}+".encode()
        if marker not in blob:
            problems.append(f"{label}: no '{stamp_token(args.n)}+' version string in the binary")
        if args.n is None and RC_STAMP.search(blob):
            problems.append(f"{label}: carries an -rc.<N> version string: a local build is never a release candidate")
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


def render(template: Path, values: dict, apk: bool = True) -> str:
    """Fill @KEY@ placeholders; a line starting with @IF_APK@ / @IF_NOAPK@ is kept (tag removed) only when
    the package has / has no Quest APK."""
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


def package_base(n, commit: str, version: str) -> str:
    """The file-name stem. X.Y.Z is read from the build's own version string (it comes from the git tag),
    never assumed: a tag v4.1.0-rc.2 packages as nevr-runtime-v4.1.0-rc.2-*."""
    match = re.match(r"^(\d+\.\d+\.\d+)", version)
    if not match:
        raise GateFailure(f"version '{version}' does not start with <x.y.z>")
    if n is None:
        return f"nevr-runtime-v{match.group(1)}-dev-{commit[:7]}"
    return f"nevr-runtime-v{match.group(1)}-rc.{n}"


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
    """The Windows half of the gate: embedded defaults, the stamp (-rc.<n> in CI), the commit."""
    problems = [f"windows: {p}" for p in defaults_check.check(Path(args.defaults), Path(args.pc_header), [dll])]
    blob = dll.read_bytes()
    if f"{stamp_token(args.n)}+".encode() not in blob:
        problems.append(f"windows: no '{stamp_token(args.n)}+' version string in the binary")
    if args.commit.encode()[:7] not in blob:
        problems.append(f"windows: commit {args.commit[:7]} is not in the binary")
    return problems


def version_in(dll: Path, n) -> str:
    token = re.escape(stamp_token(n).encode())
    match = re.search(rb"\d+\.\d+\.\d+" + token + rb"\+[0-9.a-f]+", dll.read_bytes())
    if not match:
        raise GateFailure(f"no <x.y.z>{stamp_token(n)}+<commit> version string in {dll.name}")
    return match.group().decode("ascii")


def stamp_check(dll: Path, label: str) -> str:
    """The CI assertion after the build: the stamped version agrees with the label the workflow computed.
    label `rc.<N>`: `-rc.<N>+` must be in the DLL; label empty: no -rc.<N>+ may be."""
    blob = dll.read_bytes()
    found = re.search(rb"\d+\.\d+\.\d+(?:-rc\.\d+|-dev)?\+\d+\.[0-9a-f]{7,40}", blob)
    version = found.group().decode("ascii") if found else "(no version string found)"
    if label:
        match = re.fullmatch(r"rc\.([1-9][0-9]*)", label)
        if not match:
            raise GateFailure(f"label '{label}' is not rc.<N>")
        if f"-{label}+".encode() not in blob:
            raise GateFailure(f"the build was asked for {label} but {dll.name} carries '{version}': "
                              "the tag build did not stamp the candidate")
    elif RC_STAMP.search(blob):
        raise GateFailure(f"no release-candidate label was computed but {dll.name} carries '{version}'")
    return version


def check_apk_stamp(apk: Path, n, commit: str) -> None:
    """An APK attached to a candidate must itself carry this candidate's stamp: a dev build is refused."""
    try:
        with zipfile.ZipFile(apk) as z:
            blob = z.read(SENTINEL_IN_APK)
    except KeyError:
        raise GateFailure(f"{apk.name} has no {SENTINEL_IN_APK}")
    if f"{stamp_token(n)}+".encode() not in blob:
        raise GateFailure(f"{apk.name}: its sentinel library carries no '{stamp_token(n)}+' version string; "
                          "a release candidate's APK must be stamped by a CI build of the tag")
    if commit.encode()[:7] not in blob:
        raise GateFailure(f"{apk.name}: commit {commit[:7]} is not in its sentinel library")


def write_tree(args, out: Path, version: str) -> Path:
    """The unsigned package tree: what the sign job signs. SHA256SUMS is written after signing, by seal()."""
    base = package_base(args.n, args.commit, version)
    tree = out / f"{base}-windows"
    if tree.exists():
        raise GateFailure(f"{tree} exists; not overwritten")
    tree.mkdir(parents=True)
    values = {"VERSION": version, "KIND": kind_text(args.n), "COMMIT": args.commit,
              "ZIP": f"{base}-windows.zip", "APK": f"{base}-quest.apk"}
    shutil.copyfile(args.dll, tree / "BugSplat64.dll")
    shutil.copyfile(PACKAGE_DIR / "install.ps1", tree / "install.ps1")
    shutil.copyfile(PACKAGE_DIR / "uninstall.ps1", tree / "uninstall.ps1")
    (tree / "README.txt").write_text(render(PACKAGE_DIR / "README.template.txt", values), encoding="utf-8")
    (out / "rc.json").write_text(json.dumps({"n": args.n, "commit": args.commit, "version": version,
                                              "tree": tree.name}, indent=1) + "\n", encoding="utf-8")
    return tree


def seal(tree: Path, out: Path, apk: Path = None, require_signed: bool = False) -> list:
    """Zip `tree` with a SHA256SUMS regenerated from the files as they are NOW (after signing), copy the
    APK if there is one, write the top-level SHA256SUMS and the release notes. Writes only into `out`."""
    info = json.loads((tree.parent / "rc.json").read_text(encoding="utf-8"))
    n, commit, version = info["n"], info["commit"], info["version"]
    if apk is not None:  # before anything is written: a dev or foreign APK never rides a candidate
        check_apk_stamp(apk, n, commit)
    files = sorted(p for p in tree.iterdir() if p.is_file() and p.name != "SHA256SUMS")
    if "BugSplat64.dll" not in {p.name for p in files}:
        raise GateFailure(f"{tree} has no BugSplat64.dll")
    signed_files = []
    if require_signed:
        unsigned = [p.name for p in files if p.suffix.lower() in (".dll", ".exe") and not pe_has_certificate_table(p)]
        if unsigned:
            raise GateFailure("not signed (no certificate table): " + ", ".join(unsigned))
        unsigned_scripts = [p.name for p in files if p.suffix.lower() == ".ps1" and not ps1_has_signature_block(p)]
        if unsigned_scripts:
            raise GateFailure("not signed (no signature block): " + ", ".join(unsigned_scripts))
        signed_files = [p.name for p in files if p.suffix.lower() in (".dll", ".exe", ".ps1")]
    out.mkdir(parents=True, exist_ok=True)
    base = package_base(n, commit, version)
    zip_path = out / f"{base}-windows.zip"
    apk_path = out / f"{base}-quest.apk"
    notes_path = out / "RELEASE-NOTES.md"
    sums_path = out / "SHA256SUMS"
    for target in (zip_path, apk_path if apk else None, notes_path, sums_path):
        if target is not None and target.exists():
            raise GateFailure(f"{target} exists; not overwritten")
    # The signing state is written only from what was checked: nothing here labels a file signed unless
    # --require-signed found its signature, and an unsigned package says UNSIGNED in capitals.
    if signed_files:
        signing_state = "SIGNED"
        signing_detail = ("Signed and checked when sealed (Authenticode certificate table or signature block "
                          "present): " + ", ".join(signed_files) + ".")
    else:
        signing_state = "UNSIGNED"
        signing_detail = ("No file in this package is code-signed: the policy signer is not built yet. "
                          "Windows Defender or SmartScreen may warn about it.")
    no_apk = ("\nNo Quest APK in this candidate: the release candidate is the Windows zip only "
              "(the Quest build is not part of the public set).\n") if apk is None else ""
    signing_text = f"{signing_state}\n\n{signing_detail}\n{no_apk}".encode("utf-8")
    inner = "".join(f"{sha256(p)}  {p.name}\n" for p in files)
    inner += f"{hashlib.sha256(signing_text).hexdigest()}  SIGNING.txt\n"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        entry = zipfile.ZipInfo("SIGNING.txt", date_time=(2026, 1, 1, 0, 0, 0))
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = 0o644 << 16
        z.writestr(entry, signing_text)
        for p in files:
            entry = zipfile.ZipInfo(p.name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            z.writestr(entry, p.read_bytes())
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
    values = {"VERSION": version, "KIND": kind_text(n), "COMMIT": commit, "ZIP": zip_path.name,
              "APK": apk_path.name, "SIGNING_STATE": signing_state, "SIGNING_DETAIL": signing_detail}
    notes_path.write_text(render(PACKAGE_DIR / "RELEASE-NOTES.template.md", values, apk=apk is not None),
                          encoding="utf-8")
    return written + [sums_path, notes_path]


def run_all(args) -> list:
    """`just package-dev`: the full gate for a LOCAL build (dev stamp), then the same tree and seal the CI
    uses (unsigned). Never an -rc.<N> artifact: args.n is None here."""
    with tempfile.TemporaryDirectory(prefix="package-rc-") as tmp:
        info = gate(args, Path(tmp))
        stage = Path(tmp) / "stage"
        stage.mkdir()
        tree = write_tree(args, stage, info["kVersion"])
        return seal(tree, Path(args.out), apk=Path(args.apk))


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    mode = argv.pop(0) if argv and argv[0] in ("tree", "seal", "stamp") else "all"
    parser = argparse.ArgumentParser(prog=f"package_rc.py {mode}".strip())
    if mode == "seal":
        parser.add_argument("--tree", required=True, type=Path)
        parser.add_argument("--out", required=True, type=Path)
        parser.add_argument("--apk", type=Path, default=None)
        parser.add_argument("--require-signed", action="store_true")
    elif mode == "stamp":
        parser.add_argument("--dll", required=True, type=Path)
        parser.add_argument("--label", default="")
    else:
        if mode == "tree":
            parser.add_argument("--n", type=int, required=True)
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
        args.n = None
    try:
        if mode == "stamp":
            print(f"stamped version: {stamp_check(args.dll, args.label)}")
            return 0
        if mode == "seal":
            written = seal(args.tree, args.out, apk=args.apk, require_signed=args.require_signed)
        elif mode == "tree":
            if args.n < 1:
                raise GateFailure("N must be a positive integer")
            problems = windows_problems(args, Path(args.dll))
            if problems:
                raise GateFailure("\n".join(problems))
            written = [write_tree(args, Path(args.out), version_in(Path(args.dll), args.n))]
        else:
            written = run_all(args)
    except (GateFailure, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"package_rc: FAIL:\n{error}", file=sys.stderr)
        return 1
    for path in written:
        print(f"{sha256(path) if path.is_file() else '-' * 64}  {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
