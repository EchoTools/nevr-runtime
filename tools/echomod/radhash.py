# ============================================================
# RAD Engine CRC-64 hash (polynomial 0x95AC9329AC4BC9B5)
# ============================================================
POLY = 0x95AC9329AC4BC9B5
MASK64 = 0xFFFFFFFFFFFFFFFF
SEED = 0xFFFFFFFFFFFFFFFF

def build_crc_table():
    table = [0] * 256
    for i in range(256):
        result = 0
        for bit in range(7, -1, -1):
            if (i >> bit) & 1:
                result ^= POLY
            result = (result << 1) & MASK64
        table[i] = result
    return table

TABLE = build_crc_table()

def rad_hash(s, seed=SEED):
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