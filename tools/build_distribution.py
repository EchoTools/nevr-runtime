#!/usr/bin/env python3
"""Sign a staged runtime package and atomically publish verified archives."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


PE_SUFFIXES = {".dll", ".exe"}
REQUIRED_FILES = {"BugSplat64.dll", "echovr_server.exe"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def fingerprint(value: str, label: str) -> str:
    normalized = value.replace(" ", "").replace(":", "").upper()
    if len(normalized) != 64 or any(ch not in "0123456789ABCDEF" for ch in normalized):
        raise RuntimeError(f"{label} must be a 64-digit SHA-256 fingerprint")
    return normalized


def command(args: list[str]) -> None:
    result = subprocess.run(args, check=False, capture_output=True, text=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed ({result.returncode}): {args[0]}: {detail}")


def pe_manifest(package: Path) -> dict[Path, str]:
    if not package.is_dir():
        raise RuntimeError(f"staging package is missing: {package}")
    files = sorted(p for p in package.rglob("*") if p.is_file() and p.suffix.lower() in PE_SUFFIXES)
    names = {p.name for p in files}
    missing = REQUIRED_FILES - names
    if missing:
        raise RuntimeError(f"package lacks required runtime artifacts: {', '.join(sorted(missing))}")
    if not files:
        raise RuntimeError("package contains no PE artifacts")
    for path in files:
        with path.open("rb") as stream:
            image = stream.read(1024 * 1024 + 24)
            if len(image) < 64 or image[:2] != b"MZ":
                raise RuntimeError(f"invalid or empty PE artifact: {path}")
            pe_offset = int.from_bytes(image[0x3C:0x40], "little")
            if pe_offset < 64 or pe_offset > 1024 * 1024 or pe_offset + 24 > len(image):
                raise RuntimeError(f"truncated or invalid PE header offset: {path}")
            if image[pe_offset:pe_offset + 4] != b"PE\0\0":
                raise RuntimeError(f"missing PE signature: {path}")
            machine = int.from_bytes(image[pe_offset + 4:pe_offset + 6], "little")
            optional_size = int.from_bytes(image[pe_offset + 20:pe_offset + 22], "little")
            optional_start = pe_offset + 24
            optional_end = optional_start + optional_size
            if machine != 0x8664 or optional_size < 112 or optional_size > 4096 or optional_end > len(image):
                raise RuntimeError(f"truncated or unsupported PE headers: {path}")
            if int.from_bytes(image[optional_start:optional_start + 2], "little") != 0x20B:
                raise RuntimeError(f"PE optional header is not PE32+: {path}")
    return {path: sha256(path) for path in files}


def signing_configuration(required: bool) -> tuple[str, list[str]] | None:
    if os.environ.get("CODESIGN_SKIP") == "1":
        if required:
            raise RuntimeError("CODESIGN_SKIP is forbidden in required signing mode")
        return None

    if os.name == "nt":
        tool = os.environ.get("SIGNTOOL", "signtool")
        pfx = os.environ.get("CODESIGN_PFX")
        signer = os.environ.get("CODESIGN_SIGNER_SHA256", "")
        root = os.environ.get("CODESIGN_ROOT_SHA256", "")
        if not any((pfx, os.environ.get("CODESIGN_THUMBPRINT"))):
            if required:
                raise RuntimeError("required signing credentials are missing")
            return None
        if not signer or not root:
            raise RuntimeError("Windows signing requires exact signer and root SHA-256 fingerprints")
        fingerprint(signer, "CODESIGN_SIGNER_SHA256")
        fingerprint(root, "CODESIGN_ROOT_SHA256")
        if pfx and not Path(pfx).is_file():
            raise RuntimeError(f"signing input does not exist: {pfx}")
        if shutil.which(tool) is None:
            raise RuntimeError(f"signtool not found: {tool}")
        return "windows", [tool]

    tool = os.environ.get("OSSLSIGNCODE", "osslsigncode")
    cert = os.environ.get("CODESIGN_CERT")
    key = os.environ.get("CODESIGN_KEY")
    pfx = os.environ.get("CODESIGN_PFX")
    ca_file = os.environ.get("CODESIGN_CA_FILE")
    signer = os.environ.get("CODESIGN_SIGNER_SHA256", "")
    has_credentials = bool(pfx or (cert and key))
    if not has_credentials:
        if required:
            raise RuntimeError("required signing credentials are missing")
        if cert or key or ca_file or signer:
            raise RuntimeError("incomplete optional signing configuration")
        return None
    if not ca_file or not signer:
        raise RuntimeError("signing requires CODESIGN_CA_FILE and exact CODESIGN_SIGNER_SHA256")
    signer = fingerprint(signer, "CODESIGN_SIGNER_SHA256")
    root = fingerprint(os.environ.get("CODESIGN_ROOT_SHA256", ""), "CODESIGN_ROOT_SHA256")
    if shutil.which(tool) is None:
        raise RuntimeError(f"osslsigncode not found: {tool}")
    for required_path in (pfx, cert, key, ca_file):
        if required_path and not Path(required_path).is_file():
            raise RuntimeError(f"signing input does not exist: {required_path}")
    root_result = subprocess.run(["openssl", "x509", "-in", ca_file, "-noout", "-fingerprint", "-sha256"],
                                 text=True, capture_output=True, check=False)
    if root_result.returncode:
        raise RuntimeError(f"cannot read trusted root certificate: {root_result.stderr.strip()}")
    root_actual = root_result.stdout.strip().rsplit("=", 1)[-1].replace(":", "").upper()
    if root_actual != root:
        raise RuntimeError("CA file fingerprint does not match pinned CODESIGN_ROOT_SHA256")
    return "linux", [tool]


def signer_args(kind: str, tool: list[str], source: Path, destination: Path) -> list[str]:
    if kind == "linux":
        args = [*tool, "sign", "-h", "sha256", "-in", str(source), "-out", str(destination)]
        if os.environ.get("CODESIGN_PFX"):
            args.extend(["-pkcs12", os.environ["CODESIGN_PFX"]])
            if os.environ.get("CODESIGN_PASS"):
                args.extend(["-pass", os.environ["CODESIGN_PASS"]])
        else:
            args.extend(["-certs", os.environ["CODESIGN_CERT"], "-key", os.environ["CODESIGN_KEY"]])
        chain = os.environ.get("CODESIGN_CHAIN")
        if chain:
            args.extend(["-ac", chain])
        timestamp = os.environ.get("CODESIGN_TSURL")
        if timestamp:
            args.extend(["-ts", timestamp])
        return args
    args = [*tool, "sign", "/fd", "SHA256"]
    if os.environ.get("CODESIGN_PFX"):
        args.extend(["/f", os.environ["CODESIGN_PFX"]])
        if os.environ.get("CODESIGN_PASS"):
            args.extend(["/p", os.environ["CODESIGN_PASS"]])
    else:
        args.extend(["/s", "My", "/sha1", os.environ["CODESIGN_THUMBPRINT"]])
    timestamp = os.environ.get("CODESIGN_TSURL")
    if timestamp:
        args.extend(["/tr", timestamp, "/td", "SHA256"])
    return [*args, str(destination)]


def verify_one(kind: str, tool: list[str], path: Path) -> None:
    if kind == "linux":
        signer = fingerprint(os.environ["CODESIGN_SIGNER_SHA256"], "CODESIGN_SIGNER_SHA256").lower()
        command([*tool, "verify", "-CAfile", os.environ["CODESIGN_CA_FILE"],
                 "-require-leaf-hash", f"sha256:{signer}", "-in", str(path)])
        return
    # The PowerShell verifier requires Windows' existing trusted chain and exact
    # signer/root fingerprints. It never imports certificates or changes trust.
    verifier = Path(__file__).with_name("codesign") / "verify-authenticode.ps1"
    command(["powershell", "-NoProfile", "-File", str(verifier), str(path),
             fingerprint(os.environ["CODESIGN_SIGNER_SHA256"], "CODESIGN_SIGNER_SHA256"),
             fingerprint(os.environ["CODESIGN_ROOT_SHA256"], "CODESIGN_ROOT_SHA256")])


def sign_package(package: Path, required: bool) -> tuple[str, list[str]] | None:
    manifest = pe_manifest(package)
    config = signing_configuration(required)
    if config is None:
        return None
    kind, tool = config
    # Keep candidates outside package.rglob() so enumeration is immutable.
    # Keep rename candidates on the staging filesystem, but outside the staged
    # package tree so recursive PE enumeration stays immutable.
    temp_root = package.parent
    temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nevr-sign-", dir=temp_root) as temp_name:
        temp = Path(temp_name)
        candidates: dict[Path, Path] = {}
        for index, source in enumerate(manifest):
            output = temp / f"signed-{index}{source.suffix}"
            if kind == "windows":
                shutil.copy2(source, output)
            command(signer_args(kind, tool, source, output))
            if not output.is_file() or output.stat().st_size == 0:
                raise RuntimeError(f"signer produced no output for {source.name}")
            verify_one(kind, tool, output)
            candidates[source] = output
        # Detect concurrent stage mutation before replacing any source.
        if pe_manifest(package) != manifest:
            raise RuntimeError("staged package changed while signing")
        backups: dict[Path, Path] = {}
        try:
            for index, (source, candidate) in enumerate(candidates.items()):
                backup = temp / f"original-{index}{source.suffix}"
                os.replace(source, backup)
                backups[source] = backup
                os.replace(candidate, source)
        except OSError:
            for source, backup in backups.items():
                if backup.exists():
                    source.unlink(missing_ok=True)
                    os.replace(backup, source)
            raise
    return config


def verify_archives(tar_path: Path, zip_path: Path, package: Path,
                    signing: tuple[str, list[str]] | None) -> None:
    if not tar_path.is_file() or tar_path.stat().st_size == 0:
        raise RuntimeError("tar.zst candidate is missing or empty")
    if not zip_path.is_file() or zip_path.stat().st_size == 0:
        raise RuntimeError("zip candidate is missing or empty")
    result = subprocess.run(["tar", "--zstd", "-tf", str(tar_path)], text=True,
                            capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(f"tar candidate is invalid: {result.stderr}")
    for name in REQUIRED_FILES:
        if not any(line.rstrip("/").endswith("/" + name) for line in result.stdout.splitlines()):
            raise RuntimeError(f"tar candidate lacks {name}")
    with zipfile.ZipFile(zip_path) as archive:
        names = archive.namelist()
        bad_member = archive.testzip()
        if bad_member:
            raise RuntimeError(f"zip candidate has corrupt member: {bad_member}")
    for name in REQUIRED_FILES:
        if not any(item.rstrip("/").endswith("/" + name) for item in names):
            raise RuntimeError(f"zip candidate lacks {name}")

    manifest = pe_manifest(package)
    with tempfile.TemporaryDirectory(prefix="nevr-archive-verify-", dir=tar_path.parent) as temp_name:
        temp = Path(temp_name)
        with zipfile.ZipFile(zip_path) as zip_archive:
            for index, (source, expected_hash) in enumerate(manifest.items()):
                relative = source.relative_to(package)
                member = str(Path(package.name) / relative).replace("\\", "/")
                zip_payload = zip_archive.read(member)
                if hashlib.sha256(zip_payload).hexdigest().upper() != expected_hash:
                    raise RuntimeError(f"zip payload differs from signed package: {relative}")
                tar_result = subprocess.run(["tar", "--zstd", "-xOf", str(tar_path), member],
                                            capture_output=True, check=False)
                if tar_result.returncode:
                    raise RuntimeError(f"cannot read tar payload {relative}: {tar_result.stderr.decode(errors='replace')}")
                if hashlib.sha256(tar_result.stdout).hexdigest().upper() != expected_hash:
                    raise RuntimeError(f"tar payload differs from signed package: {relative}")
                if signing is not None:
                    kind, tool = signing
                    zip_copy = temp / f"zip-{index}{source.suffix}"
                    tar_copy = temp / f"tar-{index}{source.suffix}"
                    zip_copy.write_bytes(zip_payload)
                    tar_copy.write_bytes(tar_result.stdout)
                    verify_one(kind, tool, zip_copy)
                    verify_one(kind, tool, tar_copy)


def create_zip(package: Path, archive_path: Path, archive_root: str) -> None:
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(package.rglob("*")):
            if path.is_file():
                archive.write(path, Path(archive_root) / path.relative_to(package))


def publish(package: Path, destination: Path, tar_path: Path, zip_path: Path,
            signing: tuple[str, list[str]] | None) -> None:
    root = destination.parent
    name = destination.name
    if tar_path.parent != root or zip_path.parent != root:
        raise RuntimeError("package and archive outputs must share one publication directory")
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nevr-dist-", dir=root) as temp_name:
        temp = Path(temp_name)
        package_candidate = temp / name
        shutil.copytree(package, package_candidate)
        tar_candidate = temp / tar_path.name
        zip_candidate = temp / zip_path.name
        command(["tar", "--zstd", "-cf", str(tar_candidate), "-C", str(temp), name])
        create_zip(package_candidate, zip_candidate, name)
        verify_archives(tar_candidate, zip_candidate, package_candidate, signing)

        # Preserve old published state until all outputs validate. Roll back on
        # any rename failure; callers only upload after this target succeeds.
        destinations = [
            (destination, temp / f"old-package-{name}"),
            (tar_path, temp / f"old-{tar_path.name}"),
            (zip_path, temp / f"old-{zip_path.name}"),
        ]
        candidates = [package_candidate, tar_candidate, zip_candidate]
        moved_old: list[tuple[Path, Path]] = []
        installed: list[Path] = []
        try:
            for destination, backup in destinations:
                if destination.exists():
                    os.replace(destination, backup)
                    moved_old.append((destination, backup))
            for (destination, _), candidate in zip(destinations, candidates):
                os.replace(candidate, destination)
                installed.append(destination)
        except OSError:
            for destination in installed:
                if destination.is_dir():
                    shutil.rmtree(destination)
                else:
                    destination.unlink(missing_ok=True)
            for destination, backup in reversed(moved_old):
                if backup.exists():
                    os.replace(backup, destination)
            raise
        for _, backup in moved_old:
            if backup.is_dir():
                shutil.rmtree(backup)
            else:
                backup.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--tar", type=Path, required=True)
    parser.add_argument("--zip", type=Path, required=True)
    parser.add_argument("--required-signing", action="store_true")
    args = parser.parse_args()
    try:
        signing = sign_package(args.package, args.required_signing)
        publish(args.package, args.destination, args.tar, args.zip, signing)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print(f"distribution failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
