// Host test for the variant stamp the sentinel reads before its first log line (#2170, docs/adr/0040 rule 1, docs/adr/0046 rule 5):
// `libnevr_build_variant.so` beside the sentinel's own library holds the variant word as its exact bytes, written by
// tools/quest_repack_libs.py write-build-variant (empty for a plain release). What it pins: each of the three words reads as its
// variant, an empty file reads as no variant (a plain release, from the stamp), a missing file reads as absent, and everything else
// (a differently cased or padded word, an unknown word, a file too long, a link, a directory) reads as no variant with the reason
// that says why, never as a variant. The stamp's file name equals the repack's.
//
// Built and run by `just test-quest-hooks`.

#include "quest/sentinel/build_variant.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "quest/tests/test_check.h"

namespace {

namespace bv = nevr_quest::build_variant;

bool Is(const bv::Stamp& stamp, bv::Variant variant, bv::StampSource source, const char* why = "") {
  return stamp.variant == variant && stamp.source == source && std::strcmp(stamp.why, why) == 0;
}

void TheStampFileIsNamedAsTheRepackNamesIt() {
  QCHECK(std::strcmp(bv::kStampFile, "libnevr_build_variant.so") == 0);  // tools/quest_repack_libs.py VARIANT_STAMP_NAME
}

void TheThreeWordsReadAsTheirVariants() {
  QCHECK(Is(bv::ParseStamp("q15t", 4), bv::Variant::kQ15t, bv::StampSource::kStamp));
  QCHECK(Is(bv::ParseStamp("combat", 6), bv::Variant::kCombat, bv::StampSource::kStamp));
  QCHECK(Is(bv::ParseStamp("perf-probe", 10), bv::Variant::kPerfProbe, bv::StampSource::kStamp));
}

void AnEmptyStampIsAPlainReleaseNamedByTheStamp() {
  QCHECK(Is(bv::ParseStamp("", 0), bv::Variant::kNone, bv::StampSource::kStamp));
  QCHECK(Is(bv::ParseStamp(nullptr, 0), bv::Variant::kNone, bv::StampSource::kStamp));
}

void AnythingElseIsNoVariantWithItsReason() {
  struct Case {
    const char* bytes;
    std::size_t size;
    const char* why;
  };
  const Case cases[] = {
      {"bogus", 5, "unknown_word"},        {"Q15T", 4, "unknown_word"},   {"Combat", 6, "unknown_word"},
      {" combat", 7, "unknown_word"},      {"combat\n", 7, "unknown_word"}, {"q15t ", 5, "unknown_word"},
      {"perf_probe", 10, "unknown_word"},  {"combat\0", 7, "unknown_word"}, {"q15", 3, "unknown_word"},
      {"q15tq15t", 8, "unknown_word"},     {"release", 7, "unknown_word"},  {"dev", 3, "unknown_word"},
  };
  for (const Case& c : cases) {
    const bv::Stamp stamp = bv::ParseStamp(c.bytes, c.size);
    QCHECK(Is(stamp, bv::Variant::kNone, bv::StampSource::kInvalid, c.why));
    QCHECK(!bv::RunsFoldedCombat(stamp.variant));
  }
  const std::string longWord(bv::kMaxStampBytes + 1, 'q');
  QCHECK(Is(bv::ParseStamp(longWord.data(), longWord.size()), bv::Variant::kNone, bv::StampSource::kInvalid, "too_long"));
  QCHECK(Is(bv::ParseStamp(nullptr, 5), bv::Variant::kNone, bv::StampSource::kInvalid, "unknown_word"));
}

void TheNamesAreTheWordsAndNoneForNoVariant() {
  QCHECK(std::strcmp(bv::VariantName(bv::Variant::kNone), "none") == 0);
  QCHECK(std::strcmp(bv::VariantName(bv::Variant::kQ15t), "q15t") == 0);
  QCHECK(std::strcmp(bv::VariantName(bv::Variant::kCombat), "combat") == 0);
  QCHECK(std::strcmp(bv::VariantName(bv::Variant::kPerfProbe), "perf-probe") == 0);
  QCHECK(std::strcmp(bv::SourceName(bv::StampSource::kStamp), "stamp") == 0);
  QCHECK(std::strcmp(bv::SourceName(bv::StampSource::kAbsent), "absent") == 0);
  QCHECK(std::strcmp(bv::SourceName(bv::StampSource::kUnreadable), "unreadable") == 0);
  QCHECK(std::strcmp(bv::SourceName(bv::StampSource::kInvalid), "invalid") == 0);
}

// Only the q15t and combat variants run the folded combat modules (docs/adr/0046 rule 5); a plain release and the measurement build do not.
void OnlyTheQ15tAndCombatVariantsRunFoldedCombat() {
  QCHECK(!bv::RunsFoldedCombat(bv::Variant::kNone));
  QCHECK(bv::RunsFoldedCombat(bv::Variant::kQ15t));
  QCHECK(bv::RunsFoldedCombat(bv::Variant::kCombat));
  QCHECK(!bv::RunsFoldedCombat(bv::Variant::kPerfProbe));
}

// ---- the file read ----------------------------------------------------------------------------------------------------

std::string MakeDirectory() {
  char path[] = "/tmp/nevr-build-variant-XXXXXX";
  const char* made = ::mkdtemp(path);
  return made != nullptr ? std::string(made) : std::string();
}

void Write(const std::string& file, const std::string& bytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void TheFileIsReadBesideTheLibrary() {
  const std::string dir = MakeDirectory();
  QCHECK(!dir.empty());
  if (dir.empty()) return;
  const std::string stamp = dir + "/" + bv::kStampFile;

  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kAbsent));  // no file: an APK the recipe did not stamp

  for (const char* word : {"q15t", "combat", "perf-probe"}) {
    Write(stamp, word);
    const bv::Stamp read = bv::ReadStamp(dir.c_str());
    QCHECK(read.source == bv::StampSource::kStamp);
    QCHECK(std::strcmp(bv::VariantName(read.variant), word) == 0);
  }
  Write(stamp, "");
  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kStamp));  // the plain release's stamp
  Write(stamp, "bogus");
  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kInvalid, "unknown_word"));
  Write(stamp, "combat\n");
  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kInvalid, "unknown_word"));
  Write(stamp, std::string(4096, 'c'));
  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kInvalid, "too_long"));
  ::unlink(stamp.c_str());

  // a link is not followed: the stamp is the file the installer extracted, never something it points at
  Write(dir + "/target", "combat");
  QCHECK(::symlink((dir + "/target").c_str(), stamp.c_str()) == 0);
  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kUnreadable, "symlink"));
  ::unlink(stamp.c_str());
  ::unlink((dir + "/target").c_str());

  // a directory of the name is no file
  QCHECK(::mkdir(stamp.c_str(), 0700) == 0);
  QCHECK(Is(bv::ReadStamp(dir.c_str()), bv::Variant::kNone, bv::StampSource::kUnreadable, "not_regular_file"));
  ::rmdir(stamp.c_str());
  ::rmdir(dir.c_str());

  // no directory to look in
  QCHECK(Is(bv::ReadStamp(nullptr), bv::Variant::kNone, bv::StampSource::kUnreadable, "no_library_dir"));
  QCHECK(Is(bv::ReadStamp(""), bv::Variant::kNone, bv::StampSource::kUnreadable, "no_library_dir"));
}

}  // namespace

int main() {
  TheStampFileIsNamedAsTheRepackNamesIt();
  TheThreeWordsReadAsTheirVariants();
  AnEmptyStampIsAPlainReleaseNamedByTheStamp();
  AnythingElseIsNoVariantWithItsReason();
  TheNamesAreTheWordsAndNoneForNoVariant();
  OnlyTheQ15tAndCombatVariantsRunFoldedCombat();
  TheFileIsReadBesideTheLibrary();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "build_variant_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("build_variant_test: all checks passed");
  return 0;
}
