// The equipment station's icon keys (runtime/combat/station_icon_keys.h): two generated tables that VeilNet searches with
// std::binary_search (kStationIconKeys) and std::lower_bound (kGearElements). Both searches are correct only on a table
// that is sorted ascending with no key twice, so these tests pin that, the lookups the runtime makes on the tables, and
// the closed set of gear kinds. A table regenerated out of order, with a duplicate, or with a kind outside the three
// VeilNet handles fails here, not in a match.
//
// NOT shown by these tests: the keys' meaning (that each is a station element's name hash), which only the game's
// scripts show.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>

#include "runtime/combat/station_icon_keys.h"

namespace {

constexpr uint64_t kBelowEveryKey = 0;
constexpr uint64_t kAboveEveryKey = UINT64_MAX;
constexpr uint64_t kNextValue = 1;
constexpr int kKindTacticalMod = 0;
constexpr int kKindOrdnance = 1;
constexpr int kKindWeapon = 2;
constexpr int kNotAGearElement = -1;
constexpr size_t kNone = 0;

// The lookups of runtime/combat/veilnet.cpp (StationIcon and the gear-kind lookup), written the same way.
bool StationIcon(uint64_t key) {
  return std::binary_search(std::begin(kStationIconKeys), std::end(kStationIconKeys), key);
}

int GearKind(uint64_t key) {
  auto it = std::lower_bound(std::begin(kGearElements), std::end(kGearElements), key,
                             [](const GearElement& e, uint64_t k) { return e.key < k; });
  return it != std::end(kGearElements) && it->key == key ? it->kind : kNotAGearElement;
}

}  // namespace

TEST(StationIconKeys, TablesAreNotEmpty) {
  EXPECT_GT(std::size(kStationIconKeys), kNone);
  EXPECT_GT(std::size(kGearElements), kNone);
}

TEST(StationIconKeys, IconKeysAreStrictlyAscending) {
  for (size_t i = 1; i < std::size(kStationIconKeys); ++i) {
    EXPECT_LT(kStationIconKeys[i - 1], kStationIconKeys[i]) << "index " << i;
  }
}

TEST(StationIconKeys, GearKeysAreStrictlyAscending) {
  for (size_t i = 1; i < std::size(kGearElements); ++i) {
    EXPECT_LT(kGearElements[i - 1].key, kGearElements[i].key) << "index " << i;
  }
}

TEST(StationIconKeys, IconKeysHoldNoDuplicates) {
  const std::set<uint64_t> unique(std::begin(kStationIconKeys), std::end(kStationIconKeys));
  EXPECT_EQ(unique.size(), std::size(kStationIconKeys));
}

TEST(StationIconKeys, NoTableHoldsTheZeroKey) {
  EXPECT_FALSE(StationIcon(kBelowEveryKey));
  EXPECT_NE(kStationIconKeys[0], kBelowEveryKey);
  EXPECT_NE(kGearElements[0].key, kBelowEveryKey);
}

TEST(StationIconKeys, EveryIconKeyIsFoundByTheRuntimesSearch) {
  for (const uint64_t key : kStationIconKeys) {
    EXPECT_TRUE(StationIcon(key)) << std::hex << key;
  }
}

TEST(StationIconKeys, KeysOutsideTheTableAreRefused) {
  const std::set<uint64_t> present(std::begin(kStationIconKeys), std::end(kStationIconKeys));
  EXPECT_FALSE(StationIcon(kBelowEveryKey));
  EXPECT_FALSE(StationIcon(kAboveEveryKey));
  for (const uint64_t key : kStationIconKeys) {
    const uint64_t neighbour = key + kNextValue;
    if (present.count(neighbour) == 0) {
      EXPECT_FALSE(StationIcon(neighbour)) << std::hex << neighbour;
    }
  }
}

TEST(StationIconKeys, GearKindsAreOneOfTheThreeVeilNetHandles) {
  for (const GearElement& e : kGearElements) {
    EXPECT_TRUE(e.kind == kKindTacticalMod || e.kind == kKindOrdnance || e.kind == kKindWeapon)
        << std::hex << e.key << " kind " << std::dec << e.kind;
  }
}

TEST(StationIconKeys, EveryGearKindHasAnElement) {
  const auto count = [](int kind) {
    return std::count_if(std::begin(kGearElements), std::end(kGearElements),
                         [kind](const GearElement& e) { return e.kind == kind; });
  };
  EXPECT_GT(count(kKindTacticalMod), 0);
  EXPECT_GT(count(kKindOrdnance), 0);
  EXPECT_GT(count(kKindWeapon), 0);
}

TEST(StationIconKeys, EveryGearElementIsFoundWithItsOwnKind) {
  for (const GearElement& e : kGearElements) {
    EXPECT_EQ(GearKind(e.key), e.kind) << std::hex << e.key;
  }
}

TEST(StationIconKeys, GearKeysOutsideTheTableAreRefused) {
  EXPECT_EQ(GearKind(kBelowEveryKey), kNotAGearElement);
  EXPECT_EQ(GearKind(kAboveEveryKey), kNotAGearElement);
  EXPECT_EQ(GearKind(kGearElements[0].key + kNextValue), kNotAGearElement);
}
