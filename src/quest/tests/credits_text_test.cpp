// Host test for the Quest credits patch's pure encoding (sentinel/credits_text.h, #970). It checks the arithmetic
// against the pinned words: encoding the blob's address at the pinned sites must give the words libr15 holds. A
// wrong sign, shift or register in the encoder changes one of those words and fails here.
#include <cstdint>
#include <cstdio>

#include "../sentinel/credits_text.h"
#include "test_check.h"

namespace {

using nevr_quest::credits_text::EncodeAddImm12;
using nevr_quest::credits_text::EncodeAdrp;
using nevr_quest::credits_text::kBlobVaddr;
using nevr_quest::credits_text::kDisabledSite;
using nevr_quest::credits_text::kInitSite;
using nevr_quest::credits_text::PairHoldsPinned;
using nevr_quest::credits_text::PairWords;
using nevr_quest::credits_text::PlanPair;

// Image bases are page-aligned; the ADRP page delta between the sites and the blob does not depend on the base.
constexpr std::uint64_t kBases[] = {0x7000000000ULL, 0x7a00000000ULL, 0x7c12340000ULL};

void TestPinnedWordsEncodeTheBlob() {
  for (const std::uint64_t base : kBases) {
    PairWords init{};
    PairWords disabled{};
    QCHECK(PlanPair(kInitSite, base, base + kBlobVaddr, &init));
    QCHECK(init.adr == kInitSite.adrpExpected);
    QCHECK(init.add == kInitSite.addExpected);
    QCHECK(PlanPair(kDisabledSite, base, base + kBlobVaddr, &disabled));
    QCHECK(disabled.adr == kDisabledSite.adrpExpected);
    QCHECK(disabled.add == kDisabledSite.addExpected);
  }
}

void TestAddImm12Encoding() {
  // add x8, x8, #0xde1 and add x12, x12, #0xde1, from the disassembly of libr15 at ReVault 0x1ea1290 and 0x1ea1d88.
  QCHECK(EncodeAddImm12(8, 8, 0xde1) == 0x91378508u);
  QCHECK(EncodeAddImm12(12, 12, 0xde1) == 0x9137858cu);
}

void TestAdrpReachIsFourGiB() {
  const std::uint64_t site = 0x7000001000ULL;
  std::uint32_t word = 0;
  // The largest and smallest page deltas the 21-bit immediate holds.
  QCHECK(EncodeAdrp(site, site + ((std::uint64_t{1} << 20) - 1) * 4096, 0, &word));
  QCHECK(EncodeAdrp(site, site - (std::uint64_t{1} << 20) * 4096, 0, &word));
  QCHECK(!EncodeAdrp(site, site + (std::uint64_t{1} << 20) * 4096, 0, &word));
  QCHECK(!EncodeAdrp(site, site - ((std::uint64_t{1} << 20) + 1) * 4096, 0, &word));
}

void TestOutOfReachIsRefused() {
  PairWords out{};
  const std::uint64_t base = 0x7000000000ULL;
  QCHECK(!PlanPair(kInitSite, base, base + (std::uint64_t{5} << 32), &out));
}

void TestAnyWrongWordIsNotPinned() {
  QCHECK(PairHoldsPinned(kInitSite, kInitSite.adrpExpected, kInitSite.addExpected));
  QCHECK(!PairHoldsPinned(kInitSite, kInitSite.adrpExpected ^ 1u, kInitSite.addExpected));
  QCHECK(!PairHoldsPinned(kInitSite, kInitSite.adrpExpected, kInitSite.addExpected ^ 1u));
  QCHECK(!PairHoldsPinned(kDisabledSite, kInitSite.adrpExpected, kInitSite.addExpected));
}

}  // namespace

int main() {
  TestPinnedWordsEncodeTheBlob();
  TestAddImm12Encoding();
  TestAdrpReachIsFourGiB();
  TestOutOfReachIsRefused();
  TestAnyWrongWordIsNotPinned();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "credits_text_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("credits_text_test: all checks pass\n");
  return 0;
}
