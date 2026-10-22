#!/usr/bin/env python3
"""Fetch the real signed v5.4.3 BugSplat64.dll that test_plugin_trust verifies (#745).

    tools/fetch_real_signed_dll.py <cache-dir> [--url URL]

Downloads the signed Windows zip the release-signer pipeline produced, checks the zip's SHA-256, extracts
BugSplat64.dll, checks its SHA-256, and prints the file's path (the value of NEVR_REAL_SIGNED_DLL). A file already in
<cache-dir> with the right digest is reused and nothing is downloaded. Any digest that does not match, a missing
member or a failed download exits 1 with the reason on stderr and leaves no file behind: a test over a file nobody
checked is not a test of the real signature.

The zip is data: it is read with zipfile and never run.
"""
import argparse
import hashlib
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

ZIP_URL = "https://echo.taxi/nevr-runtime/windows/nevr-runtime-v5.4.3-windows-signed.zip"
ZIP_SHA256 = "68ea39a914f17b33276131fed075cea410f6b3a379ad3d98bf5063b453eed287"
DLL_SHA256 = "a97767e62721503d4e249b016b734c42619a5aaa209ecdcd7a25a434f6d2a629"
MEMBER = "BugSplat64.dll"
MAX_ZIP_BYTES = 64 * 1024 * 1024  # the real zip is a few MB; a response past this is not it


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch(cache: Path, url: str) -> Path:
    target = cache / f"BugSplat64-v5.4.3-signed-{DLL_SHA256[:12]}.dll"
    if target.is_file() and sha256(target.read_bytes()) == DLL_SHA256:
        return target
    # echo.taxi answers 403 to urllib's default User-Agent.
    request = urllib.request.Request(url, headers={"User-Agent": "nevr-runtime-fetch-real-signed-dll"})
    with urllib.request.urlopen(request, timeout=120) as response:
        zip_bytes = response.read(MAX_ZIP_BYTES + 1)
    if len(zip_bytes) > MAX_ZIP_BYTES:
        raise SystemExit(f"fetch_real_signed_dll: the response from {url} is larger than {MAX_ZIP_BYTES} bytes")
    if sha256(zip_bytes) != ZIP_SHA256:
        raise SystemExit(f"fetch_real_signed_dll: the zip from {url} has SHA-256 {sha256(zip_bytes)}, expected {ZIP_SHA256}")
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as archive:
        if MEMBER not in archive.namelist():
            raise SystemExit(f"fetch_real_signed_dll: {MEMBER} is not in the zip: {archive.namelist()}")
        dll = archive.read(MEMBER)
    if sha256(dll) != DLL_SHA256:
        raise SystemExit(f"fetch_real_signed_dll: {MEMBER} has SHA-256 {sha256(dll)}, expected {DLL_SHA256}")
    cache.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(".part")
    partial.write_bytes(dll)
    partial.replace(target)
    return target


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("cache_dir", type=Path)
    parser.add_argument("--url", default=ZIP_URL)
    args = parser.parse_args(argv)
    try:
        print(fetch(args.cache_dir, args.url))
    except OSError as error:
        print(f"fetch_real_signed_dll: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
