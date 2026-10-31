// P27 (#1774): the -noovr argument-lookup decision (patch/cmdline_waiver.h). The waiver answers "found" for the
// spectatorstream lookup at the one pinned -noovr call site, and nowhere else.
//
// Mutants (each deletes or alters one line of the header; each fails a named test):
//   M1 kWaivers siteRva 0x116909 changed           -> NoOvrSiteIsWaivedForSpectatorStreamOnly, AbsentAtTheNoOvrSiteIsFound
//   M2 symbol compare dropped from Waives          -> NoOvrSiteIsWaivedForSpectatorStreamOnly
//   M3 `original == -1` guard dropped from Decide  -> FoundAtTheNoOvrSiteIsReturnedUntouched
//   M4 Waives always false                         -> AbsentAtTheNoOvrSiteIsFound

#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/patch/cmdline_waiver.h"

namespace {

using nevr_cmdline_waiver::Decide;
using nevr_cmdline_waiver::kSpectatorStreamSymbol;
using nevr_cmdline_waiver::Waives;

constexpr std::uint64_t kNoOvrSiteRva = 0x116909;       // the return address of the call at VA 0x140116904
constexpr std::uint64_t kSpectatorCheckSiteRva = 0x116F39;  // the spectator-stream check's own call
const std::uint64_t kNoOvrSymbol = EchoVR::CSymbol64Hash("noovr");

TEST(CmdlineWaiver, NoOvrSiteIsWaivedForSpectatorStreamOnly) {
  EXPECT_TRUE(Waives(kSpectatorStreamSymbol, kNoOvrSiteRva));
  EXPECT_FALSE(Waives(kNoOvrSymbol, kNoOvrSiteRva));
  EXPECT_FALSE(Waives(kSpectatorStreamSymbol, kSpectatorCheckSiteRva));
  EXPECT_FALSE(Waives(kNoOvrSymbol, kSpectatorCheckSiteRva));
}

TEST(CmdlineWaiver, SymbolIsTheGamesCaseInsensitiveHash) {
  EXPECT_EQ(kSpectatorStreamSymbol, EchoVR::CSymbol64Hash("SpectatorStream"));
  EXPECT_NE(kSpectatorStreamSymbol, EchoVR::CSymbol64Hash("spectatorstreams"));
}

TEST(CmdlineWaiver, AbsentAtTheNoOvrSiteIsFound) {
  const nevr_cmdline_waiver::Answer answer = Decide(-1, kSpectatorStreamSymbol, kNoOvrSiteRva);
  EXPECT_EQ(answer.value, 0) << "-1 at the -noovr site must read as found, so the -spectatorstream assert is skipped";
  EXPECT_TRUE(answer.waived);
}

TEST(CmdlineWaiver, FoundAtTheNoOvrSiteIsReturnedUntouched) {
  const nevr_cmdline_waiver::Answer answer = Decide(5, kSpectatorStreamSymbol, kNoOvrSiteRva);
  EXPECT_EQ(answer.value, 5);
  EXPECT_FALSE(answer.waived);
}

TEST(CmdlineWaiver, AbsentAtTheSpectatorCheckSiteStaysAbsent) {
  const nevr_cmdline_waiver::Answer answer = Decide(-1, kSpectatorStreamSymbol, kSpectatorCheckSiteRva);
  EXPECT_EQ(answer.value, -1);
  EXPECT_FALSE(answer.waived);
}

TEST(CmdlineWaiver, AbsentOtherSymbolAtTheNoOvrSiteStaysAbsent) {
  const nevr_cmdline_waiver::Answer answer = Decide(-1, kNoOvrSymbol, kNoOvrSiteRva);
  EXPECT_EQ(answer.value, -1);
  EXPECT_FALSE(answer.waived);
}

}  // namespace
