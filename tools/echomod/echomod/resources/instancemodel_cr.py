"""CInstanceModelCR cloner — IDA-verified format.

CInstanceModelCR uses CBinaryStreamInspectorAttach with 10 parallel data arrays.

Header layout (568B):
  [  0.. 55] Desc 0: main entries (24B each)     — per-actor
  [ 56..111] Desc 1: model config (8B each)       — shared
  [112..119] u64: per-actor count
  [120..175] Desc 2: material lookup (8B each)     — shared (cap may > count)
  [176..231] Desc 3: u16 index                     — per-actor
  [232..287] Desc 4: u64                            — per-actor
  [288..343] Desc 5: scale (16B)                    — per-actor
  [344..399] Desc 6: u16                            — per-actor
  [400..455] Desc 7: u16                            — per-actor
  [456..511] Desc 8: f32                            — per-actor
  [512..567] Desc 9: full entry (72B)               — per-actor

Data arrays follow sequentially, each using count * entry_size bytes on disk.
Arrays are aligned to min(entry_size, 8) before each read.

Array 0 entry (24B): resource_hash(8) + entity_hash(8) + flags(8)
Array 9 entry (72B): f32(4) + resource_hash(8) + entity_hash(8) + ... (entity_hash at +12)

Per-actor arrays (count == main_count): 0, 3, 4, 5, 6, 7, 8, 9
Shared arrays (different count): 1, 2
"""
import struct

HEADER_SIZE = 568
DESC_SIZE = 56

# Descriptor offsets in header
DESC_OFFSETS = [
    0x000,  # 0: main entries
    0x038,  # 1: model config
    # u64 at 0x070
    0x078,  # 2: material lookup
    0x0B0,  # 3: u16 index
    0x0E8,  # 4: u64
    0x120,  # 5: scale
    0x158,  # 6: u16
    0x190,  # 7: u16
    0x1C8,  # 8: f32
    0x200,  # 9: full entry
]

U64_OFFSET = 0x070


def _read_desc(data: bytes, off: int):
    """Read descriptor fields: (dbs, cap, count)."""
    dbs = struct.unpack_from('<Q', data, off + 8)[0]
    cap = struct.unpack_from('<Q', data, off + 40)[0]
    cnt = struct.unpack_from('<Q', data, off + 48)[0]
    return dbs, cap, cnt


def _align(pos: int, entry_size: int) -> int:
    """Align position to the natural alignment of the entry type (capped at 8)."""
    alignment = min(entry_size, 8) if entry_size > 0 else 1
    remainder = pos % alignment
    if remainder != 0:
        return pos + (alignment - remainder)
    return pos


def clone_instancemodel_cr(data: bytes, source_entity_hash: int,
                           new_entity_hash: int) -> bytes:
    """Clone CInstanceModelCR entries for a given actor (entity_hash at +8 in array 0).

    Args:
        data: Raw binary data of the CInstanceModelCR file
        source_entity_hash: Entity hash of the actor whose entries to clone
        new_entity_hash: Entity hash for the cloned actor

    Returns:
        Modified binary data with cloned entries appended.
    """
    if len(data) < HEADER_SIZE:
        return data

    source_le = struct.pack('<Q', source_entity_hash)
    new_le = struct.pack('<Q', new_entity_hash)

    # Parse all 10 descriptors
    descs = []  # list of (dbs, cap, count, entry_size)
    for off in DESC_OFFSETS:
        dbs, cap, cnt = _read_desc(data, off)
        es = dbs // cap if cap > 0 else 0
        descs.append((dbs, cap, cnt, es))

    main_count = descs[0][2]  # count of per-actor arrays

    # Read all arrays sequentially WITH alignment
    pos = HEADER_SIZE
    arrays = []  # list of (raw_bytes, count, entry_size, is_per_actor)
    for i, (dbs, cap, cnt, es) in enumerate(descs):
        # Align to entry size before reading
        pos = _align(pos, es)
        size = cnt * es
        arr_data = data[pos:pos + size]
        is_per_actor = (cnt == main_count)
        arrays.append((arr_data, cnt, es, is_per_actor))
        pos += size

    # Find matching entries in array 0 by entity_hash at +8
    arr0_data, arr0_cnt, arr0_es, _ = arrays[0]
    matching_indices = []
    for i in range(arr0_cnt):
        off = i * arr0_es
        entity = struct.unpack_from('<Q', arr0_data, off + 8)[0]
        if entity == source_entity_hash:
            matching_indices.append(i)

    if not matching_indices:
        return data

    num_cloned = len(matching_indices)

    # Clone entries in all per-actor arrays
    new_arrays = []
    for arr_idx, (arr_data, cnt, es, is_per_actor) in enumerate(arrays):
        if is_per_actor:
            new_arr = bytearray(arr_data)
            for idx in matching_indices:
                entry = bytearray(arr_data[idx * es:(idx + 1) * es])
                # Replace entity hash bytes in the cloned entry
                entry = bytes(entry).replace(source_le, new_le)
                new_arr.extend(entry)
            new_arrays.append((bytes(new_arr), cnt + num_cloned, es))
        else:
            new_arrays.append((arr_data, cnt, es))

    # Rebuild header
    new_header = bytearray(data[:HEADER_SIZE])
    new_main_count = main_count + num_cloned

    for i, off in enumerate(DESC_OFFSETS):
        _, _, cnt, es = descs[i]
        if cnt == main_count:
            # Per-actor: update count, cap, dbs
            new_cnt = new_main_count
            new_dbs = new_cnt * es
            struct.pack_into('<Q', new_header, off + 8, new_dbs)    # dbs
            struct.pack_into('<Q', new_header, off + 40, new_cnt)   # capacity
            struct.pack_into('<Q', new_header, off + 48, new_cnt)   # count

    # Update the u64 extra field (also tracks per-actor count)
    struct.pack_into('<Q', new_header, U64_OFFSET, new_main_count)

    # Reassemble with alignment padding between arrays
    result = bytearray(new_header)
    for arr_data, cnt, es in new_arrays:
        # Align to entry size before writing each array
        aligned_pos = _align(len(result), es)
        padding = aligned_pos - len(result)
        if padding > 0:
            result.extend(b'\x00' * padding)
        result.extend(arr_data)

    return bytes(result)
