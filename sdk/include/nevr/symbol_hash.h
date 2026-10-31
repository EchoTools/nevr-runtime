#pragma once

// CSymbol64 hash — RAD engine symbol hashing (CRC64 variant).
//
// Verified against the game binary: echovr.exe 0x1400ce120 (disassembly name
// CSymbol64::Lookup) and libr15.so 0x11cc580 (CalculateSymbolValue).  The same
// algorithm exists in the Go service as server/evr/core_hash.go in nakama,
// which sign-extends a byte >= 0x80 as the game does (nakama#711).
// Produces case-insensitive 64-bit hashes used as resource IDs throughout the
// engine.
//
// The lookup table is generated at compile time from the custom polynomial
// 0x95ac9329ac4bc9b5 using the same bit-processing algorithm found in the
// binary (NOT standard CRC64 — has a different MSB structure and a final *2
// shift per entry).

#include <array>
#include <cstdint>

namespace EchoVR {

constexpr uint64_t kCSymbol64Polynomial = 0x95ac9329ac4bc9b5ULL;
constexpr uint64_t kCSymbol64Seed       = 0xFFFFFFFFFFFFFFFFULL;

// Generate the 256-entry CRC64 lookup table at compile time.
//
// Bits 7-6 use hardcoded constants (an optimization of the same polynomial
// reduction), bits 5-0 use standard doubling with conditional XOR, and a
// final *2 shift is applied to each entry.
constexpr std::array<uint64_t, 256> GenerateCSymbol64Table() {
    std::array<uint64_t, 256> table = {};

    for (uint32_t i = 0; i < 256; i++) {
        uint64_t crc = 0;

        // Bits 7-6 (hardcoded reduction constants)
        if (i & 0x80) crc = 0x2b5926535897936aULL;
        if ((i & 0x40) && !(i & 0x80))
            crc = 0x95ac9329ac4bc9b5ULL;
        else if ((i & 0x40) && (i & 0x80))
            crc = 0xbef5b57af4dc5adfULL;

        // Bits 5-0: doubling with conditional XOR
        for (uint8_t bit : {0x20, 0x10, 0x08, 0x04, 0x02, 0x01}) {
            crc = crc * 2;
            if (i & bit) crc ^= kCSymbol64Polynomial;
        }

        // Final shift (critical — omitting this produces wrong values)
        table[i] = crc * 2;
    }

    return table;
}

inline constexpr auto kCSymbol64Table = GenerateCSymbol64Table();

// Compute CSymbol64 hash for a null-terminated string.
//
// Case-insensitive: uppercase A-Z (0x41-0x5A) is converted to lowercase
// before hashing, matching the binary's `(uint8_t)(c + 0xBF) <= 0x19` check.
//
// Bytes >= 0x80: the game sign-extends each byte to 64 bits before the table
// XOR (echovr.exe 0x1400ce199 `MOVSX R8, DL`; libr15.so 0x11cc760 `sxtb x11,
// w11`), so "caf\xc3\xa9" hashes to 0xcd0714df62754f5f and "\xff\xfe" to
// 0x1f17d45f0697bb92.  The widening is spelled out because plain `char` is
// signed under MinGW and the host g++ and unsigned under the NDK's AArch64
// clang: a bare static_cast<uint64_t>(c) would zero-extend on Quest.  The
// widening is (byte ^ 0x80) - 0x80 because converting a byte >= 0x80 to int8_t
// is implementation-defined before C++20.
// Empty or null input returns the seed.  constexpr so a build for any target
// can static_assert the vectors (src/quest/tests/symbol_hash_test.cpp).
//
// Known results:
//   "rwd_tint_0000"             -> 0x74d228d09dc5dc86
//   "rwd_tint_0019"             -> 0x74d228d09dc5dd8f
//   "serverdb"                  -> 0x25e886012ced8064
//   "NEVRProtobufJSONMessageV1" -> 0xc6b3710cd9c4ef47
constexpr uint64_t CSymbol64Hash(const char* str, uint64_t seed = kCSymbol64Seed) {
    if (!str || *str == '\0') return seed;

    for (const char* p = str; *p != '\0'; p++) {
        char c = *p;

        // Convert A-Z to a-z.  The unsigned cast mirrors the binary's check:
        // (uint8_t)(c + 0xBF) is equivalent to (uint8_t)(c - 0x41), which is
        // <= 25 only for ASCII uppercase letters.
        if (static_cast<uint8_t>(c + 0xBF) <= 0x19) {
            c += 0x20;
        }

        seed = static_cast<uint64_t>(static_cast<int64_t>(static_cast<uint8_t>(c) ^ 0x80) - 0x80) ^
               kCSymbol64Table[(seed >> 56) & 0xFF] ^
               (seed << 8);
    }

    return seed;
}

}  // namespace EchoVR
