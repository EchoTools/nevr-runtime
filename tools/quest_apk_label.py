#!/usr/bin/env python3
"""The application label of the Quest APK: a strict reader and in-place writer of the compiled AndroidManifest.xml.

WHY. The reference build's manifest names its app with a RAW string, `android:label="Echo Combat (port)"`, so the
installed app reads that name in the headset's library. The Quest APK of this project must read "Echo VR" like the
store game it replaces. The store APK's own label is a raw string too ("Echo VR"), so the tool only ever replaces one
raw string with another.

HOW. The label is one string of the manifest's string pool, referenced by the `android:label` attribute (resource
id 0x01010001) of the `<application>` element as a string-typed attribute whose raw index and data index are that
string. The tool overwrites that string IN PLACE: the new text must be no longer than the old, its length prefix is
rewritten, the characters follow, the terminator follows, and the bytes left over from the old text are zeroed. No
offset, chunk size or other string moves, so every other byte of the manifest is unchanged, and the tool proves that
(it re-reads the result and compares every byte outside the string's slot). It refuses when the string is also
referenced by any other attribute (the edit would rename that too), when the label is not a plain string attribute,
when the pool is not one it understands, and when the new text is longer or not ASCII.

    quest_apk_label.py set AndroidManifest.xml OUT.xml --label "Echo VR"
    quest_apk_label.py show AndroidManifest.xml
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quest_apk_version as qv  # noqa: E402

RES_ID_LABEL = 0x01010001
TYPE_STRING = 0x03


class LabelError(Exception):
    pass


def _walk(data: bytes) -> dict:
    """The string pool and the application's label attribute, with every string-typed attribute reference counted."""
    if len(data) < 8:
        raise LabelError("not a compiled XML document: shorter than a chunk header")
    kind, header_size, size = struct.unpack_from("<HHI", data, 0)
    if kind != qv.RES_XML or header_size != 8 or size != len(data):
        raise LabelError(f"not a compiled XML document: type 0x{kind:04x}, header {header_size}, size {size} of {len(data)} bytes")
    pool, strings, resource_ids = None, None, None
    refs: dict = {}
    label = None
    pos = 8
    while pos < len(data):
        try:
            kind, header_size, size = qv._chunk_header(data, pos, len(data))
            if kind == qv.RES_STRING_POOL:
                if pool is not None:
                    raise LabelError("a second string pool")
                strings = qv._string_pool(data, pos, header_size, size)
                count, styles, flags, strings_start, _ = struct.unpack_from("<IIIII", data, pos + 8)
                if styles:
                    raise LabelError("the string pool carries styles: this tool does not edit such a pool")
                pool = {"pos": pos, "header_size": header_size, "strings_start": strings_start, "utf8": bool(flags & qv.UTF8_FLAG)}
            elif kind == qv.RES_XML_RESOURCE_MAP:
                resource_ids = list(struct.unpack_from(f"<{(size - 8) // 4}I", data, pos + 8))
            elif kind == qv.RES_XML_START_ELEMENT:
                if strings is None or resource_ids is None:
                    raise LabelError("an element before the string pool or the resource map")
                ns, name, attr_start, attr_size, attr_count, _, _, _ = struct.unpack_from("<IIHHHHHH", data, pos + header_size)
                if attr_start != 0x14 or attr_size != 0x14 or header_size + attr_start + attr_size * attr_count > size:
                    raise LabelError("an element's attributes do not fit its chunk")
                element = strings[name] if name < len(strings) else ""
                for i in range(attr_count):
                    at = pos + header_size + attr_start + attr_size * i
                    a_ns, a_name, a_raw, a_size, a_res0, a_type, a_data = struct.unpack_from("<IIIHBBI", data, at)
                    if a_type == TYPE_STRING:
                        for index in {a_raw, a_data}:
                            if index != qv.NO_INDEX:
                                refs[index] = refs.get(index, 0) + 1
                    is_label = a_name < len(resource_ids) and resource_ids[a_name] == RES_ID_LABEL
                    if element == "application" and is_label:
                        if label is not None:
                            raise LabelError("two android:label attributes on <application>")
                        if a_type != TYPE_STRING or a_raw != a_data or a_data >= len(strings):
                            raise LabelError(f"the label is not a plain string attribute (type 0x{a_type:02x}, raw {a_raw:#x}, data {a_data:#x})")
                        label = a_data
        except (struct.error, qv.ManifestError) as error:
            raise LabelError(str(error)) from None
        pos += size
    if pool is None or label is None:
        raise LabelError("no string pool, or no android:label on <application>")
    return {"strings": strings, "pool": pool, "label": label, "refs": refs, "data": data}


def label_of(data: bytes) -> str:
    walked = _walk(data)
    return walked["strings"][walked["label"]]


def _slot(data: bytes, walked: dict) -> tuple:
    """(start, end, capacity in characters) of the label string's bytes: its length prefix through its terminator."""
    pool = walked["pool"]
    index = walked["label"]
    offset = struct.unpack_from("<I", data, pool["pos"] + pool["header_size"] + 4 * index)[0]
    at = pool["pos"] + pool["strings_start"] + offset
    if pool["utf8"]:
        chars, after = qv._read_len8(data, at)
        length, after = qv._read_len8(data, after)
        if after - at != 2 or chars != length or chars >= 0x80:
            raise LabelError("the label is not a short ASCII string of a UTF-8 pool")
        return at, after + length + 1, length
    first = struct.unpack_from("<H", data, at)[0]
    if first & 0x8000:
        raise LabelError("the label's length uses the long form")
    return at, at + 2 + 2 * first + 2, first


def set_label(data: bytes, text: str) -> bytes:
    """`data` with the application label replaced by `text`, in place; LabelError unless every rule in the docstring holds."""
    if not text or not text.isascii() or "\0" in text:
        raise LabelError(f"the new label {text!r} must be non-empty ASCII")
    walked = _walk(data)
    if walked["refs"].get(walked["label"], 0) != 1:
        raise LabelError(f"the label string is referenced by {walked['refs'].get(walked['label'], 0)} string attributes; "
                         "editing it would rename the others too")
    start, end, capacity = _slot(data, walked)
    if len(text) > capacity:
        raise LabelError(f"the new label has {len(text)} characters, the old one {capacity}: this tool only edits in place")
    out = bytearray(data)
    if walked["pool"]["utf8"]:
        body = bytes([len(text), len(text)]) + text.encode("ascii") + b"\0"
    else:
        body = struct.pack("<H", len(text)) + text.encode("utf-16-le") + b"\0\0"
    out[start:end] = body + b"\0" * (end - start - len(body))
    result = bytes(out)
    if len(result) != len(data) or label_of(result) != text:
        raise LabelError("the edited manifest does not read back with the new label")
    changed = [i for i, (a, b) in enumerate(zip(data, result)) if a != b]
    if changed and (changed[0] < start or changed[-1] >= end):
        raise LabelError("the edit changed a byte outside the label string's slot")
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    show = sub.add_parser("show")
    show.add_argument("manifest", type=Path)
    setter = sub.add_parser("set")
    setter.add_argument("manifest", type=Path)
    setter.add_argument("out", type=Path)
    setter.add_argument("--label", required=True)
    args = parser.parse_args(argv)
    try:
        data = args.manifest.read_bytes()
        if args.cmd == "show":
            print(json.dumps({"tag": "NEVR.QUEST.LABEL", "event": "label", "label": label_of(data)}))
            return 0
        before = label_of(data)
        args.out.write_bytes(set_label(data, args.label))
        print(json.dumps({"tag": "NEVR.QUEST.LABEL", "event": "label_set", "before": before, "after": args.label,
                          "out": args.out.name}))
        return 0
    except (OSError, LabelError) as error:
        print(json.dumps({"tag": "NEVR.QUEST.LABEL", "event": "label_refused", "reason": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
