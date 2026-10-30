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
  4. a tracked source file that is byte-identical to the main checkout's gets the main checkout's mtime; one that
     differs gets the time of the seed. ninja decides by mtime alone, and a worktree's files may carry a time older
     than the objects the main checkout built, so a differing file is made newer than every object in the copy and
     rebuilds.

The commit is not a guard. The commit (NEVR_GIT_COMMIT_FULL), the version and the describe string (with `-dirty` when
the tree was configured with uncommitted changes) are defined on the few files that read them
(cmake/nevr_build_identity.cmake), and CMake reads them at configure time. A copy made from a tree at another commit,
or configured with uncommitted changes, therefore has its configure re-run on the first build: its build.ninja is
given the epoch as its mtime, older than CMakeLists.txt, so ninja regenerates the build before any compile edge. The
identity readers, what links them and the differing sources rebuild; the rest of the copy is kept. A tree at the
worktree's own commit and configured clean is copied as it is.

The copy is scanned before it is moved into place. The checkout path is rewritten only as the tool knows it (the
resolved path); a tree configured through another spelling of it (a link to the checkout) would keep that
spelling in build.ninja and regenerate the main checkout's tree. Every file of the copy that does not have a binary
suffix is searched for the spellings of the main checkout's path (the resolved one, the one given, the one in
CMakeCache.txt), with the worktree's own path set aside (a worktree under the main checkout contains it); a hit
means the copy is not moved into place and nothing is kept.

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


def align_source_mtimes(main: pathlib.Path, worktree: pathlib.Path) -> tuple[int, int]:
    """Give each tracked file that is byte-identical to the main checkout's the main checkout's mtime, and each one that
    differs the time now (newer than every object the copy holds, so it rebuilds). Returns (aligned, touched)."""
    listing = subprocess.run(["git", "-C", str(worktree), "ls-files", "-z"], check=True, capture_output=True,
                             env=_git_env()).stdout
    now = time.time_ns()
    aligned = touched = 0
    for raw in listing.split(b"\0"):
        if not raw:
            continue
        rel = os.fsdecode(raw)
        mine, theirs = worktree / rel, main / rel
        if mine.is_symlink() or theirs.is_symlink() or not mine.is_file() or not theirs.is_file():
            continue
        a, b = mine.stat(), theirs.stat()
        if a.st_size == b.st_size and filecmp.cmp(mine, theirs, shallow=False):
            if a.st_mtime_ns != b.st_mtime_ns:
                os.utime(mine, ns=(a.st_atime_ns, b.st_mtime_ns))
                aligned += 1
        else:
            os.utime(mine, ns=(a.st_atime_ns, now))
            touched += 1
    return aligned, touched


_COMMIT_DEFINE = re.compile(rb'NEVR_GIT_COMMIT_FULL=\\*"([0-9a-f]{40})')


_DESCRIBE_DEFINE = re.compile(rb'NEVR_GIT_DESCRIBE=\\*"([^"\\ ]*)')


def _git_env() -> dict:
    """The environment for git: GIT_DIR and the other variables that override `git -C` are dropped, so a call about the
    worktree is never answered by whatever repository the caller's environment names."""
    return {k: v for k, v in os.environ.items() if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")}


def built_commits(build_ninja: pathlib.Path) -> set[str]:
    """The commits the compile commands of a build tree carry (one commit per configure)."""
    return {m.group(1).decode() for m in _COMMIT_DEFINE.finditer(build_ninja.read_bytes())}


def built_modified(build_ninja: pathlib.Path) -> bool:
    """True when the tree was configured with uncommitted changes (`git describe --dirty` ends in -dirty)."""
    return any(m.group(1).endswith(b"-dirty") for m in _DESCRIBE_DEFINE.finditer(build_ninja.read_bytes()))


def leftover_main_path(copy: pathlib.Path, spellings: set[str], worktree: pathlib.Path) -> str | None:
    """The first file (or link) of `copy` that still names the main checkout by one of `spellings`, as `path: spelling`.

    Files with a binary suffix are not read (objects keep the old path in what they embed, by design). The worktree's
    own path is set aside first: a worktree under the main checkout has the main checkout's path as its prefix."""
    own = re.compile(re.escape(str(worktree).encode()) + rb"(?![\w.+-])")
    patterns = [re.compile(re.escape(s.encode()) + rb"(?![\w.+-])") for s in sorted(spellings)]
    spelled = {p.pattern: s for p, s in zip(patterns, sorted(spellings))}

    def find(data: bytes) -> str | None:
        data = own.sub(b"", data)
        for pattern in patterns:
            if pattern.search(data):
                return spelled[pattern.pattern]
        return None

    for dirpath, dirs, files in os.walk(copy):
        for name in files + [d for d in dirs if pathlib.Path(dirpath, d).is_symlink()]:
            path = pathlib.Path(dirpath, name)
            if path.is_symlink():
                hit = find(os.readlink(path).encode())
            elif pathlib.Path(name).suffix in BINARY_SUFFIXES:
                continue
            else:
                hit = find(path.read_bytes())
            if hit:
                return f"{path.relative_to(copy)}: {hit}"
    return None


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
    given = os.path.abspath(main)
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
    head = subprocess.run(["git", "-C", str(worktree), "rev-parse", "HEAD"], capture_output=True, text=True, env=_git_env())
    if head.returncode != 0:
        return Result("not-seeded", f"cannot read the HEAD of {worktree}: {head.stderr.strip()}")
    # The identity the tree was configured with. A tree at another commit, or configured dirty, carries another one, so
    # its configure is re-run (see the module docstring); a tree that matches is copied as it is.
    identity_stale = built_commits(src / "build.ninja") != {head.stdout.strip()} or built_modified(src / "build.ninja")
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
        if identity_stale:
            # ninja regenerates the build when build.ninja is older than one of its inputs (the CMakeLists.txt files
            # of the worktree, which are newer than the epoch). The main checkout's mtime may not be older than them.
            os.utime(staging / "build.ninja", ns=(0, 0))
        spellings = {str(main), given, _cache_value(src / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY") or str(main)}
        left = leftover_main_path(staging, spellings, worktree)
        if left:
            return Result("not-seeded", f"the copy still names the main checkout after the rewrite ({left}): that tree was configured "
                          "through another spelling of its path, so building it would regenerate the main checkout's tree")
        aligned, touched = align_source_mtimes(main, worktree)
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
        "deps_paths_rewritten": deps_paths, "source_mtimes_aligned": aligned, "source_mtimes_touched": touched,
        "configure_rerun": identity_stale,
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
              f"{c['deps_paths_rewritten']} deps-log paths rewritten, {c['source_mtimes_aligned']} source mtimes aligned, "
              f"{c['source_mtimes_touched']} differing sources made newer"
              + ("; the configure re-runs first (the tree was built at another commit or with uncommitted changes)"
                 if c["configure_rerun"] else "")
              + f", in {record['seconds']} s; the first build rebuilds only what differs from the main checkout's last build")
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
