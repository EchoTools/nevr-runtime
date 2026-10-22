"""tools/fetch_real_signed_dll.py (#745): it hands the test a file only when both digests match."""
import hashlib
import importlib.util
import io
import sys
import zipfile
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[1] / "fetch_real_signed_dll.py"
spec = importlib.util.spec_from_file_location("fetch_real_signed_dll", TOOL)
fetch_tool = importlib.util.module_from_spec(spec)
sys.modules["fetch_real_signed_dll"] = fetch_tool
spec.loader.exec_module(fetch_tool)


def make_zip(member: str, payload: bytes) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as archive:
        archive.writestr(member, payload)
    return buf.getvalue()


@pytest.fixture
def served(tmp_path, monkeypatch):
    payload = b"MZ pretend signed dll"
    data = make_zip("BugSplat64.dll", payload)
    source = tmp_path / "release.zip"
    source.write_bytes(data)
    monkeypatch.setattr(fetch_tool, "ZIP_SHA256", hashlib.sha256(data).hexdigest())
    monkeypatch.setattr(fetch_tool, "DLL_SHA256", hashlib.sha256(payload).hexdigest())
    return source, payload


def test_a_matching_zip_is_extracted_into_the_cache(tmp_path, served):
    source, payload = served
    path = fetch_tool.fetch(tmp_path / "cache", source.as_uri())
    assert path.read_bytes() == payload
    assert not list((tmp_path / "cache").glob("*.part"))


def test_a_cached_file_with_the_right_digest_is_reused_without_a_download(tmp_path, served):
    source, payload = served
    first = fetch_tool.fetch(tmp_path / "cache", source.as_uri())
    source.unlink()
    assert fetch_tool.fetch(tmp_path / "cache", source.as_uri()) == first


def test_a_zip_with_another_digest_is_refused_and_leaves_no_file(tmp_path, served, monkeypatch):
    source, _ = served
    monkeypatch.setattr(fetch_tool, "ZIP_SHA256", "0" * 64)
    with pytest.raises(SystemExit, match="expected " + "0" * 64):
        fetch_tool.fetch(tmp_path / "cache", source.as_uri())
    assert not (tmp_path / "cache").exists()


def test_a_dll_with_another_digest_is_refused_and_leaves_no_file(tmp_path, served, monkeypatch):
    source, _ = served
    monkeypatch.setattr(fetch_tool, "DLL_SHA256", "1" * 64)
    with pytest.raises(SystemExit, match="expected " + "1" * 64):
        fetch_tool.fetch(tmp_path / "cache", source.as_uri())
    assert not (tmp_path / "cache").exists()


def test_a_zip_without_the_member_is_refused(tmp_path, monkeypatch):
    data = make_zip("other.dll", b"x")
    source = tmp_path / "release.zip"
    source.write_bytes(data)
    monkeypatch.setattr(fetch_tool, "ZIP_SHA256", hashlib.sha256(data).hexdigest())
    with pytest.raises(SystemExit, match="is not in the zip"):
        fetch_tool.fetch(tmp_path / "cache", source.as_uri())


def test_a_failed_download_exits_1_with_the_reason(tmp_path, capsys):
    rc = fetch_tool.main([str(tmp_path / "cache"), "--url", (tmp_path / "missing.zip").as_uri()])
    assert rc == 1
    assert "fetch_real_signed_dll:" in capsys.readouterr().err


def test_a_response_over_the_size_limit_is_refused(tmp_path, served, monkeypatch):
    source, _ = served
    monkeypatch.setattr(fetch_tool, "MAX_ZIP_BYTES", 16)
    with pytest.raises(SystemExit, match="larger than 16 bytes"):
        fetch_tool.fetch(tmp_path / "cache", source.as_uri())
    assert not (tmp_path / "cache").exists()
