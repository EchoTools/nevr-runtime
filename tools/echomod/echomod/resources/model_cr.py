"""CModelCR binary cloner — clones entries by duplicating data in all parallel sections."""

from __future__ import annotations
import struct
from ..binary_utils import BinaryWriter


def clone_model_entries(data: bytes, source_actor_hash: int, new_actor_hash: int,
                        model_hash_map: dict[int, int] | None = None) -> bytes:
    """Clone CModelCR entries for a given actor.

    Strategy: The CModelCR has a 360B header describing 3 parallel arrays
    (base 24B, scene set 264B, model 296B) plus bitfields. Each scene set
    and model entry has inline sub-array data that follows the flat entries.

    Cloned entries preserve their inline descriptors and inline data is
    copied from the source entries.

    Args:
        model_hash_map: Optional dict mapping old model resource hashes to new
            ones. When provided, the clone's scene set entry +0 and model
            entry +32 are patched to use the new hashes.
    """
    # Extract counts from 360B header
    base_count = struct.unpack_from('<Q', data, 0x30)[0]
    bf0_count = struct.unpack_from('<Q', data, 0x38 + 0x30)[0]

    # Find source entries in base array (actor_hash is at +8)
    source_indices = []
    for i in range(base_count):
        off = 360 + i * 24
        actor_hash = struct.unpack_from('<Q', data, off + 8)[0]
        if actor_hash == source_actor_hash:
            source_indices.append(i)

    if not source_indices:
        return data

    # Section boundaries
    base_start = 360
    base_end = base_start + base_count * 24

    bf0_start = base_end
    bf0_end = bf0_start + bf0_count * 8

    bf1_count = struct.unpack_from('<Q', data, 0xB0 + 0x30)[0]
    bf2_count = struct.unpack_from('<Q', data, 0xF0 + 0x30)[0]

    # Scene set section: BULK flat (264*count) then per-entry inline
    ss_flat_start = bf0_end
    ss_flat_end = ss_flat_start + base_count * 264

    # Compute per-entry scene set inline positions
    ss_inline_positions = []  # (start, end) for each entry
    pos = ss_flat_end
    for i in range(base_count):
        entry_off = ss_flat_start + i * 264
        entry_start = pos
        for desc_off in [40, 96, 152, 208]:
            dbs = struct.unpack_from('<Q', data, entry_off + desc_off + 8)[0]
            cnt = struct.unpack_from('<Q', data, entry_off + desc_off + 48)[0]
            if dbs > 0 and cnt > 0:
                pos += dbs
                if desc_off == 208:
                    sub_start = pos - dbs
                    for j in range(cnt):
                        for sdo in [8, 64]:
                            abs_off = sub_start + j * 120 + sdo
                            if abs_off + 56 <= len(data):
                                sdbs = struct.unpack_from('<Q', data, abs_off + 8)[0]
                                if sdbs > 0:
                                    pos += sdbs
        ss_inline_positions.append((entry_start, pos))

    sceneset_section_end = pos

    # bf1 and bf2
    bf1_start = sceneset_section_end
    bf1_end = bf1_start + bf1_count * 8
    bf2_start = bf1_end
    bf2_end = bf2_start + bf2_count * 8

    # Model entries section
    model_flat_start = bf2_end
    model_flat_end = model_flat_start + base_count * 296

    # Compute per-entry model inline positions
    model_inline_positions = []
    pos = model_flat_end
    for i in range(base_count):
        entry_off = model_flat_start + i * 296
        entry_start = pos
        for desc_off in [72, 128, 184, 240]:
            dbs = struct.unpack_from('<Q', data, entry_off + desc_off + 8)[0]
            cnt = struct.unpack_from('<Q', data, entry_off + desc_off + 48)[0]
            if dbs > 0 and cnt > 0:
                pos += dbs
                if desc_off == 240:
                    sub_start = pos - dbs
                    for j in range(cnt):
                        for sdo in [8, 64]:
                            abs_off = sub_start + j * 120 + sdo
                            if abs_off + 56 <= len(data):
                                sdbs = struct.unpack_from('<Q', data, abs_off + 8)[0]
                                if sdbs > 0:
                                    pos += sdbs
        model_inline_positions.append((entry_start, pos))

    model_section_end = pos
    # Include any trailing data after computed inline
    trailing = data[model_section_end:]

    # Build new file
    new_count = base_count + len(source_indices)
    new_header = bytearray(data[:360])

    _update_desc(new_header, 0x00, new_count, 24)
    _update_desc(new_header, 0x78, new_count, 264)
    _update_desc(new_header, 0x130, new_count, 296)
    struct.pack_into('<Q', new_header, 0x70, new_count)
    struct.pack_into('<Q', new_header, 0xE8, new_count)
    struct.pack_into('<Q', new_header, 0x128, new_count)

    w = BinaryWriter()
    w.write_bytes(bytes(new_header))

    # Base entries + cloned
    w.write_bytes(data[base_start:base_end])
    for idx in source_indices:
        entry = bytearray(data[base_start + idx * 24:base_start + (idx + 1) * 24])
        struct.pack_into('<Q', entry, 8, new_actor_hash)
        w.write_bytes(bytes(entry))

    # Bitfield0
    w.write_bytes(data[bf0_start:bf0_end])

    # Scene sets: flat entries (keep descriptors intact for clones)
    w.write_bytes(data[ss_flat_start:ss_flat_end])
    for idx in source_indices:
        ss_entry = bytearray(data[ss_flat_start + idx * 264:ss_flat_start + (idx + 1) * 264])
        if model_hash_map:
            # Scene set entry +0 contains the model resource hash
            old_hash = struct.unpack_from('<Q', ss_entry, 0)[0]
            if old_hash in model_hash_map:
                struct.pack_into('<Q', ss_entry, 0, model_hash_map[old_hash])
        w.write_bytes(bytes(ss_entry))
    # Original inline data, then cloned inline data (copied from source)
    w.write_bytes(data[ss_flat_end:sceneset_section_end])
    for idx in source_indices:
        istart, iend = ss_inline_positions[idx]
        w.write_bytes(data[istart:iend])

    # Bitfield1 + Bitfield2
    w.write_bytes(data[bf1_start:bf2_end])

    # Model entries: flat entries (keep descriptors intact for clones)
    w.write_bytes(data[model_flat_start:model_flat_end])
    for idx in source_indices:
        m_entry = bytearray(data[model_flat_start + idx * 296:model_flat_start + (idx + 1) * 296])
        # Do NOT patch model_entry+8 — the engine reads low bits as flags (bits 0,2,7)
        # that control rendering paths. Actor identity is set in the callback, not here.
        if model_hash_map:
            # Model entry +32 contains the model resource hash
            old_hash = struct.unpack_from('<Q', m_entry, 32)[0]
            if old_hash in model_hash_map:
                struct.pack_into('<Q', m_entry, 32, model_hash_map[old_hash])
        w.write_bytes(bytes(m_entry))
    # Original inline data, then cloned inline data (copied from source)
    w.write_bytes(data[model_flat_end:model_section_end])
    for idx in source_indices:
        istart, iend = model_inline_positions[idx]
        w.write_bytes(data[istart:iend])

    # Trailing data
    w.write_bytes(trailing)

    return w.getvalue()


def _update_desc(header: bytearray, offset: int, new_count: int, entry_size: int):
    """Update a RadArrayDescriptor56 in the header."""
    struct.pack_into('<Q', header, offset + 8, new_count * entry_size)
    struct.pack_into('<Q', header, offset + 40, new_count)
    struct.pack_into('<Q', header, offset + 48, new_count)
