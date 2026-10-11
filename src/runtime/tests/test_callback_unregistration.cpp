#include "runtime/server/callback_unregistration.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {
EchoVR::Broadcaster* FakeBroadcaster(uintptr_t address) {
  return reinterpret_cast<EchoVR::Broadcaster*>(address);
}

void SetAllUdpHandles(nevr_game_server::CallbackRegistry& callbacks) {
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
  nevr_game_server::CallbackRegistry callbacks;
  EXPECT_EQ(nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks), 0U);
  EXPECT_EQ(nevr_game_server::kBroadcasterCallbackCount, 15U);

  SetAllUdpHandles(callbacks);
  EXPECT_EQ(nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks), nevr_game_server::kBroadcasterCallbackCount);
  EXPECT_EQ(nevr_game_server::MissingBroadcasterCallbacks(callbacks), "");

  callbacks.saveLoadoutPartial = 0;
  callbacks.reliableTeamStatUpdate = 0;
  EXPECT_EQ(nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks), 13U);
  EXPECT_EQ(nevr_game_server::MissingBroadcasterCallbacks(callbacks), "saveLoadoutPartial, reliableTeamStatUpdate");
}

// The game's BroadcasterListen returns 0xFFFF when it has no free listener slot. That is not a
// handle: it is neither counted as registered nor ever passed to BroadcasterUnlisten.
TEST(CallbackRegistrationCount, TheGamesListenFailureValueIsNotARegistration) {
  nevr_game_server::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = FakeBroadcaster(0x1000);
  SetAllUdpHandles(callbacks);
  callbacks.newUnlocks = 0xFFFF;
  EXPECT_EQ(nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks), 14U);
  EXPECT_EQ(nevr_game_server::MissingBroadcasterCallbacks(callbacks), "newUnlocks");
  std::vector<uint16_t> unlistened;
  nevr_game_server::UnregisterBroadcasterCallbacks(FakeBroadcaster(0x1000), callbacks,
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
  uint16_t nevr_game_server::CallbackRegistry::* const fields[15] = {
      &nevr_game_server::CallbackRegistry::sessionStart, &nevr_game_server::CallbackRegistry::sessionError,
      &nevr_game_server::CallbackRegistry::saveLoadout, &nevr_game_server::CallbackRegistry::saveLoadoutSuccess,
      &nevr_game_server::CallbackRegistry::saveLoadoutPartial, &nevr_game_server::CallbackRegistry::currentLoadoutRequest,
      &nevr_game_server::CallbackRegistry::currentLoadoutResponse, &nevr_game_server::CallbackRegistry::refreshProfileForUser,
      &nevr_game_server::CallbackRegistry::refreshProfileFromServer, &nevr_game_server::CallbackRegistry::lobbySendClientSettings,
      &nevr_game_server::CallbackRegistry::tierReward, &nevr_game_server::CallbackRegistry::topAwards,
      &nevr_game_server::CallbackRegistry::newUnlocks, &nevr_game_server::CallbackRegistry::reliableStatUpdate,
      &nevr_game_server::CallbackRegistry::reliableTeamStatUpdate};
  for (size_t i = 0; i < expected.size(); ++i) {
    nevr_game_server::CallbackRegistry callbacks;
    SetAllUdpHandles(callbacks);
    callbacks.*(fields[i]) = 0;
    EXPECT_EQ(nevr_game_server::MissingBroadcasterCallbacks(callbacks), expected[i]) << "field " << i;
    EXPECT_EQ(nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks), 14U) << "field " << i;
  }
}

TEST(CallbackRegistrationCount, TcpSentinelsAreNotCounted) {
  nevr_game_server::CallbackRegistry callbacks;
  callbacks.tcpRegSuccess = 1;
  callbacks.tcpRegFailure = 1;
  callbacks.tcpSessionSuccess = 1;
  callbacks.tcpProtobuf = 1;
  EXPECT_EQ(nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks), 0U);
  EXPECT_EQ(nevr_game_server::MissingBroadcasterCallbacks(callbacks).substr(0, 14), "sessionStart, ");
}

TEST(CallbackRegistrationCount, TheCountMatchesWhatUnregisterRemoves) {
  nevr_game_server::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = FakeBroadcaster(0x1000);
  SetAllUdpHandles(callbacks);
  callbacks.topAwards = 0;
  const size_t registered = nevr_game_server::CountRegisteredBroadcasterCallbacks(callbacks);
  const size_t removed = nevr_game_server::UnregisterBroadcasterCallbacks(
      FakeBroadcaster(0x1000), callbacks, [](EchoVR::Broadcaster*, uint16_t) {});
  EXPECT_EQ(removed, registered);
}

TEST(CallbackUnregistration, RemovesAllFifteenHandlesFromTheirOwningBroadcaster) {
  auto* owner = FakeBroadcaster(0x1000);
  nevr_game_server::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = owner;
  SetAllUdpHandles(callbacks);
  std::vector<uint16_t> removed;

  const size_t count = nevr_game_server::UnregisterBroadcasterCallbacks(
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
  nevr_game_server::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = owner;
  callbacks.sessionError = 7;
  callbacks.tierReward = 1;
  std::vector<uint16_t> removed;

  const size_t count = nevr_game_server::UnregisterBroadcasterCallbacks(
      owner, callbacks, [&removed](EchoVR::Broadcaster*, uint16_t handle) { removed.push_back(handle); });

  EXPECT_EQ(count, 2U);
  EXPECT_EQ(removed, (std::vector<uint16_t>{7, 1}));
}

TEST(CallbackUnregistration, NullOrDifferentLiveOwnerNeverCallsUnlisten) {
  const std::array<EchoVR::Broadcaster*, 2> liveOwners = {nullptr, FakeBroadcaster(0x4000)};
  for (EchoVR::Broadcaster* liveOwner : liveOwners) {
    nevr_game_server::CallbackRegistry callbacks;
    callbacks.broadcasterOwner = FakeBroadcaster(0x3000);
    callbacks.sessionStart = 4;
    size_t calls = 0;
    EXPECT_EQ(nevr_game_server::UnregisterBroadcasterCallbacks(
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
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, broadcaster);
  context.FinalizeInitialization();

  EXPECT_EQ(nevr_game_server::RecordBroadcasterOwner(context), broadcaster);
  auto& callbacks = context.GetCallbackRegistry();
  EXPECT_EQ(callbacks.broadcasterOwner, broadcaster);
  SetAllUdpHandles(callbacks);

  EchoVR::Lobby* liveLobby = context.GetLobby();
  ASSERT_NE(liveLobby, nullptr);
  std::vector<uint16_t> removed;
  const size_t count = nevr_game_server::UnregisterBroadcasterCallbacks(
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
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  EXPECT_EQ(nevr_game_server::RecordBroadcasterOwner(context), nullptr);
  auto& callbacks = context.GetCallbackRegistry();
  EXPECT_EQ(callbacks.broadcasterOwner, nullptr);
  // Production registers nothing without a broadcaster (ListenForBroadcasterMessage
  // returns 0); non-zero handles here prove the guard, not the empty registry,
  // keeps unlisten from running.
  SetAllUdpHandles(callbacks);

  size_t calls = 0;
  EXPECT_EQ(nevr_game_server::UnregisterBroadcasterCallbacks(
                context.GetLobby()->broadcaster, callbacks,
                [&calls](EchoVR::Broadcaster*, uint16_t) { ++calls; }),
            0U);
  EXPECT_EQ(calls, 0U);
  EXPECT_EQ(callbacks.sessionStart, 0U);
}

TEST(CallbackRegistrationOwner, UninitializedContextHasNoLobbyAndRecordsNullOwner) {
  nevr_game_server::ServerContext context;
  ASSERT_EQ(context.GetLobby(), nullptr);

  EXPECT_EQ(nevr_game_server::RecordBroadcasterOwner(context), nullptr);
  EXPECT_EQ(context.GetCallbackRegistry().broadcasterOwner, nullptr);
}

TEST(CallbackUnregistration, RepeatedUnregisterIsIdempotentAndSameOwnerCanRegisterAgain) {
  auto* owner = FakeBroadcaster(0x5000);
  nevr_game_server::CallbackRegistry callbacks;
  callbacks.broadcasterOwner = owner;
  callbacks.sessionStart = 9;
  std::vector<uint16_t> removed;
  const auto unlisten = [&removed](EchoVR::Broadcaster*, uint16_t handle) { removed.push_back(handle); };

  EXPECT_EQ(nevr_game_server::UnregisterBroadcasterCallbacks(owner, callbacks, unlisten), 1U);
  EXPECT_EQ(nevr_game_server::UnregisterBroadcasterCallbacks(owner, callbacks, unlisten), 0U);
  callbacks.broadcasterOwner = owner;
  callbacks.sessionStart = 12;
  EXPECT_EQ(nevr_game_server::UnregisterBroadcasterCallbacks(owner, callbacks, unlisten), 1U);
  EXPECT_EQ(removed, (std::vector<uint16_t>{9, 12}));
}
