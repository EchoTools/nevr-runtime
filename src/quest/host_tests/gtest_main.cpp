// The entry point of the gtest programs the Quest host build runs (src/quest/host_tests/CMakeLists.txt, the GTEST flag of nevr_ht_test).
// The vcpkg gtest port installs no gtest_main archive, so each such program takes this main and links libgtest.a.
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
