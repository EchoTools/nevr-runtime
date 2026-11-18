// The game-log symbol resolver (runtime/log/symcache.cpp): ResolveSymbolHashes rewrites each 0x<16 hex digits> in a
// game log line to the symbol's name when the embedded cache knows it, and AnnotateGameLine prefixes the result with
// [EVR]. The test supplies its own table in place of the generated log/symcache_data.cpp, so each case names the hash
// it expects to be resolved.

#include <gtest/gtest.h>

#include <string>

#include "runtime/log/symcache.h"

namespace evr {

// The table the resolver loads. Defined here, in place of the generated symcache_data.cpp.
constexpr uint64_t kHashLobby = 0x00000000000000ffULL;
constexpr uint64_t kHashArena = 0x0123456789abcdefULL;
constexpr uint64_t kHashLooksLikeAHash = 0x0000000000000001ULL;
constexpr uint64_t kHashUpper = 0xABCDEF0123456789ULL;
// What a parser that did not reject the non-hex digit 'g' would compute for "0x00000000000000fg" (0xf0 | ('g' - 'A' + 10), which is 0xf0).
constexpr uint64_t kHashOfAMisparsedG = 0x00000000000000f0ULL;

extern const SymCacheEntry kSymCacheData[] = {
    {kHashLobby, "lobby"},
    {kHashArena, "arena"},
    {kHashLooksLikeAHash, "0x00000000000000ff"},
    {kHashUpper, "upper"},
    {kHashOfAMisparsedG, "misparsed"},
};
extern const int kSymCacheCount = static_cast<int>(sizeof(kSymCacheData) / sizeof(kSymCacheData[0]));

}  // namespace evr

namespace {

// Runs first (gtest runs a suite's tests in definition order): the cache is process-wide and loads once, so the
// not-loaded behaviour can only be seen before any other test loads it.
TEST(SymCache, ResolvesNothingUntilTheCacheIsLoaded) {
  const std::string line = "event 0x00000000000000ff end";
  EXPECT_EQ(evr::ResolveSymbolHashes(line), line);
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes(line), "event lobby end");
  // A second call is a no-op: it neither fails nor changes what resolves.
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes(line), "event lobby end");
}

TEST(SymCache, ReplacesAKnownHashAndLeavesTheTextAroundItAlone) {
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes("loading 0x0123456789abcdef now"), "loading arena now");
  EXPECT_EQ(evr::ResolveSymbolHashes("[0x0123456789abcdef]"), "[arena]");
  EXPECT_EQ(evr::ResolveSymbolHashes("a=0x0123456789abcdef,b=0x00000000000000ff"), "a=arena,b=lobby");
}

TEST(SymCache, ResolvesAHashAtTheStartAndAtTheEndOfTheLine) {
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0123456789abcdef tail"), "arena tail");
  EXPECT_EQ(evr::ResolveSymbolHashes("head 0x0123456789abcdef"), "head arena");
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0123456789abcdef"), "arena");
}

TEST(SymCache, AcceptsAnUpperCasePrefixAndUpperCaseDigits) {
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes("x 0XABCDEF0123456789 y"), "x upper y");
  EXPECT_EQ(evr::ResolveSymbolHashes("x 0xabcdef0123456789 y"), "x upper y");
}

TEST(SymCache, ResolvesAdjacentHashesIndependently) {
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0123456789abcdef0x00000000000000ff"), "arenalobby");
}

TEST(SymCache, LeavesAnUnknownHashUnchangedAndStillResolvesTheKnownOnesAroundIt) {
  evr::InitSymbolCache();
  EXPECT_EQ(evr::ResolveSymbolHashes("0x1111111111111111"), "0x1111111111111111");
  EXPECT_EQ(evr::ResolveSymbolHashes("0x1111111111111111 0x00000000000000ff"), "0x1111111111111111 lobby");
}

TEST(SymCache, DoesNotResolveAnythingThatIsNotSixteenHexDigitsAfterThePrefix) {
  evr::InitSymbolCache();
  // Fifteen digits: one short.
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0123456789abcde"), "0x0123456789abcde");
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0123456789abcde tail"), "0x0123456789abcde tail");
  // A non-hex digit inside the sixteen.
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0123456789abcdeg"), "0x0123456789abcdeg");
  EXPECT_EQ(evr::ResolveSymbolHashes("0x00000000000000fg"), "0x00000000000000fg");
  // No prefix: sixteen bare digits are not a hash.
  EXPECT_EQ(evr::ResolveSymbolHashes("0123456789abcdef"), "0123456789abcdef");
  // Too short a line to hold a hash, and an empty line.
  EXPECT_EQ(evr::ResolveSymbolHashes("0x"), "0x");
  EXPECT_EQ(evr::ResolveSymbolHashes(""), "");
}

TEST(SymCache, DoesNotResolveAgainWhatAReplacementSpellsLikeAHash) {
  evr::InitSymbolCache();
  // kHashLooksLikeAHash's name is the text of kHashLobby; it is written out as it is, not resolved a second time.
  EXPECT_EQ(evr::ResolveSymbolHashes("0x0000000000000001"), "0x00000000000000ff");
}

TEST(SymCache, AnnotateGameLinePrefixesTheLineAndResolvesItsHashes) {
  evr::InitSymbolCache();
  EXPECT_EQ(evr::AnnotateGameLine("[INFO] joined 0x00000000000000ff"), "[EVR] [INFO] joined lobby");
  EXPECT_EQ(evr::AnnotateGameLine("no hashes here"), "[EVR] no hashes here");
  EXPECT_EQ(evr::AnnotateGameLine(""), "[EVR] ");
}

}  // namespace
