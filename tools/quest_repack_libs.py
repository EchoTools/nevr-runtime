#!/usr/bin/env python3
"""The library entries of the Quest repack: what a plugin may be called, and what must survive signing (#1117).

`just android-repack-apk` stages the sentinel as `lib/arm64-v8a/libovrplatformloader.so` and the store's own loader as
`libovrplatformloader_orig.so`, and, for a headset run of the plugin loader (docs/adr/0019-quest-plugin-loader.md),
one plugin `lib/arm64-v8a/<name>`. `zip -0 -X` replaces an entry that already has the path it is given, so a plugin
named like a game library would silently take that library's place and the signed APK would not boot. Two commands:

  check-plugin-name --apk STORE_APK --plugin PLUGIN_FILE
      Refuses (exit 1) a plugin whose file name is the file name of ANY `lib/` entry of the store APK (in any ABI
      directory) or of a library the recipe stages itself.

  verify-signed --source STORE_APK --signed SIGNED_APK --stage STAGE_DIR --patcher PATCHER [--extra-rows FILE]
                [--scratch DIR]
      Reads the SIGNED APK. Refuses (exit 1) unless every `lib/` entry of the store APK, except the two libraries the
      recipe replaces on purpose, is there with the same bytes, and every file the recipe staged under `lib/arm64-v8a/`
      is there with the staged bytes. The two are the loader (the shim takes its name) and libr15.so, which must equal
      what the patcher (quest_patch_libr15, docs/adr/0021 section 9) makes of the store APK's libr15.so: the tool runs
      here, on a copy of the store file in a temporary directory (under --scratch when given), with the same
      --extra-rows file the recipe gave it, and the sha-256 of the store file, of the patcher's output and of the signed
      entry are printed. A signed libr15 that is the store's bytes, a patcher run on another input, a missing entry, a
      patcher that fails or writes no file, and a patcher whose output is the store file are each a named problem. When the
      source APK's libvrapi.so is the reference runtime (it carries the checksum list of tools/quest_runtime_pin.py; the
      store's own library does not), the signed APK's libvrapi.so must equal the pin tool's output for the source file and
      the md5 of the SIGNED libr15.so, and the three digests are printed; an unpinned copy and a copy pinned to another
      libr15 are each a named problem. Without a runtime the library is compared with the store's bytes. The staged
      pack (stage-pack) is held to the same rule: each of its files is in the signed APK with the staged bytes, STORED, the
      signed APK carries no `assets/nevr-pack/` entry that was not staged (so a build with no pack carries none), and the
      signed APK is under 4 GiB.

  check-shim-rows --shim SHIM [--extra-rows FILE]
      Refuses (exit 1) a shim that was not built for the rows the library is patched with (docs/adr/0021 section 11). The
      patched libr15's build ID depends on the extra rows; a shim built for other rows reports `build_id_mismatch` and
      installs nothing, while the patched library's stubs branch through slots that only an attached shim fills, so the
      game would fault on the first patched call. With --extra-rows the shim must carry the file's sha-256 (the
      `kSha256` string the variant build embeds); without it the shim must not be a variant build (it must not carry the
      variant's `libr15_extra_rows` log event name).

  write-plugin-manifest --stage STAGE_DIR --plugin PLUGIN_FILE... [--early NAME]... [--off NAME]...
      Writes `lib/arm64-v8a/libnevr_plugin_manifest.so` under STAGE_DIR: the packaged plugins, in the order given, each with
      its sha-256 (of the staged file), its `early` and its `enabled`. The loader reads it at launch and loads these plugins
      with no config.yaml entry (docs/adr/0019-quest-plugin-loader.md, section 2). Refuses (exit 1) a plugin not named
      lib<name>.so, two of one name, and an --early or --off name that is not a packaged plugin. JSON with sorted keys, so
      the same plugins give the same bytes.

  check-plugin-manifest --stage STAGE_DIR
      Refuses (exit 1) a staged manifest whose sha-256 for a plugin is not the staged file's, and a staged plugin the
      manifest does not list. Run by verify-signed, before anything is signed.

  stage-pack --pack-zip PACK_ZIP --meta META_JSON --stage STAGE_DIR --store-apk STORE_APK [--plan-header FILE]
      Stages a bundled content pack (docs/adr/0029-quest-bundled-content-pack.md) as APK assets under STAGE_DIR:
      `assets/nevr-pack/files/<path>` for each file of the zip and `assets/nevr-pack/pack.json` describing them. META_JSON holds
      the pack's `id`, `version` (X.Y.Z), `requires_stock_manifest_sha256` (one or two sha-256 of the stock manifests it may
      replace) and `zip_prefix` (`_data/<version>/rad15/android/`, the directory the zip's entries sit in, which names the stock
      data version). The pins it must agree with (the manifest id and the data version folder) are read from the data step's
      `kManifestPath` in PLAN_HEADER (default src/quest/data/plan.h). Every file gets every check, and nothing is written when
      one fails: each zip entry name is safe (no absolute path, `..`, backslash, drive letter or control character, at most
      16 components), no entry is a link or another special file or is encrypted, no two names are one (in any case), every
      file sits under the prefix in `manifests/` or `packages/` with the one file name the runtime accepts, the sizes the zip
      records are the sizes read, the sha-256 is taken from the bytes, there are at most 8 files, none empty or over 2 GiB, the
      store APK has no `assets/nevr-pack/` entry of its own, and the APK (the store file plus the pack) stays under the 4 GiB a
      zip without zip64 can address, which the sentinel's zip reader requires.

  pack-problems --stage STAGE_DIR [--plan-header FILE]
      Refuses (exit 1) a staged pack the runtime would not take (the rules of src/quest/data/pack.cpp ParsePackJson), a staged
      file `pack.json` does not list or whose sha-256 or size it gets wrong, and a file listed but not staged. Nothing staged is
      fine. Run by verify-signed, before anything is signed.

  check-plugin-build-ids --apk STORE_APK --plugin PLUGIN_FILE
      Refuses (exit 1) a plugin the Quest loader would refuse for its build IDs (src/quest/plugin/plugin_image.h):
      one with no NEVR_DECLARE_SUPPORTED_BUILD_IDS note, or whose list does not hold the GNU build ID of the store
      APK's own `lib/arm64-v8a/libr15.so`, and a store libr15.so with no build ID. The notes are read the way the loader
      reads them: from the PT_NOTE segments, each name and description padded to 4 bytes, the IDs split on spaces, commas, tabs and line ends and matched whole.

All read zip entries with the standard library and name every entry they refuse.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
import struct
import pathlib
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import quest_runtime_pin as runtime_pin  # noqa: E402

ABI_DIR = "lib/arm64-v8a"
# The manifest of packaged plugins (docs/adr/0019-quest-plugin-loader.md, section 2): written only when a plugin is packaged.
# Its name is a lib*.so name so the installer extracts it beside the plugins; the loader reads it as JSON, never as a library.
MANIFEST_NAME = "libnevr_plugin_manifest.so"
MANIFEST_ENTRY = f"{ABI_DIR}/{MANIFEST_NAME}"
MANIFEST_SCHEMA = 1
# The libraries the recipe stages itself (the shim under the loader's name, the store's loader renamed, and the manifest).
STAGED_BY_RECIPE = ("libovrplatformloader.so", "libovrplatformloader_orig.so", MANIFEST_NAME)
# The one store entry the recipe replaces on purpose: the shim takes the loader's name.
REPLACED_ON_PURPOSE = f"{ABI_DIR}/libovrplatformloader.so"
# The other one: the patcher's output takes libr15's place (docs/adr/0021 section 9). It is compared with that output,
# never with the store's bytes.
PATCHED_LIBRARY = f"{ABI_DIR}/libr15.so"
# The third: the reference runtime (tools/quest_runtime_pin.py) takes its own checksum list pinned to the final libr15. The
# store's own libvrapi.so carries no such list and is compared with the store's bytes like every other library.
PINNED_LIBRARY = f"{ABI_DIR}/libvrapi.so"
# The patcher reads and writes one file of about 58 MB; a run that takes longer than this is a hang.
PATCHER_TIMEOUT_SECONDS = 600
_CHUNK = 1 << 20
LIBR15 = f"{ABI_DIR}/libr15.so"
PT_NOTE = 4
NT_GNU_BUILD_ID = 3
NEVR_NOTE_TYPE = 1


# The bundled content pack (docs/adr/0029-quest-bundled-content-pack.md): APK assets the sentinel places over the stock data.
PACK_DIR = "assets/nevr-pack"
PACK_JSON_ENTRY = f"{PACK_DIR}/pack.json"
PACK_FILES_DIR = f"{PACK_DIR}/files"
PACK_SCHEMA = 1
MAX_PACK_FILES = 8
MAX_PACK_FILE_BYTES = (1 << 31) - 1  # the sentinel's zip reader limit for one entry
# A zip without zip64 addresses 4 GiB; the sentinel's reader refuses zip64. The slack is the central directory, the signing
# block and the alignment padding.
APK_LIMIT_BYTES = 0xFFFFFFFF
APK_SLACK_BYTES = 8 * 1024 * 1024
PLAN_HEADER = pathlib.Path(__file__).resolve().parent.parent / "src/quest/data/plan.h"
RESERVED_SUFFIXES = (".nevr-pack.tmp", ".stock", ".nevr-owned")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_PACK_ID = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
_PACK_VERSION = re.compile(r"^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$")
_PACK_FILE_NAME = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
_ZIP_PREFIX = re.compile(r"^_data/([A-Za-z0-9._-]{1,32})/rad15/android/$")
_MANIFEST_PIN = re.compile(r'kManifestPath\s*=\s*"_data/([A-Za-z0-9._-]+)/rad15/android/manifests/([A-Za-z0-9._-]+)"')


def lib_entries(archive: zipfile.ZipFile) -> list[str]:
    """Every file entry under `lib/`."""
    return [i.filename for i in archive.infolist() if i.filename.startswith("lib/") and not i.is_dir()]


def entry_digest(archive: zipfile.ZipFile, name: str) -> str:
    digest = hashlib.sha256()
    with archive.open(name) as stream:
        while chunk := stream.read(_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def file_digest(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def plugin_name_collisions(store_entries: list[str], plugin_name: str) -> list[str]:
    """What `plugin_name` collides with: the `lib/` entries of the store APK and the libraries the recipe stages."""
    found = [entry for entry in store_entries if entry.rsplit("/", 1)[-1] == plugin_name]
    found += [f"{ABI_DIR}/{name} (staged by the recipe)" for name in STAGED_BY_RECIPE if name == plugin_name]
    return found


def check_plugin_name(apk: pathlib.Path, plugin: pathlib.Path) -> list[str]:
    with zipfile.ZipFile(apk) as archive:
        return plugin_name_collisions(lib_entries(archive), plugin.name)


# The event name only a variant build (NEVR_LIBR15_EXTRA_ROWS) compiles in: sentinel/libr15_identity.cpp logs it when the
# embedded rows are unparsable. A NUL-bounded string in the shim's read-only data.
VARIANT_MARKER = b"\0libr15_extra_rows\0"


def shim_rows_problems(shim: pathlib.Path, extra_rows: pathlib.Path | None) -> list[str]:
    """The reasons `shim` was not built for the rows the library is patched with; empty when it was."""
    try:
        data = shim.read_bytes()
    except OSError as error:
        return [f"{shim}: cannot be read: {error}"]
    if extra_rows is None:
        if VARIANT_MARKER in data:
            return [f"{shim.name} is a variant build (it carries extra rows) but no extra_rows file was given, so the library "
                    "would be patched without them and the shim would refuse it (build_id_mismatch)"]
        return []
    digest = file_digest(extra_rows)
    if digest.encode("ascii") + b"\0" not in data:
        return [f"{shim.name} was not built with {extra_rows.name} (sha-256 {digest}): rebuild it with "
                "-DNEVR_LIBR15_EXTRA_ROWS=<the same file>, else it refuses the patched library (build_id_mismatch)"]
    return []


def patched_digest(archive: zipfile.ZipFile, patcher: pathlib.Path, extra_rows: pathlib.Path | None,
                   scratch: pathlib.Path | None, digests: dict[str, str], problems: list[str]) -> str | None:
    """The sha-256 of the patcher's output for the store APK's libr15.so, or None with the reason in `problems`.

    The store file is copied out of the archive into a temporary directory that the `with` removes; the patcher is run on
    that copy and its output is read back from the file it was told to write. `digests` receives the store file's."""
    with tempfile.TemporaryDirectory(prefix="quest-repack-libr15-", dir=scratch) as tmp:
        work = pathlib.Path(tmp)
        stock, out, report = work / "stock-libr15.so", work / "patched-libr15.so", work / "report.json"
        with archive.open(PATCHED_LIBRARY) as source, stock.open("wb") as sink:
            while chunk := source.read(_CHUNK):
                sink.write(chunk)
        digests["store"] = file_digest(stock)
        command = [str(patcher.resolve()), "--stock", str(stock), "--out", str(out), "--report", str(report)]
        if extra_rows is not None:
            command += ["--extra-rows", str(extra_rows)]
        try:
            run = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=PATCHER_TIMEOUT_SECONDS)
        except (OSError, subprocess.TimeoutExpired) as error:
            problems.append(f"{PATCHED_LIBRARY}: the patcher {patcher} did not run to its end: {error}")
            return None
        if run.returncode != 0:
            problems.append(f"{PATCHED_LIBRARY}: the patcher {patcher} exited {run.returncode} on the store APK's libr15.so: "
                            f"{run.stderr.strip()[-500:]}")
            return None
        if not out.is_file() or out.stat().st_size == 0:
            problems.append(f"{PATCHED_LIBRARY}: the patcher {patcher} exited 0 and wrote no file")
            return None
        digests["patched"] = file_digest(out)
    return digests["patched"]


def plugin_is_named_for_the_loader(name: str) -> bool:
    """The file name the loader takes a plugin by: lib<name>.so, with a non-empty name."""
    return name.startswith("lib") and name.endswith(".so") and len(name) > len("lib.so")


def manifest_bytes(plugins: list[dict]) -> bytes:
    """The manifest's bytes: the same plugins give the same bytes (sorted keys, no spaces, one trailing newline)."""
    text = json.dumps({"schema": MANIFEST_SCHEMA, "plugins": plugins}, sort_keys=True, separators=(",", ":"))
    return (text + "\n").encode("ascii")


def write_plugin_manifest(stage: pathlib.Path, plugins: list[pathlib.Path], early: list[str], off: list[str]) -> list[str]:
    """Writes the manifest of the packaged `plugins` under `stage`; returns the problems, and writes nothing when there are any.

    `plugins` are the files as the recipe was given them (only the file names matter); each is read from the staged copy,
    so the hash is of the bytes the APK will carry. `early` and `off` name packaged files."""
    names = [p.name for p in plugins]
    problems: list[str] = []
    for name in names:
        if not plugin_is_named_for_the_loader(name):
            problems.append(f"plugin {name} must be named lib<name>.so")
        if names.count(name) > 1:
            problems.append(f"two plugins are named {name}")
    for label, group in (("plugin_early", early), ("plugin_off", off)):
        for name in group:
            if name not in names:
                problems.append(f"{label} names {name}, which is not one of the packaged plugins")
    if problems:
        return problems
    entries: list[dict] = []
    for plugin in plugins:
        staged = stage / ABI_DIR / plugin.name
        if not staged.is_file():
            return [f"{ABI_DIR}/{plugin.name}: not staged, so its hash cannot be written"]
        entries.append({
            "name": plugin.name[len("lib"):-len(".so")],
            "file": plugin.name,
            "sha256": file_digest(staged),
            "enabled": plugin.name not in off,
            "early": plugin.name in early,
        })
    (stage / ABI_DIR / MANIFEST_NAME).write_bytes(manifest_bytes(entries))
    return []


def plugin_manifest_problems(stage: pathlib.Path) -> list[str]:
    """The problems with the staged manifest, empty when there is none or it holds every staged plugin with its hash."""
    path = stage / ABI_DIR / MANIFEST_NAME
    if not path.is_file():
        return []
    try:
        document = json.loads(path.read_bytes())
    except (ValueError, OSError) as error:
        return [f"{MANIFEST_ENTRY}: cannot be read as JSON: {error}"]
    listed: set[str] = set()
    problems: list[str] = []
    for entry in document.get("plugins", []):
        file_name = str(entry.get("file"))
        listed.add(file_name)
        staged = stage / ABI_DIR / file_name
        if not staged.is_file():
            problems.append(f"{MANIFEST_ENTRY}: lists {file_name}, which is not staged")
            continue
        actual = file_digest(staged)
        if actual != entry.get("sha256"):
            problems.append(f"{MANIFEST_ENTRY}: the sha256 it lists for {file_name} is {entry.get('sha256')}, "
                            f"the staged file's is {actual}")
    own = set(STAGED_BY_RECIPE) | {pathlib.Path(PATCHED_LIBRARY).name, pathlib.Path(PINNED_LIBRARY).name}
    for staged in sorted(p for p in (stage / ABI_DIR).iterdir() if p.is_file()):
        if staged.name in own or staged.name in listed:
            continue
        problems.append(f"{ABI_DIR}/{staged.name}: staged, not listed in {MANIFEST_NAME}")
    return problems


def entry_md5(archive: zipfile.ZipFile, name: str) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with archive.open(name) as stream:
        while chunk := stream.read(_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def source_is_reference_runtime(original: zipfile.ZipFile) -> bool:
    """True when the source APK's libvrapi.so carries the reference runtime's checksum list (the store's does not)."""
    return PINNED_LIBRARY in original.namelist() and runtime_pin.carries_table(original.read(PINNED_LIBRARY))


def pinned_runtime_problems(original: zipfile.ZipFile, result: zipfile.ZipFile) -> list[str]:
    """The problems with the signed APK's libvrapi.so: it must be the pin tool's output for the source APK's libvrapi.so
    and the SIGNED libr15.so, byte for byte. An unpinned copy, a copy pinned to another libr15 and a missing entry are each named."""
    if PINNED_LIBRARY not in result.namelist():
        return [f"{PINNED_LIBRARY}: the pinned runtime belongs here, missing from the signed APK"]
    if PATCHED_LIBRARY not in result.namelist():
        return [f"{PINNED_LIBRARY}: cannot be checked, the signed APK has no {PATCHED_LIBRARY} to pin it to"]
    signed_md5 = entry_md5(result, PATCHED_LIBRARY)
    source = original.read(PINNED_LIBRARY)
    try:
        expected, report = runtime_pin.pin_data(source, signed_md5)
    except runtime_pin.PinError as error:
        return [f"{PINNED_LIBRARY}: the source APK's runtime cannot be pinned to the signed libr15.so (md5 {signed_md5}): {error}"]
    signed = result.read(PINNED_LIBRARY)
    print(f"quest_repack_libs: libvrapi.so sha256 source  {runtime_pin.sha256_hex(source)}", flush=True)
    print(f"quest_repack_libs: libvrapi.so sha256 pinned  {runtime_pin.sha256_hex(expected)} (libr15.so md5 {signed_md5})", flush=True)
    print(f"quest_repack_libs: libvrapi.so sha256 signed  {runtime_pin.sha256_hex(signed)}", flush=True)
    if signed == expected:
        return []
    if signed == source:
        return [f"{PINNED_LIBRARY}: the signed APK holds the source APK's runtime unpinned: its modules would refuse "
                f"the signed libr15.so (md5 {signed_md5})"]
    return [f"{PINNED_LIBRARY}: the signed APK does not hold the source APK's runtime pinned to the signed libr15.so "
            f"(md5 {signed_md5}): another libr15 or another edit"]


def verify_signed(source: pathlib.Path, signed: pathlib.Path, stage: pathlib.Path, patcher: pathlib.Path,
                  extra_rows: pathlib.Path | None = None, scratch: pathlib.Path | None = None) -> list[str]:
    """The problems found in `signed`; empty when every original library survived, libr15.so is the patcher's output
    for the store APK's, the reference runtime (when the source APK carries one) is pinned to the signed libr15.so and
    every staged one is there. The three digests of libr15.so are printed."""
    problems: list[str] = []
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(signed) as result:
        present = set(result.namelist())
        runtime = source_is_reference_runtime(original)
        for entry in lib_entries(original):
            if entry in (REPLACED_ON_PURPOSE, PATCHED_LIBRARY) or (runtime and entry == PINNED_LIBRARY):
                continue
            if entry not in present:
                problems.append(f"{entry}: in the store APK, missing from the signed APK")
            elif entry_digest(original, entry) != entry_digest(result, entry):
                problems.append(f"{entry}: the signed APK holds different bytes than the store APK")
        digests: dict[str, str] = {}
        if PATCHED_LIBRARY not in original.namelist():
            problems.append(f"{PATCHED_LIBRARY}: missing from the store APK, so there is nothing to patch")
        else:
            patched = patched_digest(original, patcher, extra_rows, scratch, digests, problems)
            if PATCHED_LIBRARY not in present:
                problems.append(f"{PATCHED_LIBRARY}: the patcher's output belongs here, missing from the signed APK")
            else:
                digests["signed"] = entry_digest(result, PATCHED_LIBRARY)
                if patched is not None and digests["signed"] != patched:
                    problems.append(f"{PATCHED_LIBRARY}: the signed APK does not hold the patcher's output for the store "
                                    f"APK's libr15.so (signed sha-256 {digests['signed']}, patcher output {patched})")
            if patched is not None and patched == digests["store"]:
                problems.append(f"{PATCHED_LIBRARY}: the patcher's output is the store APK's libr15.so, byte for byte: "
                                "nothing was patched")
        for label in ("store", "patched", "signed"):
            print(f"quest_repack_libs: libr15.so sha256 {label:<7} {digests.get(label, '(not available)')}", flush=True)
        if runtime:
            problems.extend(pinned_runtime_problems(original, result))
        staged_dir = stage / ABI_DIR
        for staged in sorted(p for p in staged_dir.iterdir() if p.is_file()):
            entry = f"{ABI_DIR}/{staged.name}"
            if entry not in present:
                problems.append(f"{entry}: staged, missing from the signed APK")
            elif entry_digest(result, entry) != file_digest(staged):
                problems.append(f"{entry}: the signed APK holds different bytes than the staged file")
        problems.extend(plugin_manifest_problems(stage))
        problems.extend(pack_problems(stage))
        problems.extend(pack_signed_problems(result, stage))
    size = signed.stat().st_size
    if size > APK_LIMIT_BYTES:
        problems.append(f"{signed.name}: {size} bytes, over the 4 GiB a zip without zip64 holds (the sentinel's zip reader "
                        "refuses zip64)")
    return problems


def pinned_manifest(plan_header: pathlib.Path) -> tuple[str, str] | None:
    """`(version folder, manifest id)` of the data step's `kManifestPath`, read from `plan_header`; None when it names none."""
    try:
        found = _MANIFEST_PIN.search(plan_header.read_text())
    except OSError:
        return None
    return (found.group(1), found.group(2)) if found else None


def safe_pack_entry_name(name: str) -> bool:
    """The rule of the sentinel's zip reader (src/quest/data/zip.cpp SafeEntryName): a relative path of plain components."""
    if not name or len(name.encode("utf-8")) > 512 or name.startswith("/"):
        return False
    body = name[:-1] if name.endswith("/") else name
    if not body:
        return False
    parts = body.split("/")
    if len(parts) > 16:
        return False
    for part in parts:
        if not part or part in (".", "..") or len(part.encode("utf-8")) > 255:
            return False
        if any(c == "\\" or ord(c) < 0x20 or ord(c) == 0x7F for c in part):
            return False
        if len(part) >= 2 and part[1] == ":":
            return False
    return True


def is_pack_path(path: str, manifest_id: str) -> str | None:
    """None when `path` is a file the runtime places (`manifests/<the game's manifest>` or `packages/<name>`), else the reason."""
    directory, _, name = path.partition("/")
    if directory not in ("manifests", "packages") or not name:
        return f"{path}: not a file directly in manifests/ or packages/"
    if not _PACK_FILE_NAME.match(name) or name.startswith("."):
        return f"{path}: not a plain file name"
    if name.endswith(RESERVED_SUFFIXES):
        return f"{path}: a name the step writes for itself"
    if directory == "manifests" and name != manifest_id:
        return f"{path}: the only manifest a pack may carry is manifests/{manifest_id}"
    return None


def pack_meta_problems(meta: dict, version_folder: str, manifest_id: str) -> tuple[list[str], list[str]]:
    """The problems with the pack's description (its meta file) and the sorted `requires` list."""
    problems: list[str] = []
    if not isinstance(meta.get("id"), str) or not _PACK_ID.match(meta["id"]):
        problems.append("meta: id must match [a-z0-9][a-z0-9._-]{0,63}")
    if not isinstance(meta.get("version"), str) or not _PACK_VERSION.match(meta["version"]):
        problems.append("meta: version must be X.Y.Z")
    requires = meta.get("requires_stock_manifest_sha256")
    if (not isinstance(requires, list) or not 1 <= len(requires) <= 2
            or not all(isinstance(h, str) and _HEX64.match(h) for h in requires)):
        problems.append("meta: requires_stock_manifest_sha256 must be one or two lowercase sha-256 hex digests")
        requires = []
    elif len(set(requires)) != len(requires):
        problems.append("meta: requires_stock_manifest_sha256 names a digest twice")
    prefix = _ZIP_PREFIX.match(meta.get("zip_prefix", "")) if isinstance(meta.get("zip_prefix"), str) else None
    if prefix is None:
        problems.append("meta: zip_prefix must be _data/<version>/rad15/android/")
    elif prefix.group(1) != version_folder:
        problems.append(f"meta: zip_prefix names data version {prefix.group(1)}, the data step's kManifestPath says {version_folder}")
    return problems, sorted(requires)


def pack_zip_scan(pack_zip: pathlib.Path, prefix: str, manifest_id: str) -> tuple[list[str], dict[str, tuple[str, int]]]:
    """Reads every entry of `pack_zip`: the problems, and `{path below prefix: (sha-256 of the bytes read, size)}`."""
    problems: list[str] = []
    found: dict[str, tuple[str, int]] = {}
    try:
        archive = zipfile.ZipFile(pack_zip)
    except (OSError, zipfile.BadZipFile) as error:
        return [f"{pack_zip}: not a readable zip archive: {error}"], found
    with archive:
        infos = archive.infolist()
        folded: dict[str, str] = {}
        for info in infos:
            name = info.filename
            if not safe_pack_entry_name(name):
                problems.append(f"{name!r}: an entry name the sentinel's zip reader refuses")
                continue
            key = (name[:-1] if name.endswith("/") else name).lower()
            if key in folded:
                problems.append(f"{name}: the same name as {folded[key]} (names that differ only in case are one name)")
            folded[key] = name
            if info.flag_bits & 0x1:
                problems.append(f"{name}: encrypted")
            if info.create_system == 3:
                mode = info.external_attr >> 16  # a mode with no file type bits (python's zipfile writes one) is a plain file
                if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                    problems.append(f"{name}: a link or another special file")
            if info.is_dir():
                if not (prefix.startswith(name) or name.startswith(prefix)):
                    problems.append(f"{name}: a directory outside {prefix}")
                continue
            if not name.startswith(prefix):
                problems.append(f"{name}: outside {prefix}")
                continue
            relative = name[len(prefix):]
            reason = is_pack_path(relative, manifest_id)
            if reason is not None:
                problems.append(reason)
                continue
            digest, size = hashlib.sha256(), 0
            try:
                with archive.open(info) as stream:
                    while chunk := stream.read(_CHUNK):
                        digest.update(chunk)
                        size += len(chunk)
            except (OSError, zipfile.BadZipFile, RuntimeError, EOFError) as error:
                problems.append(f"{name}: cannot be read: {error}")
                continue
            if size != info.file_size:
                problems.append(f"{name}: the zip records {info.file_size} bytes, {size} were read")
            if size == 0 or size > MAX_PACK_FILE_BYTES:
                problems.append(f"{name}: {size} bytes (a pack file is 1 byte to {MAX_PACK_FILE_BYTES})")
            found[relative] = (digest.hexdigest(), size)
    if len(found) > MAX_PACK_FILES:
        problems.append(f"{len(found)} files: a pack carries at most {MAX_PACK_FILES}")
    if f"manifests/{manifest_id}" not in found:
        problems.append(f"the zip has no {prefix}manifests/{manifest_id}")
    return problems, found


def pack_json_bytes(meta: dict, requires: list[str], version_folder: str, files: dict[str, tuple[str, int]],
                    zip_sha256: str, zip_size: int) -> bytes:
    """pack.json: schema 1, sorted keys, no spaces, one trailing newline: the same pack gives the same bytes."""
    document = {
        "schema": PACK_SCHEMA,
        "id": meta["id"],
        "version": meta["version"],
        "stock_data_version": version_folder,
        "requires_stock_manifest_sha256": requires,
        "files": [{"path": path, "sha256": files[path][0], "size": files[path][1]} for path in sorted(files)],
        "source_zip_sha256": zip_sha256,
        "source_zip_size": zip_size,
    }
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def stage_pack(pack_zip: pathlib.Path, meta_path: pathlib.Path, stage: pathlib.Path, store_apk: pathlib.Path,
               plan_header: pathlib.Path = PLAN_HEADER) -> list[str]:
    """Stages the pack under `stage` as APK assets; returns the problems and writes nothing when there are any."""
    pin = pinned_manifest(plan_header)
    if pin is None:
        return [f"{plan_header}: names no kManifestPath, so the pack cannot be checked against the data step's pins"]
    version_folder, manifest_id = pin
    try:
        meta = json.loads(meta_path.read_bytes())
        if not isinstance(meta, dict):
            raise ValueError("not an object")
    except (OSError, ValueError) as error:
        return [f"{meta_path}: cannot be read as a JSON object: {error}"]
    problems, requires = pack_meta_problems(meta, version_folder, manifest_id)
    if problems:
        return problems
    prefix = meta["zip_prefix"]
    problems, files = pack_zip_scan(pack_zip, prefix, manifest_id)
    try:
        with zipfile.ZipFile(store_apk) as store:
            taken = [n for n in store.namelist() if n == PACK_DIR or n.startswith(PACK_DIR + "/")]
        store_size = store_apk.stat().st_size
    except (OSError, zipfile.BadZipFile) as error:
        return problems + [f"{store_apk}: cannot be read as the store APK: {error}"]
    problems += [f"{store_apk.name} already has the entry {n}: a staged pack would replace it" for n in taken]
    total = sum(size for _digest, size in files.values())
    if store_size + total + APK_SLACK_BYTES > APK_LIMIT_BYTES:
        problems.append(f"the APK would be about {store_size + total} bytes: a zip without zip64 holds {APK_LIMIT_BYTES} "
                        "(the sentinel's zip reader refuses zip64)")
    if problems:
        return problems
    zip_sha256, zip_size = file_digest(pack_zip), pack_zip.stat().st_size
    root = stage / PACK_FILES_DIR
    with zipfile.ZipFile(pack_zip) as archive:
        for relative, (digest, size) in sorted(files.items()):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            copied, written = hashlib.sha256(), 0
            with archive.open(prefix + relative) as source, target.open("wb") as sink:
                while chunk := source.read(_CHUNK):
                    sink.write(chunk)
                    copied.update(chunk)
                    written += len(chunk)
            if (copied.hexdigest(), written) != (digest, size):
                return [f"{prefix}{relative}: the bytes read twice differ"]
    (stage / PACK_JSON_ENTRY).write_bytes(pack_json_bytes(meta, requires, version_folder, files, zip_sha256, zip_size))
    for relative in sorted(files):
        print(f"quest_repack_libs: pack file {relative} sha256 {files[relative][0]} size {files[relative][1]}", flush=True)
    return []


def pack_document_problems(document: object, version_folder: str, manifest_id: str) -> list[str]:
    """The rules of the runtime's ParsePackJson (src/quest/data/pack.cpp) over a parsed pack.json."""
    if not isinstance(document, dict):
        return ["pack.json: not a JSON object"]
    problems: list[str] = []
    if document.get("schema") != PACK_SCHEMA or isinstance(document.get("schema"), bool):
        problems.append("pack.json: schema must be 1")
    if not isinstance(document.get("id"), str) or not _PACK_ID.match(document["id"]):
        problems.append("pack.json: bad id")
    if not isinstance(document.get("version"), str) or not _PACK_VERSION.match(document["version"]):
        problems.append("pack.json: bad version")
    if document.get("stock_data_version") != version_folder:
        problems.append(f"pack.json: stock_data_version is not {version_folder}, the data step's")
    requires = document.get("requires_stock_manifest_sha256")
    if (not isinstance(requires, list) or not 1 <= len(requires) <= 2
            or not all(isinstance(h, str) and _HEX64.match(h) for h in requires) or requires != sorted(set(requires))):
        problems.append("pack.json: requires_stock_manifest_sha256 must be one or two sorted, distinct sha-256 digests")
    files = document.get("files")
    manifests = 0
    if not isinstance(files, list) or not 1 <= len(files) <= MAX_PACK_FILES:
        problems.append(f"pack.json: files must hold 1 to {MAX_PACK_FILES} entries")
        files = []
    seen: set[str] = set()
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            problems.append("pack.json: a file entry has no path")
            continue
        reason = is_pack_path(entry["path"], manifest_id)
        if reason is not None:
            problems.append(f"pack.json: {reason}")
        if entry["path"].lower() in seen:
            problems.append(f"pack.json: {entry['path']} twice")
        seen.add(entry["path"].lower())
        manifests += entry["path"] == f"manifests/{manifest_id}"
        if not isinstance(entry.get("sha256"), str) or not _HEX64.match(entry["sha256"]):
            problems.append(f"pack.json: {entry['path']}: bad sha256")
        size = entry.get("size")
        if not isinstance(size, int) or isinstance(size, bool) or not 1 <= size <= MAX_PACK_FILE_BYTES:
            problems.append(f"pack.json: {entry['path']}: bad size")
    if manifests != 1:
        problems.append(f"pack.json: files must name manifests/{manifest_id} once")
    source_size = document.get("source_zip_size")
    if (not isinstance(document.get("source_zip_sha256"), str) or not _HEX64.match(document["source_zip_sha256"])
            or not isinstance(source_size, int) or isinstance(source_size, bool) or source_size < 1):
        problems.append("pack.json: bad source_zip_sha256 or source_zip_size")
    return problems


def pack_problems(stage: pathlib.Path, plan_header: pathlib.Path = PLAN_HEADER) -> list[str]:
    """The problems with the pack staged under `stage`; empty when none is staged or it is whole and the runtime takes it."""
    pack_dir = stage / PACK_DIR
    if not pack_dir.exists():
        return []
    pin = pinned_manifest(plan_header)
    if pin is None:
        return [f"{plan_header}: names no kManifestPath, so the staged pack cannot be checked"]
    json_path = stage / PACK_JSON_ENTRY
    if not json_path.is_file():
        return [f"{PACK_JSON_ENTRY}: staged files without a pack.json"]
    try:
        document = json.loads(json_path.read_bytes())
    except (OSError, ValueError) as error:
        return [f"{PACK_JSON_ENTRY}: cannot be read as JSON: {error}"]
    problems = pack_document_problems(document, *pin)
    if problems:
        return problems
    listed: set[str] = set()
    for entry in document["files"]:
        listed.add(entry["path"])
        staged = stage / PACK_FILES_DIR / entry["path"]
        if staged.is_symlink() or not staged.is_file():
            problems.append(f"{PACK_FILES_DIR}/{entry['path']}: listed in pack.json, not staged as a file")
            continue
        if staged.stat().st_size != entry["size"]:
            problems.append(f"{PACK_FILES_DIR}/{entry['path']}: {staged.stat().st_size} bytes staged, pack.json says {entry['size']}")
        elif file_digest(staged) != entry["sha256"]:
            problems.append(f"{PACK_FILES_DIR}/{entry['path']}: the sha256 pack.json lists is not the staged file's")
    for found in sorted(p for p in (stage / PACK_FILES_DIR).rglob("*") if p.is_file() or p.is_symlink()):
        relative = found.relative_to(stage / PACK_FILES_DIR).as_posix()
        if relative not in listed:
            problems.append(f"{PACK_FILES_DIR}/{relative}: staged, not listed in pack.json")
    for found in sorted(p for p in pack_dir.iterdir() if p.name not in ("pack.json", "files")):
        problems.append(f"{PACK_DIR}/{found.name}: not part of a pack")
    return problems


def pack_signed_problems(result: zipfile.ZipFile, stage: pathlib.Path) -> list[str]:
    """Every staged pack file is in the signed APK with the staged bytes, STORED, and the signed APK has no pack entry that
    was not staged (a build with no pack carries none)."""
    problems: list[str] = []
    staged_root = stage / PACK_DIR
    staged = sorted(p for p in staged_root.rglob("*") if p.is_file()) if staged_root.exists() else []
    expected = {p.relative_to(stage).as_posix() for p in staged}
    present = {i.filename: i for i in result.infolist()}
    for name in sorted(n for n in present if (n == PACK_DIR or n.startswith(PACK_DIR + "/")) and n not in expected
                       and not present[n].is_dir()):
        problems.append(f"{name}: in the signed APK, not staged")
    for path in staged:
        name = path.relative_to(stage).as_posix()
        info = present.get(name)
        if info is None:
            problems.append(f"{name}: staged, missing from the signed APK")
        elif info.compress_type != zipfile.ZIP_STORED:
            problems.append(f"{name}: in the signed APK but not STORED (compression {info.compress_type})")
        elif entry_digest(result, name) != file_digest(path):
            problems.append(f"{name}: the signed APK holds different bytes than the staged file")
    return problems


def note_desc(image: bytes, owner: bytes, note_type: int) -> bytes | None:
    """The description of the first note owned by `owner` with `note_type` in a PT_NOTE segment of the 64-bit
    little-endian ELF `image`; None when there is none or the headers do not hold together (plugin_image.cpp)."""
    if len(image) < 64 or image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        return None
    phoff, = struct.unpack_from("<Q", image, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", image, 0x36)
    if phentsize < 56 or phoff + phnum * phentsize > len(image):
        return None
    for k in range(phnum):
        p_type, _flags, p_offset, _vaddr, _paddr, p_filesz = struct.unpack_from("<IIQQQQ", image, phoff + k * phentsize)
        if p_type != PT_NOTE or p_offset + p_filesz > len(image):
            continue
        at, end = p_offset, p_offset + p_filesz
        while end - at >= 12:
            namesz, descsz, n_type = struct.unpack_from("<III", image, at)
            at += 12
            name_len, desc_len = (namesz + 3) & ~3, (descsz + 3) & ~3
            if name_len > end - at:
                break
            name = image[at:at + namesz]
            if n_type == note_type and name == owner and descsz <= end - at - name_len:
                return image[at + name_len:at + name_len + descsz]
            if desc_len > end - at - name_len:
                break
            at += name_len + desc_len
    return None


def build_id_problems(libr15: bytes, plugin: bytes, plugin_name: str) -> list[str]:
    """Why the loader would refuse `plugin` for its build IDs against this `libr15`; empty when it would not."""
    running = note_desc(libr15, b"GNU\0", NT_GNU_BUILD_ID)
    if not running:
        return [f"the store APK's {LIBR15} has no GNU build ID: nothing can be checked"]
    declared = note_desc(plugin, b"NEVR\0", NEVR_NOTE_TYPE)
    if declared is None:
        return [f"plugin {plugin_name} declares no libr15 build IDs (NEVR_DECLARE_SUPPORTED_BUILD_IDS); the loader "
                "refuses it as build_ids_undeclared"]
    ids = [i for i in re.split(r"[ ,\t\r\n]+", declared.split(b"\0", 1)[0].decode("ascii", "replace")) if i]
    if running.hex() not in ids:
        return [f"plugin {plugin_name} declares {ids or 'an empty list'}, not the store libr15 build ID {running.hex()}; "
                "the loader refuses it as build_id_unsupported"]
    return []


def check_plugin_build_ids(apk: pathlib.Path, plugin: pathlib.Path) -> list[str]:
    with zipfile.ZipFile(apk) as archive:
        if LIBR15 not in archive.namelist():
            return [f"the store APK has no {LIBR15}"]
        libr15 = archive.read(LIBR15)
    return build_id_problems(libr15, plugin.read_bytes(), plugin.name)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    name = sub.add_parser("check-plugin-name")
    name.add_argument("--apk", type=pathlib.Path, required=True)
    name.add_argument("--plugin", type=pathlib.Path, required=True)
    rows = sub.add_parser("check-shim-rows")
    rows.add_argument("--shim", type=pathlib.Path, required=True)
    rows.add_argument("--extra-rows", type=pathlib.Path, default=None)
    ids = sub.add_parser("check-plugin-build-ids")
    ids.add_argument("--apk", type=pathlib.Path, required=True)
    ids.add_argument("--plugin", type=pathlib.Path, required=True)
    manifest = sub.add_parser("write-plugin-manifest")
    manifest.add_argument("--stage", type=pathlib.Path, required=True)
    manifest.add_argument("--plugin", type=pathlib.Path, nargs="+", required=True)
    manifest.add_argument("--early", action="append", default=[])
    manifest.add_argument("--off", action="append", default=[])
    pack = sub.add_parser("stage-pack")
    pack.add_argument("--pack-zip", type=pathlib.Path, required=True)
    pack.add_argument("--meta", type=pathlib.Path, required=True)
    pack.add_argument("--stage", type=pathlib.Path, required=True)
    pack.add_argument("--store-apk", type=pathlib.Path, required=True)
    pack.add_argument("--plan-header", type=pathlib.Path, default=PLAN_HEADER)
    pack_check = sub.add_parser("pack-problems")
    pack_check.add_argument("--stage", type=pathlib.Path, required=True)
    pack_check.add_argument("--plan-header", type=pathlib.Path, default=PLAN_HEADER)
    staged_manifest = sub.add_parser("check-plugin-manifest")
    staged_manifest.add_argument("--stage", type=pathlib.Path, required=True)
    verify = sub.add_parser("verify-signed")
    verify.add_argument("--source", type=pathlib.Path, required=True)
    verify.add_argument("--signed", type=pathlib.Path, required=True)
    verify.add_argument("--stage", type=pathlib.Path, required=True)
    verify.add_argument("--patcher", type=pathlib.Path, required=True)
    verify.add_argument("--extra-rows", type=pathlib.Path, default=None)
    verify.add_argument("--scratch", type=pathlib.Path, default=None)
    args = parser.parse_args(argv)
    if args.command == "check-plugin-name":
        problems = [f"plugin {args.plugin.name} would replace {c}" for c in check_plugin_name(args.apk, args.plugin)]
    elif args.command == "check-shim-rows":
        problems = shim_rows_problems(args.shim, args.extra_rows)
    elif args.command == "check-plugin-build-ids":
        problems = check_plugin_build_ids(args.apk, args.plugin)
    elif args.command == "write-plugin-manifest":
        problems = write_plugin_manifest(args.stage, args.plugin, args.early, args.off)
    elif args.command == "check-plugin-manifest":
        problems = plugin_manifest_problems(args.stage)
    elif args.command == "stage-pack":
        problems = stage_pack(args.pack_zip, args.meta, args.stage, args.store_apk, args.plan_header)
    elif args.command == "pack-problems":
        problems = pack_problems(args.stage, args.plan_header)
    else:
        problems = verify_signed(args.source, args.signed, args.stage, args.patcher, args.extra_rows, args.scratch)
    for problem in problems:
        print(f"quest_repack_libs: {problem}", file=sys.stderr)
    if not problems:
        print(f"quest_repack_libs: {args.command} OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
