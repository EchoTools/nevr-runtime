// instmodel: the replacement constructor sets the eight CTableXT growth steps to 1 after the stock constructor, and only where the stock
// value (0x20) is still there. The stock constructor is a fake here that writes what disassembly 0x1a078dc writes (the 0x20 steps, zeroed
// capacities and counts); the install path (the GOT word swap and the word pins) runs on a game copy, not here.
#include <cstdio>
#include <cstring>

#include "instmodel.h"

namespace {

int g_failures = 0;
#define CHECK(c, msg)                       \
  do {                                      \
    if (!(c)) {                             \
      std::fprintf(stderr, "FAIL %s\n", msg); \
      ++g_failures;                         \
    }                                       \
  } while (0)

constexpr unsigned long kMembers[] = {0x78, 0xb0, 0xe8, 0x120, 0x158, 0x190, 0x1c8, 0x200};
constexpr unsigned long kSteps[] = {0x98, 0xd0, 0x108, 0x140, 0x178, 0x1b0, 0x1e8, 0x220};
constexpr unsigned long kObjSize = 0x240;

alignas(16) unsigned char g_obj[kObjSize];
int g_real_calls = 0;
// What the stock constructor's stores leave behind in the fields this module reads: every step is 0x20.
unsigned long g_stock_step_value = 0x20;
unsigned long g_stock_step_index = ~0UL;  // when not ~0UL, that step gets g_stock_step_override instead of 0x20
unsigned long g_stock_step_override = 0x21;

void StockCtor(void* self, const void* init) {
  (void)init;
  ++g_real_calls;
  unsigned char* o = static_cast<unsigned char*>(self);
  std::memset(o, 0, kObjSize);
  for (std::size_t i = 0; i < sizeof kSteps / sizeof kSteps[0]; ++i) {
    unsigned long v = g_stock_step_value;
    if (i == g_stock_step_index) v = g_stock_step_override;
    std::memcpy(o + kSteps[i], &v, sizeof v);
  }
}

unsigned long Read(unsigned long off) {
  unsigned long v = 0;
  std::memcpy(&v, g_obj + off, sizeof v);
  return v;
}

void Reset() {
  g_real_calls = 0;
  g_stock_step_index = ~0UL;
  g_stock_step_override = 0x21;
  std::memset(g_obj, 0xAB, sizeof g_obj);
  q15t::ResetInstModelForTest();
  q15t::SetInstModelRealForTest(&StockCtor);
}

// The stock object, built by the stock constructor alone; the wrapper's result must equal it with the eight steps replaced by 1.
void StockObject(unsigned char* out) {
  std::memset(out, 0xAB, kObjSize);
  StockCtor(out, nullptr);
  g_real_calls = 0;
}

void TestAllStepsBecomeOne() {
  Reset();
  q15t::InstModelCtorWrapper(g_obj, nullptr);
  CHECK(g_real_calls == 1, "the stock constructor runs exactly once");
  for (unsigned long s : kSteps) CHECK(Read(s) == 1, "every growth step reads 1 after the wrapper");
}

void TestOnlyTheStepsChange() {
  Reset();
  unsigned char expect[kObjSize];
  StockObject(expect);
  for (unsigned long s : kSteps) {
    unsigned long one = 1;
    std::memcpy(expect + s, &one, sizeof one);
  }
  q15t::InstModelCtorWrapper(g_obj, nullptr);
  CHECK(std::memcmp(g_obj, expect, kObjSize) == 0, "the object is the stock object with the eight steps set to 1, nothing else");
  for (unsigned long m : kMembers) CHECK(Read(m + 0x28) == 0, "the capacity fields the stock constructor zeroes stay zero");
}

void TestMismatchLeftAlone() {
  Reset();
  g_stock_step_index = 6;  // the step at 0x1e8 holds 0x21 after the stock constructor
  q15t::InstModelCtorWrapper(g_obj, nullptr);
  CHECK(Read(0x1e8) == 0x21, "a step that does not hold the stock value is left as it is");
  for (std::size_t i = 0; i < sizeof kSteps / sizeof kSteps[0]; ++i)
    if (i != 6) CHECK(Read(kSteps[i]) == 1, "the other seven steps still become 1 when one does not match");
}

void TestNoStockValueChangesNothing() {
  Reset();
  g_stock_step_value = 0x40;  // no step holds the stock value: nothing is written
  q15t::InstModelCtorWrapper(g_obj, nullptr);
  g_stock_step_value = 0x20;
  for (unsigned long s : kSteps) CHECK(Read(s) == 0x40, "no step holds 0x20, so none is changed");
}

}  // namespace

int main() {
  TestAllStepsBecomeOne();
  TestOnlyTheStepsChange();
  TestMismatchLeftAlone();
  TestNoStockValueChangesNothing();
  q15t::ResetInstModelForTest();
  if (g_failures != 0) {
    std::fprintf(stderr, "instmodel_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("instmodel_test: OK\n");
  return 0;
}
