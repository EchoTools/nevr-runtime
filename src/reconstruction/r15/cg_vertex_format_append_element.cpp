// NRadEngine::CGVertexFormat::AppendElement
// Quest libr15.so VA 0x290a4c8; ReVault binary 576.
// ReVault raw parent 100330679, reconstruction node 100600766; grade H: all 115 instructions (0x290a4c8..0x290a690, stored size 460) traced to the ret at 0x290a65c and the out-of-line size cases at 0x290a660..0x290a690; it calls nothing and reads no global, so 0 placeholders; the compiler's 2-way unrolled stream scan is written as the plain loop it computes, CGVertexFormat is raw storage with the 8-byte element record at byte offsets, and the function is void as in ReVault's raw decompilation and the merged tree (x0 incidentally still holds the pre-append count at the ret).
// @0x290a4c8 — NRadEngine::CGVertexFormat::AppendElement
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// NRadEngine::CGVertexFormat, 0x128 bytes: 36 eight-byte element records at +0x00..+0x120 and the number of
// records in use as a 64-bit count at +0x120 (the constructor, 0x2909098, fills every record with 0x0c,0xff x7 and
// stores 0 at +0x120). The sibling overload AppendElement(SVertexElement*), 0x290a4a8, copies one such 8-byte record
// to the slot at the count, increments the count and returns the old count.
class CGVertexFormat {
 public:
  // Enumerators are not established.
  enum EUsage : std::uint32_t {};
  enum EType : std::uint32_t {};

  // NRadEngine::CGVertexFormat::AppendElement(EUsage, EType, unsigned long long, unsigned long long,
  // unsigned long long, unsigned int) @ 0x290a4c8 (thunk 0x10348e0). The parameter names are not recovered.
  // Void: ReVault's raw decompilation of 0x290a4c8 returns void, as does the merged tree's declaration. The listing
  // never writes x0 after loading the count into it, so x0 incidentally still holds the pre-append count at the
  // ret, but no caller is established to read it.
  void AppendElement(EUsage usage, EType type, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5,
                     std::uint32_t arg6);

  alignas(8) unsigned char storage[0x128];
};
static_assert(sizeof(CGVertexFormat) == 0x128, "CGVertexFormat is 0x128 bytes");

namespace {

// +0x120 of the object: the record count.
constexpr std::size_t kCountOffset = 0x120;
// Size of one element record.
constexpr std::size_t kRecordBytes = 8;

// Byte offsets inside one element record (the shifts below are 8 * offset).
//   +0 arg1 (EUsage), low byte                         ; the GetElement overload at 0x29094bc compares it to its argument
//   +1 running offset of the record inside its stream  ; VertexSize at 0x2909538 sums +1 and +5 over matching records
//   +2 arg2 (EType), low byte
//   +3 arg3, low byte
//   +4 arg4, low byte
//   +5 size in bytes derived from the type and arg3
//   +6 arg5, low byte                                  ; VertexSize compares +6 with its argument
//   +7 arg6 (usage 10 clamps it to at least 1), low byte
constexpr std::size_t kOffsetByte = 1;
constexpr std::size_t kSizeByte = 5;
constexpr std::size_t kStreamByte = 6;

// bfm/bfi of the low byte of value into byte `index` of a record.
constexpr std::uint64_t Byte(std::uint64_t value, unsigned index) {
  return (value & 0xff) << (8 * index);
}

}  // namespace

// @0x290a4c8 — NRadEngine::CGVertexFormat::AppendElement
/* @confidence: H */
void CGVertexFormat::AppendElement(EUsage usage, EType type, std::uint64_t arg3, std::uint64_t arg4,
                                   std::uint64_t arg5, std::uint32_t arg6) {
  // 0x290a4d0: x0 = [this+0x120], the count; it stays in x0 to the ret (incidentally, the function is void).
  std::uint64_t count;
  std::memcpy(&count, storage + kCountOffset, sizeof(count));

  // 0x290a4d4..0x290a5f4: end of the last record of arg5's stream. The listing unrolls the scan two records at a
  // time (even and odd records keep separate maxima, joined at 0x290a59c) and finishes one leftover record in the
  // loop at 0x290a5ac; the result is the maximum of byte +1 plus byte +5 over the records whose byte +6 equals arg5
  // (a 64-bit compare, so an arg5 above 0xff matches nothing), or 0 for an empty format (cbz at 0x290a4d8).
  std::uint64_t stream_end = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const unsigned char* record = storage + i * kRecordBytes;
    if (static_cast<std::uint64_t>(record[kStreamByte]) == arg5) {
      const std::uint64_t end =
          static_cast<std::uint64_t>(record[kOffsetByte]) + static_cast<std::uint64_t>(record[kSizeByte]);
      if (stream_end < end) {
        stream_end = end;
      }
    }
  }

  // 0x290a5f8..0x290a604: byte +7. cmp w6,#1 / csinc is a signed max(arg6, 1); only usage 10 (cmp w1,#0xa) uses it.
  const std::uint32_t at_least_one = static_cast<std::int32_t>(arg6) > 1 ? arg6 : 1u;
  const std::uint32_t last_byte = static_cast<std::uint32_t>(usage) == 10 ? at_least_one : arg6;

  // 0x290a608..0x290a690: byte +5, the size in bytes selected by the type.
  const std::uint32_t type_value = static_cast<std::uint32_t>(type);
  std::uint64_t size;
  if (type_value < 2) {
    size = arg3;  // 0x290a60c mov x11,x3; b.cc 0x290a620
  } else if (type_value - 2 < 5) {
    size = arg3 << 1;  // types 2..6, lsl x11,x3,#1 at 0x290a61c
  } else if (type_value == 7) {
    size = 4;  // 0x290a678
  } else if (type_value == 8) {
    size = arg3 << 2;  // 0x290a670
  } else {
    size = (type_value - 9 < 2) ? 4 : 0;  // types 9 and 10 give 4, anything else 0 (0x290a680..0x290a68c)
  }

  // 0x290a620..0x290a644: pack the record.
  const std::uint64_t record = Byte(static_cast<std::uint32_t>(usage), 0) | Byte(stream_end, 1) |
                               Byte(static_cast<std::uint32_t>(type), 2) | Byte(arg3, 3) |
                               Byte(arg4, 4) | Byte(size, 5) | Byte(arg5, 6) | Byte(last_byte, 7);

  // 0x290a648: str x9,[x8,x0,lsl #3]. There is no bounds check: the 37th record lands on the count itself.
  std::memcpy(storage + count * kRecordBytes, &record, sizeof(record));

  // 0x290a64c..0x290a654: the count is read again after the record store, then incremented.
  std::uint64_t reread;
  std::memcpy(&reread, storage + kCountOffset, sizeof(reread));
  ++reread;
  std::memcpy(storage + kCountOffset, &reread, sizeof(reread));
}

}  // namespace NRadEngine
