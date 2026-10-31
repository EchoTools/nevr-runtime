"""A binary AndroidManifest.xml (Android's compiled XML, "AXML") built by hand, for the tests of
tools/quest_apk_version.py, tools/quest_apk_label.py and tools/quest_apk_rename_package.py.

The layout is the one aapt2 writes (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h):
an XML document chunk holding a string pool, a resource map (one resource id per leading string: the id is
what Android looks an attribute up by, the name is only a label), then the namespace and element chunks.
Every test that builds a manifest here is also run against the real store APK's manifest when the file is
present (test_quest_apk_version.py), so a builder that mirrors a misreading of the format is caught.
"""

from __future__ import annotations

import struct

ANDROID_NS = "http://schemas.android.com/apk/res/android"
RES_VERSION_CODE = 0x0101021B
RES_VERSION_NAME = 0x0101021C
RES_VERSION_CODE_MAJOR = 0x01010576
RES_MIN_SDK = 0x0101020C
RES_LABEL = 0x01010001
TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11
NO_INDEX = 0xFFFFFFFF


def _chunk(kind: int, header_size: int, body: bytes, header_extra: bytes = b"") -> bytes:
    head = header_extra
    assert header_size == 8 + len(head)
    size = header_size + len(body)
    assert size % 4 == 0
    return struct.pack("<HHI", kind, header_size, size) + head + body


def _u8_len(n: int) -> bytes:
    return bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF])


def _u16_len(n: int) -> bytes:
    return struct.pack("<H", n) if n < 0x8000 else struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)


def string_pool(strings: list, utf8: bool) -> bytes:
    blobs = []
    for text in strings:
        if utf8:
            raw = text.encode("utf-8")
            blobs.append(_u8_len(len(text)) + _u8_len(len(raw)) + raw + b"\0")
        else:
            blobs.append(_u16_len(len(text)) + text.encode("utf-16-le") + b"\0\0")
    offsets, at = [], 0
    for blob in blobs:
        offsets.append(at)
        at += len(blob)
    data = b"".join(blobs)
    data += b"\0" * (-len(data) % 4)
    strings_start = 28 + 4 * len(strings)
    head = struct.pack("<IIIII", len(strings), 0, 0x100 if utf8 else 0, strings_start, 0)
    return _chunk(0x0001, 28, struct.pack(f"<{len(offsets)}I", *offsets) + data, head)


def resource_map(ids: list) -> bytes:
    return _chunk(0x0180, 8, struct.pack(f"<{len(ids)}I", *ids))


def _attr(ns: int, name: int, raw: int, data_type: int, data: int) -> bytes:
    return struct.pack("<IIIHBBI", ns, name, raw, 8, 0, data_type, data)


def _start_element(ns: int, name: int, attrs: list) -> bytes:
    body = struct.pack("<IIHHHHHH", ns, name, 0x14, 0x14, len(attrs), 0, 0, 0) + b"".join(attrs)
    return _chunk(0x0102, 16, body, struct.pack("<II", 1, NO_INDEX))


def _end_element(ns: int, name: int) -> bytes:
    return _chunk(0x0103, 16, struct.pack("<II", ns, name), struct.pack("<II", 1, NO_INDEX))


def _start_ns(prefix: int, uri: int) -> bytes:
    return _chunk(0x0100, 16, struct.pack("<II", prefix, uri), struct.pack("<II", 1, NO_INDEX))


def _end_ns(prefix: int, uri: int) -> bytes:
    return _chunk(0x0101, 16, struct.pack("<II", prefix, uri), struct.pack("<II", 1, NO_INDEX))


def build_manifest(version_code: int = 4987566, *, utf8: bool = True, version_name: str = "4987566",
                   version_code_type: int = TYPE_INT_DEC, version_code_raw: int = NO_INDEX,
                   version_code_count: int = 1, version_code_id: int = RES_VERSION_CODE,
                   version_code_ns_android: bool = True, with_version_code_major: bool = False,
                   with_resource_map: bool = True, root: str = "manifest", nested_version_code: bool = False,
                   trailing: bytes = b"") -> bytes:
    """A manifest shaped like the store APK's: <manifest versionCode versionName package><uses-sdk minSdkVersion/>.

    The keyword arguments plant one defect each (a second versionCode, a string-typed one, a wrong resource id,
    a versionCodeMajor, ...) for the validator's refusal tests."""
    names = ["versionCode", "versionName", "minSdkVersion", "versionCodeMajor"]
    ids = [version_code_id, RES_VERSION_NAME, RES_MIN_SDK, RES_VERSION_CODE_MAJOR]
    strings = names + ["android", ANDROID_NS, root, "package", "com.readyatdawn.r15", "uses-sdk", version_name]
    idx = {s: i for i, s in enumerate(strings)}
    ns_uri = idx[ANDROID_NS] if version_code_ns_android else NO_INDEX
    attrs = []
    for _ in range(version_code_count):
        attrs.append(_attr(ns_uri, idx["versionCode"], version_code_raw, version_code_type, version_code))
    attrs.append(_attr(idx[ANDROID_NS], idx["versionName"], idx[version_name], TYPE_STRING, idx[version_name]))
    if with_version_code_major:
        attrs.append(_attr(idx[ANDROID_NS], idx["versionCodeMajor"], NO_INDEX, TYPE_INT_DEC, 1))
    attrs.append(_attr(NO_INDEX, idx["package"], idx["com.readyatdawn.r15"], TYPE_STRING, idx["com.readyatdawn.r15"]))
    child_attrs = [_attr(idx[ANDROID_NS], idx["minSdkVersion"], NO_INDEX, TYPE_INT_DEC, 29)]
    if nested_version_code:
        child_attrs.append(_attr(idx[ANDROID_NS], idx["versionCode"], NO_INDEX, TYPE_INT_DEC, 7))
    body = (
        string_pool(strings, utf8)
        + (resource_map(ids) if with_resource_map else b"")
        + _start_ns(idx["android"], idx[ANDROID_NS])
        + _start_element(NO_INDEX, idx[root], attrs)
        + _start_element(NO_INDEX, idx["uses-sdk"], child_attrs)
        + _end_element(NO_INDEX, idx["uses-sdk"])
        + _end_element(NO_INDEX, idx[root])
        + _end_ns(idx["android"], idx[ANDROID_NS])
        + trailing
    )
    return struct.pack("<HHI", 0x0003, 8, 8 + len(body)) + body


def build_label_manifest(package: str, label: str, *, label_id: int = RES_LABEL, share_label: bool = False) -> bytes:
    """A manifest with a UTF-16 string pool holding `package` once and `label` once:
    <manifest package><application android:label="LABEL"/></manifest>, the label a raw string attribute that no other
    attribute references (the shape tools/quest_apk_label.py edits in place). `label_id` is the resource id the map
    gives the label attribute's name; `share_label` adds a second string attribute on <manifest> that references the same
    pool string, so editing the label would rename that too. Both are for the refusal tests."""
    strings = ["label", "android", ANDROID_NS, "manifest", "package", package, "application", label]
    idx = {s: i for i, s in enumerate(strings)}
    root_attrs = [_attr(NO_INDEX, idx["package"], idx[package], TYPE_STRING, idx[package])]
    if share_label:
        root_attrs.append(_attr(NO_INDEX, idx["label"], idx[label], TYPE_STRING, idx[label]))
    app_attrs = [_attr(idx[ANDROID_NS], idx["label"], idx[label], TYPE_STRING, idx[label])]
    body = (
        string_pool(strings, False)
        + resource_map([label_id])
        + _start_ns(idx["android"], idx[ANDROID_NS])
        + _start_element(NO_INDEX, idx["manifest"], root_attrs)
        + _start_element(NO_INDEX, idx["application"], app_attrs)
        + _end_element(NO_INDEX, idx["application"])
        + _end_element(NO_INDEX, idx["manifest"])
        + _end_ns(idx["android"], idx[ANDROID_NS])
    )
    return struct.pack("<HHI", 0x0003, 8, 8 + len(body)) + body
