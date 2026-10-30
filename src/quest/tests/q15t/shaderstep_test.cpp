// Host test of shaderstep's step correction (src/quest/plugins/q15t/src/shaderstep.cpp, issue #1553). ApplyShaderSetStep is the
// whole change the wrapper makes to a built object: on a local buffer laid out as the game's object (the step word at +0x98),
// it must write 1 only where the game wrote 0x20, and leave every other word alone.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shaderstep.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

namespace {

constexpr std::size_t kObjBytes = 0x100;  // covers the step at +0x98 and the words around it
constexpr std::size_t kStepOff = 0x98;

std::uint64_t ReadWord(const std::uint8_t* b, std::size_t off) {
  std::uint64_t v;
  std::memcpy(&v, b + off, sizeof v);
  return v;
}
void WriteWord(std::uint8_t* b, std::size_t off, std::uint64_t v) { std::memcpy(b + off, &v, sizeof v); }

}  // namespace

int main() {
  // 1. The game wrote 0x20 (ConstructInstance's own store): the wrapper writes 1, and reads it back.
  {
    alignas(8) std::uint8_t obj[kObjBytes];
    std::memset(obj, 0xab, sizeof obj);
    WriteWord(obj, kStepOff, 0x20);
    WriteWord(obj, kStepOff - 8, 0x1122334455667788ull);  // a neighbour word: must stay
    WriteWord(obj, kStepOff + 8, 0x99aabbccddeeff00ull);  // the capacity word after the step: must stay
    const q15t::StepFix r = q15t::ApplyShaderSetStep(obj);
    CHECK(r == q15t::StepFix::kSet, "stock 0x20 must give kSet, got %d", static_cast<int>(r));
    CHECK(ReadWord(obj, kStepOff) == 1, "step must read back 1, got 0x%llx", static_cast<unsigned long long>(ReadWord(obj, kStepOff)));
    CHECK(ReadWord(obj, kStepOff - 8) == 0x1122334455667788ull, "word before the step changed");
    CHECK(ReadWord(obj, kStepOff + 8) == 0x99aabbccddeeff00ull, "word after the step changed");
  }
  // 2. Any other word there is not the game's store: nothing is written, and the caller is told so.
  {
    alignas(8) std::uint8_t obj[kObjBytes];
    std::memset(obj, 0, sizeof obj);
    WriteWord(obj, kStepOff, 0x40);
    const q15t::StepFix r = q15t::ApplyShaderSetStep(obj);
    CHECK(r == q15t::StepFix::kNotStock, "a word other than 0x20 must give kNotStock, got %d", static_cast<int>(r));
    CHECK(ReadWord(obj, kStepOff) == 0x40, "a word other than 0x20 must be left alone");
  }
  // 3. A null object is unreadable: nothing is written anywhere.
  {
    const q15t::StepFix r = q15t::ApplyShaderSetStep(nullptr);
    CHECK(r == q15t::StepFix::kUnreadable, "null object must give kUnreadable, got %d", static_cast<int>(r));
  }
  if (g_fail != 0) {
    std::printf("shaderstep_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("shaderstep_test: all checks passed\n");
  return 0;
}
