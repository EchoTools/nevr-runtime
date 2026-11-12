// Host test for the cargo validator (src/core/cargo_validate.*): every rule refuses by name, and a good record passes every rule.
// One test per rule, written so that removing the rule's check turns exactly that test red (tools/ mutants in the cargo REPORT).
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <string>

#include "core/cargo_limits.h"
#include "core/cargo_validate.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;

const unsigned char* U(const std::string& s) { return reinterpret_cast<const unsigned char*>(s.data()); }

nc::Limits TestLimits() {
  nc::Limits l;
  l.ok = true;
  l.max_object_bytes = 1000000;
  return l;
}

void TheRuleNamesAreTheLoggedWords() {
  QCHECK(std::string(nc::RuleName(nc::Rule::kNone)) == "none");
  QCHECK(std::string(nc::RuleName(nc::Rule::kFrameMagic)) == "frame_magic");
  QCHECK(std::string(nc::RuleName(nc::Rule::kContentSizeMissing)) == "content_size_missing");
  QCHECK(std::string(nc::RuleName(nc::Rule::kContentSizeMismatch)) == "content_size_mismatch");
  QCHECK(std::string(nc::RuleName(nc::Rule::kAlignment)) == "alignment");
  QCHECK(std::string(nc::RuleName(nc::Rule::kDuplicateKey)) == "duplicate_key");
  QCHECK(std::string(nc::RuleName(nc::Rule::kZeroBytes)) == "zero_bytes");
  QCHECK(std::string(nc::RuleName(nc::Rule::kSizeRange)) == "size_range");
  QCHECK(std::string(nc::RuleName(nc::Rule::kObjectTooLarge)) == "object_too_large");
}

// ── frame magic ──────────────────────────────────────────────────────────────────────────────────────────────────────────
void AFrameThatDoesNotStartWithTheZstdMagicIsRefused() {
  std::string f = cargo_test::MakeFrame(100, 64);
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 100) == nc::Rule::kNone);
  f[0] = 'X';
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 100) == nc::Rule::kFrameMagic);
  f = cargo_test::MakeFrame(100, 64);
  f[3] = static_cast<char>(0xFE);  // the magic's last byte
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 100) == nc::Rule::kFrameMagic);
  // A skippable frame (magic 0x184D2A5x) is a frame the engine's reader does not decode either.
  f = cargo_test::MakeFrame(100, 64);
  f[0] = 0x50;
  f[1] = 0x2A;
  f[2] = 0x4D;
  f[3] = 0x18;
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 100) == nc::Rule::kFrameMagic);
}

void AFrameTooShortToHoldItsHeaderIsNotAFrame() {
  const std::string f = cargo_test::MakeFrame(100, 64);
  QCHECK(nc::CheckFrame(U(f), 3, 3, 100) == nc::Rule::kFrameMagic);
  QCHECK(nc::CheckFrame(U(f), 5, 5, 100) == nc::Rule::kFrameMagic);  // the descriptor says a content-size byte follows; it is cut off
  QCHECK(nc::CheckFrame(U(f), 0, 0, 100) == nc::Rule::kFrameMagic);
}

// ── header content size ──────────────────────────────────────────────────────────────────────────────────────────────────
void TheHeadersContentSizeMustEqualTheDeclaredSize() {
  for (const std::uint64_t bytes : {std::uint64_t{1}, std::uint64_t{255}, std::uint64_t{256}, std::uint64_t{65791}, std::uint64_t{65792},
                                    std::uint64_t{217748}, std::uint64_t{54312960}}) {
    const std::string f = cargo_test::MakeFrame(bytes, 40);
    QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), bytes) == nc::Rule::kNone);
    QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), bytes + 1) == nc::Rule::kContentSizeMismatch);
    QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), bytes - 1) == nc::Rule::kContentSizeMismatch);
  }
}

void AHeaderWithNoContentSizeIsRefused() {
  // magic, descriptor 0x00 (no single segment, no content size, no dictionary), window descriptor 0x58: a streaming frame.
  std::string f("\x28\xB5\x2F\xFD\x00\x58", 6);
  f.append(30, 'z');
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 100) == nc::Rule::kContentSizeMissing);
}

void TheHeaderIsReadPastAWindowDescriptorAndADictionaryId() {
  // descriptor 0x41: content size flag 1 (2 bytes), dictionary id flag 1 (1 byte), not single segment, so a window byte follows.
  std::string f = "\x28\xB5\x2F\xFD\x41\x58\x07\x2C\x01";  // window, dict id 7, content size (0x012C + 256) = 556
  f.append(30, 'z');
  QCHECK(nc::FrameHeaderBytes(U(f), f.size()) == 9);
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 556) == nc::Rule::kNone);
  QCHECK(nc::CheckFrame(U(f), f.size(), f.size(), 557) == nc::Rule::kContentSizeMismatch);
  // the eight-byte content size
  std::string g = "\x28\xB5\x2F\xFD\xE0";  // flag 3 (8 bytes), single segment, dictionary id flag 0
  const std::uint64_t big = 6000000000ull;
  for (int i = 0; i < 8; ++i) g.push_back(static_cast<char>((big >> (8 * i)) & 0xff));
  g.append(30, 'z');
  QCHECK(nc::FrameHeaderBytes(U(g), g.size()) == 13);
  QCHECK(nc::CheckFrame(U(g), g.size(), g.size(), big) == nc::Rule::kNone);
}

void TheLongestHeaderIsEighteenBytes() {
  std::string f = "\x28\xB5\x2F\xFD\xC3\x58";  // flag 3, dictionary id flag 3, window byte
  f.append("\x01\x02\x03\x04", 4);              // dictionary id
  f.append("\x00\x00\x00\x00\x00\x00\x00\x00", 8);
  f.append(10, 'z');
  QCHECK(nc::FrameHeaderBytes(U(f), f.size()) == nc::kMaxFrameHeaderBytes);
  QCHECK(nc::kMaxFrameHeaderBytes == 18);
}

// ── alignment, zero bytes, size range, object size ───────────────────────────────────────────────────────────────────────
void AnAlignmentIsAPowerOfTwoOfAtMost4096() {
  nc::RecordFields r;
  r.size = 100;
  r.bytes = 200;
  for (const std::uint64_t ok : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{4}, std::uint64_t{16}, std::uint64_t{256},
                                 std::uint64_t{2048}, std::uint64_t{4096}}) {
    r.align = ok;
    QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kNone);
  }
  for (const std::uint64_t bad : {std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{6}, std::uint64_t{12}, std::uint64_t{4095},
                                  std::uint64_t{8192}, std::uint64_t{65536}, std::uint64_t{1} << 40}) {
    r.align = bad;
    QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kAlignment);
  }
}

void ARecordOfZeroBytesIsNotAnEntry() {
  nc::RecordFields r;
  r.size = 100;
  r.bytes = 0;
  r.align = 16;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kZeroBytes);
  r.size = 0;
  r.bytes = 100;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kZeroBytes);
}

void ASizeThatDoesNotFitTheEnginesWordIsRefused() {
  nc::RecordFields r;
  r.align = 16;
  r.bytes = 100;
  r.size = nc::kFormatMaxObjectBytes + 1;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kSizeRange);
  r.size = 100;
  r.bytes = nc::kFormatMaxObjectBytes + 1;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kSizeRange);
}

void AnObjectLargerThanTheDeviceCanGiveIsRefused() {
  nc::RecordFields r;
  r.align = 16;
  r.size = 100;
  r.bytes = 1000000;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kNone);
  r.bytes = 1000001;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kObjectTooLarge);
}

void TheRulesAreCheckedInTheDocumentedOrder() {
  nc::RecordFields r;  // everything wrong: zero bytes is reported first, then range, then alignment, then size
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kZeroBytes);
  r.size = nc::kFormatMaxObjectBytes + 1;
  r.bytes = 5;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kSizeRange);
  r.size = 5;
  QCHECK(nc::CheckRecord(r, TestLimits()) == nc::Rule::kAlignment);
}

// ── duplicate key ────────────────────────────────────────────────────────────────────────────────────────────────────────
void ASecondEntryWithOneKeyIsADuplicate() {
  nc::KeySet keys;
  QCHECK(!keys.Add("android-arm64", 1, 2));
  QCHECK(keys.Add("android-arm64", 1, 2));
  QCHECK(!keys.Add("android-arm64", 1, 3));
  QCHECK(!keys.Add("android-arm64", 2, 2));
  QCHECK(!keys.Add("windows-x86_64", 1, 2));  // another platform's key is another key
  QCHECK(keys.Add("windows-x86_64", 1, 2));
}

}  // namespace

int main() {
  TheRuleNamesAreTheLoggedWords();
  AFrameThatDoesNotStartWithTheZstdMagicIsRefused();
  AFrameTooShortToHoldItsHeaderIsNotAFrame();
  TheHeadersContentSizeMustEqualTheDeclaredSize();
  AHeaderWithNoContentSizeIsRefused();
  TheHeaderIsReadPastAWindowDescriptorAndADictionaryId();
  TheLongestHeaderIsEighteenBytes();
  AnAlignmentIsAPowerOfTwoOfAtMost4096();
  ARecordOfZeroBytesIsNotAnEntry();
  ASizeThatDoesNotFitTheEnginesWordIsRefused();
  AnObjectLargerThanTheDeviceCanGiveIsRefused();
  TheRulesAreCheckedInTheDocumentedOrder();
  ASecondEntryWithOneKeyIsADuplicate();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_validate_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_validate_test: all checks passed\n");
  return 0;
}
