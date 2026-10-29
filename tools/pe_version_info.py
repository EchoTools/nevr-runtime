#!/usr/bin/env python3
"""Read the VERSIONINFO resource of a Windows PE file, and check the three files this project ships.

BugSplat64.dll (the host), nevr-runtime.dll (the runtime) and echovr_server.exe carry a VERSIONINFO resource
(cmake/nevr_version_info.cmake), so Explorer's Details tab names them. This reader parses the PE resource directory and the VS_VERSIONINFO block with the
standard library only: it reads a stripped file, a signed file and a file from a release zip alike, and needs no
Windows host and no binutils.

    pe_version_info.py show FILE
    pe_version_info.py check --dll BugSplat64.dll --runtime-dll nevr-runtime.dll --exe echovr_server.exe
                             [--version X.Y.Z[-dev.N+sha]]

`check` is fail-closed: a file with no resource, a truncated or duplicated block, a field that is not the pinned
value, or a version other than the one the DLL's own NEVR-BUILD literal states (or --version) exits 1. Exit 0
means every field of both files is exactly what EXPECTED_FILES and the version say.

The 4-part numeric version Windows needs is MAJOR.MINOR.PATCH.DISTANCE of the nearest release tag: a release
`X.Y.Z` is X.Y.Z.0 and a development build `X.Y.(Z+1)-dev.N+sha` is X.Y.Z.N, so the number sorts like the semver
string: after the tag it descends from and before the next release. (A development build exactly on a tag, distance
0, has the tag's number; it differs from the release in its strings.)
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

RT_VERSION = 16
FIXED_SIGNATURE = 0xFEEF04BD
FIXED_SIZE = 52
VOS_NT_WINDOWS32 = 0x00040004
VFT_APP = 0x1
VFT_DLL = 0x2
VFT2_UNKNOWN = 0x0
# US English, Unicode: the one language block both files carry, as the table key and as the Translation pair.
TABLE_KEY = "040904B0"
TRANSLATION = (0x0409, 0x04B0)

PRODUCT_NAME = "nEVR runtime"
LEGAL_COPYRIGHT = "© 2026 Andrew Bates <a@sprock.io>"
COMPANY_NAME = "Andrew Bates"

# Every string each file carries, by file name, and nothing else: an extra string is an unreviewed field.
EXPECTED_FILES = {
    "BugSplat64.dll": {"FileDescription": "nEVR host for Echo VR", "file_type": VFT_DLL},
    "nevr-runtime.dll": {"FileDescription": "nEVR runtime for Echo VR", "file_type": VFT_DLL},
    "echovr_server.exe": {"FileDescription": "nEVR dedicated server launcher", "file_type": VFT_APP},
}

# A release (X.Y.Z) or a development build (X.Y.(Z+1)-dev.N+sha), as set_project_version_from_git.cmake writes it.
# A release X.Y.Z, an rc X.Y.Z-rc.N (#714), a development build X.Y.(Z+1)-dev.N+sha, or X.Y.Z-rc.K.dev.N+sha after an rc.
# Group 4 is N of an rc development build X.Y.Z-rc.K.dev.N+sha; group 5 is N of a development build X.Y.(Z+1)-dev.N+sha.
VERSION = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-rc\.[1-9][0-9]*(?:\.dev\.(0|[1-9][0-9]*)\+[0-9a-f]{7,40})?|-dev\.(0|[1-9][0-9]*)\+[0-9a-f]{7,40})?$")
IDENTITY = re.compile(rb"(?<=\x00)NEVR-BUILD ([0-9A-Za-z.+-]+) ([0-9a-f]{40})(?=\x00)")


class PeVersionError(Exception):
    """The file has no readable VERSIONINFO resource, or the block is malformed."""


@dataclass
class VersionInfo:
    file_version: tuple
    product_version: tuple
    file_flags_mask: int
    file_flags: int
    file_os: int
    file_type: int
    file_subtype: int
    tables: dict = field(default_factory=dict)  # table key ("040904B0") -> {string name: value}
    translations: list = field(default_factory=list)  # [(language, codepage)]


class _Reader:
    """Bounds-checked little-endian reads from one buffer; any read outside it is a PeVersionError."""

    def __init__(self, data: bytes, what: str):
        self.data, self.what = data, what

    def unpack(self, fmt: str, offset: int):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(self.data):
            raise PeVersionError(f"{self.what}: read of {size} bytes at {offset:#x} is outside the data ({len(self.data):#x} bytes)")
        return struct.unpack_from(fmt, self.data, offset)


def _sections(pe: _Reader):
    if pe.data[:2] != b"MZ":
        raise PeVersionError("not a PE file: no MZ header")
    (lfanew,) = pe.unpack("<I", 0x3C)
    if pe.unpack("<4s", lfanew)[0] != b"PE\0\0":
        raise PeVersionError("not a PE file: no PE signature")
    _, nsections, _, _, _, optsize, _ = pe.unpack("<HHIIIHH", lfanew + 4)
    opt = lfanew + 24
    (magic,) = pe.unpack("<H", opt)
    if magic == 0x20B:
        count_at, dirs_at = opt + 108, opt + 112
    elif magic == 0x10B:
        count_at, dirs_at = opt + 92, opt + 96
    else:
        raise PeVersionError(f"unknown optional header magic {magic:#x}")
    (ndirs,) = pe.unpack("<I", count_at)
    if ndirs <= 2:
        raise PeVersionError("the file has no resource data directory entry")
    rva, size = pe.unpack("<II", dirs_at + 2 * 8)
    table = []
    for index in range(nsections):
        name, vsize, vaddr, rawsize, rawptr = pe.unpack("<8sIIII", opt + optsize + 40 * index)
        table.append((name.rstrip(b"\0").decode("latin-1"), vsize, vaddr, rawsize, rawptr))
    return rva, size, table


def _rva_to_offset(pe: _Reader, table, rva: int) -> int:
    for _, vsize, vaddr, rawsize, rawptr in table:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            offset = rva - vaddr
            if offset >= rawsize:
                raise PeVersionError(f"RVA {rva:#x} is in the zero-filled tail of its section")
            return rawptr + offset
    raise PeVersionError(f"RVA {rva:#x} is in no section")


def _version_blobs(pe: _Reader, table, rsrc_rva: int, rsrc_size: int) -> list:
    """The raw data of every RT_VERSION resource, through the three levels type / name / language."""
    base = _rva_to_offset(pe, table, rsrc_rva)

    def entries(rel: int):
        _, _, _, _, nnamed, nid = pe.unpack("<IIHHHH", base + rel)
        return [pe.unpack("<II", base + rel + 16 + 8 * index) for index in range(nnamed + nid)], nnamed

    blobs = []
    top, nnamed = entries(0)
    for position, (ident, target) in enumerate(top):
        if position < nnamed or ident != RT_VERSION or not target & 0x80000000:
            continue
        for _, name_target in entries(target & 0x7FFFFFFF)[0]:
            if not name_target & 0x80000000:
                raise PeVersionError("RT_VERSION name entry is not a directory")
            for _, leaf in entries(name_target & 0x7FFFFFFF)[0]:
                if leaf & 0x80000000:
                    raise PeVersionError("RT_VERSION language entry is a directory")
                data_rva, data_size, _, _ = pe.unpack("<IIII", base + leaf)
                start = _rva_to_offset(pe, table, data_rva)
                if start + data_size > len(pe.data):
                    raise PeVersionError("RT_VERSION data runs past the end of the file")
                blobs.append(pe.data[start:start + data_size])
    return blobs


def _align4(value: int) -> int:
    return (value + 3) & ~3


def _block(buf: _Reader, offset: int, limit: int):
    """One version block: (key, value offset, value length in bytes, type, end offset, children offset)."""
    length, value_length, kind = buf.unpack("<HHH", offset)
    end = offset + length
    if length < 6 or end > limit:
        raise PeVersionError(f"version block at {offset:#x} is {length} bytes, outside its parent ({limit:#x})")
    key_end = offset + 6
    while True:
        (unit,) = buf.unpack("<H", key_end)
        key_end += 2
        if unit == 0:
            break
        if key_end > end:
            raise PeVersionError(f"version block key at {offset:#x} is not terminated")
    key = buf.data[offset + 6:key_end - 2].decode("utf-16-le")
    value_at = _align4(key_end)
    value_bytes = value_length * 2 if kind == 1 else value_length
    if value_at + value_bytes > end:
        raise PeVersionError(f"version block '{key}' value runs past its end")
    return key, value_at, value_bytes, kind, end, _align4(value_at + value_bytes)


def _children(buf: _Reader, start: int, end: int):
    position = start
    while position + 6 <= end:
        child = _block(buf, position, end)
        yield child
        position = _align4(child[4])


def parse_version_block(blob: bytes) -> VersionInfo:
    buf = _Reader(blob, "VS_VERSIONINFO")
    key, value_at, value_bytes, _, end, children_at = _block(buf, 0, len(blob))
    if key != "VS_VERSION_INFO":
        raise PeVersionError(f"the version block is named '{key}', not VS_VERSION_INFO")
    if value_bytes != FIXED_SIZE:
        raise PeVersionError(f"VS_FIXEDFILEINFO is {value_bytes} bytes, not {FIXED_SIZE}")
    sig, _, fv_ms, fv_ls, pv_ms, pv_ls, mask, flags, file_os, file_type, subtype, _, _ = buf.unpack("<13I", value_at)
    if sig != FIXED_SIGNATURE:
        raise PeVersionError(f"VS_FIXEDFILEINFO signature is {sig:#010x}, not {FIXED_SIGNATURE:#010x}")
    info = VersionInfo(
        file_version=(fv_ms >> 16, fv_ms & 0xFFFF, fv_ls >> 16, fv_ls & 0xFFFF),
        product_version=(pv_ms >> 16, pv_ms & 0xFFFF, pv_ls >> 16, pv_ls & 0xFFFF),
        file_flags_mask=mask, file_flags=flags, file_os=file_os, file_type=file_type, file_subtype=subtype)
    for name, c_at, c_bytes, _, c_end, c_children in _children(buf, children_at, end):
        if name == "StringFileInfo":
            for table_key, _, _, _, t_end, t_children in _children(buf, c_children, c_end):
                if table_key in info.tables:
                    raise PeVersionError(f"string table {table_key} appears twice")
                strings = {}
                for s_name, s_at, s_bytes, s_kind, _, _ in _children(buf, t_children, t_end):
                    if s_kind != 1:
                        raise PeVersionError(f"string '{s_name}' is not a text value")
                    if s_name in strings:
                        raise PeVersionError(f"string '{s_name}' appears twice in table {table_key}")
                    strings[s_name] = blob[s_at:s_at + s_bytes].decode("utf-16-le").rstrip("\0")
                info.tables[table_key] = strings
        elif name == "VarFileInfo":
            for v_name, v_at, v_bytes, _, _, _ in _children(buf, c_children, c_end):
                if v_name == "Translation":
                    if v_bytes % 4:
                        raise PeVersionError("Translation is not a list of language/codepage pairs")
                    info.translations += [buf.unpack("<HH", v_at + 4 * index) for index in range(v_bytes // 4)]
        else:
            raise PeVersionError(f"unexpected block '{name}' in VS_VERSIONINFO")
    return info


def read_version_info(path) -> VersionInfo:
    """The one VERSIONINFO resource of the PE file at `path`; raises PeVersionError when there is not exactly one."""
    pe = _Reader(Path(path).read_bytes(), str(path))
    rsrc_rva, rsrc_size, table = _sections(pe)
    if rsrc_rva == 0 or rsrc_size == 0:
        raise PeVersionError(f"{Path(path).name} has no resource directory, so no VERSIONINFO")
    blobs = _version_blobs(pe, table, rsrc_rva, rsrc_size)
    if len(blobs) != 1:
        raise PeVersionError(f"{Path(path).name} has {len(blobs)} VERSIONINFO resources, expected exactly 1")
    return parse_version_block(blobs[0])


def numeric_version(version: str) -> tuple:
    """The 4-part number for a version string: X.Y.Z and X.Y.Z-rc.K -> (X, Y, Z, 0); X.Y.(Z+1)-dev.N+sha -> (X, Y, Z, N);
    X.Y.Z-rc.K.dev.N+sha -> (X, Y, Z, N). A 4-part number cannot order an rc below its release, so an rc and its release
    share (X, Y, Z, 0); the identity literal, not the PE version, names the pre-release."""
    match = VERSION.match(version)
    if not match:
        raise PeVersionError(f"'{version}' is neither X.Y.Z, X.Y.Z-rc.N nor a development version (+sha)")
    major, minor, patch = int(match[1]), int(match[2]), int(match[3])
    if match[4] is not None:
        parts = (major, minor, patch, int(match[4]))
    elif match[5] is not None:
        if patch < 1:
            raise PeVersionError(f"development version '{version}': the patch of X.Y.(Z+1) is at least 1")
        parts = (major, minor, patch - 1, int(match[5]))
    else:
        parts = (major, minor, patch, 0)
    if max(parts) > 0xFFFF:
        raise PeVersionError(f"version '{version}' has a part above 65535, which a Windows version cannot hold")
    return parts


def expected_strings(file_name: str, version: str) -> dict:
    spec = EXPECTED_FILES[file_name]
    return {
        "CompanyName": COMPANY_NAME,
        "FileDescription": spec["FileDescription"],
        "FileVersion": version,
        "InternalName": file_name,
        "LegalCopyright": LEGAL_COPYRIGHT,
        "OriginalFilename": file_name,
        "ProductName": PRODUCT_NAME,
        "ProductVersion": version,
    }


def problems_for(path, version: str) -> list:
    """Everything that differs between the file's VERSIONINFO and what the pins and `version` say; [] is a pass."""
    path = Path(path)
    if path.name not in EXPECTED_FILES:
        return [f"{path.name}: not one of {sorted(EXPECTED_FILES)}"]
    try:
        info = read_version_info(path)
        numeric = numeric_version(version)
    except (PeVersionError, OSError) as exc:
        return [f"{path.name}: {exc}"]
    found = []
    spec = EXPECTED_FILES[path.name]
    if info.file_version != numeric:
        found.append(f"{path.name}: FILEVERSION {'.'.join(map(str, info.file_version))} is not {'.'.join(map(str, numeric))}")
    if info.product_version != numeric:
        found.append(f"{path.name}: PRODUCTVERSION {'.'.join(map(str, info.product_version))} is not {'.'.join(map(str, numeric))}")
    for label, got, want in (("FILEOS", info.file_os, VOS_NT_WINDOWS32), ("FILETYPE", info.file_type, spec["file_type"]),
                             ("FILESUBTYPE", info.file_subtype, VFT2_UNKNOWN)):
        if got != want:
            found.append(f"{path.name}: {label} {got:#x} is not {want:#x}")
    if info.translations != [TRANSLATION]:
        found.append(f"{path.name}: Translation {info.translations} is not [{TRANSLATION}]")
    if list(info.tables) != [TABLE_KEY]:
        found.append(f"{path.name}: string tables {list(info.tables)} are not [{TABLE_KEY!r}]")
    strings = info.tables.get(TABLE_KEY, {})
    want = expected_strings(path.name, version)
    for name in sorted(set(strings) | set(want)):
        if strings.get(name) != want.get(name):
            found.append(f"{path.name}: string {name} is {strings.get(name)!r}, expected {want.get(name)!r}")
    return found


def identity_version(dll: Path) -> str:
    """The version in the DLL's single NEVR-BUILD identity literal."""
    found = IDENTITY.findall(dll.read_bytes())
    if len(found) != 1:
        raise PeVersionError(f"{dll.name} has {len(found)} NEVR-BUILD identity literals, expected exactly 1")
    return found[0][0].decode()


def check(dll: Path, exe: Path, version: str | None = None, runtime: Path | None = None) -> list:
    """Problems for the shipped files against `version`, or against the host DLL's own identity literal. The
    runtime (when given) must also state that same version in its own identity literal: the pair is one build."""
    try:
        wanted = version if version is not None else identity_version(dll)
        runtime_problems = []
        if runtime is not None:
            if identity_version(runtime) != identity_version(dll):
                runtime_problems.append(f"{runtime.name}: its identity version {identity_version(runtime)} is not "
                                        f"{dll.name}'s {identity_version(dll)}")
            runtime_problems += problems_for(runtime, wanted)
    except (PeVersionError, OSError) as exc:
        return [f"{dll.name}: {exc}"]
    return problems_for(dll, wanted) + runtime_problems + problems_for(exe, wanted)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    show = sub.add_parser("show", help="print the VERSIONINFO of one file")
    show.add_argument("file", type=Path)
    verify = sub.add_parser("check", help="check the shipped files against the pins and one version")
    verify.add_argument("--dll", type=Path, required=True)
    verify.add_argument("--runtime-dll", type=Path, help="nevr-runtime.dll, checked at the same version")
    verify.add_argument("--exe", type=Path, required=True)
    verify.add_argument("--version", help="the version to expect; default: the DLL's own NEVR-BUILD literal")
    args = parser.parse_args(argv)
    if args.command == "show":
        try:
            info = read_version_info(args.file)
        except (PeVersionError, OSError) as exc:
            print(f"pe-version-info: FAIL: {exc}", file=sys.stderr)
            return 1
        print(f"FILEVERSION     {'.'.join(map(str, info.file_version))}")
        print(f"PRODUCTVERSION  {'.'.join(map(str, info.product_version))}")
        print(f"FILEOS {info.file_os:#x}  FILETYPE {info.file_type:#x}  FILESUBTYPE {info.file_subtype:#x}")
        print(f"Translation     {info.translations}")
        for table_key, strings in info.tables.items():
            for name in sorted(strings):
                print(f"{table_key} {name} = {strings[name]}")
        return 0
    try:
        problems = check(args.dll, args.exe, args.version, args.runtime_dll)
    except OSError as exc:
        problems = [str(exc)]
    for problem in problems:
        print(f"pe-version-info: FAIL: {problem}", file=sys.stderr)
    if problems:
        return 1
    names = [args.dll.name] + ([args.runtime_dll.name] if args.runtime_dll else []) + [args.exe.name]
    print(f"pe-version-info: OK: {', '.join(names[:-1])} and {names[-1]} carry the pinned VERSIONINFO "
          f"({args.version or identity_version(args.dll)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
