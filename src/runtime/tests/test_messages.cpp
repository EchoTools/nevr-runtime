#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include <winsock2.h>

#include "abi/echovr_functions.h"
#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/messages.h"

namespace {

constexpr char kLobbyId[] = "00112233-4455-6677-8899-aabbccddeeff";
constexpr char kGroupId[] = "ffeeddcc-bbaa-9988-7766-554433221100";
constexpr char kEntrantId[] = "12345678-9abc-def0-1234-56789abcdef0";

constexpr std::array<uint8_t, 16> kLobbyGuidBytes = {0x33, 0x22, 0x11, 0x00,
                                                      0x55, 0x44, 0x77, 0x66,
                                                      0x88, 0x99, 0xaa, 0xbb,
                                                      0xcc, 0xdd, 0xee, 0xff};
constexpr std::array<uint8_t, 16> kGroupGuidBytes = {0xcc, 0xdd, 0xee, 0xff,
                                                      0xaa, 0xbb, 0x88, 0x99,
                                                      0x77, 0x66, 0x55, 0x44,
                                                      0x33, 0x22, 0x11, 0x00};
constexpr std::array<uint8_t, 16> kEntrantGuidBytes = {0x78, 0x56, 0x34, 0x12,
                                                        0xbc, 0x9a, 0xf0, 0xde,
                                                        0x12, 0x34, 0x56, 0x78,
                                                        0x9a, 0xbc, 0xde, 0xf0};

constexpr uint64_t MakeEncoderFlags(uint64_t macKeySize, uint64_t encryptionKeySize,
                                    uint64_t randomKeySize) {
  constexpr uint64_t kEncryptionEnabled = 1;
  constexpr uint64_t kMacEnabled = 2;
  constexpr uint64_t kDigestSize = 32;
  constexpr uint64_t kIterations = 100;
  return kEncryptionEnabled | kMacEnabled | (kDigestSize << 2) | (kIterations << 14) |
         (macKeySize << 26) | (encryptionKeySize << 38) | (randomKeySize << 50);
}

constexpr uint64_t MakeEncoderFlags() {
  return MakeEncoderFlags(32, 32, 32);
}

uint64_t ReadLe64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t result = 0;
  for (size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<uint64_t>(bytes[offset + index]) <<
              static_cast<unsigned int>(index * 8);
  }
  return result;
}

template <size_t N>
void ExpectBytesAt(const std::vector<uint8_t>& bytes, size_t offset,
                   const std::array<uint8_t, N>& expected) {
  ASSERT_LE(offset + expected.size(), bytes.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(bytes[offset + index], expected[index]);
  }
}

void ExpectRepeatedBytesAt(const std::vector<uint8_t>& bytes, size_t offset, size_t count,
                           uint8_t expected) {
  ASSERT_LE(offset + count, bytes.size());
  for (size_t index = 0; index < count; ++index) {
    EXPECT_EQ(bytes[offset + index], expected);
  }
}

void IgnoreGameLog(EchoVR::LogLevel, UINT64, const CHAR*, va_list) {}

class ScopedGameLogger {
 public:
  ScopedGameLogger() : previous_(EchoVR::WriteLog) {
    EchoVR::WriteLog = &IgnoreGameLog;
  }

  ~ScopedGameLogger() {
    EchoVR::WriteLog = previous_;
  }

  ScopedGameLogger(const ScopedGameLogger&) = delete;
  ScopedGameLogger& operator=(const ScopedGameLogger&) = delete;

 private:
  EchoVR::WriteLogFunc* previous_;
};

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

TEST(MessagesEncoding, LobbySessionStartV4CarriesFixedFieldsAndSettingsPayload) {
  GUID lobbyId = {};
  GUID groupId = {};
  ASSERT_TRUE(ParseUuidToGuid(kLobbyId, lobbyId));
  ASSERT_TRUE(ParseUuidToGuid(kGroupId, groupId));

  const std::string settingsJson = "{\"mode\":\"arena\"}";
  const EncodedMessage encoded = EncodeLobbySessionStartV4(lobbyId, groupId, 6, 2, 4, 0xa5, settingsJson);

  ASSERT_EQ(encoded.size(), 36U + settingsJson.size() + 1U);
  ExpectBytesAt(encoded.data, 0, kLobbyGuidBytes);
  ExpectBytesAt(encoded.data, 16, kGroupGuidBytes);
  EXPECT_EQ(encoded.data[32], 6U);
  EXPECT_EQ(encoded.data[33], 2U);
  EXPECT_EQ(encoded.data[34], 4U);
  EXPECT_EQ(encoded.data[35], 0xa5U);
  EXPECT_EQ(std::string(encoded.data.begin() + 36, encoded.data.end()), settingsJson + '\0');
}

TEST(MessagesEndpoint, ValidEndpointCarriesNetworkOrderAddressesAndPort) {
  uint32_t internalIp = 0;
  uint32_t externalIp = 0;
  uint16_t port = 0;

  ASSERT_TRUE(ParseEndpoint("10.0.0.1:203.0.113.9:6721", internalIp, externalIp, port));
  EXPECT_EQ(internalIp, 0x0100000aU);
  EXPECT_EQ(externalIp, 0x097100cbU);
  EXPECT_EQ(port, 6721U);
}

TEST(MessagesEndpoint, MalformedOrInvalidEndpointDoesNotRequireUninitializedOutputs) {
  uint32_t internalIp = 0x11111111U;
  uint32_t externalIp = 0x22222222U;
  uint16_t port = 0x3333U;

  EXPECT_FALSE(ParseEndpoint("malformed-endpoint", internalIp, externalIp, port));
  EXPECT_EQ(internalIp, 0x11111111U);
  EXPECT_EQ(externalIp, 0x22222222U);
  EXPECT_EQ(port, 0x3333U);

  EXPECT_FALSE(ParseEndpoint("not-an-ip:203.0.113.9:6721", internalIp, externalIp, port));
  EXPECT_EQ(internalIp, 0x11111111U);
  EXPECT_EQ(externalIp, 0x22222222U);
  EXPECT_EQ(port, 0x3333U);

  ScopedGameLogger logger;
  EXPECT_FALSE(ParseEndpoint("10.0.0.1:203.0.113.9:65536", internalIp, externalIp, port));
  EXPECT_EQ(internalIp, 0x0100000aU);
  EXPECT_EQ(externalIp, 0x097100cbU);
  EXPECT_EQ(port, 0x3333U);
}

TEST(MessagesEncoding, LobbySessionCreateCarriesGuidsSettingsAndLimits) {
  gameservice::v1::LobbySessionCreateMessage message;
  message.set_lobby_session_id(kLobbyId);
  message.set_group_id(kGroupId);
  message.set_max_entrants(6);
  message.set_lobby_type(2);
  message.set_settings_json("{\"mode\":\"arena\"}");

  const EncodedMessage encoded = EncodeLobbySessionCreate(message);
  ASSERT_EQ(encoded.size(), 36U + message.settings_json().size() + 1U);
  ExpectBytesAt(encoded.data, 0, kLobbyGuidBytes);
  ExpectBytesAt(encoded.data, 16, kGroupGuidBytes);
  EXPECT_EQ(encoded.data[32], 6U);
  EXPECT_EQ(encoded.data[33], 0U);
  EXPECT_EQ(encoded.data[34], 2U);
  EXPECT_EQ(encoded.data[35], 0U);
  EXPECT_EQ(std::string(encoded.data.begin() + 36, encoded.data.end()), message.settings_json() + '\0');
}

TEST(MessagesEncoding, EntrantAndSmiteMessagesHaveExpectedBinaryShapes) {
  gameservice::v1::LobbyEntrantsAcceptMessage accept;
  accept.add_entrant_ids(kLobbyId);
  accept.add_entrant_ids(kEntrantId);
  const EncodedMessage accepted = EncodeLobbyEntrantsAccept(accept);
  ASSERT_EQ(accepted.size(), 33U);
  EXPECT_EQ(accepted.data.front(), 0U);
  ExpectBytesAt(accepted.data, 1, kLobbyGuidBytes);
  ExpectBytesAt(accepted.data, 17, kEntrantGuidBytes);

  gameservice::v1::LobbyEntrantsRejectMessage reject;
  reject.set_code(7);
  reject.add_entrant_ids(kGroupId);
  reject.add_entrant_ids(kEntrantId);
  const EncodedMessage rejected = EncodeLobbyEntrantsReject(reject);
  ASSERT_EQ(rejected.size(), 33U);
  EXPECT_EQ(rejected.data.front(), 7U);
  ExpectBytesAt(rejected.data, 1, kGroupGuidBytes);
  ExpectBytesAt(rejected.data, 17, kEntrantGuidBytes);

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
  ExpectBytesAt(encoded.data, 8, kLobbyGuidBytes);
  ExpectBytesAt(encoded.data, 24, kGroupGuidBytes);
  ExpectBytesAt(encoded.data, 40, std::array<uint8_t, 4>{10, 0, 0, 1});
  ExpectBytesAt(encoded.data, 44, std::array<uint8_t, 4>{203, 0, 113, 9});
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

TEST(MessagesEncoding, SessionSuccessInvalidEndpointReturnsEncodedPrefix) {
  gameservice::v1::SNSLobbySessionSuccessV5Message message;
  message.set_game_mode(0x1122334455667788ULL);
  message.set_lobby_id(kLobbyId);
  message.set_group_id(kGroupId);
  message.set_endpoint("invalid-endpoint");

  const EncodedMessage encoded = EncodeLobbySessionSuccessV5(message);

  ASSERT_EQ(encoded.size(), 40U);
  EXPECT_EQ(ReadLe64(encoded.data, 0), 0x1122334455667788ULL);
  ExpectBytesAt(encoded.data, 8, kLobbyGuidBytes);
  ExpectBytesAt(encoded.data, 24, kGroupGuidBytes);
}

TEST(MessagesEncoding, SessionSuccessUsesEncoderKeySizesForVariableOffsets) {
  constexpr uint64_t kServerFlags = MakeEncoderFlags(9, 24, 5);
  constexpr uint64_t kClientFlags = MakeEncoderFlags(12, 16, 7);
  gameservice::v1::SNSLobbySessionSuccessV5Message message;
  message.set_game_mode(1);
  message.set_lobby_id(kLobbyId);
  message.set_group_id(kGroupId);
  message.set_endpoint("10.0.0.1:203.0.113.9:6721");
  message.set_server_encoder_flags(kServerFlags);
  message.set_client_encoder_flags(kClientFlags);
  message.set_server_sequence_id(0x0102030405060708ULL);
  message.set_client_sequence_id(0x1112131415161718ULL);
  message.set_server_mac_key(std::string(9, 'M'));
  message.set_server_enc_key(std::string(24, 'E'));
  message.set_server_random_key(std::string(5, 'R'));
  message.set_client_mac_key(std::string(12, 'm'));
  message.set_client_enc_key(std::string(16, 'e'));
  message.set_client_random_key(std::string(7, 'r'));

  const EncodedMessage encoded = EncodeLobbySessionSuccessV5(message);

  constexpr size_t kFixedHeaderSize = 0x48;
  constexpr size_t kServerKeyOffset = kFixedHeaderSize + sizeof(uint64_t);
  constexpr size_t kClientSequenceOffset = kServerKeyOffset + 9 + 24 + 5;
  constexpr size_t kClientKeyOffset = kClientSequenceOffset + sizeof(uint64_t);
  ASSERT_EQ(encoded.size(), kClientKeyOffset + 12 + 16 + 7);
  EXPECT_EQ(ReadLe64(encoded.data, 0x38), kServerFlags);
  EXPECT_EQ(ReadLe64(encoded.data, 0x40), kClientFlags);
  EXPECT_EQ(ReadLe64(encoded.data, 0x48), 0x0102030405060708ULL);
  ExpectRepeatedBytesAt(encoded.data, kServerKeyOffset, 9, static_cast<uint8_t>('M'));
  ExpectRepeatedBytesAt(encoded.data, kServerKeyOffset + 9, 24, static_cast<uint8_t>('E'));
  ExpectRepeatedBytesAt(encoded.data, kServerKeyOffset + 9 + 24, 5, static_cast<uint8_t>('R'));
  EXPECT_EQ(ReadLe64(encoded.data, kClientSequenceOffset), 0x1112131415161718ULL);
  ExpectRepeatedBytesAt(encoded.data, kClientKeyOffset, 12, static_cast<uint8_t>('m'));
  ExpectRepeatedBytesAt(encoded.data, kClientKeyOffset + 12, 16, static_cast<uint8_t>('e'));
  ExpectRepeatedBytesAt(encoded.data, kClientKeyOffset + 12 + 16, 7, static_cast<uint8_t>('r'));
}
