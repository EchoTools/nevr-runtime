/* The one validator of cargo (docs/adr/0045-cdn-texture-delivery.md). A manifest entry, a downloaded frame and a mounted record are
 * all judged here, by rules that have names: a refusal carries the rule's name, and whoever refuses logs that name.
 *
 * The rules, and what each one protects the engine from:
 *   frame_magic             the stored bytes do not start with the zstd frame magic: the engine's reader would hand garbage to ZSTD.
 *   content_size_missing    the frame header does not state the content size: the reader sizes its buffer from the record, so the
 *                           header must agree with it, and "absent" cannot agree.
 *   content_size_mismatch   the header's content size is not the size the record declares: a short buffer under a long frame.
 *   alignment               not a power of two of at most 4096: the engine's allocator takes the value as it is.
 *   duplicate_key           two entries share (platform, type, name): the manifest search finds either, so the engine would read
 *                           one of two files depending on the order of a sort (the shadow builder refuses it as well).
 *   zero_bytes              an entry of size or bytes 0. Zero-byte records are listed in the manifest's own `empty` list and mounted
 *                           without a frame (see cargo_store.h, "zero-byte records").
 *   size_range              a size that does not fit the engine's 32-bit words.
 *   object_too_large        more decompressed bytes than this device can give one object (Limits::max_object_bytes).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>

#include "core/cargo_format.h"
#include "core/cargo_limits.h"

namespace nevr_cargo {

enum class Rule : std::uint8_t {
  kNone = 0,
  kFrameMagic,
  kContentSizeMissing,
  kContentSizeMismatch,
  kAlignment,
  kDuplicateKey,
  kZeroBytes,
  kSizeRange,
  kObjectTooLarge,
};

const char* RuleName(Rule rule) noexcept;

// The longest zstd frame header (RFC 8878 3.1.1.1): magic 4, descriptor 1, window 1, dictionary id 4, content size 8.
inline constexpr std::size_t kMaxFrameHeaderBytes = kZstdMaxFrameHeaderBytes;

// How many leading bytes the header of the frame that starts with `bytes` (at least 5 of them) takes; 0 when the first five bytes
// are not a frame start.
std::size_t FrameHeaderBytes(const unsigned char* bytes, std::size_t length) noexcept;

// The content size a frame's header states. kNone with *size set; kFrameMagic when `head` is not a whole frame header; kContentSizeMissing
// when the header states none. A manifest frame is read with this before its body is downloaded, so its declared size is held to the
// device's memory first.
Rule FrameContentSize(const unsigned char* head, std::size_t head_length, std::uint64_t* size) noexcept;

// Checks the header of a stored frame against the size the record declares. `head` holds the first bytes of the frame (as many as
// the frame has, up to kMaxFrameHeaderBytes); `frame_bytes` is the whole stored size. kNone when the frame is acceptable.
Rule CheckFrame(const unsigned char* head, std::size_t head_length, std::uint64_t frame_bytes, std::uint64_t declared_bytes) noexcept;

// The fields of a record, as the manifest states them.
struct RecordFields {
  std::uint64_t size = 0;   // the stored frame's bytes
  std::uint64_t bytes = 0;  // what the engine decompresses it to
  std::uint64_t align = 0;
};

// The rules that need one record only. Order of the checks is the order of the rules above.
Rule CheckRecord(const RecordFields& record, const Limits& limits) noexcept;

// The rule that needs all the records: a key seen twice.
class KeySet {
 public:
  // True when (platform, type, name) was added before; the key is added either way.
  bool Add(const std::string& platform, std::uint64_t type, std::uint64_t name) {
    return !seen_.insert(std::make_tuple(platform, type, name)).second;
  }

 private:
  std::set<std::tuple<std::string, std::uint64_t, std::uint64_t>> seen_;
};

}  // namespace nevr_cargo
