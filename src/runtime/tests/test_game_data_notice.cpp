#include "runtime/lifecycle/game_data_notice.h"

#include <gtest/gtest.h>

namespace {

TEST(GameDataNotice, DialogOnlyForAClientWithAScreen) {
  EXPECT_TRUE(nevr_game_data::DialogDue(false, false));
  EXPECT_FALSE(nevr_game_data::DialogDue(true, false));
  EXPECT_FALSE(nevr_game_data::DialogDue(false, true));
  EXPECT_FALSE(nevr_game_data::DialogDue(true, true));
}

TEST(GameDataNotice, TextNamesTheFolderAndTheFix) {
  const std::string text = nevr_game_data::Text("C:\\echovr\\_data");
  EXPECT_NE(text.find("C:\\echovr\\_data"), std::string::npos);
  EXPECT_NE(text.find("Echo Launcher"), std::string::npos);
  EXPECT_NE(text.find("Verify Files"), std::string::npos);
}

}  // namespace
