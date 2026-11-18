// What the dedicated server tells ServerDB about a session's players, and how it reads a slot out of a broadcaster
// message: the lock, unlock, accept and removal envelopes (server/session_envelopes.h), the GUID text they carry
// (server/guid_text.h) and ExtractSlotIndex (server/slot_index.h). GameServerLib::LockPlayerSessions,
// UnlockPlayerSessions, AcceptPlayerSessions and RemovePlayerSession send these envelopes as built here.

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/server/guid_text.h"
#include "runtime/server/session_envelopes.h"
#include "runtime/server/slot_index.h"

namespace {

using gameservice::v1::Envelope;
using gameservice::v1::LobbyEntrantRemovedMessage;
using gameservice::v1::LobbySessionEventMessage;

const std::string kSession = "5c0a8a5e-1b1f-4a9d-9c1e-0d2a3b4c5d6e";

// A GUID's Data4 is eight bytes.
constexpr size_t kGuidData4Size = 8;

// GUID A, written as "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9".
constexpr uint32_t kGuidAData1 = 0x0a1b2c3d;
constexpr uint16_t kGuidAData2 = 0x4e5f;
constexpr uint16_t kGuidAData3 = 0x6071;
constexpr uint8_t kGuidAData4[kGuidData4Size] = {0x82, 0x93, 0xa4, 0xb5, 0xc6, 0xd7, 0xe8, 0xf9};

// GUID B, written as "000000ab-0001-00ff-0001-020304050607"; every group has leading zeros to pad.
constexpr uint32_t kGuidBData1 = 0x000000ab;
constexpr uint16_t kGuidBData2 = 0x0001;
constexpr uint16_t kGuidBData3 = 0x00ff;
constexpr uint8_t kGuidBData4[kGuidData4Size] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

// The accept envelope test names three GUIDs; the third is at index 2.
constexpr int kAcceptedGuidCount = 3;
constexpr int kThirdEntrantIndex = 2;

// Slot words: the low sixteen bits are the slot, the high sixteen the generation.
constexpr uint32_t kPackedSlotThreeGenBeef = 0xBEEF0003u;
constexpr int kSlotThree = 0x0003;
constexpr int kGenerationBeef = 0xBEEF;
constexpr uint32_t kPackedSlotOnlyMax = 0x0000FFFFu;
constexpr uint32_t kPackedGenerationOnlyMax = 0xFFFF0000u;
constexpr int kSixteenBitMax = 0xFFFF;

// Message bytes for the byte-buffer cases: slot 7, generation 9 (little endian), then filler past the first word.
constexpr int kSlotSeven = 7;
constexpr int kGenerationNine = 9;
constexpr uint8_t kSlotSevenLowByte = 0x07;
constexpr uint8_t kGenerationNineLowByte = 0x09;
constexpr uint8_t kFillerByte = 0xEE;
constexpr size_t kSlotWordBytes = 4;
constexpr size_t kTooShortLength = 3;

GUID MakeGuid(uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t (&d4)[kGuidData4Size]) {
  GUID g = {};
  g.Data1 = d1;
  g.Data2 = d2;
  g.Data3 = d3;
  for (size_t i = 0; i < kGuidData4Size; ++i) g.Data4[i] = d4[i];
  return g;
}

const GUID kGuidA = MakeGuid(kGuidAData1, kGuidAData2, kGuidAData3, kGuidAData4);
const GUID kGuidB = MakeGuid(kGuidBData1, kGuidBData2, kGuidBData3, kGuidBData4);

// ---- GuidToUuidString ----

TEST(GuidText, IsLowerCaseHexInEightFourFourFourTwelveGroups) {
  EXPECT_EQ(GuidToUuidString(kGuidA), "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9");
}

TEST(GuidText, PadsEachGroupWithLeadingZeros) {
  EXPECT_EQ(GuidToUuidString(kGuidB), "000000ab-0001-00ff-0001-020304050607");
  EXPECT_EQ(GuidToUuidString(GUID{}), "00000000-0000-0000-0000-000000000000");
}

// ---- the session lock and unlock envelopes ----

TEST(SessionEnvelopes, LockCarriesTheSessionIdAndTheLockedCode) {
  const Envelope e = nevr_game_server::BuildSessionLockEnvelope(kSession, true);
  ASSERT_EQ(e.message_case(), Envelope::kLobbySessionEvent);
  EXPECT_EQ(e.lobby_session_event().lobby_session_id(), kSession);
  EXPECT_EQ(e.lobby_session_event().code(), LobbySessionEventMessage::CODE_LOCKED);
}

TEST(SessionEnvelopes, UnlockCarriesTheSessionIdAndTheUnlockedCode) {
  const Envelope e = nevr_game_server::BuildSessionLockEnvelope(kSession, false);
  ASSERT_EQ(e.message_case(), Envelope::kLobbySessionEvent);
  EXPECT_EQ(e.lobby_session_event().lobby_session_id(), kSession);
  EXPECT_EQ(e.lobby_session_event().code(), LobbySessionEventMessage::CODE_UNLOCKED);
}

// ---- the accept envelope ----

TEST(SessionEnvelopes, AcceptNamesEveryGuidInOrder) {
  const std::vector<GUID> guids = {kGuidA, kGuidB, kGuidA};
  const Envelope e = nevr_game_server::BuildEntrantsConnectedEnvelope(kSession, guids.data(), guids.size());
  ASSERT_EQ(e.message_case(), Envelope::kLobbyEntrantConnected);
  EXPECT_EQ(e.lobby_entrant_connected().lobby_session_id(), kSession);
  ASSERT_EQ(e.lobby_entrant_connected().entrant_ids_size(), kAcceptedGuidCount);
  EXPECT_EQ(e.lobby_entrant_connected().entrant_ids(0), GuidToUuidString(kGuidA));
  EXPECT_EQ(e.lobby_entrant_connected().entrant_ids(1), GuidToUuidString(kGuidB));
  EXPECT_EQ(e.lobby_entrant_connected().entrant_ids(kThirdEntrantIndex), GuidToUuidString(kGuidA));
}

TEST(SessionEnvelopes, AcceptOfNoPlayersStillNamesTheSessionAndAcceptsNobody) {
  const Envelope e = nevr_game_server::BuildEntrantsConnectedEnvelope(kSession, nullptr, 0);
  ASSERT_EQ(e.message_case(), Envelope::kLobbyEntrantConnected);
  EXPECT_EQ(e.lobby_entrant_connected().lobby_session_id(), kSession);
  EXPECT_EQ(e.lobby_entrant_connected().entrant_ids_size(), 0);
}

TEST(SessionEnvelopes, AcceptReadsOnlyTheCountItIsGiven) {
  const std::vector<GUID> guids = {kGuidA, kGuidB};
  const Envelope e = nevr_game_server::BuildEntrantsConnectedEnvelope(kSession, guids.data(), 1);
  ASSERT_EQ(e.lobby_entrant_connected().entrant_ids_size(), 1);
  EXPECT_EQ(e.lobby_entrant_connected().entrant_ids(0), GuidToUuidString(kGuidA));
}

// ---- the removal envelope ----

TEST(SessionEnvelopes, RemovalNamesTheSessionTheEntrantAndTheDisconnectedCode) {
  const Envelope e = nevr_game_server::BuildEntrantRemovedEnvelope(kSession, kGuidB);
  ASSERT_EQ(e.message_case(), Envelope::kLobbyEntrantRemoved);
  EXPECT_EQ(e.lobby_entrant_removed().lobby_session_id(), kSession);
  EXPECT_EQ(e.lobby_entrant_removed().entrant_id(), GuidToUuidString(kGuidB));
  EXPECT_EQ(e.lobby_entrant_removed().code(), LobbyEntrantRemovedMessage::CODE_DISCONNECTED);
}

// ---- ExtractSlotIndex ----

SlotInfo Extract(uint32_t packed) { return ExtractSlotIndex(&packed, sizeof(packed)); }

TEST(SlotIndex, SlotIsTheLowSixteenBitsAndGenerationTheHigh) {
  const SlotInfo info = Extract(kPackedSlotThreeGenBeef);
  EXPECT_EQ(info.slot, kSlotThree);
  EXPECT_EQ(info.genId, kGenerationBeef);
}

TEST(SlotIndex, TheTwoHalvesDoNotBleedIntoEachOther) {
  EXPECT_EQ(Extract(kPackedSlotOnlyMax).slot, kSixteenBitMax);
  EXPECT_EQ(Extract(kPackedSlotOnlyMax).genId, 0);
  EXPECT_EQ(Extract(kPackedGenerationOnlyMax).slot, 0);
  EXPECT_EQ(Extract(kPackedGenerationOnlyMax).genId, kSixteenBitMax);
}

TEST(SlotIndex, ReadsOnlyTheFirstFourBytesOfALongerMessage) {
  const uint8_t message[] = {kSlotSevenLowByte, 0x00, kGenerationNineLowByte, 0x00,
                             kFillerByte,       kFillerByte, kFillerByte,       kFillerByte};
  const SlotInfo info = ExtractSlotIndex(message, sizeof(message));
  EXPECT_EQ(info.slot, kSlotSeven);
  EXPECT_EQ(info.genId, kGenerationNine);
}

TEST(SlotIndex, ANullOrShortMessageNamesSlotZeroGenerationZero) {
  const uint8_t three[] = {kSlotSevenLowByte, 0x00, kGenerationNineLowByte};
  EXPECT_EQ(ExtractSlotIndex(nullptr, kSlotWordBytes).slot, 0);
  EXPECT_EQ(ExtractSlotIndex(nullptr, kSlotWordBytes).genId, 0);
  EXPECT_EQ(ExtractSlotIndex(three, sizeof(three)).slot, 0);
  EXPECT_EQ(ExtractSlotIndex(three, sizeof(three)).genId, 0);
  const uint8_t four[] = {kSlotSevenLowByte, 0x00, kGenerationNineLowByte, 0x00};
  EXPECT_EQ(ExtractSlotIndex(four, 0).slot, 0);
  EXPECT_EQ(ExtractSlotIndex(four, kTooShortLength).slot, 0);
  EXPECT_EQ(ExtractSlotIndex(four, kSlotWordBytes).slot, kSlotSeven);
}

}  // namespace
