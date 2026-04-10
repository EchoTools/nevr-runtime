"""CAnimationCR cloner.

File layout — ALL DESCRIPTORS FIRST, then ALL DATA:
  [56B desc1: base entries, 24B each]
  [56B desc2: bitfield, 8B each]
  [8B  parentCount]
  [56B desc3: arr3, 176B each]
  [56B desc4: arr4, 8B each]
  [56B desc5: arr5, 216B each]
  --- header = 288B ---
  [count*24B  base entries]    type_hash at +8
  [bf_count*8B bitfield]
  [count*176B arr3]            parallel to base
  [count*8B   arr4]            parallel to base
  [count*216B arr5]            parallel to base

Base entry format (24B): [actor_hash(8)][type_hash(8)][flags(8)]
"""

from __future__ import annotations
import struct
from ..binary_utils import BinaryWriter

HEADER_SIZE = 288  # 5 descriptors (56B each) + parentCount (8B)
DESC_OFFSETS = [0, 56, 120, 176, 232]  # desc1-5
PARENT_COUNT_OFF = 112
ENTRY_SIZES = [24, 8, 176, 8, 216]  # per desc1-5


def clone_animation_entries(data: bytes, source_actor_hash: int, new_actor_hash: int) -> bytes:
    if len(data) < HEADER_SIZE:
        return data

    count = struct.unpack_from('<Q', data, DESC_OFFSETS[0] + 48)[0]
    bf_count = struct.unpack_from('<Q', data, DESC_OFFSETS[1] + 48)[0]
    if count == 0:
        return data

    # Find source entries by type_hash at +8 in base entries
    base_start = HEADER_SIZE
    source_indices = []
    for i in range(count):
        off = base_start + i * 24
        type_hash = struct.unpack_from('<Q', data, off + 8)[0]
        if type_hash == source_actor_hash:
            source_indices.append(i)
    if not source_indices:
        return data

    new_count = count + len(source_indices)

    # Parse all sections
    pos = HEADER_SIZE
    base_data = data[pos:pos + count * 24]; pos += count * 24
    bf_data = data[pos:pos + bf_count * 8]; pos += bf_count * 8
    a3_data = data[pos:pos + count * 176]; pos += count * 176
    a4_data = data[pos:pos + count * 8]; pos += count * 8
    a5_data = data[pos:pos + count * 216]; pos += count * 216
    tail = data[pos:]

    w = BinaryWriter()

    # Write updated descriptors
    def write_desc(desc_off, new_cnt, elem_size):
        d = bytearray(data[desc_off:desc_off + 56])
        struct.pack_into('<Q', d, 8, new_cnt * elem_size)   # dbs
        struct.pack_into('<Q', d, 40, new_cnt)               # count
        struct.pack_into('<Q', d, 48, new_cnt)               # capacity
        w.write_bytes(bytes(d))

    # Desc1: base entries
    write_desc(DESC_OFFSETS[0], new_count, 24)
    # Desc2: bitfield (unchanged count)
    w.write_bytes(data[DESC_OFFSETS[1]:DESC_OFFSETS[1] + 56])
    # ParentCount
    w.write_bytes(struct.pack('<Q', new_count))
    # Desc3-5: parallel arrays
    write_desc(DESC_OFFSETS[2], new_count, 176)
    write_desc(DESC_OFFSETS[3], new_count, 8)
    write_desc(DESC_OFFSETS[4], new_count, 216)

    # Base entries + clones
    w.write_bytes(base_data)
    for idx in source_indices:
        entry = bytearray(base_data[idx * 24:(idx + 1) * 24])
        struct.pack_into('<Q', entry, 8, new_actor_hash)  # type_hash at +8
        w.write_bytes(bytes(entry))

    # Bitfield (unchanged)
    w.write_bytes(bf_data)

    # Arr3 + clones (176B each)
    w.write_bytes(a3_data)
    for idx in source_indices:
        w.write_bytes(a3_data[idx * 176:(idx + 1) * 176])

    # Arr4 + clones (8B each)
    w.write_bytes(a4_data)
    for idx in source_indices:
        w.write_bytes(a4_data[idx * 8:(idx + 1) * 8])

    # Arr5 + clones (216B each, patch actor_hash at +8)
    w.write_bytes(a5_data)
    for idx in source_indices:
        entry = bytearray(a5_data[idx * 216:(idx + 1) * 216])
        struct.pack_into('<Q', entry, 8, new_actor_hash)
        w.write_bytes(bytes(entry))

    w.write_bytes(tail)
    return w.getvalue()
