#!/usr/bin/env python3
"""The versionCode of the Quest APK: the numbering rule, a strict reader and writer of the binary manifest.

WHY. Android refuses to install an APK whose versionCode is lower than the installed one
(INSTALL_FAILED_VERSION_DOWNGRADE). The store APK that `just android-repack-apk` repacks is versionCode
4987566. The community Quest installer pins game build 4987570, so a headset it set up could not take our APK
even after the signer difference was dealt with. The repack therefore sets the versionCode itself, at repack
time, to a number derived from the build's own version, above 4987570 and growing with every build.

NUMBERING RULE (the same on every machine, derived from the version string alone):

    versionCode = major * 10^7 + minor * 10^5 + patch * 10^3 + slot

    a release       X.Y.Z                  slot 999
    a development   X.Y.Z-dev.N+<sha>      slot N      (N = commits since the nearest release tag, 0..998)

  The version is the one cmake/set_project_version_from_git.cmake writes into the build (kVersion in
  nevr_build_info.h). That makes the codes follow semantic-version precedence: a development build of X.Y.Z sits
  between the release before it and the release X.Y.Z (semver item 11), so a tester moving from a development
  build to its release, or to the next development build, is always an upgrade.

  Limits, each refused loudly NAMING THE FIELD, never clamped: major 0..209, minor 0..99, patch 0..99, development
  distance N 0..998. The largest code, 209.99.99, is 2,090,000,000 + 9,900,000 + 99,000 + 999 = 2,099,999,999,
  inside Android's int32 (2,147,483,647) and under the 2,100,000,000 that the Play Store documents as its ceiling.
  `field_warnings` (used by tools/release_preflight.py) warns early: development distance N above 900, minor or
  patch above 90. A version below the last published release's major (LAST_RELEASE, 5.6.0) is refused: a build
  with no git reports 1.0.0, and building it would be a downgrade. From major 5 up every code is at least
  50,000,000, ten times the installer's 4987570 (5.0.0-dev.0 is the smallest accepted).

  Two development builds from different branches at the same distance N have the same versionCode (the code
  carries no commit); the commit is in the package's file name.

  versionName is left as the store APK has it (the game's own build, "4987566"): it names the game, the
  versionCode names our packaging of it.

THE MANIFEST. An APK holds AndroidManifest.xml as Android's compiled XML. versionCode there is one 4-byte
integer attribute of the root <manifest> element. Setting it is a 4-byte write: nothing in the string pool, the
resource map, the chunk sizes or any other attribute moves. The reader/writer below refuses unless the field is
exactly where Android looks for it: the first element is `manifest`, exactly one attribute on it carries the
resource id 0x0101021b (versionCode) in the android namespace, it is a decimal integer with no text twin, no
versionCodeMajor is present (it would change the code Android reports), and every chunk is well formed and
accounted for. After the write the manifest is parsed again and must differ from the original in those 4
bytes and nowhere else.

  quest_apk_version.py code <version>                       print the versionCode for a version
  quest_apk_version.py read <apk>                           print the versionCode in an APK's manifest
  quest_apk_version.py patch-manifest <AndroidManifest.xml> --build-info <nevr_build_info.h> --shim <so>
                                                            set the versionCode in an extracted manifest, in place;
                                                            print it. The version is the build info's, and the shim
                                                            must carry the same version string (a stale header
                                                            cannot stamp a different build's shim)

One JSON line per action goes to stderr; a refusal is one JSON line and exit 1.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

# The game build the community installer pins (Quest-EchoVR-Installer, Download.cs EvrVersion).
INSTALLER_VERSION_CODE = 4987570
# The last published release's version, pinned here because a build with no git cannot ask (tools/tests pins it to the
# repository's newest release tag). A version below its major is refused: with no git the build reports 1.0.0
# (cmake/set_project_version_from_git.cmake), which is a well-formed release version whose code, 10,000,999, is far
# below every published build's. Every code from major 5 up is at least 50,000,000, above INSTALLER_VERSION_CODE.
LAST_RELEASE = "5.6.0"
MIN_MAJOR = int(LAST_RELEASE.split(".")[0])
# Android's manifest versionCode is an int32; the Play Store documents 2,100,000,000 as the ceiling.
MAX_VERSION_CODE = 2_100_000_000

MAX_MAJOR, MAX_MINOR, MAX_PATCH = 209, 99, 99
RELEASE_SLOT = 999
MAX_DEV_N = RELEASE_SLOT - 1  # so that slot 999 is only ever a release
# The early warning of field_warnings: development distance N above this, minor or patch above the other.
WARN_DEV_N = 900
WARN_MINOR_PATCH = 90

_NUM = r"(0|[1-9][0-9]*)"
VERSION = re.compile(rf"{_NUM}\.{_NUM}\.{_NUM}(?:-dev\.{_NUM}\+[0-9a-f]{{7,40}})?")

ANDROID_NS = "http://schemas.android.com/apk/res/android"
RES_ID_VERSION_CODE = 0x0101021B
RES_ID_VERSION_CODE_MAJOR = 0x01010576
MANIFEST_ENTRY = "AndroidManifest.xml"

# Android compiled XML (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h).
RES_STRING_POOL = 0x0001
RES_XML = 0x0003
RES_XML_START_NAMESPACE = 0x0100
RES_XML_END_NAMESPACE = 0x0101
RES_XML_START_ELEMENT = 0x0102
RES_XML_END_ELEMENT = 0x0103
RES_XML_CDATA = 0x0104
RES_XML_RESOURCE_MAP = 0x0180
TYPE_INT_DEC = 0x10
NO_INDEX = 0xFFFFFFFF
UTF8_FLAG = 0x100


class VersionError(Exception):
    """A version string the numbering rule does not accept."""


class ManifestError(Exception):
    """A manifest the validator does not accept, or a patch that did not land as intended."""


# ---------------------------------------------------------------------------------------------------------------
# The numbering rule
# ---------------------------------------------------------------------------------------------------------------

def _parts(version: str):
    """(major, minor, patch, N or None for a release), or None when the string is neither X.Y.Z nor X.Y.Z-dev.N+sha."""
    match = VERSION.fullmatch(version)
    if not match:
        return None
    major, minor, patch = (int(match.group(i)) for i in (1, 2, 3))
    return major, minor, patch, (None if match.group(4) is None else int(match.group(4)))


def field_problems(version: str) -> list:
    """One message per part of `version` outside the rule's limits, each naming the field. Empty for a version that
    does not parse (version_code_for is what refuses that)."""
    parts = _parts(version)
    if parts is None:
        return []
    major, minor, patch, n = parts
    fields = [("major", major, MAX_MAJOR), ("minor", minor, MAX_MINOR), ("patch", patch, MAX_PATCH)]
    if n is not None:
        fields.append(("development distance N", n, MAX_DEV_N))
    return [f"version '{version}': {name} {value} is above {limit}, the largest the versionCode rule encodes"
            for name, value, limit in fields if value > limit]


def field_warnings(version: str) -> list:
    """One message per part within the limits but close to them (development distance N above 900, minor or patch
    above 90), each naming the field and the limit it is approaching. Never raises."""
    parts = _parts(version)
    if parts is None:
        return []
    major, minor, patch, n = parts
    out = []
    if n is not None and WARN_DEV_N < n <= MAX_DEV_N:
        out.append(f"version '{version}': development distance N {n} is above {WARN_DEV_N}; the versionCode rule "
                   f"refuses above {MAX_DEV_N}")
    for name, value, limit in (("minor", minor, MAX_MINOR), ("patch", patch, MAX_PATCH)):
        if WARN_MINOR_PATCH < value <= limit:
            out.append(f"version '{version}': {name} {value} is above {WARN_MINOR_PATCH}; the versionCode rule "
                       f"refuses above {limit}")
    return out


def version_code_for(version: str) -> int:
    parts = _parts(version)
    if parts is None:
        raise VersionError(f"version '{version}' is neither X.Y.Z nor X.Y.Z-dev.N+<sha> (a release tag, or a development build)")
    major, minor, patch, n = parts
    if major < MIN_MAJOR:
        raise VersionError(f"version '{version}' is below major {MIN_MAJOR}, the major of the last published release "
                           f"(v{LAST_RELEASE}): a version that low is not a release line of this project (a build with no git "
                           f"reports 1.0.0, cmake/set_project_version_from_git.cmake) and its code would be a downgrade")
    problems = field_problems(version)
    if problems:
        raise VersionError("; ".join(problems))
    slot = RELEASE_SLOT if n is None else n
    # at most 209*10^7 + 99*10^5 + 99*10^3 + 999 = 2,099,999,999 < MAX_VERSION_CODE, by the limits above
    return major * 10**7 + minor * 10**5 + patch * 10**3 + slot


# ---------------------------------------------------------------------------------------------------------------
# The binary manifest
# ---------------------------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class ManifestVersion:
    version_code: int
    offset: int  # of the 4-byte value in the manifest bytes


def _chunk_header(data: bytes, pos: int, end: int) -> tuple:
    if end - pos < 8:
        raise ManifestError(f"truncated chunk header at byte {pos}")
    kind, header_size, size = struct.unpack_from("<HHI", data, pos)
    if header_size < 8 or size < header_size or size % 4 or pos + size > end:
        raise ManifestError(f"chunk 0x{kind:04x} at byte {pos} has header size {header_size}, size {size} "
                            f"(not a whole, 4-aligned chunk inside {end} bytes)")
    return kind, header_size, size


def _read_len8(data: bytes, pos: int) -> tuple:
    first = data[pos]
    if first & 0x80:
        return ((first & 0x7F) << 8) | data[pos + 1], pos + 2
    return first, pos + 1


def _string_pool(data: bytes, pos: int, header_size: int, size: int) -> list:
    if header_size != 28:
        raise ManifestError(f"string pool header size {header_size}, expected 28")
    count, styles, flags, strings_start, styles_start = struct.unpack_from("<IIIII", data, pos + 8)
    if flags & ~(UTF8_FLAG | 1):
        raise ManifestError(f"string pool flags 0x{flags:x} carry bits this reader does not know")
    if header_size + 4 * (count + styles) > size or strings_start >= size:
        raise ManifestError("string pool offsets run past the chunk")
    utf8 = bool(flags & UTF8_FLAG)
    out = []
    for i in range(count):
        offset = struct.unpack_from("<I", data, pos + header_size + 4 * i)[0]
        at = pos + strings_start + offset
        if at >= pos + size:
            raise ManifestError(f"string {i} starts past the pool")
        try:
            if utf8:
                _, at = _read_len8(data, at)
                length, at = _read_len8(data, at)
                out.append(data[at:at + length].decode("utf-8"))
            else:
                length = struct.unpack_from("<H", data, at)[0]
                at += 2
                if length & 0x8000:
                    length = ((length & 0x7FFF) << 16) | struct.unpack_from("<H", data, at)[0]
                    at += 2
                out.append(data[at:at + 2 * length].decode("utf-16-le"))
        except (UnicodeDecodeError, struct.error, IndexError) as error:
            raise ManifestError(f"string {i} of the pool does not decode: {error}") from None
    return out


def parse_manifest(data: bytes) -> ManifestVersion:
    """The versionCode of a compiled AndroidManifest.xml and where its 4 bytes sit; ManifestError unless every check
    in the module docstring holds."""
    if len(data) < 8:
        raise ManifestError("not a compiled XML document: shorter than a chunk header")
    kind, header_size, size = struct.unpack_from("<HHI", data, 0)
    if kind != RES_XML or header_size != 8 or size != len(data):
        raise ManifestError(f"not a compiled XML document: type 0x{kind:04x}, header {header_size}, size {size} of {len(data)} bytes")
    strings, resource_ids, root = None, None, None
    pos = 8
    while pos < len(data):
        kind, header_size, size = _chunk_header(data, pos, len(data))
        if kind == RES_STRING_POOL:
            if strings is not None or root is not None:
                raise ManifestError("a second string pool, or one after the first element")
            strings = _string_pool(data, pos, header_size, size)
        elif kind == RES_XML_RESOURCE_MAP:
            if resource_ids is not None or root is not None:
                raise ManifestError("a second resource map, or one after the first element")
            if header_size != 8:
                raise ManifestError(f"resource map header size {header_size}, expected 8")
            resource_ids = list(struct.unpack_from(f"<{(size - 8) // 4}I", data, pos + 8))
        elif kind == RES_XML_START_ELEMENT:
            if root is None:
                root = (pos, header_size, size)
        elif kind not in (RES_XML_START_NAMESPACE, RES_XML_END_NAMESPACE, RES_XML_END_ELEMENT, RES_XML_CDATA):
            raise ManifestError(f"unknown chunk type 0x{kind:04x} at byte {pos}")
        pos += size
    if strings is None or resource_ids is None or root is None:
        raise ManifestError("no string pool, resource map or element: the version attribute cannot be located by resource id")

    def name_of(index: int) -> str:
        if index == NO_INDEX:
            return ""
        if index >= len(strings):
            raise ManifestError(f"string index {index} is outside the pool of {len(strings)}")
        return strings[index]

    pos, header_size, size = root
    if header_size != 16 or size < 36:
        raise ManifestError(f"root element chunk has header {header_size}, size {size}")
    ns, name, attr_start, attr_size, attr_count, _, _, _ = struct.unpack_from("<IIHHHHHH", data, pos + header_size)
    if name_of(name) != "manifest" or ns != NO_INDEX:
        raise ManifestError(f"the first element is '{name_of(name)}', not the root 'manifest'")
    if attr_start != 0x14 or attr_size != 0x14 or header_size + attr_start + attr_size * attr_count > size:
        raise ManifestError(f"root attributes: start {attr_start}, size {attr_size}, count {attr_count} do not fit the chunk")
    found = []
    for i in range(attr_count):
        at = pos + header_size + attr_start + attr_size * i
        a_ns, a_name, a_raw, a_size, a_res0, a_type, a_data = struct.unpack_from("<IIIHBBI", data, at)
        if a_name >= len(strings):
            raise ManifestError(f"attribute {i}: name index {a_name} is outside the pool")
        resource_id = resource_ids[a_name] if a_name < len(resource_ids) else None
        label = strings[a_name]
        if resource_id == RES_ID_VERSION_CODE_MAJOR or label == "versionCodeMajor":
            raise ManifestError("the manifest carries versionCodeMajor: the code Android reports would not be the one set here")
        if resource_id == RES_ID_VERSION_CODE or label == "versionCode":
            found.append((i, at, a_ns, label, resource_id, a_raw, a_size, a_res0, a_type, a_data))
    if len(found) != 1:
        raise ManifestError(f"{len(found)} versionCode attributes on the root <manifest> (need exactly 1)")
    index, at, a_ns, label, resource_id, a_raw, a_size, a_res0, a_type, a_data = found[0]
    if label != "versionCode" or resource_id != RES_ID_VERSION_CODE or name_of(a_ns) != ANDROID_NS:
        raise ManifestError(f"attribute {index}: name '{label}', resource id {resource_id}, namespace '{name_of(a_ns)}' "
                            f"are not android:versionCode (0x{RES_ID_VERSION_CODE:08x})")
    if a_type != TYPE_INT_DEC or a_size != 8 or a_res0 != 0 or a_raw != NO_INDEX:
        raise ManifestError(f"versionCode is not a plain decimal integer (type 0x{a_type:02x}, size {a_size}, "
                            f"raw text index {a_raw:#x}): it has a text twin or another type that a 4-byte write would leave stale")
    if not 0 < a_data < 2**31:
        raise ManifestError(f"versionCode {a_data} is not a positive int32")
    return ManifestVersion(a_data, at + 16)


def patch_manifest(data: bytes, version_code: int) -> bytes:
    """`data` with the root versionCode set to `version_code`: a 4-byte write, re-read and compared."""
    if not 0 < version_code < MAX_VERSION_CODE:
        raise ManifestError(f"refusing to write versionCode {version_code}")
    before = parse_manifest(data)
    if version_code <= before.version_code:
        raise ManifestError(f"refusing to write versionCode {version_code}: the manifest already has {before.version_code} "
                            f"and the stamp only ever raises it")
    out = bytearray(data)
    struct.pack_into("<I", out, before.offset, version_code)
    after = parse_manifest(bytes(out))
    changed = [i for i, (a, b) in enumerate(zip(data, out)) if a != b]
    if after.version_code != version_code or after.offset != before.offset or len(out) != len(data) \
            or not changed or changed[0] < before.offset or changed[-1] >= before.offset + 4:
        raise ManifestError("the patched manifest does not differ from the original in exactly the versionCode bytes")
    return bytes(out)


def manifest_of_apk(apk: Path) -> bytes:
    with zipfile.ZipFile(apk) as z:
        names = [n for n in z.namelist() if n == MANIFEST_ENTRY]
        if len(names) != 1:
            raise ManifestError(f"{apk.name}: {len(names)} {MANIFEST_ENTRY} entries (need exactly 1)")
        return z.read(MANIFEST_ENTRY)


def version_code_of_apk(apk: Path) -> int:
    return parse_manifest(manifest_of_apk(apk)).version_code


# ---------------------------------------------------------------------------------------------------------------
# The build's version
# ---------------------------------------------------------------------------------------------------------------

def read_build_version(build_info: Path) -> str:
    """kVersion from the nevr_build_info.h the Quest build generates (cmake/nevr_build_info.h.in)."""
    try:
        text = build_info.read_text(encoding="utf-8")
    except OSError as error:
        raise VersionError(f"cannot read {build_info}: {error.strerror}") from None
    found = re.findall(r'\bkVersion\s*=\s*"([^"]*)";', text)
    if len(found) != 1:
        raise VersionError(f"{build_info}: expected exactly one kVersion, found {len(found)}")
    return found[0]


def shim_carries_version(shim: bytes, version: str) -> bool:
    """The sentinel stores kVersion as its own NUL-bounded string; a bare substring would pass 5.6.1-dev.3 for
    5.6.1-dev.37."""
    return b"\x00" + version.encode("ascii") + b"\x00" in shim


# ---------------------------------------------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------------------------------------------

def _log(event: str, **fields) -> None:
    print(json.dumps({"tag": "NEVR.QUEST.APK", "event": event, **fields}, separators=(",", ":")), file=sys.stderr)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="quest_apk_version.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    code = sub.add_parser("code")
    code.add_argument("version")
    read = sub.add_parser("read")
    read.add_argument("apk", type=Path)
    patch = sub.add_parser("patch-manifest")
    patch.add_argument("manifest", type=Path)
    patch.add_argument("--build-info", required=True, type=Path)
    patch.add_argument("--shim", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "code":
            print(version_code_for(args.version))
        elif args.command == "read":
            print(version_code_of_apk(args.apk))
        else:
            version = read_build_version(args.build_info)
            new_code = version_code_for(version)
            try:
                shim = args.shim.read_bytes()
            except OSError as error:
                raise VersionError(f"cannot read the shim {args.shim}: {error.strerror}") from None
            if not shim_carries_version(shim, version):
                raise VersionError(f"the shim {args.shim.name} does not carry the version string '{version}' that "
                                   f"{args.build_info.name} names: the header and the shim are from different builds")
            original = args.manifest.read_bytes()
            old_code = parse_manifest(original).version_code
            patched = patch_manifest(original, new_code)
            args.manifest.write_bytes(patched)
            _log("version_code_stamped", version=version, version_code_before=old_code, version_code_after=new_code,
                 manifest_sha256_before=hashlib.sha256(original).hexdigest(),
                 manifest_sha256_after=hashlib.sha256(patched).hexdigest(), bytes_changed=4)
            print(new_code)
    except (VersionError, ManifestError, OSError, zipfile.BadZipFile) as error:
        _log("refused", command=args.command, reason=str(error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
