"""Generic compound CR cloner.

All compound CRs follow the BULK-THEN-INLINE pattern:
  [56B CRDescriptor]
  [entry_size * count flat entries]  (bulk read)
  [per-entry inline data]           (read sequentially per entry)

For cloning: append new flat entries at the end of the flat block with
zeroed inline descriptors (so no inline data is read for them).
The inline data for original entries follows unchanged.

Entry format varies by type but the common pattern is:
  +0: type_hash/node_hash (u64)
  +8: actor_hash (u64)
  +16: node_index (u16) + pad (u16)
  +20+: component data with embedded RadArrayDescriptor56 sub-arrays

The sub-array descriptors within each entry have their count at offset +48.
Setting count=0 prevents the inline reader from reading any data.
"""

from __future__ import annotations
import struct


def clone_compound_cr(data: bytes, source_actor_hash: int, new_actor_hash: int,
                      search_field: str = "actor_hash",
                      copy_inline: bool = False,
                      header_extra: int = 0) -> bytes:
    """Clone entries in a compound CR file.

    Args:
        data: Raw binary data of the compound CR file
        source_actor_hash: Hash of the actor to clone
        new_actor_hash: Hash for the cloned actor
        search_field: Which field to search ("actor_hash" at +8, or "type_hash" at +0)
        header_extra: Extra bytes after the 56B descriptor before entries start

    Returns:
        Modified binary data with cloned entries, or unchanged data if no entries found.
    """
    if len(data) < 56:
        return data

    # Read CRDescriptor
    desc_dbs = struct.unpack_from('<Q', data, 8)[0]
    desc_count = struct.unpack_from('<Q', data, 0x28)[0]
    desc_cap = struct.unpack_from('<Q', data, 0x30)[0]

    if desc_count == 0:
        return data

    entry_size = desc_dbs // desc_count
    flat_start = 56 + header_extra
    flat_end = flat_start + desc_count * entry_size
    inline_start = flat_end
    inline_end = len(data)

    # Find source entries
    search_offset = 8 if search_field == "actor_hash" else 0
    source_indices = []
    for i in range(desc_count):
        off = flat_start + i * entry_size
        h = struct.unpack_from('<Q', data, off + search_offset)[0]
        if h == source_actor_hash:
            source_indices.append(i)

    if not source_indices:
        return data

    # Compute per-entry inline data positions (for copy_inline mode)
    inline_positions = []  # (inline_start, inline_end) for each original entry
    if copy_inline:
        KNOWN_OFFSETS = {
            720: [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664],
            96: [40],
            152: [96],  # CParticleEffectCR (SGParticleEffectParam at +96, 32B sub-entries)
            24: [],
        }
        desc_offsets = KNOWN_OFFSETS.get(entry_size, [])
        # Compute inline size per entry from descriptors
        pos = flat_end
        for i in range(desc_count):
            start = pos
            eoff = flat_start + i * entry_size
            for d in desc_offsets:
                if eoff + d + 56 <= flat_end:
                    dbs = struct.unpack_from('<Q', data, eoff + d + 8)[0]
                    cnt = struct.unpack_from('<Q', data, eoff + d + 48)[0]
                    if dbs > 0 and cnt > 0:
                        pos += dbs
            inline_positions.append((start, pos))

    # Build cloned entries
    new_count = desc_count + len(source_indices)
    cloned_entries = bytearray()
    cloned_inline = bytearray()

    for idx in source_indices:
        off = flat_start + idx * entry_size
        entry = bytearray(data[off:off + entry_size])

        # Update the actor/type hash for the clone
        if search_field == "actor_hash":
            struct.pack_into('<Q', entry, 8, new_actor_hash)
        else:
            struct.pack_into('<Q', entry, 0, new_actor_hash)

        if copy_inline and inline_positions:
            # Keep descriptors intact, copy inline data
            istart, iend = inline_positions[idx]
            cloned_inline.extend(data[istart:iend])
        else:
            # Zero inline descriptors so no inline data is read
            _zero_inline_descriptors(entry, entry_size)

        cloned_entries.extend(entry)

    # Build new file: [updated header] [original flat] [cloned flat] [original inline] [cloned inline]
    new_header = bytearray(data[:flat_start])
    new_dbs = new_count * entry_size
    struct.pack_into('<Q', new_header, 8, new_dbs)     # data_size
    struct.pack_into('<Q', new_header, 0x28, new_count) # count
    struct.pack_into('<Q', new_header, 0x30, new_count) # capacity

    result = bytearray()
    result.extend(new_header)
    result.extend(data[flat_start:flat_end])  # Original flat entries
    result.extend(cloned_entries)              # Cloned flat entries
    result.extend(data[inline_start:inline_end])  # Original inline data
    result.extend(cloned_inline)               # Cloned inline data (if copy_inline)

    return bytes(result)


def _zero_inline_descriptors(entry: bytearray, entry_size: int):
    """Zero all RadArrayDescriptor56 count/capacity/dbs fields within an entry.

    Uses known descriptor offsets for each entry size, with a fallback scan.
    """
    # Known descriptor offsets by entry size
    KNOWN_OFFSETS = {
        720: [48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664],  # CScriptCR
        96:  [40],  # CComponentLODCR (descriptor at +40, dbs=960=8*120)
        152: [96],  # CParticleEffectCR (descriptor at +96, 32B sub-entries)
        24:  [],    # CAnimationCR, CR15SyncGrabCR (no room for descriptors)
    }

    offsets = KNOWN_OFFSETS.get(entry_size, None)

    if offsets is None:
        # Fallback: scan at 56-byte intervals from offset 20
        offsets = list(range(20, entry_size - 48, 56))
        # Also try 8-byte aligned offsets
        offsets.extend(range(24, entry_size - 48, 56))

    for desc_start in offsets:
        if desc_start + 56 > entry_size:
            continue
        # Zero the count, capacity, and dbs fields
        count = struct.unpack_from('<Q', entry, desc_start + 48)[0]
        if count > 0:
            struct.pack_into('<Q', entry, desc_start + 8, 0)   # dbs = 0
            struct.pack_into('<Q', entry, desc_start + 40, 0)  # capacity = 0
            struct.pack_into('<Q', entry, desc_start + 48, 0)  # count = 0
