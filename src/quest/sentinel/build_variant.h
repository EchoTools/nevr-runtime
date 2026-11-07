// The variant of the APK this process runs (docs/adr/0040-every-build-names-its-variant.md rule 1, docs/adr/0046 rule 5).
//
// Where it comes from. The repack (`just android-repack-apk`, tools/quest_repack_libs.py write-build-variant) writes
// `lib/arm64-v8a/libnevr_build_variant.so` into every APK it makes: the variant word as the file's exact bytes, empty for a plain
// release or a development build of the tree. Its name is a lib*.so name so the installer extracts it beside the sentinel, as it
// does the plugin manifest (ADR 0019 section 2). The sentinel reads it from the directory its own library was loaded from, before
// its first log line, so the first line names the variant and nothing is read from the game or the configuration to learn it.
//
// The words. `q15t` and `combat` are the APKs that carry the q15t libraries and, with them, the folded combat modules
// (combat/combat_active.h); `perf-probe` is the measurement build (ADR 0037), which carries no combat; `arena` is the release's APK built from the store APK, which
// carries no combat and no plugin (#2298). An empty file is a plain
// release. A word is compared exactly: a different case, a padded word, an unknown word or a file longer than kMaxStampBytes is
// never read as a variant; it reads as no variant, with `StampSource::kInvalid` and the reason, so a stamp that is wrong costs the
// variant's behaviour and says so, never the reverse. A missing file is `kAbsent`: every APK this sentinel ships in is stamped by the
// repack (which stamps unconditionally), so it means the stamp did not reach the library directory (a repack outside the recipe, or an
// install that did not extract it), a packaging or install fault. A file that cannot be read is `kUnreadable`; both read as no variant.
//
// Where it is read. Only from the directory of the extracted library (dladdr's path for the sentinel). A sentinel mapped straight from
// the APK (`<apk>!/lib/<abi>/<file>`, the form data_wiring.cpp's OwnApkPath handles; ADR 0019 section 1 leaves it unverified) has no
// such directory, and the stamp is not read from the APK: that case is `kUnreadable` with the reason `mapped_from_apk`, so it is named
// distinctly and never read as a missing file.
//
// Pure apart from ReadStamp's file read and ReadOwnStamp's dladdr; it allocates nothing, so the host test and the NDK build compile this
// one source and the hook path may call it. Never throws.
#pragma once

#include <cstddef>

namespace nevr_quest::build_variant {

// The stamp's file name in the library directory; tools/quest_repack_libs.py VARIANT_STAMP_NAME is the same string.
inline constexpr const char* kStampFile = "libnevr_build_variant.so";
// The longest word is `perf-probe` (10 bytes); a file past this is not a stamp.
inline constexpr std::size_t kMaxStampBytes = 32;

enum class Variant { kNone, kQ15t, kCombat, kPerfProbe, kArena };
enum class StampSource { kStamp, kAbsent, kUnreadable, kInvalid };

struct Stamp {
  Variant variant = Variant::kNone;
  StampSource source = StampSource::kAbsent;
  // A token that says why the stamp is not a variant's: no_file (kAbsent); unknown_word, too_long (kInvalid); symlink,
  // not_regular_file, open_failed, read_failed, no_library_dir, mapped_from_apk (kUnreadable); empty for a stamp that was read.
  const char* why = "";
};

// "none", "q15t", "combat", "perf-probe" or "arena": the value of the log's `variant` field.
const char* VariantName(Variant variant) noexcept;
// "stamp", "absent", "unreadable" or "invalid": the value of the log's `variant_source` field.
const char* SourceName(StampSource source) noexcept;

// True for the variants that carry the folded combat modules: `q15t` and `combat` (docs/adr/0046 rule 5).
constexpr bool RunsFoldedCombat(Variant variant) noexcept { return variant == Variant::kQ15t || variant == Variant::kCombat; }

// The stamp for the file's bytes. An empty file is a plain release read from the stamp (kNone, kStamp).
Stamp ParseStamp(const char* bytes, std::size_t size) noexcept;

// The directory of the library at `path` (dladdr's dli_fname for the sentinel) into dir[cap]: nullptr on success, else the reason token that
// goes in a kUnreadable stamp's `why`: `mapped_from_apk` for a path that names a file inside an APK (`<apk>!/lib/...`), `no_library_dir`
// for no path, no directory part, or a directory that does not fit.
const char* LibraryDirectoryOf(const char* path, char* dir, std::size_t cap) noexcept;

// The stamp in `directory`'s kStampFile. The file is opened without following a link and must be a regular file.
Stamp ReadStamp(const char* directory) noexcept;

// The stamp beside the library this code was loaded from (dladdr on this function). `kUnreadable` with `no_library_dir` when the loader
// cannot say which file that is.
Stamp ReadOwnStamp() noexcept;

}  // namespace nevr_quest::build_variant
