/* The numbers the formats define, each cited to its definition, so no other file in the cargo code carries a bare literal for one:
 * SHA-256 (FIPS 180-4), hexadecimal text, CRC-32 (ISO 3309 / IEEE 802.3, as zlib), the zstd frame header (RFC 8878 section 3.1.1), HTTP
 * status codes (RFC 9110) and the layout of the blob index (cargo_store.h). A number that limits how much of a device resource the
 * code uses is not here: it is read from the device (cargo_limits.h, device_memory.h).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_cargo {

// Bytes and bits.
inline constexpr unsigned kBitsPerByte = 8;
inline constexpr unsigned kByteMask = 0xff;
inline constexpr std::size_t kU32Bytes = sizeof(std::uint32_t);

// SHA-256 (FIPS 180-4): a 256-bit digest, 32 bytes, written as two lowercase hex characters a byte.
inline constexpr std::size_t kSha256DigestBytes = 32;
inline constexpr std::size_t kHexCharsPerByte = 2;
inline constexpr std::size_t kSha256HexChars = kHexCharsPerByte * kSha256DigestBytes;

// Hexadecimal text: four bits a digit; a letter digit counts from ten; a type or a name is the game's 64-bit symbol, "0x" and at most
// sixteen digits (64 bits over four).
inline constexpr unsigned kHexDigitBits = 4;
inline constexpr unsigned kHexDigitMask = 0xf;
inline constexpr unsigned kHexLetterBase = 10;
inline constexpr std::size_t kHexPrefixChars = 2;
inline constexpr std::size_t kSymbolBits = 64;
inline constexpr std::size_t kSymbolHexDigits = kSymbolBits / kHexDigitBits;
inline constexpr char kHexDigits[] = "0123456789abcdef";

// The value of one lowercase hex digit, or false.
inline bool HexDigitValue(char c, unsigned* value) noexcept {
  if (c >= '0' && c <= '9') {
    *value = static_cast<unsigned>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *value = static_cast<unsigned>(c - 'a') + kHexLetterBase;
    return true;
  }
  return false;
}

// CRC-32 (ISO 3309, the one zlib computes): the reflected polynomial, the all-ones start and end, a table of 256 entries.
inline constexpr std::uint32_t kCrc32Polynomial = 0xEDB88320u;
inline constexpr std::uint32_t kCrc32Init = 0xFFFFFFFFu;
inline constexpr std::size_t kCrc32TableEntries = 256;

// The blob index (cargo_store.h): a header of the magic, the version and the record size, then records of the digest and five 32-bit
// words: file, offset, size, bytes, and the CRC-32 of the record before it.
inline constexpr std::size_t kIndexMagicBytes = 8;
inline constexpr std::size_t kIndexVersionAt = kIndexMagicBytes;
inline constexpr std::size_t kIndexRecordSizeAt = kIndexVersionAt + kU32Bytes;
inline constexpr std::size_t kIndexHeaderBytes = kIndexRecordSizeAt + kU32Bytes;
inline constexpr std::size_t kIndexShaAt = 0;
inline constexpr std::size_t kIndexFileAt = kIndexShaAt + kSha256DigestBytes;
inline constexpr std::size_t kIndexOffsetAt = kIndexFileAt + kU32Bytes;
inline constexpr std::size_t kIndexSizeAt = kIndexOffsetAt + kU32Bytes;
inline constexpr std::size_t kIndexBytesAt = kIndexSizeAt + kU32Bytes;
inline constexpr std::size_t kIndexCrcAt = kIndexBytesAt + kU32Bytes;
inline constexpr std::size_t kIndexRecordBytes = kIndexCrcAt + kU32Bytes;

// The zstd frame header (RFC 8878 3.1.1): the magic number, one frame header descriptor, an optional window descriptor, an optional
// dictionary id and an optional frame content size.
inline constexpr unsigned char kZstdMagic[] = {0x28, 0xB5, 0x2F, 0xFD};  // 0xFD2FB528, little endian
inline constexpr std::size_t kZstdMagicBytes = sizeof(kZstdMagic);
inline constexpr std::size_t kZstdDescriptorAt = kZstdMagicBytes;
inline constexpr std::size_t kZstdProbeBytes = kZstdMagicBytes + 1;               // the magic and the descriptor: what tells a header's length
inline constexpr unsigned char kZstdReservedBit = 0x08;                          // must be zero
inline constexpr unsigned kZstdContentSizeFlagShift = 6;                         // the top two bits of the descriptor
inline constexpr unsigned kZstdSingleSegmentShift = 5;                           // the bit under them
inline constexpr unsigned char kZstdDictionaryFlagMask = 0x03;                   // the low two bits
inline constexpr std::size_t kZstdWindowDescriptorBytes = 1;                     // present unless the frame is a single segment
inline constexpr std::size_t kZstdDictionaryBytes[] = {0, 1, 2, 4};               // by dictionary id flag
inline constexpr std::size_t kZstdContentSizeBytes[] = {0, 2, 4, 8};              // by content size flag (flag 0: one byte when a single segment)
inline constexpr std::size_t kZstdSingleSegmentSizeBytes = 1;
inline constexpr unsigned kZstdContentSizeNone = 0;                              // the content size flag values
inline constexpr unsigned kZstdContentSizeTwoBytes = 1;
inline constexpr std::uint64_t kZstdTwoByteContentSizeBase = 256;                // a two-byte content size counts from 256
inline constexpr std::size_t kZstdMaxDictionaryBytes = 4;
inline constexpr std::size_t kZstdMaxContentSizeBytes = 8;
inline constexpr std::size_t kZstdDescriptorBytes = 1;
inline constexpr std::size_t kZstdMaxFrameHeaderBytes = kZstdMagicBytes + kZstdDescriptorBytes + kZstdWindowDescriptorBytes +
                                                         kZstdMaxDictionaryBytes + kZstdMaxContentSizeBytes;

// HTTP status codes (RFC 9110 section 15.3): a body from its first byte arrives as 200, or as 206 to a request with a range.
inline constexpr long kHttpOk = 200;
inline constexpr long kHttpPartialContent = 206;

}  // namespace nevr_cargo
