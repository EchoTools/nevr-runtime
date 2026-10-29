#!/usr/bin/env python3
"""Assemble and gate the Windows release package: the sealed zip, and the release body that describes it.

A release is a version (plain semver, taken from the tag) plus GitHub's pre-release flag. Nothing in a
file depends on that flag, so promoting a release changes no byte: this tool has no flag input.

  package_release.py tree  --version X.Y.Z --commit <sha40> --out DIR --dll BugSplat64.dll --runtime-dll nevr-runtime.dll
                           --server-exe echovr_server.exe --installer-exe nevr-install.exe --pc-header H
      CI: gate the two DLLs and write the package tree (what the signing step signs) + release.json
  package_release.py seal  --tree DIR --out DIR [--require-signed]
      CI: zip the (signed or unsigned) tree with a SHA256SUMS regenerated from the files as they are now.
      The release is that one zip (the Quest library is built by its own job); it carries no Quest APK, and
      no top-level SHA256SUMS or RELEASE-NOTES.md.
  package_release.py notes --assets DIR --out FILE --base vX.Y.Z --rev vX.Y.Z [--repo DIR]
      the release body (tools/package-release/RELEASE-BODY.template.md): a header, one line for players (from the
      commits in base..rev, tools/release_audience_section.py), and one line per file. It holds no hash: the zip's
      own SHA256SUMS and the attestation carry integrity. Read the zip's identity literals from DIR's zip.
  package_release.py stamp --dll BugSplat64.dll [--version X.Y.Z] [--commit <sha40>]
      CI: assert a DLL's one identity literal agrees with what the workflow computed (run on each of the two)
  package_release.py quest-library --lib libovrplatformloader.so --version X.Y.Z --commit <sha40>
      CI: assert the Quest library carries exactly this version and this commit. It has no identity literal:
      kVersion and the short kCommit (cmake/nevr_build_info.h.in) and the full commit of the sentinel's build
      record are separate NUL-bounded strings, so each is matched whole.
  package_release.py --commit <sha40> --out DIR --dll ... --runtime-dll ... --server-exe ... --installer-exe ... --apk ... --pc-header ...
                     --quest-header ... --quest-build-info ...        (`just package-dev`, a LOCAL build)
      the full gate for a local development build (its version is X.Y.(Z+1)-dev.N+sha, never a release),
      then the same tree + seal, with the tester Quest APK, its SHA256SUMS and notes. Never attached to a
      release.

The identity literal (src/core/build_identity.cpp) is the one string binding version and commit:
    NEVR-BUILD <version> <commit40>        (NUL-bounded, exactly one per DLL)
The package carries two DLLs of one build: BugSplat64.dll, the thin host the game loads, and nevr-runtime.dll, the
runtime the host loads (docs/adr/0012-static-install-root.md). It does not build anything and never tags or uploads.
It refuses (exit 1, nothing written) unless the runtime embeds exactly config/public-defaults.env
(tools/check_embedded_defaults.py; the host embeds no endpoints), each DLL's identity literal carries the expected
version and the build's commit and the two literals are the same, and the runtime (alone) says it is a release build.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_embedded_defaults as defaults_check  # noqa: E402
import pe_version_info  # noqa: E402
import quest_apk_version  # noqa: E402
import release_audience_section  # noqa: E402

PACKAGE_DIR = REPO / "tools" / "package-release"
BODY_TEMPLATE = PACKAGE_DIR / "RELEASE-BODY.template.md"
SENTINEL_IN_APK = "lib/arm64-v8a/libovrplatformloader.so"
# The two DLLs of the package: the host the game loads, and the runtime the host loads at the rendezvous.
HOST_DLL = "BugSplat64.dll"
RUNTIME_DLL = "nevr-runtime.dll"
# The dedicated-server launcher that rides in the Windows zip next to BugSplat64.dll.
SERVER_EXE = "echovr_server.exe"
# The core installer (installer/, ADR 0023) that install.ps1 and quest-install.ps1 call. It rides in the zip as its own
# file, so the signed manifest's `contents` for the zip covers it.
INSTALLER_EXE = "nevr-install.exe"

# The one identity literal: NUL-bounded "NEVR-BUILD <version> <commit40>".
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")
# The plugin-trust literal (ADR 0010, #741): NUL-bounded "NEVR-PLUGIN-TRUST <build>". Separate from the identity
# literal, which is a cross-repo contract and has no such field. It names the kind of build, not the mode a run
# enforces (a run's mode comes from -require-signed-plugins, #924). A packaged DLL carries exactly one, and it says
# release; dev is the mingw-plugin-dev preset and is never packaged.
PLUGIN_TRUST = re.compile(rb"(?<=\x00)NEVR-PLUGIN-TRUST ([a-z-]+)(?=\x00)")
# The host's counterpart (#1122): "NEVR-HOST-BUILD <build>". The host carries no plugin-trust literal, yet a host built
# as the plugin developers' build loads an unsigned nevr-runtime.dll, so a package needs to know which host it holds.
HOST_BUILD = re.compile(rb"(?<=\x00)NEVR-HOST-BUILD ([a-z-]+)(?=\x00)")
_NUM = r"(?:0|[1-9]\d*)"  # semver: no leading zeros
RELEASE_VERSION = re.compile(rf"^{_NUM}\.{_NUM}\.{_NUM}$")
DEV_VERSION = re.compile(rf"^{_NUM}\.{_NUM}\.{_NUM}-dev\.{_NUM}\+[0-9a-f]{{7,40}}$")


class GateFailure(Exception):
    pass


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_build_info(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    out = {}
    for name in ("kVersion", "kCommit"):
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


def plugin_trust_problems(label: str, blob: bytes) -> list:
    """The packaged DLL must say it is a release build: exactly one literal, release."""
    found = PLUGIN_TRUST.findall(blob)
    if len(found) != 1:
        return [f"{label}: expected exactly one 'NEVR-PLUGIN-TRUST <build>' literal, found {len(found)}"]
    if found[0] != b"release":
        return [f"{label}: NEVR-PLUGIN-TRUST is '{found[0].decode('ascii')}', not 'release': a dev build "
                f"(the plugin-developer preset) is never packaged"]
    return []


def host_trust_problems(label: str, blob: bytes) -> list:
    """The host loads no plugin, so a plugin-trust literal in it means runtime code moved into the host. It must say it
    is a release host: a dev host (the plugin-developer preset) loads an unsigned nevr-runtime.dll."""
    problems = []
    if PLUGIN_TRUST.search(blob):
        problems.append(f"{label}: carries a NEVR-PLUGIN-TRUST literal; the plugin loader is the runtime's, not the host's")
    found = HOST_BUILD.findall(blob)
    if len(found) != 1:
        problems.append(f"{label}: expected exactly one 'NEVR-HOST-BUILD <build>' literal, found {len(found)}")
    elif found[0] != b"release":
        problems.append(f"{label}: NEVR-HOST-BUILD is '{found[0].decode('ascii')}', not 'release': a dev host (the "
                        f"plugin-developer or scenario preset) loads an unsigned nevr-runtime.dll and is never packaged")
    return problems


def expected_identity_problems(label: str, blob: bytes, version, commit: str, runtime: bool = True) -> list:
    """`version` None: any development version (a local build); else exactly that release version. `runtime`:
    the runtime says it is a release build; the host carries no plugin-trust literal at all."""
    try:
        got_version, got_commit = identity_in(blob, label)
    except GateFailure as error:
        return [str(error)]
    problems = plugin_trust_problems(label, blob) if runtime else host_trust_problems(label, blob)
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
    match = re.match(rf"^({_NUM}\.{_NUM}\.{_NUM}-dev\.{_NUM})\+", version)
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


def windows_problems(args, dll: Path, runtime: Path) -> list:
    """The Windows half of the gate: the runtime embeds the defaults (the host embeds none), each DLL's identity
    literal (version and commit) and the two literals are one, and the VERSIONINFO of the three files at that
    version (tools/pe_version_info.py)."""
    problems = [f"windows: {p}" for p in defaults_check.check(Path(args.defaults), Path(args.pc_header), [runtime])]
    problems += expected_identity_problems(HOST_DLL, dll.read_bytes(), args.version, args.commit, runtime=False)
    problems += expected_identity_problems(RUNTIME_DLL, runtime.read_bytes(), args.version, args.commit)
    try:
        if identity_in(dll.read_bytes(), HOST_DLL) != identity_in(runtime.read_bytes(), RUNTIME_DLL):
            problems.append(f"{HOST_DLL} and {RUNTIME_DLL} carry different identity literals: not one build")
    except GateFailure:
        pass  # the literal that is missing or doubled is already a problem above
    server = Path(args.server_exe)
    if not server.is_file() or server.read_bytes()[:2] != b"MZ":
        problems.append(f"windows: {SERVER_EXE} is missing or not a PE file: {server}")
    installer = Path(args.installer_exe)
    if not installer.is_file() or installer.read_bytes()[:2] != b"MZ":
        problems.append(f"windows: {INSTALLER_EXE} is missing or not a PE file: {installer}")
    problems += [f"windows: {p}" for p in pe_version_info.check(dll, server, args.version or None, runtime)]
    return problems


def apk_version_code_problems(apk: Path, version: str) -> list:
    """The tester APK's versionCode, read from its own compiled manifest, must be the one the package's version
    gives (tools/quest_apk_version.py). The store APK's 4987566 is below the community installer's 4987570, so an
    APK that skipped the repack's stamp would be refused by a headset as a downgrade."""
    try:
        want = quest_apk_version.version_code_for(version)
        got = quest_apk_version.version_code_of_apk(apk)
    except (quest_apk_version.VersionError, quest_apk_version.ManifestError, zipfile.BadZipFile, OSError) as error:
        return [f"{apk.name}: cannot establish the APK's versionCode (AndroidManifest.xml): {error}"]
    if got != want:
        return [f"{apk.name}: AndroidManifest.xml has versionCode {got}, not {want} (the code for version {version}): "
                f"the repack did not stamp this build"]
    return []


def gate(args, work: Path) -> dict:
    """The full local gate (`just package-dev`): Windows and Quest, development version only."""
    problems = windows_problems(args, Path(args.dll), Path(args.runtime_dll))
    with zipfile.ZipFile(args.apk) as apk:
        try:
            sentinel = work / "libovrplatformloader.so"
            sentinel.write_bytes(apk.read(SENTINEL_IN_APK))
        except KeyError:
            raise GateFailure(f"the APK has no {SENTINEL_IN_APK}")
    problems += [f"quest: {p}" for p in defaults_check.check(Path(args.defaults), Path(args.quest_header), [sentinel])]
    problems += [f"quest: {p}" for p in apk_version_code_problems(Path(args.apk), read_build_info(Path(args.quest_build_info))["kVersion"])]
    info = read_build_info(Path(args.quest_build_info))
    # No config.yaml key turns the redirect, the bridge or the login on: they follow from the embedded socket URI
    # and server key. An APK that embeds neither would need a config.yaml to log in.
    embedded = defaults_check.read_header(Path(args.quest_header))
    for key, what in (("NEVR_SOCKET_URI", "socket URI"), ("NEVR_PUBLIC_SOCKET_KEY", "server key")):
        if not embedded[key]:
            problems.append(f"quest: no embedded {what}: the APK would need a config.yaml to log in")
    version, _ = identity_in(Path(args.runtime_dll).read_bytes(), "windows") if not problems else (None, None)
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


def quest_library_problems(blob: bytes, version: str, commit: str) -> list:
    """The Quest library has no identity literal. It carries kVersion, the short kCommit (cmake/nevr_build_info.h.in)
    and the sentinel's full build-record commit as separate NUL-bounded strings. A bare substring match passes
    `5.5.0-dev.4` for `5.5.0` and any string that contains the version, so each is matched whole: a string equal to
    the version, a string of 7 to 39 hex digits that is a prefix of the commit, and a string equal to the commit.
    A second NUL-bounded string equal to the version (a dependency's version) would still satisfy the first."""
    problems = []
    if b"\x00" + version.encode() + b"\x00" not in blob:
        problems.append(f"quest library: no string equal to the version '{version}'")
    hex_strings = [found.decode("ascii") for found in re.findall(rb"(?<=\x00)([0-9a-f]{7,40})(?=\x00)", blob)]
    if not any(len(found) < 40 and commit.startswith(found) for found in hex_strings):
        problems.append(f"quest library: no string of 7 to 39 hex digits that is a prefix of the commit {commit[:8]} "
                        f"(found {len(hex_strings)} hex string(s) of 7 to 40 digits)")
    if commit not in hex_strings:
        problems.append(f"quest library: no string equal to the commit {commit}")
    return problems


def stamp_check(dll: Path, version, commit: str = "") -> str:
    """The CI assertion after the build. `version` set: the identity literal must be exactly that release
    version; unset: it must be a development version. `commit` (when given): the checked-out commit."""
    problems = expected_identity_problems(dll.name, dll.read_bytes(), version, commit, runtime=dll.name != HOST_DLL)
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
    problems = apk_version_code_problems(apk, version)
    if problems:
        raise GateFailure("\n".join(problems))


def write_tree(args, out: Path, version: str) -> Path:
    """The package tree: what the signing step signs. SHA256SUMS is written after, by seal()."""
    base = package_base(version, args.commit)
    tree = out / f"{base}-windows"
    if tree.exists():
        raise GateFailure(f"{tree} exists; not overwritten")
    tree.mkdir(parents=True)
    values = {"VERSION": version, "COMMIT": args.commit,
              "ZIP": f"{base}-windows.zip", "APK": f"{base}-quest.apk"}
    shutil.copyfile(args.dll, tree / HOST_DLL)
    shutil.copyfile(args.runtime_dll, tree / RUNTIME_DLL)
    shutil.copyfile(args.server_exe, tree / SERVER_EXE)
    shutil.copyfile(args.installer_exe, tree / INSTALLER_EXE)
    shutil.copyfile(PACKAGE_DIR / "install.ps1", tree / "install.ps1")
    shutil.copyfile(PACKAGE_DIR / "uninstall.ps1", tree / "uninstall.ps1")
    shutil.copyfile(PACKAGE_DIR / "quest-diagnose.ps1", tree / "quest-diagnose.ps1")
    (tree / "README.txt").write_text(render(PACKAGE_DIR / "README.template.txt", values), encoding="utf-8")
    (out / "release.json").write_text(json.dumps({"version": version, "commit": args.commit,
                                                  "tree": tree.name}, indent=1) + "\n", encoding="utf-8")
    return tree


# The signing statement is the same text whether or not a signer has run: it states how to check, never
# which state this copy is in, so a signer replacing the signed entries never has to edit it.
SIGNING_TEXT = """Signing

The build itself signs nothing. A separate signing step may sign BugSplat64.dll, nevr-runtime.dll,
echovr_server.exe, nevr-install.exe, install.ps1, uninstall.ps1 and quest-diagnose.ps1 afterwards and replace this zip under the same name. This
text is the same either way: it says how to check, not which state your copy is in.

A file is signed when it carries a signature:
  BugSplat64.dll, nevr-runtime.dll, echovr_server.exe, nevr-install.exe
                              an Authenticode signature
                              (PowerShell: Get-AuthenticodeSignature .\\BugSplat64.dll)
  install.ps1, uninstall.ps1, quest-diagnose.ps1
                              an Authenticode signature block at the end of the script
                              (PowerShell: Get-AuthenticodeSignature .\\install.ps1)
Status "Valid" means signed; "NotSigned" means not signed.

SHA256SUMS in this zip lists the SHA-256 of every other file in it. Windows Defender or SmartScreen may
warn about a file that is not signed.
"""


def seal(tree: Path, out: Path, apk: Path = None, require_signed: bool = False) -> list:
    """Zip `tree` with a SHA256SUMS regenerated from the files as they are NOW. A release is that zip alone.
    With an APK (a local tester package only) the APK is copied too, and a top-level SHA256SUMS and the
    tester notes are written. Writes only into `out`. The output depends on the tree and its release.json
    alone: no release flag exists here."""
    info = json.loads((tree.parent / "release.json").read_text(encoding="utf-8"))
    commit, version = info["commit"], info["version"]
    if apk is not None:  # before anything is written: a foreign APK never rides a package
        check_apk_version(apk, version, commit)
    files = sorted(p for p in tree.iterdir() if p.is_file() and p.name != "SHA256SUMS")
    for required in (HOST_DLL, RUNTIME_DLL, SERVER_EXE, INSTALLER_EXE):
        if required not in {p.name for p in files}:
            raise GateFailure(f"{tree} has no {required}")
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
    for target in (zip_path, apk_path if apk else None, notes_path if apk else None, sums_path if apk else None):
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
    if apk is None:
        return [zip_path]
    shutil.copyfile(apk, apk_path)
    sums_path.write_text(f"{sha256(zip_path)}  {zip_path.name}\n{sha256(apk_path)}  {apk_path.name}\n", encoding="ascii")
    values = {"VERSION": version, "COMMIT": commit, "ZIP": zip_path.name, "APK": apk_path.name}
    notes_path.write_text(render(PACKAGE_DIR / "RELEASE-NOTES.template.md", values, apk=True), encoding="utf-8")
    return [zip_path, apk_path, sums_path, notes_path]


def quest_asset_name(version: str) -> str:
    """The Quest library's release name; tools/release_asset_names.sh `release_quest_asset_name` is the source."""
    return f"nevr-runtime-v{version}-quest-libovrplatformloader.so"


def release_notes(assets: Path, repo: Path, base: str, rev: str) -> str:
    """The release body for the two assets in `assets`: a header, the player line from the feat commits in base..rev, and
    one line per file. The version and commit are the ones the zip's own BugSplat64.dll and nevr-runtime.dll carry
    (their identity literals, which must agree), so the body cannot describe a zip that is not this release's. It
    holds no hash: the zip's SHA256SUMS and the attestation carry integrity."""
    zips = sorted(assets.glob("nevr-runtime-v*-windows.zip"))
    if len(zips) != 1:
        raise GateFailure(f"{assets}: expected exactly one nevr-runtime-v<X.Y.Z>-windows.zip, found {len(zips)}")
    zip_path = zips[0]
    match = re.fullmatch(rf"nevr-runtime-v({_NUM}\.{_NUM}\.{_NUM})-windows\.zip", zip_path.name)
    if not match:
        raise GateFailure(f"{zip_path.name}: not a release zip name (nevr-runtime-vX.Y.Z-windows.zip)")
    version = match.group(1)
    literals = {}
    with zipfile.ZipFile(zip_path) as z:
        for name in (HOST_DLL, RUNTIME_DLL):
            try:
                blob = z.read(name)
            except KeyError:
                raise GateFailure(f"{zip_path.name} has no {name}")
            literals[name] = identity_in(blob, f"{zip_path.name}:{name}")
    if literals[HOST_DLL] != literals[RUNTIME_DLL]:
        raise GateFailure(f"{zip_path.name}: {HOST_DLL} and {RUNTIME_DLL} carry different identity literals: not one build")
    found_version, commit = literals[HOST_DLL]
    if found_version != version:
        raise GateFailure(f"{zip_path.name}: its {HOST_DLL} carries version '{found_version}', not '{version}'")
    quest = assets / quest_asset_name(version)
    if not quest.is_file():
        raise GateFailure(f"{assets}: no {quest.name}")
    extra = sorted(p.name for p in assets.iterdir() if p.name not in (zip_path.name, quest.name))
    if extra:
        raise GateFailure(f"{assets}: files that are not release assets: {', '.join(extra)}")
    try:
        messages = release_audience_section.messages_between(str(repo), base, rev)
    except subprocess.CalledProcessError as error:
        raise GateFailure(f"git log {base}..{rev} in {repo} failed: {error.stderr.strip()}")
    values = {"VERSION": version, "PLAYER_LINE": release_audience_section.player_line(messages),
              "ZIP": zip_path.name, "APK": f"{package_base(version, commit)}-quest.apk", "QUEST": quest.name}
    return render(BODY_TEMPLATE, values)


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
    mode = argv.pop(0) if argv and argv[0] in ("tree", "seal", "stamp", "notes", "quest-library") else "all"
    parser = argparse.ArgumentParser(prog=f"package_release.py {mode}".strip())
    if mode == "seal":
        parser.add_argument("--tree", required=True, type=Path)
        parser.add_argument("--out", required=True, type=Path)
        parser.add_argument("--require-signed", action="store_true")
    elif mode == "notes":
        parser.add_argument("--assets", required=True, type=Path)
        parser.add_argument("--out", required=True, type=Path)
        parser.add_argument("--repo", default=".", type=Path, help="the repository the range is read from")
        parser.add_argument("--base", required=True, help="the older ref of the player line, excluded")
        parser.add_argument("--rev", required=True, help="the newer ref of the player line, included")
    elif mode == "quest-library":
        parser.add_argument("--lib", required=True, type=Path)
        parser.add_argument("--version", required=True)
        parser.add_argument("--commit", required=True)
    elif mode == "stamp":
        parser.add_argument("--dll", required=True, type=Path)
        parser.add_argument("--version", default="")
        parser.add_argument("--commit", default="")
    else:
        if mode == "tree":
            parser.add_argument("--version", required=True)
        parser.add_argument("--commit", required=True)
        parser.add_argument("--out", required=True)
        parser.add_argument("--dll", required=True, help="BugSplat64.dll, the host")
        parser.add_argument("--runtime-dll", required=True, help="nevr-runtime.dll, the runtime")
        parser.add_argument("--server-exe", required=True)
        parser.add_argument("--installer-exe", required=True, help="nevr-install.exe, the core installer (installer/)")
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
        if mode == "quest-library":
            if not RELEASE_VERSION.match(args.version):
                raise GateFailure(f"--version '{args.version}' is not X.Y.Z")
            if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
                raise GateFailure(f"--commit '{args.commit}' is not 40 hex digits")
            problems = quest_library_problems(args.lib.read_bytes(), args.version, args.commit)
            if problems:
                raise GateFailure("\n".join(problems))
            print(f"quest library: version {args.version} commit {args.commit[:8]}")
            return 0
        if mode == "stamp":
            if args.version and not RELEASE_VERSION.match(args.version):
                raise GateFailure(f"--version '{args.version}' is not X.Y.Z")
            print(f"stamped identity: {stamp_check(args.dll, args.version or None, args.commit)}")
            return 0
        if mode == "notes":
            if args.out.exists():
                raise GateFailure(f"{args.out} exists; not overwritten")
            body = release_notes(args.assets, args.repo, args.base, args.rev)
            args.out.write_text(body, encoding="utf-8")
            written = [args.out]
        elif mode == "seal":
            written = seal(args.tree, args.out, require_signed=args.require_signed)
        elif mode == "tree":
            if not RELEASE_VERSION.match(args.version):
                raise GateFailure(f"--version '{args.version}' is not X.Y.Z")
            problems = windows_problems(args, Path(args.dll), Path(args.runtime_dll))
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
