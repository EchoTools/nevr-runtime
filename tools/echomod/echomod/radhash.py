"""RAD Engine CRC-64 hash (polynomial 0x95AC9329AC4BC9B5, case-insensitive)."""

POLY = 0x95AC9329AC4BC9B5
MASK64 = 0xFFFFFFFFFFFFFFFF
SEED = 0xFFFFFFFFFFFFFFFF


def _build_crc_table():
    table = [0] * 256
    for i in range(256):
        result = 0
        for bit in range(7, -1, -1):
            if (i >> bit) & 1:
                result ^= POLY
            result = (result << 1) & MASK64
        table[i] = result
    return table


TABLE = _build_crc_table()


def rad_hash(s: str, seed: int = SEED) -> int:
    """Compute the RAD Engine CRC-64 hash of a string (case-insensitive)."""
    h = seed
    for ch in s:
        c = ord(ch)
        if 0x41 <= c <= 0x5A:
            c += 0x20
        if c >= 0x80:
            c = c | (~0xFF & MASK64)
        idx = (h >> 56) & 0xFF
        h = (c ^ TABLE[idx]) ^ ((h << 8) & MASK64)
    return h
