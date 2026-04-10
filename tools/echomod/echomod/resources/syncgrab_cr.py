"""CR15SyncGrabCR cloner — IDA-verified format.

Uses CBinaryStreamInspectorAttach pattern:
  288B header block (5 descriptors + 8B extra field)
  5 data arrays in order, some with per-entry inline data

Header layout (288B):
  [  0.. 55] CTable<SLookup>           56B desc (24B entries)
  [ 56..111] CTable<u64>               56B desc (8B entries)
  [112..119] extra_field                8B (actor count)
  [120..175] CTableXT<SInitData>        56B desc (56B entries, inline)
  [176..231] CTableXT<SRuntimeData>     56B desc (824B entries, inline)
  [232..287] CTableXT<SProperties>      56B desc (128B entries, inline)

Data arrays:
  1. SLookup:      count × 24B  (actor_hash at +8)
  2. u64:          count × 8B
  3. SInitData:    count × 56B  + per-entry inline (CTableXT<SSplineData>)
  4. SRuntimeData: count × 824B + per-entry inline (14 CTables)
  5. SProperties:  count × 128B + per-entry inline (CTableXT<SSyncGrabDef>)

Inline data is read per-entry in order.  Each entry's CTable descriptors
(at known offsets within the entry) have count fields that determine
how much inline data follows.
"""
import struct
from ..binary_utils import BinaryReader


# Descriptor offsets within each entry type, and elem_size per CTable
# Format: list of (offset_in_entry, elem_size)
# For CTableXT: the descriptor IS the entry (it's a 56B descriptor whose
# count determines how many sub-entries follow, each of which may have
# their own inline CTables)

# SInitData (56B): it IS a CTableXT<SSplineData> descriptor
# SSplineData = 232B bulk entry with 4 inline CTables:
#   +0:  CTable<float>    56B desc, 4B/elem
#   +56: CTable<C3Vector> 56B desc, 12B/elem
#   +112: CTable<C3Vector> 56B desc, 12B/elem
#   +168: CTable<C3Vector> 56B desc, 12B/elem
#   +224: 8B trailing
SPLINE_INLINE_CTABLES = [(0, 4), (56, 12), (112, 12), (168, 12)]
SPLINE_ENTRY_SIZE = 232

# SRuntimeData (824B): 14 CTable descriptors at known offsets
# 16B fixed + 14×56B descs + 24B trailing = 16+784+24 = 824
RUNTIME_CTABLES = [
    (16, 8),     # CTable<CSymbol64>
    # +72: CTableXT<CTable<CTransfQ>> — needs special handling
    (128, 32),   # CTable<CTransfQ> "reforis"
    (184, 32),   # CTable<CTransfQ>
    (240, 32),   # CTable<CTransfQ> "lhreforis"
    (296, 32),   # CTable<CTransfQ> "lhinvreforis"
    (352, 40),   # CTableXT<COBB> "syncvolumes"
    (408, 4),    # CTable<uint> "grabtypes"
    (464, 4),    # CTable<CFlagsT<uint>>
    (520, 12),   # CTable<C3Vector> "closestrefs"
    (576, 4),    # CTable<float>
    (632, 8),    # CTable<CSymbol64>
    (688, 8),    # CTable<CSymbol64> "righthandgripanims"
    (744, 8),    # CTable<CSymbol64> "propanims"
]
# +72: CTableXT<CTable<CTransfQ>> — count of 56B inner descriptors, each with 32B/elem inline
RUNTIME_TABLEXT_OFFSET = 72
RUNTIME_TABLEXT_INNER_ELEM = 32

# SProperties (128B): CTableXT<SSyncGrabDefinition> at +48
# SSyncGrabDefinition = 496B with 5 inline CTables:
#   +48:  CTable<CTransfQ>  56B desc, 32B/elem
#   +104: CTable<float>     56B desc, 4B/elem
#   +160: CTable<C3Vector>  56B desc, 12B/elem
#   +216: CTable<C3Vector>  56B desc, 12B/elem
#   +272: CTable<C3Vector>  56B desc, 12B/elem
SYNCGRABDEF_INLINE_CTABLES = [(48, 32), (104, 4), (160, 12), (216, 12), (272, 12)]
SYNCGRABDEF_ENTRY_SIZE = 496
PROPS_TABLEXT_OFFSET = 48


def _read_desc_count(data: bytes, offset: int) -> int:
    """Read the count field from a 56B CTable descriptor at +48."""
    return struct.unpack_from("<Q", data, offset + 48)[0]


def _read_desc_dbs(data: bytes, offset: int) -> int:
    """Read the data_byte_size field from a 56B descriptor at +8."""
    return struct.unpack_from("<Q", data, offset + 8)[0]


def _compute_ctable_inline(entry_data: bytes, ctable_offsets: list) -> int:
    """Compute total inline data size from CTable descriptors in an entry."""
    total = 0
    for off, elem_size in ctable_offsets:
        count = _read_desc_count(entry_data, off)
        total += count * elem_size
    return total


def _read_inline_for_entry(r: BinaryReader, entry_data: bytes,
                           ctable_offsets: list) -> bytes:
    """Read inline data for one entry's CTable descriptors."""
    start = r.tell()
    for off, elem_size in ctable_offsets:
        count = _read_desc_count(entry_data, off)
        if count > 0:
            r.read_bytes(count * elem_size)
    return r._data[start:r.tell()]


def clone_syncgrab_cr(data: bytes, source_actor_hash: int,
                      new_actor_hash: int) -> bytes:
    """Clone an actor entry in CR15SyncGrabCR."""
    source_le = struct.pack("<Q", source_actor_hash)
    new_le = struct.pack("<Q", new_actor_hash)

    # --- Parse 288B header ---
    header = bytearray(data[:288])

    # Descriptor counts (all at +48 within each 56B desc)
    lookup_count = _read_desc_count(header, 0)
    u64_count = _read_desc_count(header, 56)
    extra_field = struct.unpack_from("<Q", header, 112)[0]
    init_count = _read_desc_count(header, 120)
    runtime_count = _read_desc_count(header, 176)
    props_count = _read_desc_count(header, 232)

    # --- Read data arrays ---
    r = BinaryReader(data)
    r.seek(288)

    # Array 1: SLookup (24B entries, no inline)
    lookup_entries = []
    for i in range(lookup_count):
        lookup_entries.append(r.read_bytes(24))

    # Array 2: u64 (8B entries, no inline)
    u64_entries = []
    for i in range(u64_count):
        u64_entries.append(r.read_bytes(8))

    # Array 3: SInitData (56B entries + inline SSplineData)
    init_entries = []  # (flat_bytes, inline_bytes)
    init_flat = []
    for i in range(init_count):
        init_flat.append(r.read_bytes(56))

    # Read inline for each SInitData entry
    for i in range(init_count):
        entry = init_flat[i]
        spline_count = _read_desc_count(entry, 0)  # SInitData IS a descriptor
        start = r.tell()
        # Read spline_count × 232B bulk entries
        spline_entries = []
        for j in range(spline_count):
            spline_entries.append(r.read_bytes(SPLINE_ENTRY_SIZE))
        # Read inline for each spline entry's 4 CTables
        spline_inline = []
        for j in range(spline_count):
            si_start = r.tell()
            for off, esz in SPLINE_INLINE_CTABLES:
                cnt = _read_desc_count(spline_entries[j], off)
                if cnt > 0:
                    r.read_bytes(cnt * esz)
            spline_inline.append(data[si_start:r.tell()])
        inline_bytes = data[start:r.tell()]
        init_entries.append((entry, inline_bytes))

    # Array 4: SRuntimeData (824B entries + inline for 14 CTables)
    runtime_entries = []
    runtime_flat = []
    for i in range(runtime_count):
        runtime_flat.append(r.read_bytes(824))

    for i in range(runtime_count):
        entry = runtime_flat[i]
        start = r.tell()

        # Read inline for CTable<CSymbol64> at +16
        for off, esz in RUNTIME_CTABLES:
            if off == RUNTIME_TABLEXT_OFFSET:
                continue  # handle separately
            # Skip the CTableXT at +72 position
        # Actually, read ALL in descriptor offset order within the entry

        # The inline is read in the ORDER the descriptors appear in the entry.
        # +16: CTable<CSymbol64> (8B)
        cnt = _read_desc_count(entry, 16)
        if cnt > 0:
            r.read_bytes(cnt * 8)

        # +72: CTableXT<CTable<CTransfQ>> — inner descriptors, then inner data
        tableXT_count = _read_desc_count(entry, 72)
        inner_descs = []
        for j in range(tableXT_count):
            inner_descs.append(r.read_bytes(56))
        for j in range(tableXT_count):
            inner_cnt = _read_desc_count(inner_descs[j], 0)
            if inner_cnt > 0:
                r.read_bytes(inner_cnt * RUNTIME_TABLEXT_INNER_ELEM)

        # +128 through +744: remaining 12 CTables
        remaining_ctables = [
            (128, 32), (184, 32), (240, 32), (296, 32),
            (352, 40), (408, 4), (464, 4), (520, 12),
            (576, 4), (632, 8), (688, 8), (744, 8),
        ]
        for off, esz in remaining_ctables:
            cnt = _read_desc_count(entry, off)
            if cnt > 0:
                r.read_bytes(cnt * esz)

        inline_bytes = data[start:r.tell()]
        runtime_entries.append((entry, inline_bytes))

    # Array 5: SProperties (128B entries + inline SSyncGrabDefinition)
    props_entries = []
    props_flat = []
    for i in range(props_count):
        props_flat.append(r.read_bytes(128))

    for i in range(props_count):
        entry = props_flat[i]
        start = r.tell()
        # CTableXT<SSyncGrabDefinition> at +48 in the 128B entry
        sgd_count = _read_desc_count(entry, PROPS_TABLEXT_OFFSET)
        # Read sgd_count × 496B bulk entries
        sgd_entries = []
        for j in range(sgd_count):
            sgd_entries.append(r.read_bytes(SYNCGRABDEF_ENTRY_SIZE))
        # Read inline for each SGD entry's 5 CTables
        for j in range(sgd_count):
            for off, esz in SYNCGRABDEF_INLINE_CTABLES:
                cnt = _read_desc_count(sgd_entries[j], off)
                if cnt > 0:
                    r.read_bytes(cnt * esz)
        inline_bytes = data[start:r.tell()]
        props_entries.append((entry, inline_bytes))

    assert r.tell() == len(data), (
        f"SyncGrab parse didn't consume all data: {r.tell()} != {len(data)}"
    )

    # --- Find frisbee entry index ---
    frisbee_idx = -1
    for i, entry in enumerate(lookup_entries):
        if source_le in entry:
            frisbee_idx = i
            break
    if frisbee_idx < 0:
        return data  # no frisbee found

    # --- Clone the frisbee entry in each array ---
    # SLookup: clone with new actor hash
    clone_lookup = bytearray(lookup_entries[frisbee_idx])
    idx = clone_lookup.find(source_le)
    if idx >= 0:
        clone_lookup[idx:idx + 8] = new_le
    lookup_entries.append(bytes(clone_lookup))

    # u64: NOT per-actor (count=1 while actors=2), do NOT clone

    # SInitData: clone entry + inline
    if frisbee_idx < len(init_entries):
        flat, inline = init_entries[frisbee_idx]
        init_entries.append((flat, inline))

    # SRuntimeData: clone entry + inline (replace actor hash in inline if present)
    if frisbee_idx < len(runtime_entries):
        flat, inline = runtime_entries[frisbee_idx]
        clone_inline = bytearray(inline)
        # Replace any occurrences of source hash in inline data
        idx = 0
        while True:
            found = clone_inline.find(source_le, idx)
            if found < 0:
                break
            clone_inline[found:found + 8] = new_le
            idx = found + 8
        runtime_entries.append((flat, bytes(clone_inline)))

    # SProperties: clone entry + inline
    if frisbee_idx < len(props_entries):
        flat, inline = props_entries[frisbee_idx]
        props_entries.append((flat, inline))

    # --- Update header ---
    new_actor_count = lookup_count + 1

    def _update_desc(hdr, desc_off, old_count, entry_size):
        nc = old_count + 1
        struct.pack_into("<Q", hdr, desc_off + 8, nc * entry_size)  # byte_size
        struct.pack_into("<Q", hdr, desc_off + 40, nc)  # capacity
        struct.pack_into("<Q", hdr, desc_off + 48, nc)  # count

    _update_desc(header, 0, lookup_count, 24)        # SLookup: 2→3
    # u64 descriptor: NOT per-actor (count=1), leave unchanged
    struct.pack_into("<Q", header, 112, new_actor_count)  # extra field
    _update_desc(header, 120, init_count, 56)        # SInitData: 2→3
    _update_desc(header, 176, runtime_count, 824)    # SRuntimeData: 2→3
    _update_desc(header, 232, props_count, 128)      # SProperties: 2→3

    # --- Serialize ---
    out = bytearray(header)

    for entry in lookup_entries:
        out += entry
    for entry in u64_entries:
        out += entry
    for flat, inline in init_entries:
        out += flat
    for flat, inline in init_entries:
        out += inline
    for flat, inline in runtime_entries:
        out += flat
    for flat, inline in runtime_entries:
        out += inline
    for flat, inline in props_entries:
        out += flat
    for flat, inline in props_entries:
        out += inline

    return bytes(out)
