#include "core/cargo_validate.h"

namespace nevr_cargo {

namespace {

constexpr unsigned char kMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};  // 0xFD2FB528, little endian (RFC 8878 3.1.1)

bool IsPowerOfTwo(std::uint64_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

}  // namespace

const char* RuleName(Rule rule) noexcept {
  switch (rule) {
    case Rule::kNone: return "none";
    case Rule::kFrameMagic: return "frame_magic";
    case Rule::kContentSizeMissing: return "content_size_missing";
    case Rule::kContentSizeMismatch: return "content_size_mismatch";
    case Rule::kAlignment: return "alignment";
    case Rule::kDuplicateKey: return "duplicate_key";
    case Rule::kZeroBytes: return "zero_bytes";
    case Rule::kSizeRange: return "size_range";
    case Rule::kObjectTooLarge: return "object_too_large";
  }
  return "unknown";
}

std::size_t FrameHeaderBytes(const unsigned char* bytes, std::size_t length) noexcept {
  if (length < 5) return 0;
  for (std::size_t i = 0; i < 4; ++i) {
    if (bytes[i] != kMagic[i]) return 0;
  }
  const unsigned char descriptor = bytes[4];
  if ((descriptor & 0x08) != 0) return 0;  // the reserved bit must be 0
  const unsigned fcs = descriptor >> 6;
  const bool single_segment = ((descriptor >> 5) & 1) != 0;
  static constexpr std::size_t kDictionaryBytes[4] = {0, 1, 2, 4};
  std::size_t size = 5;
  if (!single_segment) size += 1;  // the window descriptor
  size += kDictionaryBytes[descriptor & 3];
  switch (fcs) {
    case 0: size += single_segment ? 1 : 0; break;
    case 1: size += 2; break;
    case 2: size += 4; break;
    default: size += 8; break;
  }
  return size;
}

Rule FrameContentSize(const unsigned char* head, std::size_t head_length, std::uint64_t* size) noexcept {
  const std::size_t header = FrameHeaderBytes(head, head_length);
  if (header == 0 || head_length < header) return Rule::kFrameMagic;
  const unsigned char descriptor = head[4];
  const unsigned fcs = descriptor >> 6;
  const bool single_segment = ((descriptor >> 5) & 1) != 0;
  if (fcs == 0 && !single_segment) return Rule::kContentSizeMissing;
  static constexpr std::size_t kDictionaryBytes[4] = {0, 1, 2, 4};
  const std::size_t at = 5 + (single_segment ? 0 : 1) + kDictionaryBytes[descriptor & 3];
  const std::size_t width = fcs == 0 ? 1 : (fcs == 1 ? 2 : (fcs == 2 ? 4 : 8));
  std::uint64_t content = 0;
  for (std::size_t i = 0; i < width; ++i) content |= static_cast<std::uint64_t>(head[at + i]) << (8 * i);
  if (fcs == 1) content += 256;  // the two-byte form counts from 256
  *size = content;
  return Rule::kNone;
}

Rule CheckFrame(const unsigned char* head, std::size_t head_length, std::uint64_t frame_bytes, std::uint64_t declared_bytes) noexcept {
  std::uint64_t content = 0;
  const Rule r = FrameContentSize(head, head_length, &content);
  if (r != Rule::kNone) return r;
  if (frame_bytes < FrameHeaderBytes(head, head_length)) return Rule::kFrameMagic;
  return content == declared_bytes ? Rule::kNone : Rule::kContentSizeMismatch;
}

Rule CheckRecord(const RecordFields& record, const Limits& limits) noexcept {
  if (record.size == 0 || record.bytes == 0) return Rule::kZeroBytes;
  if (record.size > kFormatMaxObjectBytes || record.bytes > kFormatMaxObjectBytes) return Rule::kSizeRange;
  if (!IsPowerOfTwo(record.align) || record.align > kFormatMaxAlignment) return Rule::kAlignment;
  if (record.bytes > limits.max_object_bytes) return Rule::kObjectTooLarge;
  return Rule::kNone;
}

}  // namespace nevr_cargo
