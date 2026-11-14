// The session pin's login entries as the PC login carries them (ADR 0048 decision 9): with nothing staged or mounted the login is
// byte-identical to today's, and with maps it carries `nevr_maps` with the entries the core builds. The pin's decisions and state
// are tested in src/core/tests/map_session_pin_test.cpp; this file reads the real login_profile.cpp.

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/map_session_pin.h"
#include "nevr/symbol_hash.h"
#include "runtime/compat/login_profile.h"
#include "runtime/content/map_session_pin_entry.h"
#include "runtime/content/map_session_pin_notice.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/prologue_head.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace {

namespace sp = nevr_map_session_pin;

const std::string kSourceA = "0123456789abcdef" + std::string(48, 'a');
const std::string kSourceB = "fedcba9876543210" + std::string(48, 'b');

sp::MapPin Pin(const std::string& id, const std::string& version, const std::string& source) {
  sp::MapPin pin;
  pin.id = id;
  pin.version = version;
  pin.source_sha256 = source;
  pin.index_sha256 = std::string(64, '1');
  pin.platform = "windows-x86_64";
  pin.title = id;
  pin.level = EchoVR::CSymbol64Hash(id.c_str());
  return pin;
}

class MapSessionPinLogin : public ::testing::Test {
 protected:
  void SetUp() override { sp::ResetForTest(); }
  void TearDown() override { sp::ResetForTest(); }
};

TEST_F(MapSessionPinLogin, WithNothingStagedTheLoginIsUnchanged) {
  nevr_login_profile::LoginProfileInputs before;
  before.account_id = 42;
  before.display_name = "player";
  nevr_login_profile::LoginProfileInputs after = before;
  after.maps = sp::LoginEntries();
  EXPECT_EQ(nevr_login_profile::BuildLoginProfileJson(after), nevr_login_profile::BuildLoginProfileJson(before));

  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(after));
  std::vector<std::string> keys;
  for (auto it = profile.begin(); it != profile.end(); ++it) keys.push_back(it.key());
  const std::vector<std::string> stock = {"access_token", "accountid",    "appid",          "buildversion",
                                          "bypassauth",   "desiredclientprofileversion",    "displayname",
                                          "hmdserialnumber", "lobbyversion", "nevr_identity", "nevr_plugins",
                                          "nevr_social",  "nonce",        "password",       "publisher_lock",
                                          "system_info"};
  EXPECT_EQ(keys, stock);
}

TEST_F(MapSessionPinLogin, TheLoginCarriesStagedAndMountedMaps) {
  sp::ChannelView view;
  view.staged = {Pin("arena2", "1.0", kSourceA), Pin("dock", "2.0", kSourceB)};
  sp::SetChannelView(view);
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});

  const nlohmann::json entries = sp::LoginEntries();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0]["id"], "arena2");
  EXPECT_EQ(entries[0]["mounted"], true);
  EXPECT_EQ(entries[1]["id"], "dock");
  EXPECT_EQ(entries[1]["mounted"], false);

  nevr_login_profile::LoginProfileInputs inputs;
  inputs.maps = entries;
  EXPECT_EQ(nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs))["nevr_maps"], entries);
}

// The PC notice goes into the net game's error block, never into echovr.exe's image (F1). WriteNotice passes NoticeBaseFor(g_pGame)
// to WriteErrorBlock, so this test fails on a base that is the image (the old GameBase() choice).
TEST(MapSessionPinNotice, TheNoticeIsWrittenIntoTheNetGameNotTheImage) {
  namespace notice = nevr_map_session_pin_notice;
  namespace layout = nevr_lobby_failure_message;
  const std::size_t contextBytes = nevr_patch_addresses::CONTEXT_NETGAME_OFFSET + sizeof(void*);
  std::vector<std::uint8_t> context(contextBytes, 0);
  const std::size_t blockEnd = layout::kErrorBlockOffset + 1 + layout::kErrorLines * layout::kErrorLineBytes;
  std::vector<std::uint8_t> netGame(blockEnd, 0);
  void* net = netGame.data();
  std::memcpy(context.data() + nevr_patch_addresses::CONTEXT_NETGAME_OFFSET, &net, sizeof(net));

  std::uint8_t* base = notice::NoticeBaseFor(context.data());
  ASSERT_EQ(static_cast<void*>(base), static_cast<void*>(netGame.data()));
  EXPECT_NE(static_cast<void*>(base), static_cast<void*>(GetModuleHandleW(nullptr)));

  const char* title = "Map version mismatch";
  const char* line = "Needs arena2 (not your 1.0); updating for next launch.";
  ASSERT_TRUE(layout::WriteErrorBlock(base, title, line, std::strlen(line) + 1));
  EXPECT_EQ(netGame[layout::kErrorBlockOffset], layout::kErrorFlagFourLines);
  EXPECT_EQ(std::memcmp(netGame.data() + layout::kErrorBlockOffset + 1, title, std::strlen(title)), 0);
  EXPECT_EQ(std::memcmp(netGame.data() + layout::kErrorBlockOffset + 1 + layout::kErrorLineBytes, line, std::strlen(line)), 0);
}

// The entry check of the owner-JSON getter (the PC's stock prologue, CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE), on a buffer. A stock entry
// passes; a jmp in front of a stock tail passes (EntryOk); a changed byte fails, and the record names the VA and both byte runs with
// the expected bytes first (F4, logging.md Rule 6).
const std::uint8_t kGetterHead[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x81, 0xc1, 0x10, 0x03, 0x00, 0x00};
constexpr std::uintptr_t kEntryRva = 0x10;
constexpr std::size_t kImageTail = 8;
constexpr std::size_t kChangedByte = 3;  // a byte of the head after the first, so the jmp rule does not see it

std::vector<std::uint8_t> ImageWithHead() {
  std::vector<std::uint8_t> image(kEntryRva + sizeof(kGetterHead) + kImageTail, 0);
  std::memcpy(image.data() + kEntryRva, kGetterHead, sizeof(kGetterHead));
  return image;
}

const std::string& FieldValue(const nevr_map_session_pin::PinFields& fields, const char* key) {
  for (const auto& field : fields)
    if (field.first == key) return field.second;
  static const std::string kMissing = "(missing)";
  return kMissing;
}

TEST(MapSessionPinEntry, AStockEntryPasses) {
  std::vector<std::uint8_t> image = ImageWithHead();
  nevr_map_session_pin::PinFields refusal;
  EXPECT_TRUE(nevr_map_session_pin_entry::CheckEntry(image.data(), kEntryRva, kGetterHead, sizeof(kGetterHead), "r", "n", refusal));
  EXPECT_TRUE(refusal.empty());
}

TEST(MapSessionPinEntry, AJmpInFrontOfAStockTailPasses) {
  std::vector<std::uint8_t> image = ImageWithHead();
  image[kEntryRva] = nevr_prologue_head::kJmpOpcode;
  nevr_map_session_pin::PinFields refusal;
  EXPECT_TRUE(nevr_map_session_pin_entry::CheckEntry(image.data(), kEntryRva, kGetterHead, sizeof(kGetterHead), "r", "n", refusal));
  EXPECT_TRUE(refusal.empty());
}

TEST(MapSessionPinEntry, AChangedByteFailsAndTheRecordNamesTheEntry) {
  std::vector<std::uint8_t> image = ImageWithHead();
  image[kEntryRva + kChangedByte] ^= 0xff;
  nevr_map_session_pin::PinFields refusal;
  EXPECT_FALSE(nevr_map_session_pin_entry::CheckEntry(image.data(), kEntryRva, kGetterHead, sizeof(kGetterHead),
                                                      "owner_getter_mismatch", "CNSLobby::OwnerJson_GetSymbol", refusal));
  EXPECT_EQ(FieldValue(refusal, "reason"), "owner_getter_mismatch");
  EXPECT_EQ(FieldValue(refusal, "name"), "CNSLobby::OwnerJson_GetSymbol");
  EXPECT_EQ(FieldValue(refusal, "va"),
            nevr_map_session_pin::HexText(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.data() + kEntryRva))));
  EXPECT_EQ(FieldValue(refusal, "expected"), nevr_map_session_pin::HexBytesText(kGetterHead, sizeof(kGetterHead)));
  EXPECT_EQ(FieldValue(refusal, "actual"),
            nevr_map_session_pin::HexBytesText(image.data() + kEntryRva, sizeof(kGetterHead)));
  EXPECT_NE(FieldValue(refusal, "expected"), FieldValue(refusal, "actual"));
}

// No net game in the context: the base is null, and WriteErrorBlock declines (so the core records notice_not_written).
TEST(MapSessionPinNotice, NoNetGameMeansNoBaseAndNoWrite) {
  namespace notice = nevr_map_session_pin_notice;
  namespace layout = nevr_lobby_failure_message;
  std::vector<std::uint8_t> context(nevr_patch_addresses::CONTEXT_NETGAME_OFFSET + sizeof(void*), 0);
  EXPECT_EQ(notice::NoticeBaseFor(context.data()), nullptr);
  EXPECT_EQ(notice::NoticeBaseFor(nullptr), nullptr);
  const char* message = "m";
  EXPECT_FALSE(layout::WriteErrorBlock(notice::NoticeBaseFor(nullptr), "t", message, std::strlen(message) + 1));
}

}  // namespace
