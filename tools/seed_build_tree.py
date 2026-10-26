#!/usr/bin/env python3
"""Seed a linked worktree's build/<preset> from the main checkout's, so the worktree's first build is incremental.

    tools/seed_build_tree.py --main DIR --worktree DIR --preset NAME [--log FILE]

`just worktree-setup` runs it. The worktree's build/<preset> is filled only when it is absent or an empty
directory; a directory with content is kept as it is. The main checkout is only read.

A build tree is not relocatable as it stands, so the copy is made in a staging directory inside the worktree's
build/ and moved into place when it is complete:

  1. the checkout path in every text file of the copy (CMakeCache.txt, build.ninja, compile_commands.json,
     CMakeFiles/**, *.cmake ...) becomes the worktree's path, at a path-component boundary (a sibling directory
     whose name starts with the checkout's name is left alone);
  2. .ninja_deps (binary: one record per path, each padded to four bytes) is rewritten the same way, so a header
     edit in the worktree reaches the objects that read it;
  3. .ninja_log is rewritten: the paths, and the command hash of each edge. ninja compares that hash with the hash
     of the edge's command line, and every command line holds absolute paths, so an entry with the old hash is
     "command line changed" and rebuilds. The new hash is computed from the command ninja prints for the copy
     (`ninja -t commands`) with the path replaced; an entry whose command cannot be found is dropped (the edge
     rebuilds, nothing wrong is kept);
  4. a tracked source file that is byte-identical to the main checkout's gets the main checkout's mtime. A new
     worktree's files carry the time of the checkout, newer than every object in the copy, and ninja decides by
     mtime alone. A file that differs keeps its own mtime, so it rebuilds.

The commit is a guard. The root CMakeLists.txt puts the commit (NEVR_GIT_COMMIT_FULL) and the version on every
compile command, so at any other commit ninja sees every compile command changed and rebuilds every object
whatever was copied. A copy whose build.ninja names a commit other than the worktree's HEAD is therefore not made
(the first build is the ordinary one, answered object by object from the shared compiler cache). A tree whose
commands carry no such definition is not checked. The version carries `-dirty` when the tree was configured with
uncommitted changes; such a tree is refused for the same reason.

What is not carried: objects built in the main checkout keep the main checkout's path in anything they embed
(`__FILE__`, debug info); the rebuilt ones carry the worktree's.

Every outcome (seeded, kept, not seeded and why) is printed and appended as one JSON line to the log
(default $XDG_STATE_HOME/nevr-runtime/seed-build-tree.jsonl); a log that cannot be written is an error.
Exit status: 0 for seeded, kept and not seeded (the worktree still builds, from scratch); 1 for an error; 2 for usage.
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime
import filecmp
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time

# A file with one of these suffixes is never read as text (objects, archives, images, packages).
BINARY_SUFFIXES = frozenset({
    ".o", ".obj", ".a", ".lib", ".exe", ".dll", ".dbg", ".pdb", ".gch", ".pch", ".so", ".dylib", ".zip", ".gz",
    ".xz", ".zst", ".tar", ".png", ".ico", ".bin", ".res", ".pyc",
})
DEPS_SIGNATURE = b"# ninjadeps\n"
DEPS_VERSION = 4
LOG_HEADER = "# ninja log v7"
COMMANDS_PER_CALL = 300

# rapidhash (version 1) with its default seed and secret: the hash ninja 1.12 and later keeps of each command line.
_M64 = (1 << 64) - 1
_SECRET = (0x2D358DCCAA6C78A5, 0x8BB84B93962EACC9, 0x4B33A62ED433D4A3)
_SEED = 0xBDD89AA982704029


class SeedError(Exception):
    """A seed that failed after it started: the staging copy is removed and the destination is untouched."""


@dataclasses.dataclass
class Result:
    status: str  # "seeded", "kept", "not-seeded" or "error"
    reason: str = ""
    counts: dict = dataclasses.field(default_factory=dict)


def _mum(a: int, b: int) -> tuple[int, int]:
    product = a * b
    return product & _M64, product >> 64


def _mix(a: int, b: int) -> int:
    lo, hi = _mum(a, b)
    return lo ^ hi


def command_hash(data: bytes) -> int:
    """The hash ninja stores in .ninja_log for a command line."""
    n = len(data)
    r64 = lambda i: int.from_bytes(data[i:i + 8], "little")  # noqa: E731
    r32 = lambda i: int.from_bytes(data[i:i + 4], "little")  # noqa: E731
    seed = (_SEED ^ _mix(_SEED ^ _SECRET[0], _SECRET[1]) ^ n) & _M64
    if n <= 16:
        if n >= 4:
            a = (r32(0) << 32) | r32(n - 4)
            delta = (n & 24) >> (n >> 3)
            b = (r32(delta) << 32) | r32(n - 4 - delta)
        elif n > 0:
            a = (data[0] << 56) | (data[n >> 1] << 32) | data[n - 1]
            b = 0
        else:
            a = b = 0
    else:
        off = 0
        left = n
        if left > 48:
            see1 = see2 = seed
            while left >= 48:
                seed = _mix(r64(off) ^ _SECRET[0], r64(off + 8) ^ seed)
                see1 = _mix(r64(off + 16) ^ _SECRET[1], r64(off + 24) ^ see1)
                see2 = _mix(r64(off + 32) ^ _SECRET[2], r64(off + 40) ^ see2)
                off += 48
                left -= 48
            seed ^= see1 ^ see2
        if left > 16:
            seed = _mix(r64(off) ^ _SECRET[2], r64(off + 8) ^ seed ^ _SECRET[1])
            if left > 32:
                seed = _mix(r64(off + 16) ^ _SECRET[2], r64(off + 24) ^ seed)
        a = r64(off + left - 16)
        b = r64(off + left - 8)
    a ^= _SECRET[1]
    b ^= seed
    a, b = _mum(a, b)
    return _mix(a ^ _SECRET[0] ^ n, b ^ _SECRET[1])


def path_rewriter(old: str, new: str):
    """bytes -> (bytes, count): `old` becomes `new` where `old` ends at a path-component boundary."""
    pattern = re.compile(re.escape(old.encode()) + rb"(?![\w.+-])")
    replacement = new.encode().replace(b"\\", b"\\\\")

    def rewrite(data: bytes) -> tuple[bytes, int]:
        if old.encode() not in data:
            return data, 0
        return pattern.subn(replacement, data)

    return rewrite


def rewrite_deps_log(data: bytes, rewrite) -> tuple[bytes, int]:
    """Rewrite the path records of a ninja deps log (version 4); deps records refer to paths by index and stay."""
    if not data.startswith(DEPS_SIGNATURE) or len(data) < len(DEPS_SIGNATURE) + 4:
        raise SeedError(".ninja_deps does not start with the ninja deps log signature")
    (version,) = struct.unpack_from("<i", data, len(DEPS_SIGNATURE))
    if version != DEPS_VERSION:
        raise SeedError(f".ninja_deps is version {version}; this tool reads version {DEPS_VERSION}")
    off = len(DEPS_SIGNATURE) + 4
    out = bytearray(data[:off])
    changed = 0
    while off < len(data):
        if off + 4 > len(data):
            raise SeedError(".ninja_deps ends inside a record header")
        (word,) = struct.unpack_from("<I", data, off)
        size = word & 0x7FFFFFFF
        payload = data[off + 4:off + 4 + size]
        if len(payload) != size or (not word & 0x80000000 and size < 4):
            raise SeedError(".ninja_deps ends inside a record")
        if word & 0x80000000:
            out += data[off:off + 4 + size]
        else:
            path, checksum = payload[:-4].rstrip(b"\0"), payload[-4:]
            new_path, n = rewrite(path)
            changed += n
            padded = new_path + b"\0" * (-len(new_path) % 4) + checksum
            out += struct.pack("<I", len(padded)) + padded
        off += 4 + size
    return bytes(out), changed


def _ninja(ninja: str, build: pathlib.Path, *args: str) -> str:
    proc = subprocess.run([ninja, "-C", str(build), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise SeedError(f"`ninja -C <copy> {' '.join(args[:2])}` failed: {proc.stderr.strip() or proc.stdout.strip()}")
    return proc.stdout


def command_hash_map(ninja: str, copy: pathlib.Path, outputs: set[str], rewrite) -> dict[int, int]:
    """old command hash -> hash of the same command with the path rewritten, for the edges of `outputs`."""
    manifest = {line.rsplit(": ", 1)[0] for line in _ninja(ninja, copy, "-t", "targets", "all").splitlines() if ": " in line}
    targets = sorted(outputs & manifest)
    mapping: dict[int, int] = {}
    for i in range(0, len(targets), COMMANDS_PER_CALL):
        text = _ninja(ninja, copy, "-t", "commands", *targets[i:i + COMMANDS_PER_CALL])
        for command in text.splitlines():
            old = command.encode()
            mapping[command_hash(old)] = command_hash(rewrite(old)[0])
    return mapping


def rewrite_ninja_log(text: str, rewrite, hashes: dict[int, int]) -> tuple[str, int, int]:
    lines = text.splitlines()
    if not lines or lines[0] != LOG_HEADER:
        raise SeedError(f".ninja_log does not start with `{LOG_HEADER}` (a different ninja version wrote it)")
    out = [LOG_HEADER]
    kept = dropped = 0
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 5:
            dropped += 1
            continue
        try:
            new_hash = hashes.get(int(fields[4], 16))
        except ValueError:
            new_hash = None
        if new_hash is None:
            dropped += 1
            continue
        fields[3] = rewrite(fields[3].encode())[0].decode()
        fields[4] = f"{new_hash:x}"
        out.append("\t".join(fields))
        kept += 1
    return "\n".join(out) + "\n", kept, dropped


def _is_text(path: pathlib.Path) -> bool:
    if path.suffix in BINARY_SUFFIXES:
        return False
    with open(path, "rb") as f:
        return b"\0" not in f.read(8192)


def rewrite_text_files(copy: pathlib.Path, rewrite) -> int:
    rewritten = 0
    for dirpath, dirs, files in os.walk(copy):
        # A link to a directory is listed with the directories; retarget it like a link to a file.
        files = files + [d for d in dirs if pathlib.Path(dirpath, d).is_symlink()]
        for name in files:
            path = pathlib.Path(dirpath, name)
            st = path.lstat()
            if name in (".ninja_log", ".ninja_deps"):
                continue
            if path.is_symlink():
                target = os.readlink(path)
                new_target, n = rewrite(target.encode())
                if n:
                    path.unlink()
                    path.symlink_to(new_target.decode())
                    rewritten += 1
                continue
            if not _is_text(path):
                continue
            data = path.read_bytes()
            new_data, n = rewrite(data)
            if n:
                path.write_bytes(new_data)
                os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
                rewritten += 1
    return rewritten


def align_source_mtimes(main: pathlib.Path, worktree: pathlib.Path) -> int:
    """Give each tracked file that is byte-identical to the main checkout's the main checkout's mtime."""
    listing = subprocess.run(["git", "-C", str(worktree), "ls-files", "-z"], check=True, capture_output=True).stdout
    aligned = 0
    for raw in listing.split(b"\0"):
        if not raw:
            continue
        rel = os.fsdecode(raw)
        mine, theirs = worktree / rel, main / rel
        if mine.is_symlink() or theirs.is_symlink() or not mine.is_file() or not theirs.is_file():
            continue
        a, b = mine.stat(), theirs.stat()
        if a.st_size != b.st_size or a.st_mtime_ns == b.st_mtime_ns:
            continue
        if filecmp.cmp(mine, theirs, shallow=False):
            os.utime(mine, ns=(a.st_atime_ns, b.st_mtime_ns))
            aligned += 1
    return aligned


_COMMIT_DEFINE = re.compile(rb'NEVR_GIT_COMMIT_FULL=\\*"([0-9a-f]{40})')


_DESCRIBE_DEFINE = re.compile(rb'NEVR_GIT_DESCRIBE=\\*"([^"\\ ]*)')


def built_commits(build_ninja: pathlib.Path) -> set[str]:
    """The commits the compile commands of a build tree carry (one commit per configure)."""
    return {m.group(1).decode() for m in _COMMIT_DEFINE.finditer(build_ninja.read_bytes())}


def built_modified(build_ninja: pathlib.Path) -> bool:
    """True when the tree was configured with uncommitted changes (`git describe --dirty` ends in -dirty)."""
    return any(m.group(1).endswith(b"-dirty") for m in _DESCRIBE_DEFINE.finditer(build_ninja.read_bytes()))


def _cache_value(cache: pathlib.Path, key: str) -> str | None:
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1]
    return None


BUILD_PROCESSES = frozenset({"ninja", "cmake", "ctest"})


def _running_in(directory: pathlib.Path) -> list[str]:
    """ninja, cmake and ctest processes working in `directory` (cwd inside it, or it named on the command line)."""
    root = str(directory.resolve())
    found = []
    for proc in pathlib.Path("/proc").iterdir():
        if not proc.name.isdigit() or int(proc.name) == os.getpid():
            continue
        try:
            argv = (proc / "cmdline").read_bytes().split(b"\0")
            cwd = os.readlink(proc / "cwd")
        except OSError:
            continue
        if os.path.basename(argv[0].decode(errors="replace")) not in BUILD_PROCESSES:
            continue
        cmdline = b" ".join(argv).decode(errors="replace").strip()
        if cwd == root or cwd.startswith(root + "/") or root in cmdline:
            found.append(f"{proc.name} {cmdline[:80]}")
    return found


def _fingerprint(build: pathlib.Path) -> list:
    out = []
    for name in ("CMakeCache.txt", "build.ninja", ".ninja_log", ".ninja_deps"):
        try:
            st = (build / name).stat()
        except FileNotFoundError:  # a tree that never recorded a dependency has no deps log
            continue
        out.append((name, st.st_size, st.st_mtime_ns))
    return out


def _is_empty_dir(path: pathlib.Path) -> bool:
    return path.is_dir() and not path.is_symlink() and not any(path.iterdir())


def seed(main: pathlib.Path, worktree: pathlib.Path, preset: str, *, ninja: str = "ninja") -> Result:
    main, worktree = pathlib.Path(main).resolve(), pathlib.Path(worktree).resolve()
    if main == worktree:
        return Result("not-seeded", "the worktree is the main checkout; there is nothing to seed it from")
    src = main / "build" / preset
    if worktree == src or src in worktree.parents:
        return Result("not-seeded", f"the worktree {worktree} is inside {src}, the tree it would be copied from")
    dst = worktree / "build" / preset
    if dst.exists() or dst.is_symlink():
        if not _is_empty_dir(dst):
            return Result("kept", f"build/{preset} already has content; left as it is")
    for name in ("CMakeCache.txt", "build.ninja", ".ninja_log"):
        if not (src / name).is_file():
            return Result("not-seeded", f"{main}/build/{preset} has no {name}: the main checkout has not built {preset}")
    for key, want in (("CMAKE_HOME_DIRECTORY", main), ("CMAKE_CACHEFILE_DIR", src)):
        have = _cache_value(src / "CMakeCache.txt", key)
        if have is None or pathlib.Path(have).resolve() != want:
            return Result("not-seeded", f"{src}/CMakeCache.txt has {key}={have}, not {want}: that tree was configured for another checkout")
    commits = built_commits(src / "build.ninja")
    if commits:
        head = subprocess.run(["git", "-C", str(worktree), "rev-parse", "HEAD"], capture_output=True, text=True)
        if head.returncode != 0:
            return Result("not-seeded", f"cannot read the HEAD of {worktree}: {head.stderr.strip()}")
        if commits != {head.stdout.strip()}:
            built = ", ".join(sorted(c[:8] for c in commits))
            return Result("not-seeded", f"{src} was built at {built} and this worktree is at {head.stdout.strip()[:8]}: every compile "
                          "command carries the commit, so ninja would rebuild every object either way")
    if commits and built_modified(src / "build.ninja"):
        return Result("not-seeded", f"{src} was configured with uncommitted changes (its version ends in -dirty), and every compile "
                      "command carries the version, so ninja would rebuild every object at a clean checkout")
    busy = _running_in(src)
    if busy:
        return Result("not-seeded", f"a process is running in {src} (a build or a test): {busy[0]}")
    if shutil.which(ninja) is None:
        return Result("not-seeded", f"{ninja} is not installed; it is needed to recompute the build log's command hashes")

    staging = worktree / "build" / f".seed-{preset}.{os.getpid()}"
    (worktree / "build").mkdir(exist_ok=True)
    try:
        before = _fingerprint(src)
        staging.mkdir()
        proc = subprocess.run(["cp", "-a", "--reflink=auto", f"{src}/.", str(staging)], capture_output=True, text=True)
        if proc.returncode != 0:
            raise SeedError(f"copying {src} failed: {proc.stderr.strip()}")
        if _fingerprint(src) != before:
            return Result("not-seeded", f"{src} changed while it was being copied (a build started?); nothing was kept")
        rewrite = path_rewriter(str(main), str(worktree))
        log_text = (staging / ".ninja_log").read_text()
        outputs = {f.split("\t")[3] for f in log_text.splitlines()[1:] if f.count("\t") == 4}
        hashes = command_hash_map(ninja, staging, outputs, rewrite)
        new_log, kept, dropped = rewrite_ninja_log(log_text, rewrite, hashes)
        deps_paths = 0
        rewritten_logs = [(".ninja_log", new_log.encode())]
        if (staging / ".ninja_deps").exists():
            new_deps, deps_paths = rewrite_deps_log((staging / ".ninja_deps").read_bytes(), rewrite)
            rewritten_logs.append((".ninja_deps", new_deps))
        files = rewrite_text_files(staging, rewrite)
        for name, content in rewritten_logs:
            st = (staging / name).stat()
            (staging / name).write_bytes(content)
            os.utime(staging / name, ns=(st.st_atime_ns, st.st_mtime_ns))
        aligned = align_source_mtimes(main, worktree)
        if dst.exists():
            dst.rmdir()
        os.rename(staging, dst)
    except SeedError as e:
        return Result("error", str(e))
    except (OSError, subprocess.CalledProcessError) as e:
        return Result("error", f"{type(e).__name__}: {e}")
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return Result("seeded", counts={
        "text_files_rewritten": files, "log_entries_kept": kept, "log_entries_dropped": dropped,
        "deps_paths_rewritten": deps_paths, "source_mtimes_aligned": aligned,
    })


def default_log() -> pathlib.Path:
    state = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
    return pathlib.Path(state) / "nevr-runtime" / "seed-build-tree.jsonl"


def append_record(log: pathlib.Path, record: dict) -> None:
    try:
        with open(log, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, sort_keys=True) + "\n")
    except OSError as e:
        print(f"error: cannot write the log {log}: {e}", file=sys.stderr)
        raise SystemExit(1)


def main_cli(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Seed a worktree's build/<preset> from the main checkout's.")
    ap.add_argument("--main", required=True, type=pathlib.Path, help="the main checkout")
    ap.add_argument("--worktree", required=True, type=pathlib.Path, help="the linked worktree to seed")
    ap.add_argument("--preset", required=True, help="CMake preset, the build/<preset> directory")
    ap.add_argument("--log", type=pathlib.Path, help="JSON-lines log (default: see the module docstring)")
    args = ap.parse_args(argv)
    log = args.log
    if log is None:
        log = default_log()
        log.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    result = seed(args.main, args.worktree, args.preset)
    record = {
        "ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "tool": "seed_build_tree", "main": str(args.main.resolve()), "worktree": str(args.worktree.resolve()),
        "preset": args.preset, "status": result.status, "reason": result.reason,
        "seconds": round(time.monotonic() - started, 1), **result.counts,
    }
    append_record(log, record)
    c = result.counts
    if result.status == "seeded":
        print(f"seeded build/{args.preset} from {args.main}/build/{args.preset}: {c['text_files_rewritten']} text files "
              f"rewritten, {c['log_entries_kept']} ninja log entries carried over ({c['log_entries_dropped']} dropped), "
              f"{c['deps_paths_rewritten']} deps-log paths rewritten, {c['source_mtimes_aligned']} source mtimes aligned "
              f"in {record['seconds']} s; the first build rebuilds only what differs from the main checkout's last build")
    elif result.status == "kept":
        print(f"kept: {result.reason}")
    elif result.status == "not-seeded":
        print(f"not seeded: {result.reason}; the first build in this worktree compiles from scratch", file=sys.stderr)
    else:
        print(f"error: seeding build/{args.preset} failed: {result.reason}; build/{args.preset} was not created", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main_cli(sys.argv[1:]))
