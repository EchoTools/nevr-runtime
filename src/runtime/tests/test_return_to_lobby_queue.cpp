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

// The runtime's g_pGame is the game CONTEXT; the game's function takes the net game, which the context
// holds at CONTEXT_NETGAME_OFFSET (GetNetGameFromContext, echovr.exe 0x140113a90: mov rax,[rcx+0x8518]).
struct FakeContext {
  unsigned char bytes[0x8600] = {};
  void SetNetGame(const void* netGame) {
    std::memcpy(bytes + nevr_patch_addresses::CONTEXT_NETGAME_OFFSET, &netGame, sizeof(netGame));
  }
};

TEST(ReturnToLobbyQueue, TheContextOffsetIsTheOneGetNetGameFromContextReads) {
  EXPECT_EQ(nevr_patch_addresses::CONTEXT_NETGAME_OFFSET, 0x8518u);
}

TEST(ReturnToLobbyQueue, TheNetGameIsTheOneTheContextHolds) {
  FakeNetGame netGame;
  FakeContext context;
  context.SetNetGame(&netGame);
  EXPECT_EQ(nevr_return_to_lobby_queue::NetGameFromContext(&context), &netGame);
  EXPECT_EQ(nevr_return_to_lobby_queue::NetGameFromContext(nullptr), nullptr);
}

// The failure #730 hit: the runtime handed the game the context. A context whose own [+8][+8] chain is
// empty is refused, while the net game it holds, with a queue, is ready: the check keys on the object the
// caller names, so a caller that passes the context is refused even when the net game is fine.
TEST(ReturnToLobbyQueue, PassingTheContextIsNotPassingTheNetGame) {
  FakeGame game;
  int storage = 0;
  game.SetQueueStorage(&storage);
  FakeNetGame netGame;
  netGame.SetGame(&game);
  FakeContext context;
  context.SetNetGame(&netGame);
  EXPECT_EQ(Check(nevr_return_to_lobby_queue::NetGameFromContext(&context)), Verdict::Ready);
  EXPECT_NE(Check(&context), Verdict::Ready);
}

TEST(ReturnToLobbyQueue, ANullNetGameInTheContextIsRefusedNotDereferenced) {
  FakeContext context;
  EXPECT_EQ(Check(nevr_return_to_lobby_queue::NetGameFromContext(&context)), Verdict::NoNetGame);
}

TEST(ReturnToLobbyQueue, EveryVerdictHasADistinctName) {
  EXPECT_STREQ(Name(Verdict::Ready), "ready");
  EXPECT_STREQ(Name(Verdict::NoNetGame), "no_net_game");
  EXPECT_STREQ(Name(Verdict::NoGameObject), "no_game_object");
  EXPECT_STREQ(Name(Verdict::QueueNotAllocated), "queue_not_allocated");
}

// #1756: the running-script count the game's return-to-lobby callback defers on (echovr.exe 0x1401a4ad0), read for the
// log line that says why a service-ended session did not leave.
TEST(ReturnToLobbyQueue, RunningScriptsReadsTheCountTheGameDefersOn) {
  struct BigGame {
    unsigned char bytes[0x2000] = {};
  } game;
  const uint64_t count = 3;
  std::memcpy(game.bytes + nevr_patch_addresses::GAME_SCRIPT_COUNT_OFFSET, &count, sizeof(count));
  FakeNetGame netGame;
  netGame.SetGame(&game);
  EXPECT_EQ(nevr_return_to_lobby_queue::RunningScripts(&netGame), 3U);
  EXPECT_EQ(nevr_patch_addresses::GAME_SCRIPT_COUNT_OFFSET, 0x1F10U);
}

TEST(ReturnToLobbyQueue, RunningScriptsIsUnknownWithoutAGameObject) {
  FakeNetGame netGame;
  EXPECT_EQ(nevr_return_to_lobby_queue::RunningScripts(&netGame), UINT64_MAX);
  EXPECT_EQ(nevr_return_to_lobby_queue::RunningScripts(nullptr), UINT64_MAX);
}

}  // namespace
