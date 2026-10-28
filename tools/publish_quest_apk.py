#!/usr/bin/env python3
"""Put a release's Quest APK and its .sha256 on R2, then fetch both back from the public host and compare (RELEASING.md,
"The Quest APK"; #1098). Usage: publish_quest_apk.py vX.Y.Z [--dir build/quest-release/vX.Y.Z]

The files are the ones `just release-quest-apk vX.Y.Z` wrote. The APK goes up through the release signer's
`put-quest-apk`, which checks that the release is published, that the APK was built from the tag's commit and signed with
the pinned certificate, writes the object only if absent and reads it back from R2. The sidecar goes up with
`rclone copyto --ignore-existing`, so a name that is already there is never replaced; the fetch-back is what proves the
file that is there is the local one.

Everything that is a host, a URL or a credential comes from the environment and nothing here holds a value:
R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY (the bucket), RELEASE_PUBLIC_BASE (where the files are
served, e.g. the release host), RELEASE_SIGNER_BIN (the release-signer executable), RELEASE_SIGNER_DIR (the release-signer
checkout, whose config/manifest.yml and config/sources.yml the signer reads from its working directory) and GH_TOKEN (a
read token the signer uses to see the release). A missing or empty one is a refusal that names it. No value is printed or
logged.

Exit 0 only when both files fetched back from RELEASE_PUBLIC_BASE equal the local ones in size and sha256. Every step
appends a JSON line to $XDG_STATE_HOME/nevr-runtime/publish-quest-apk.jsonl (default ~/.local/state/...); a log that
cannot be written is an error.
"""

import argparse
import datetime
import getpass
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

REQUIRED_ENV = (
    "R2_ENDPOINT",
    "R2_BUCKET",
    "R2_ACCESS_KEY_ID",
    "R2_SECRET_ACCESS_KEY",
    "RELEASE_PUBLIC_BASE",
    "RELEASE_SIGNER_BIN",
    "RELEASE_SIGNER_DIR",
    "GH_TOKEN",
)
SIGNER_CONFIG = ("config/manifest.yml", "config/sources.yml")
TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
LOOPBACK = {"127.0.0.1", "localhost"}
KEY_PREFIX = "nevr-runtime"
CHUNK = 1 << 20
SIDECAR_LIMIT = 4096
FETCH_TIMEOUT_SECONDS = 120
USER_AGENT = "nevr-runtime-publish-quest-apk"  # the host answers 403 to urllib's default agent


class Failure(Exception):
    """A step failed; the message is for the operator and holds no secret."""


class Log:
    def __init__(self, path: pathlib.Path, tag: str) -> None:
        self.path = path
        self.base = {"tag": tag, "pid": os.getpid(), "user": getpass.getuser()}
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.file = path.open("a", encoding="utf-8")
        except OSError as error:
            raise Failure(f"cannot write the log {path}: {error.strerror}") from error

    def event(self, event: str, **fields: object) -> None:
        record = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(), "event": event, **self.base, **fields}
        try:
            self.file.write(json.dumps(record, sort_keys=True) + "\n")
            self.file.flush()
        except OSError as error:
            raise Failure(f"cannot write the log {self.path}: {error.strerror}") from error

    def close(self) -> None:
        self.file.close()


def state_log_path(env: dict[str, str]) -> pathlib.Path:
    state = env.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return pathlib.Path(state) / "nevr-runtime" / "publish-quest-apk.jsonl"


def missing_env(env: dict[str, str]) -> list[str]:
    return [name for name in REQUIRED_ENV if not env.get(name)]


def public_base(env: dict[str, str]) -> str:
    base = env["RELEASE_PUBLIC_BASE"].rstrip("/")
    parts = urllib.parse.urlsplit(base)
    if parts.scheme != "https" and not (parts.scheme == "http" and parts.hostname in LOOPBACK):
        raise Failure("RELEASE_PUBLIC_BASE must be an https URL (http only for 127.0.0.1)")
    if not parts.hostname:
        raise Failure("RELEASE_PUBLIC_BASE has no host")
    return base


def signer_dir(env: dict[str, str]) -> str:
    directory = pathlib.Path(env["RELEASE_SIGNER_DIR"])
    for relative in SIGNER_CONFIG:
        if not (directory / relative).is_file():
            raise Failure(f"RELEASE_SIGNER_DIR has no {relative}")
    return str(directory)


def sha256_file(path: pathlib.Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        while chunk := handle.read(CHUNK):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def local_files(directory: pathlib.Path, tag: str) -> tuple[str, pathlib.Path, pathlib.Path, str, int, bytes]:
    name = f"nevr-runtime-{tag}-quest.apk"
    apk, sidecar = directory / name, directory / f"{name}.sha256"
    if not apk.is_file():
        raise Failure(f"{apk} does not exist; run `just release-quest-apk {tag}` first")
    if not sidecar.is_file():
        raise Failure(f"{sidecar} does not exist; run `just release-quest-apk {tag}` first")
    digest, size = sha256_file(apk)
    if size < 1:
        raise Failure(f"{name} is empty")
    sidecar_bytes = sidecar.read_bytes()
    if sidecar_bytes.split() != [digest.encode(), name.encode()]:
        raise Failure(f"the sidecar {sidecar.name} does not match {name} (sha256 {digest})")
    return name, apk, sidecar, digest, size, sidecar_bytes


def run_step(argv: list[str], env: dict[str, str], what: str, cwd: str | None = None) -> subprocess.CompletedProcess:
    try:
        result = subprocess.run(argv, env=env, cwd=cwd, capture_output=True, text=True, check=False)
    except OSError as error:
        raise Failure(f"{what}: cannot run {pathlib.Path(argv[0]).name}: {error.strerror}") from error
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise Failure(f"{what} failed with exit {result.returncode}")
    return result


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):  # noqa: ANN002, ANN003, ANN201 - the handler's own signature
        return None


def fetch(url: str, limit: int, keep: bool = False) -> tuple[bytes, str, int]:
    """Fetch `url` without following a redirect, reading at most `limit` + 1 bytes. Returns (the body when `keep`, else
    empty; its sha256; its size). Raises Failure for anything but a 200 of a file that is not HTML."""
    opener = urllib.request.build_opener(NoRedirect)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        response = opener.open(request, timeout=FETCH_TIMEOUT_SECONDS)
    except urllib.error.HTTPError as error:
        if 300 <= error.code < 400:
            raise Failure(f"{url} answered a redirect ({error.code}); a redirect is not followed") from error
        raise Failure(f"{url} answered {error.code}") from error
    except OSError as error:
        raise Failure(f"cannot fetch {url}: {error}") from error
    with response:
        content_type = response.headers.get("Content-Type", "")
        if content_type.lower().startswith("text/html"):
            raise Failure(f"{url} answered 200 with {content_type}, which is not the file")
        digest = hashlib.sha256()
        size = 0
        kept = bytearray()
        while chunk := response.read(CHUNK):
            size += len(chunk)
            if size > limit:
                raise Failure(f"{url} sent more than the expected {limit} bytes")
            digest.update(chunk)
            if keep:
                kept += chunk
    return bytes(kept), digest.hexdigest(), size


def check_apk(url: str, name: str, digest: str, size: int) -> None:
    _, got_digest, got_size = fetch(url, size)
    if got_size != size:
        raise Failure(f"{name} from the public host is {got_size} bytes, the local file is {size}")
    if got_digest != digest:
        raise Failure(f"{name} from the public host has sha256 {got_digest}, the local file has {digest}")


def check_sidecar(url: str, name: str, sidecar_bytes: bytes) -> None:
    body, _, _ = fetch(url, SIDECAR_LIMIT, keep=True)
    if body != sidecar_bytes:
        raise Failure(f"{name}.sha256 from the public host differs from the local sidecar")


def publish(args: argparse.Namespace, env: dict[str, str], log: Log) -> None:
    refused = missing_env(env)
    if refused:
        log.event("refused", missing=refused)
        raise Failure(f"refused: set {', '.join(refused)}")
    if not TAG.match(args.tag):
        raise Failure(f"{args.tag} is not a release tag (vX.Y.Z)")
    base = public_base(env)
    signer_cwd = signer_dir(env)
    directory = pathlib.Path(args.dir) if args.dir else pathlib.Path("build") / "quest-release" / args.tag
    name, apk, sidecar, digest, size, sidecar_bytes = local_files(directory.resolve(), args.tag)
    log.event("local_files", apk=name, sha256=digest, size=size)

    with tempfile.TemporaryDirectory() as scratch:
        extra = os.path.join(scratch, "extra.json")
        result = run_step([env["RELEASE_SIGNER_BIN"], "put-quest-apk", "--tag", args.tag, "--apk", str(apk), "--out", extra],
                          env, "the signer's put-quest-apk", cwd=signer_cwd)
    log.event("put_apk", object=f"{KEY_PREFIX}/{args.tag}/{name}", signer_line=result.stdout.strip())

    rclone_env = {
        **env,
        "RCLONE_CONFIG_R2_TYPE": "s3",
        "RCLONE_CONFIG_R2_PROVIDER": "Cloudflare",
        "RCLONE_CONFIG_R2_ENDPOINT": env["R2_ENDPOINT"],
        "RCLONE_CONFIG_R2_ACCESS_KEY_ID": env["R2_ACCESS_KEY_ID"],
        "RCLONE_CONFIG_R2_SECRET_ACCESS_KEY": env["R2_SECRET_ACCESS_KEY"],
    }
    key = f"{KEY_PREFIX}/{args.tag}/{name}.sha256"
    run_step(["rclone", "copyto", "--ignore-existing", "--s3-no-check-bucket", str(sidecar), f"R2:{env['R2_BUCKET']}/{key}"],
             rclone_env, "rclone copyto of the sidecar")
    log.event("put_sidecar", object=key)

    apk_url = f"{base}/{KEY_PREFIX}/{args.tag}/{name}"
    for url, check in ((apk_url, lambda: check_apk(apk_url, name, digest, size)),
                       (f"{apk_url}.sha256", lambda: check_sidecar(f"{apk_url}.sha256", name, sidecar_bytes))):
        try:
            check()
        except Failure as failure:
            log.event("fetch", url=url, ok=False, error=str(failure))
            raise
        log.event("fetch", url=url, ok=True)
    log.event("published", apk=name, sha256=digest, size=size, url=apk_url)
    sys.stdout.write(f"{name} {digest} {size} {apk_url}\n")


def main(argv: list[str], env: dict[str, str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tag", help="the release tag, vX.Y.Z")
    parser.add_argument("--dir", help="where release-quest-apk wrote the files (default build/quest-release/<tag>)")
    args = parser.parse_args(argv)
    log = None
    try:
        log = Log(state_log_path(env), args.tag)
        log.event("start", dir=args.dir or "")
        publish(args, env, log)
    except Failure as failure:
        sys.stderr.write(f"publish-quest-apk: {failure}\n")
        if log is not None:
            try:
                log.event("failed", error=str(failure))
            except Failure as log_failure:
                sys.stderr.write(f"publish-quest-apk: {log_failure}\n")
        return 1
    finally:
        if log is not None:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
