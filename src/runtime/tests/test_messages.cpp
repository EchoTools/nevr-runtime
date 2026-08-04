#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/messages.h"

namespace {

constexpr char kLobbyId[] = "00112233-4455-6677-8899-aabbccddeeff";
constexpr char kGroupId[] = "ffeeddcc-bbaa-9988-7766-554433221100";

uint64_t MakeEncoderFlags() {
  constexpr uint64_t kEncryptionEnabled = 1;
  constexpr uint64_t kMacEnabled = 2;
  constexpr uint64_t kDigestSize = 32;
  constexpr uint64_t kIterations = 100;
  constexpr uint64_t kMacKeySize = 32;
  constexpr uint64_t kEncryptionKeySize = 32;
  constexpr uint64_t kRandomKeySize = 32;
  return kEncryptionEnabled | kMacEnabled | (kDigestSize << 2) | (kIterations << 14) |
         (kMacKeySize << 26) | (kEncryptionKeySize << 38) | (kRandomKeySize << 50);
}

uint64_t ReadLe64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t result = 0;
  for (size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<uint64_t>(bytes[offset + index]) <<
              static_cast<unsigned int>(index * 8);
  }
  return result;
}

}  // namespace

TEST(MessagesUuid, ValidUuidProducesExpectedWindowsGuidLayout) {
  GUID guid = {};
  ASSERT_TRUE(ParseUuidToGuid(kLobbyId, guid));
  EXPECT_EQ(guid.Data1, 0x00112233U);
  EXPECT_EQ(guid.Data2, 0x4455U);
  EXPECT_EQ(guid.Data3, 0x6677U);
  const std::array<uint8_t, 8> expectedData4 = {0x88, 0x99, 0xaa, 0xbb,
                                                 0xcc, 0xdd, 0xee, 0xff};
  for (size_t index = 0; index < expectedData4.size(); ++index) {
    EXPECT_EQ(guid.Data4[index], expectedData4[index]);
  }
}

TEST(MessagesUuid, InvalidUuidIsRejected) {
  GUID guid = {};
  EXPECT_FALSE(ParseUuidToGuid("not-a-uuid", guid));
  EXPECT_FALSE(ParseUuidToGuid("00112233-4455-6677-8899-aabbccddeefg", guid));
}

TEST(MessagesEncoderSettings, FlagsRoundTripToAllPacketSettings) {
  const PacketEncoderSettings settings = PacketEncoderSettings::FromFlags(MakeEncoderFlags());
  EXPECT_TRUE(settings.encryptionEnabled);
  EXPECT_TRUE(settings.macEnabled);
  EXPECT_EQ(settings.macDigestSize, 32);
  EXPECT_EQ(settings.macPBKDF2IterationCount, 100);
  EXPECT_EQ(settings.macKeySize, 32);
  EXPECT_EQ(settings.encryptionKeySize, 32);
  EXPECT_EQ(settings.randomKeySize, 32);
}

TEST(MessagesEncoding, RegistrationSuccessHasExpectedFixedLayout) {
  gameservice::v1::GameServerRegistrationSuccessMessage message;
  message.set_server_id(0x1122334455667788ULL);
  message.set_external_ip_address("203.0.113.9");

  const EncodedMessage encoded = EncodeRegistrationSuccess(message);
  ASSERT_EQ(encoded.size(), 20U);
  EXPECT_EQ(ReadLe64(encoded.data, 0), 0x1122334455667788ULL);
  EXPECT_EQ(encoded.data[8], 203U);
  EXPECT_EQ(encoded.data[9], 0U);
  EXPECT_EQ(encoded.data[10], 113U);
  EXPECT_EQ(encoded.data[11], 9U);
  EXPECT_EQ(ReadLe64(encoded.data, 12), 0U);
}

TEST(MessagesEncoding, LobbySessionCreateCarriesGuidsSettingsAndLimits) {
  gameservice::v1::LobbySessionCreateMessage message;
  message.set_lobby_session_id(kLobbyId);
  message.set_group_id(kGroupId);
  message.set_max_entrants(6);
  message.set_lobby_type(2);
  message.set_settings_json("{\"mode\":\"arena\"}");

  const EncodedMessage encoded = EncodeLobbySessionCreate(message);
  ASSERT_GT(encoded.size(), 36U);
  EXPECT_EQ(encoded.data[32], 6U);
  EXPECT_EQ(encoded.data[33], 0U);
  EXPECT_EQ(encoded.data[34], 2U);
  EXPECT_EQ(encoded.data[35], 0U);
  EXPECT_EQ(encoded.data.back(), 0U);
}

TEST(MessagesEncoding, EntrantAndSmiteMessagesHaveExpectedBinaryShapes) {
  gameservice::v1::LobbyEntrantsAcceptMessage accept;
  accept.add_entrant_ids(kLobbyId);
  const EncodedMessage accepted = EncodeLobbyEntrantsAccept(accept);
  ASSERT_EQ(accepted.size(), 17U);
  EXPECT_EQ(accepted.data.front(), 0U);

  gameservice::v1::LobbyEntrantsRejectMessage reject;
  reject.set_code(7);
  reject.add_entrant_ids(kGroupId);
  const EncodedMessage rejected = EncodeLobbyEntrantsReject(reject);
  ASSERT_EQ(rejected.size(), 17U);
  EXPECT_EQ(rejected.data.front(), 7U);

  const EncodedMessage smite = EncodeLobbySmiteEntrant(99);
  ASSERT_EQ(smite.size(), 16U);
  EXPECT_EQ(ReadLe64(smite.data, 0), 99U);
  EXPECT_EQ(ReadLe64(smite.data, 8), 0U);
}

TEST(MessagesEncoding, SessionSuccessEncodesValidatedKeysAndSequences) {
  gameservice::v1::SNSLobbySessionSuccessV5Message message;
  message.set_game_mode(1);
  message.set_lobby_id(kLobbyId);
  message.set_group_id(kGroupId);
  message.set_endpoint("10.0.0.1:203.0.113.9:6721");
  message.set_team_index(2);
  message.set_session_flags(3);
  message.set_server_encoder_flags(MakeEncoderFlags());
  message.set_client_encoder_flags(MakeEncoderFlags());
  message.set_server_sequence_id(17);
  message.set_client_sequence_id(19);
  const std::string key(32, 'k');
  message.set_server_mac_key(key);
  message.set_server_enc_key(key);
  message.set_server_random_key(key);
  message.set_client_mac_key(key);
  message.set_client_enc_key(key);
  message.set_client_random_key(key);

  const EncodedMessage encoded = EncodeLobbySessionSuccessV5(message);
  ASSERT_EQ(encoded.size(), 280U);
  EXPECT_EQ(ReadLe64(encoded.data, 0), 1U);
  EXPECT_EQ(encoded.data[0x30], 0x1aU);
  EXPECT_EQ(encoded.data[0x31], 0x41U);
  EXPECT_EQ(encoded.data[0x32], 2U);
  EXPECT_EQ(encoded.data[0x34], 3U);
  EXPECT_EQ(ReadLe64(encoded.data, 0x38), MakeEncoderFlags());
  EXPECT_EQ(ReadLe64(encoded.data, 0x40), MakeEncoderFlags());
  EXPECT_EQ(ReadLe64(encoded.data, 0x48), 17U);
  EXPECT_EQ(encoded.data[0x50], static_cast<uint8_t>('k'));
  EXPECT_EQ(ReadLe64(encoded.data, 0xb0), 19U);
}
