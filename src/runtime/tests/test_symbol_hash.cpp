// CSymbol64Hash on bytes >= 0x80, with plain `char` unsigned (#657). This target is built with
// -funsigned-char (src/runtime/CMakeLists.txt), the signedness of the NDK's AArch64 clang, so the hash must
// sign-extend explicitly, as the game does (echovr.exe 0x1400ce199 MOVSX; libr15.so 0x11cc760 sxtb).

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>

#include "abi/symbol_hash.h"

static_assert(CHAR_MIN == 0, "this test must be built with -funsigned-char");

TEST(SymbolHashUnsignedChar, NonAsciiBytesAreSignExtended) {
  EXPECT_EQ(EchoVR::CSymbol64Hash("main_menu"), 0x1509ee27440e1d7bULL);
  EXPECT_EQ(EchoVR::CSymbol64Hash("MAIN_MENU"), 0x1509ee27440e1d7bULL);
  EXPECT_EQ(EchoVR::CSymbol64Hash(""), EchoVR::kCSymbol64Seed);
  EXPECT_EQ(EchoVR::CSymbol64Hash(nullptr), EchoVR::kCSymbol64Seed);
  EXPECT_EQ(EchoVR::CSymbol64Hash("caf\xc3\xa9"), 0xcd0714df62754f5fULL);
  EXPECT_EQ(EchoVR::CSymbol64Hash("\xff\xfe"), 0x1f17d45f0697bb92ULL);
}
