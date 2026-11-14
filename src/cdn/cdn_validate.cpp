#include "cdn/cdn_validate.h"

namespace nevr_cdn {

namespace {

bool IsPowerOfTwo(std::uint64_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

struct FrameDescriptor {
  unsigned contentSizeFlag;
  bool singleSegment;
  unsigned dictionaryFlag;
};

FrameDescriptor Describe(unsigned char descriptor) noexcept {
  return FrameDescriptor{static_cast<unsigned>(descriptor >> kZstdContentSizeFlagShift),
                         ((descriptor >> kZstdSingleSegmentShift) & 1u) != 0,
                         static_cast<unsigned>(descriptor & kZstdDictionaryFlagMask)};
}

// How many bytes the frame content size takes: its flag's width, and a single segment with flag 0 states one byte.
std::size_t ContentSizeBytes(const FrameDescriptor& d) noexcept {
  if (d.contentSizeFlag == kZstdContentSizeNone) return d.singleSegment ? kZstdSingleSegmentSizeBytes : 0;
  return kZstdContentSizeBytes[d.contentSizeFlag];
}

// Where the content size starts: after the magic, the descriptor, the window descriptor (not in a single segment) and the dictionary id.
std::size_t ContentSizeAt(const FrameDescriptor& d) noexcept {
  return kZstdMagicBytes + kZstdDescriptorBytes + (d.singleSegment ? 0 : kZstdWindowDescriptorBytes) + kZstdDictionaryBytes[d.dictionaryFlag];
}

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
  if (length < kZstdProbeBytes) return 0;
  for (std::size_t i = 0; i < kZstdMagicBytes; ++i) {
    if (bytes[i] != kZstdMagic[i]) return 0;
  }
  const unsigned char descriptor = bytes[kZstdDescriptorAt];
  if ((descriptor & kZstdReservedBit) != 0) return 0;
  const FrameDescriptor d = Describe(descriptor);
  return ContentSizeAt(d) + ContentSizeBytes(d);
}

Rule FrameContentSize(const unsigned char* head, std::size_t head_length, std::uint64_t* size) noexcept {
  const std::size_t header = FrameHeaderBytes(head, head_length);
  if (header == 0 || head_length < header) return Rule::kFrameMagic;
  const FrameDescriptor d = Describe(head[kZstdDescriptorAt]);
  const std::size_t width = ContentSizeBytes(d);
  if (width == 0) return Rule::kContentSizeMissing;
  const std::size_t at = ContentSizeAt(d);
  std::uint64_t content = 0;
  for (std::size_t i = 0; i < width; ++i) content |= static_cast<std::uint64_t>(head[at + i]) << (kBitsPerByte * i);
  if (d.contentSizeFlag == kZstdContentSizeTwoBytes) content += kZstdTwoByteContentSizeBase;
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

}  // namespace nevr_cdn
