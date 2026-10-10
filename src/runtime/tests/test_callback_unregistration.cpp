#include "runtime/server/callback_unregistration.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {
EchoVR::Broadcaster* FakeBroadcaster(uintptr_t address) {
  return reinterpret_cast<EchoVR::Broadcaster*>(address);
}

void SetAllUdpHandles(GameServer::CallbackRegistry& callbacks) {
  callbacks.sessionStart = 1;
  callbacks.sessionError = 2;
  callbacks.saveLoadout = 3;
  callbacks.saveLoadoutSuccess = 4;
  callbacks.saveLoadoutPartial = 5;
  callbacks.currentLoadoutRequest = 6;
  callbacks.currentLoadoutResponse = 7;
  callbacks.refreshProfileForUser = 8;
  callbacks.refreshProfileFromServer = 9;
  callbacks.lobbySendClientSettings = 10;
  callbacks.tierReward = 11;
  callbacks.topAwards = 12;
  callbacks.newUnlocks = 13;
  callbacks.reliableStatUpdate = 14;
  callbacks.reliableTeamStatUpdate = 15;
  callbacks.tcpRegSuccess = 1;
  callbacks.tcpRegFailure = 1;
  callbacks.tcpSessionSuccess = 1;
  callbacks.tcpProtobuf = 1;
}
}  // namespace

TEST(CallbackRegistrationCount, CountsAndNamesTheCallbacksWithoutAHandle) {
  GameServer::CallbackRegistry callbacks;
  EXPECT_EQ(GameServer::CountRegisteredBroadcasterCallbacks(callbacks), 0U);
  EXPECT_EQ(GameServer::kBroadcasterCallbackCount, 15U);

  SetAllUdpHandles(callbacks);
  EXPECT_EQ(GameServer::CountRegisteredBroadcasterCallbacks(callbacks), GameServer::kBroadcasterCallbackCount);
  EXPECT_EQ(GameServer::MissingBroadcasterCallbacks(callbacks), "");

  callbacks.saveLoadoutPartial = 0;
  callbacks.reliableTeamStatUpdate = 0;
  EXPECT_EQ(GameServer::CountRegisteredBroadcasterCallbacks(callbacks), 13U);
  EXPECT_EQ(GameServer::MissingBroadcasterCallbacks(callbacks), "saveLoadoutPartial, reliableTeamStatUpdate");
}

// The game's BroadcasterListen returns 0xFFFF when it has no free listener slot. That is not a
// handle: it is neither counted as registered nor ever passed to BroadcasterUnlisten.
TEST(CallbackRegistrationCount, TheGamesListenFailureValueIsNotARegistration) {
  GameServer::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = FakeBroadcaster(0x1000);
  SetAllUdpHandles(callbacks);
  callbacks.newUnlocks = 0xFFFF;
  EXPECT_EQ(GameServer::CountRegisteredBroadcasterCallbacks(callbacks), 14U);
  EXPECT_EQ(GameServer::MissingBroadcasterCallbacks(callbacks), "newUnlocks");
  std::vector<uint16_t> unlistened;
  GameServer::UnregisterBroadcasterCallbacks(FakeBroadcaster(0x1000), callbacks,
      [&](EchoVR::Broadcaster*, uint16_t handle) { unlistened.push_back(handle); });
  EXPECT_EQ(unlistened.size(), 14U);
  for (uint16_t handle : unlistened) EXPECT_NE(handle, 0xFFFF);
}

TEST(CallbackRegistrationCount, EveryCallbackIsNamedExactlyWhenItsHandleIsMissing) {
  const std::array<const char*, 15> expected = {
      "sessionStart", "sessionError", "saveLoadout", "saveLoadoutSuccess", "saveLoadoutPartial",
      "currentLoadoutRequest", "currentLoadoutResponse", "refreshProfileForUser", "refreshProfileFromServer",
      "lobbySendClientSettings", "tierReward", "topAwards", "newUnlocks", "reliableStatUpdate",
      "reliableTeamStatUpdate"};
  uint16_t GameServer::CallbackRegistry::* const fields[15] = {
      &GameServer::CallbackRegistry::sessionStart, &GameServer::CallbackRegistry::sessionError,
      &GameServer::CallbackRegistry::saveLoadout, &GameServer::CallbackRegistry::saveLoadoutSuccess,
      &GameServer::CallbackRegistry::saveLoadoutPartial, &GameServer::CallbackRegistry::currentLoadoutRequest,
      &GameServer::CallbackRegistry::currentLoadoutResponse, &GameServer::CallbackRegistry::refreshProfileForUser,
      &GameServer::CallbackRegistry::refreshProfileFromServer, &GameServer::CallbackRegistry::lobbySendClientSettings,
      &GameServer::CallbackRegistry::tierReward, &GameServer::CallbackRegistry::topAwards,
      &GameServer::CallbackRegistry::newUnlocks, &GameServer::CallbackRegistry::reliableStatUpdate,
      &GameServer::CallbackRegistry::reliableTeamStatUpdate};
  for (size_t i = 0; i < expected.size(); ++i) {
    GameServer::CallbackRegistry callbacks;
    SetAllUdpHandles(callbacks);
    callbacks.*(fields[i]) = 0;
    EXPECT_EQ(GameServer::MissingBroadcasterCallbacks(callbacks), expected[i]) << "field " << i;
    EXPECT_EQ(GameServer::CountRegisteredBroadcasterCallbacks(callbacks), 14U) << "field " << i;
  }
}

TEST(CallbackRegistrationCount, TcpSentinelsAreNotCounted) {
  GameServer::CallbackRegistry callbacks;
  callbacks.tcpRegSuccess = 1;
  callbacks.tcpRegFailure = 1;
  callbacks.tcpSessionSuccess = 1;
  callbacks.tcpProtobuf = 1;
  EXPECT_EQ(GameServer::CountRegisteredBroadcasterCallbacks(callbacks), 0U);
  EXPECT_EQ(GameServer::MissingBroadcasterCallbacks(callbacks).substr(0, 14), "sessionStart, ");
}

TEST(CallbackRegistrationCount, TheCountMatchesWhatUnregisterRemoves) {
  GameServer::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = FakeBroadcaster(0x1000);
  SetAllUdpHandles(callbacks);
  callbacks.topAwards = 0;
  const size_t registered = GameServer::CountRegisteredBroadcasterCallbacks(callbacks);
  const size_t removed = GameServer::UnregisterBroadcasterCallbacks(
      FakeBroadcaster(0x1000), callbacks, [](EchoVR::Broadcaster*, uint16_t) {});
  EXPECT_EQ(removed, registered);
}

TEST(CallbackUnregistration, RemovesAllFifteenHandlesFromTheirOwningBroadcaster) {
  auto* owner = FakeBroadcaster(0x1000);
  GameServer::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = owner;
  SetAllUdpHandles(callbacks);
  std::vector<uint16_t> removed;

  const size_t count = GameServer::UnregisterBroadcasterCallbacks(
      owner, callbacks, [&removed, owner](EchoVR::Broadcaster* suppliedOwner, uint16_t handle) {
        EXPECT_EQ(suppliedOwner, owner);
        removed.push_back(handle);
      });

  EXPECT_EQ(count, 15U);
  ASSERT_EQ(removed.size(), 15U);
  for (uint16_t index = 0; index < removed.size(); ++index) EXPECT_EQ(removed[index], index + 1);
  EXPECT_EQ(callbacks.broadcasterOwner, nullptr);
  EXPECT_EQ(callbacks.tcpRegSuccess, 0U);
}

TEST(CallbackUnregistration, SkipsZeroHandlesAndSupportsPartialRegistration) {
  auto* owner = FakeBroadcaster(0x2000);
  GameServer::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = owner;
  callbacks.sessionError = 7;
  callbacks.tierReward = 1;
  std::vector<uint16_t> removed;

  const size_t count = GameServer::UnregisterBroadcasterCallbacks(
      owner, callbacks, [&removed](EchoVR::Broadcaster*, uint16_t handle) { removed.push_back(handle); });

  EXPECT_EQ(count, 2U);
  EXPECT_EQ(removed, (std::vector<uint16_t>{7, 1}));
}

TEST(CallbackUnregistration, NullOrDifferentLiveOwnerNeverCallsUnlisten) {
  const std::array<EchoVR::Broadcaster*, 2> liveOwners = {nullptr, FakeBroadcaster(0x4000)};
  for (EchoVR::Broadcaster* liveOwner : liveOwners) {
    GameServer::CallbackRegistry callbacks;
    callbacks.broadcasterOwner = FakeBroadcaster(0x3000);
    callbacks.sessionStart = 4;
    size_t calls = 0;
    EXPECT_EQ(GameServer::UnregisterBroadcasterCallbacks(
                  liveOwner, callbacks, [&calls](EchoVR::Broadcaster*, uint16_t) { ++calls; }),
              0U);
    EXPECT_EQ(calls, 0U);
    EXPECT_EQ(callbacks.broadcasterOwner, nullptr);
    EXPECT_EQ(callbacks.sessionStart, 0U);
  }
}

// Issue #117: merge 033b303 dropped the owner assignment from
// GameServerLib::RegisterBroadcasterCallbacks, so broadcasterOwner stayed null
// and every unregister skipped EchoVR::BroadcasterUnlisten. These tests drive
// the owner through the helper production registration calls, then unregister
// with the live owner exactly as GameServerLib::UnregisterAllCallbacks derives
// it (the context's lobby->broadcaster).
TEST(CallbackRegistrationOwner, RegisterThenUnregisterReachesUnlistenOnTheLobbysBroadcaster) {
  auto* broadcaster = FakeBroadcaster(0x6000);
  EchoVR::Lobby lobby{};
  lobby.broadcaster = broadcaster;
  GameServer::ServerContext context;
  context.Initialize(&lobby, broadcaster);
  context.FinalizeInitialization();

  EXPECT_EQ(GameServer::RecordBroadcasterOwner(context), broadcaster);
  auto& callbacks = context.GetCallbackRegistry();
  EXPECT_EQ(callbacks.broadcasterOwner, broadcaster);
  SetAllUdpHandles(callbacks);

  EchoVR::Lobby* liveLobby = context.GetLobby();
  ASSERT_NE(liveLobby, nullptr);
  std::vector<uint16_t> removed;
  const size_t count = GameServer::UnregisterBroadcasterCallbacks(
      liveLobby->broadcaster, callbacks,
      [&removed, broadcaster](EchoVR::Broadcaster* suppliedOwner, uint16_t handle) {
        EXPECT_EQ(suppliedOwner, broadcaster);
        removed.push_back(handle);
      });

  EXPECT_EQ(count, 15U);
  EXPECT_EQ(removed.size(), 15U);
  EXPECT_EQ(callbacks.broadcasterOwner, nullptr);
}

TEST(CallbackRegistrationOwner, SessionWithoutBroadcasterRecordsNullOwnerAndNeverUnlistens) {
  EchoVR::Lobby lobby{};
  lobby.broadcaster = nullptr;
  GameServer::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  EXPECT_EQ(GameServer::RecordBroadcasterOwner(context), nullptr);
  auto& callbacks = context.GetCallbackRegistry();
  EXPECT_EQ(callbacks.broadcasterOwner, nullptr);
  // Production registers nothing without a broadcaster (ListenForBroadcasterMessage
  // returns 0); non-zero handles here prove the guard, not the empty registry,
  // keeps unlisten from running.
  SetAllUdpHandles(callbacks);

  size_t calls = 0;
  EXPECT_EQ(GameServer::UnregisterBroadcasterCallbacks(
                context.GetLobby()->broadcaster, callbacks,
                [&calls](EchoVR::Broadcaster*, uint16_t) { ++calls; }),
            0U);
  EXPECT_EQ(calls, 0U);
  EXPECT_EQ(callbacks.sessionStart, 0U);
}

TEST(CallbackRegistrationOwner, UninitializedContextHasNoLobbyAndRecordsNullOwner) {
  GameServer::ServerContext context;
  ASSERT_EQ(context.GetLobby(), nullptr);

  EXPECT_EQ(GameServer::RecordBroadcasterOwner(context), nullptr);
  EXPECT_EQ(context.GetCallbackRegistry().broadcasterOwner, nullptr);
}

TEST(CallbackUnregistration, RepeatedUnregisterIsIdempotentAndSameOwnerCanRegisterAgain) {
  auto* owner = FakeBroadcaster(0x5000);
  GameServer::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = owner;
  callbacks.sessionStart = 9;
  std::vector<uint16_t> removed;
  const auto unlisten = [&removed](EchoVR::Broadcaster*, uint16_t handle) { removed.push_back(handle); };

  EXPECT_EQ(GameServer::UnregisterBroadcasterCallbacks(owner, callbacks, unlisten), 1U);
  EXPECT_EQ(GameServer::UnregisterBroadcasterCallbacks(owner, callbacks, unlisten), 0U);
  callbacks.broadcasterOwner = owner;
  callbacks.sessionStart = 12;
  EXPECT_EQ(GameServer::UnregisterBroadcasterCallbacks(owner, callbacks, unlisten), 1U);
  EXPECT_EQ(removed, (std::vector<uint16_t>{9, 12}));
}
