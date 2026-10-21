// #730: the game's return to lobby is only called when the call queue it pushes to exists.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"

namespace {

using nevr_return_to_lobby_queue::Check;
using nevr_return_to_lobby_queue::Name;
using nevr_return_to_lobby_queue::Verdict;
using nevr_patch_addresses::GAME_CALL_QUEUE_OFFSET;
using nevr_patch_addresses::NETGAME_GAME_OFFSET;

// A stand-in for the game's layout: net game -> (+8) game object -> (+8) queue whose first field is
// the queue storage the game's push reads.
struct FakeGame {
  unsigned char bytes[0x40] = {};
  void SetQueueStorage(const void* storage) {
    std::memcpy(bytes + GAME_CALL_QUEUE_OFFSET, &storage, sizeof(storage));
  }
};
struct FakeNetGame {
  unsigned char bytes[0x40] = {};
  void SetGame(const void* game) { std::memcpy(bytes + NETGAME_GAME_OFFSET, &game, sizeof(game)); }
};

TEST(ReturnToLobbyQueue, ReadyWhenTheQueueStorageExists) {
  FakeGame game;
  int storage = 0;
  game.SetQueueStorage(&storage);
  FakeNetGame netGame;
  netGame.SetGame(&game);
  EXPECT_EQ(Check(&netGame), Verdict::Ready);
}

TEST(ReturnToLobbyQueue, RefusedWhenTheQueueStorageIsNull) {
  FakeGame game;  // the queue's first field stays null: the game's push would read [0 + 0x80]
  FakeNetGame netGame;
  netGame.SetGame(&game);
  EXPECT_EQ(Check(&netGame), Verdict::QueueNotAllocated);
}

TEST(ReturnToLobbyQueue, RefusedWithoutAGameObject) {
  FakeNetGame netGame;
  EXPECT_EQ(Check(&netGame), Verdict::NoGameObject);
}

TEST(ReturnToLobbyQueue, RefusedWithoutANetGame) {
  EXPECT_EQ(Check(nullptr), Verdict::NoNetGame);
}

TEST(ReturnToLobbyQueue, EveryVerdictHasADistinctName) {
  EXPECT_STREQ(Name(Verdict::Ready), "ready");
  EXPECT_STREQ(Name(Verdict::NoNetGame), "no_net_game");
  EXPECT_STREQ(Name(Verdict::NoGameObject), "no_game_object");
  EXPECT_STREQ(Name(Verdict::QueueNotAllocated), "queue_not_allocated");
}

}  // namespace
