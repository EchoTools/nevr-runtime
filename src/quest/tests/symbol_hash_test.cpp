// CSymbol64Hash on bytes >= 0x80, against the game's rule (#657).
//
// The game sign-extends each byte before the table XOR (echovr.exe 0x1400ce199 MOVSX; libr15.so 0x11cc760
// sxtb). Plain `char` is signed under MinGW and the host g++ and unsigned under the NDK's AArch64 clang, so a
// hash that casts `char` directly differs by target. The vectors are checked at compile time (static_assert),
// so the NDK compile of this file is the Quest-side proof, and again at run time by main().
//
// Build: host g++ (test-quest-shared) and the NDK's aarch64-linux-android clang++ (test-android).

#include <cstdint>
#include <cstdio>

#include "abi/symbol_hash.h"

namespace {

using EchoVR::CSymbol64Hash;
using EchoVR::kCSymbol64Seed;

constexpr uint64_t kMainMenu = 0x1509ee27440e1d7bULL;
constexpr uint64_t kCafe = 0xcd0714df62754f5fULL;      // "caf\xc3\xa9", UTF-8 "cafe" with acute e
constexpr uint64_t kFfFe = 0x1f17d45f0697bb92ULL;      // "\xff\xfe"

static_assert(CSymbol64Hash("main_menu") == kMainMenu, "ASCII hash");
static_assert(CSymbol64Hash("MAIN_MENU") == kMainMenu, "A-Z folds to a-z");
static_assert(CSymbol64Hash("") == kCSymbol64Seed, "empty string returns the seed");
static_assert(CSymbol64Hash(nullptr) == kCSymbol64Seed, "null returns the seed");
static_assert(CSymbol64Hash("caf\xc3\xa9") == kCafe, "byte >= 0x80 is sign-extended, as the game does");
static_assert(CSymbol64Hash("\xff\xfe") == kFfFe, "byte >= 0x80 is sign-extended, as the game does");

struct Row {
  const char* name;
  uint64_t want;
};

}  // namespace

int main() {
  const Row rows[] = {
      {"main_menu", kMainMenu}, {"MAIN_MENU", kMainMenu}, {"", kCSymbol64Seed},
      {"caf\xc3\xa9", kCafe},   {"\xff\xfe", kFfFe},
  };
  int bad = 0;
  for (const Row& r : rows) {
    const uint64_t got = CSymbol64Hash(r.name);
    if (got != r.want) {
      std::fprintf(stderr, "FAIL CSymbol64Hash: got 0x%016llx want 0x%016llx\n",
                   static_cast<unsigned long long>(got), static_cast<unsigned long long>(r.want));
      ++bad;
    }
  }
  if (bad != 0) return 1;
  std::printf("symbol_hash_test: %zu vectors pass\n", sizeof(rows) / sizeof(rows[0]));
  return 0;
}
