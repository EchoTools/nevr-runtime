#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "runtime/patch/broadcaster_pointer_guard.h"

TEST(BroadcasterPointerGuard, RejectsLowPointersBeforeReadingAndAcceptsValidChain) {
  std::int64_t table = 0x1234;
  EXPECT_FALSE(nevr_broadcaster_guard::TryReadListenerTable(0x10, table));
  EXPECT_EQ(table, 0x1234U);

  std::uintptr_t outer = 0x10;
  EXPECT_FALSE(nevr_broadcaster_guard::TryReadListenerTable(
      reinterpret_cast<std::uintptr_t>(&outer), table));
  EXPECT_EQ(table, 0x1234U);

  alignas(std::uintptr_t) std::array<std::uint8_t, 0x5e8> inner = {};
  const std::int64_t listenerTable = reinterpret_cast<std::uintptr_t>(&outer);
  *reinterpret_cast<std::int64_t*>(inner.data() + 0x5e0) = listenerTable;
  outer = reinterpret_cast<std::uintptr_t>(inner.data());
  EXPECT_TRUE(nevr_broadcaster_guard::TryReadListenerTable(
      reinterpret_cast<std::uintptr_t>(&outer), table));
  EXPECT_EQ(table, listenerTable);

  *reinterpret_cast<std::int64_t*>(inner.data() + 0x5e0) = -1;
  EXPECT_TRUE(nevr_broadcaster_guard::TryReadListenerTable(
      reinterpret_cast<std::uintptr_t>(&outer), table));
  EXPECT_EQ(table, -1);
}
