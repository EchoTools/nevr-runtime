#!/usr/bin/env python3
"""gen/ holds the C++ that `buf generate` writes from the BSR commit the justfile pins (proto_pin).

gen/ is gitignored, so a worktree gets it by copying, and a copy can be stale. gen/.proto-stamp (JSON) records
what produced it: the pin, the sha256 of buf.gen.yaml, and the sha256 of every file the generator wrote. `check`
accepts gen/ only when all three still match; `ensure` regenerates only when `check` refuses.

  proto_stamp.py --root DIR check                       0 current; 1 stale (the reason on stderr)
  proto_stamp.py --root DIR ensure --protoc-dir DIR     regenerate only when check says stale
  proto_stamp.py --root DIR generate --protoc-dir DIR   regenerate now

Exit codes: 0 done or current; 1 gen/ is stale or generation failed; 2 cannot decide (no pin, no protoc, no buf).

generate runs buf with DIR first on PATH, so the protoc that builds the tree (vcpkg's, installed by `just configure`)
writes the code. A missing protoc is an error, never a fall back to whatever protoc is on PATH. A generate that fails
leaves gen/ and its stamp as they were. It removes only files the old stamp listed, so a file the user keeps in gen/
is never touched. Each ensure or generate is one JSON line in $XDG_STATE_HOME/nevr-runtime/proto-stamp.jsonl and one
line on stderr; a record that cannot be written is reported on stderr.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

STAMP = ".proto-stamp"
PIN_RE = re.compile(r'^proto_pin\s*:=\s*"([0-9a-f]+)"', re.M)


class StampError(Exception):
    """A question the stamp cannot answer: a missing pin, protoc or buf. Exit 2; it is never a stale verdict."""


class GenerationFailed(StampError):
    """buf ran and gen/ was left as it was, or the files it wrote do not pass the check. Exit 1."""


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_pin(root):
    try:
        text = (root / "justfile").read_text(encoding="utf-8")
    except OSError as e:
        raise StampError(f"cannot read {root / 'justfile'}: {e}")
    match = PIN_RE.search(text)
    if not match:
        raise StampError('justfile has no `proto_pin := "<BSR commit>"` line')
    return match.group(1)


def config_sha(root):
    try:
        return sha256(root / "buf.gen.yaml")
    except OSError as e:
        raise StampError(f"cannot read buf.gen.yaml: {e}")


def read_stamp(gen):
    """(stamp, None) when gen/.proto-stamp reads; (None, reason) when it is absent or unreadable."""
    path = gen / STAMP
    if not path.exists():
        return None, "no stamp: gen/ was not written by the generator"
    try:
        stamp = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return None, f"stamp unreadable: {e}"
    if not isinstance(stamp, dict) or not isinstance(stamp.get("files"), dict):
        return None, "stamp has no file list"
    return stamp, None


def write_stamp(gen, pin, config, digests):
    doc = {"pin": pin, "buf_gen_yaml_sha256": config, "files": dict(sorted(digests.items()))}
    tmp = gen / (STAMP + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, gen / STAMP)


def record_existing(root, rels):
    """Stamp files that are already in gen/ as the output for the pin and buf.gen.yaml the justfile names.

    Used by tests to build a gen/ that is current; the CLI never adopts a gen/ it did not generate.
    """
    write_stamp(root / "gen", read_pin(root), config_sha(root), {rel: sha256(root / "gen" / rel) for rel in rels})


def check(root):
    """(True, why) when gen/ is the output for the pin and buf.gen.yaml; (False, why) when it is not."""
    gen = root / "gen"
    pin = read_pin(root)
    stamp, why = read_stamp(gen)
    if stamp is None:
        return False, why
    if stamp.get("pin") != pin:
        return False, f"pin changed: gen/ was made at {stamp.get('pin')}, the justfile pins {pin}"
    if stamp.get("buf_gen_yaml_sha256") != config_sha(root):
        return False, "buf.gen.yaml changed since gen/ was made"
    for rel, digest in stamp["files"].items():
        path = gen / rel
        if not path.is_file():
            return False, f"gen/{rel} is missing"
        if sha256(path) != digest:
            return False, f"gen/{rel} differs from what the generator wrote"
    return True, f"gen/ is the output for pin {pin}: {len(stamp['files'])} files"


def generate(root, protoc_dir):
    """Run buf at the pin into a temporary directory, then move its files into gen/ and stamp them."""
    pin = read_pin(root)
    if not os.access(protoc_dir / "protoc", os.X_OK):
        raise StampError(f"no protoc at {protoc_dir / 'protoc'}: the generator needs the vcpkg protoc "
                         "(`just configure` installs it)")
    if shutil.which("buf") is None:
        raise StampError("buf is not on PATH")
    gen = root / "gen"
    old, _ = read_stamp(gen)
    old_files = list(old["files"]) if old else []
    config = config_sha(root)
    with tempfile.TemporaryDirectory(prefix=".proto-gen-", dir=root) as tmp:
        env = dict(os.environ, PATH=f"{protoc_dir}{os.pathsep}{os.environ.get('PATH', '')}")
        proc = subprocess.run(["buf", "generate", f"buf.build/echotools/nevr-api:{pin}", "-o", tmp], cwd=root,
                              env=env, capture_output=True, text=True)
        if proc.returncode != 0:
            raise GenerationFailed(f"buf generate failed (exit {proc.returncode}); gen/ is unchanged\n{proc.stdout}{proc.stderr}")
        out = pathlib.Path(tmp) / "gen"
        written = sorted(p for p in out.rglob("*") if p.is_file())
        if not written:
            raise GenerationFailed("buf generate wrote no files; gen/ is unchanged")
        gen.mkdir(exist_ok=True)
        digests = {}
        for src in written:
            rel = src.relative_to(out).as_posix()
            dst = gen / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            digests[rel] = sha256(dst)
        for rel in old_files:
            if rel not in digests:
                (gen / rel).unlink(missing_ok=True)
        write_stamp(gen, pin, config, digests)
    return pin, len(digests)


def record(verb, root, outcome, **fields):
    entry = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"), "verb": verb,
             "root": str(root), "outcome": outcome, **fields}
    state = pathlib.Path(os.environ.get("XDG_STATE_HOME") or pathlib.Path.home() / ".local/state") / "nevr-runtime"
    try:
        state.mkdir(parents=True, exist_ok=True)
        with open(state / "proto-stamp.jsonl", "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")
    except OSError as e:
        print(f"proto_stamp: ERROR cannot write {state / 'proto-stamp.jsonl'}: {e}", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    sub = parser.add_subparsers(dest="verb", required=True)
    sub.add_parser("check")
    for verb in ("ensure", "generate"):
        sub.add_parser(verb).add_argument("--protoc-dir", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if args.verb == "check":
            ok, why = check(root)
            print(("current: " if ok else "stale: ") + why, file=sys.stderr)
            return 0 if ok else 1
        if args.verb == "ensure":
            ok, why = check(root)
            if ok:
                print(f"proto_stamp: ensure: current: {why}", file=sys.stderr)
                record("ensure", root, "current", reason=why)
                return 0
            print(f"proto_stamp: ensure: stale: {why}; regenerating", file=sys.stderr)
            pin, count = generate(root, args.protoc_dir.resolve())
            ok, why = check(root)
            if not ok:
                raise GenerationFailed(f"generate ran but gen/ still fails the check: {why}")
            print(f"proto_stamp: ensure: regenerated at {pin}: {count} files", file=sys.stderr)
            record("ensure", root, "regenerated", pin=pin, files=count, reason=why)
            return 0
        pin, count = generate(root, args.protoc_dir.resolve())
        print(f"proto_stamp: generate: {count} files at {pin}", file=sys.stderr)
        record("generate", root, "generated", pin=pin, files=count)
        return 0
    except StampError as e:
        print(f"proto_stamp: ERROR: {e}", file=sys.stderr)
        if args.verb != "check":
            record(args.verb, root, "failed", reason=str(e))
        return 1 if isinstance(e, GenerationFailed) else 2


if __name__ == "__main__":
    sys.exit(main())
