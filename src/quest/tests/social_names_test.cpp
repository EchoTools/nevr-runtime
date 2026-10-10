// Friend display names on Quest: the zstd profile decoder is compiled into the social package
// (social_names.cpp, with NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION) and registered by an explicit call, because
// the sentinel may carry no dynamic initializer. The profile reply is the real zstd frame the PC tests use.
//
// Run: social_names_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_frames.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"

namespace {

using namespace quest_social;

// A real zstd frame (zstd CLI) of {"displayname":"Bob","x":1}, as in src/runtime/tests/test_social_facade.cpp.
const unsigned char kBobFrame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x58, 0xd9, 0x00, 0x00, 0x7b, 0x22, 0x64, 0x69, 0x73,
                                   0x70, 0x6c, 0x61, 0x79, 0x6e, 0x61, 0x6d, 0x65, 0x22, 0x3a, 0x22, 0x42, 0x6f, 0x62,
                                   0x22, 0x2c, 0x22, 0x78, 0x22, 0x3a, 0x31, 0x7d, 0xb4, 0xfb, 0x07, 0x17};

std::string ProfileReply(std::uint64_t accountId, const std::string& frame) {
  std::string payload;
  SocialParty::AppendLe(payload, SocialNames::kPlatformOvrOrg, 8);
  SocialParty::AppendLe(payload, accountId, 8);
  SocialParty::AppendLe(payload, 27, 4);  // the length word the server writes before the stream
  return payload + frame;
}

std::string Bob() { return std::string(reinterpret_cast<const char*>(kBobFrame), sizeof(kBobFrame)); }

std::vector<SocialParty::Message> g_sent;
bool RecordingSend(const std::vector<SocialParty::Message>& messages) {
  for (const SocialParty::Message& m : messages) g_sent.push_back(m);
  return true;
}

std::string Le(std::uint64_t value, int bytes) {
  std::string out;
  SocialParty::AppendLe(out, value, bytes);
  return out;
}

void Feed(const Ports& ports, std::uint64_t symbol, const std::string& payload) {
  SocialParty::Message m;
  m.symbol = symbol;
  m.payload = payload;
  const std::string frame = SocialParty::Frame(m);
  ObserveFrames(ports, Direction::kServerToGame, reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size(), 1000);
}

// Nothing registers the decoder behind the caller's back: the Quest object carries no static initializer.
void TestNoStaticRegistration() { QCHECK(SocialNames::DecoderSlot().load() == nullptr); }

void TestExplicitRegistrationDecodesARealProfile() {
  SocialNames::RegisterDefaultDecoder();
  QCHECK(SocialNames::DecoderSlot().load() != nullptr);
  SocialNames::RegisterDefaultDecoder();  // twice is the same decoder
  const std::string reply = ProfileReply(695081603180789771ULL, Bob());
  std::uint64_t id = 0;
  std::string name;
  QCHECK(SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size(), &id, &name));
  QCHECK(id == 695081603180789771ULL);
  QCHECK(name == "Bob");
  const std::string cut = ProfileReply(1, Bob().substr(0, 12));
  QCHECK(!SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(cut.data()), cut.size(), &id, &name));
  const std::string junk = ProfileReply(1, "not a zstd frame at all");
  QCHECK(!SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size(), &id, &name));
  QCHECK(!SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(reply.data()), 10, &id, &name));
}

// The whole path: a friend appears, the profile is requested, the reply is read, the row shows the name.
void TestAFriendRowShowsTheNameFromTheProfileReply() {
  SocialParty::State party;
  SocialRoster::Roster friends;
  SocialRoster::RecentList recent;
  Ports ports;
  ports.party = &party;
  ports.friends = &friends;
  ports.recent = &recent;
  ports.send = &RecordingSend;
  SocialNames::GlobalResolver().Reset();
  Facade facade(ports);
  party.SetSelf(1001, "alice");
  Feed(ports, kSymFriendListResponse, Le(0, 8) + Le(0, 4) + Le(0, 4) + Le(1, 4) + Le(0, 4) + Le(0, 4) + Le(0, 4));
  Feed(ports, kSymFriendStatusNotify, Le(0, 8) + Le(2002, 8) + Le(0, 1) + Le(0, 7));
  std::size_t profileRequests = 0;
  for (const SocialParty::Message& m : g_sent) profileRequests += m.symbol == SocialNames::kProfileRequest ? 1 : 0;
  QCHECK(profileRequests == 1);  // asked, because a decoder is registered
  Feed(ports, SocialNames::kProfileSuccess, ProfileReply(2002, Bob()));
  void* obj = facade.Object();
  const std::uintptr_t* vtable = nullptr;
  std::memcpy(&vtable, obj, sizeof(vtable));
  using NameFn = const char* (*)(void*, std::uint32_t);
  NameFn friendName = nullptr;
  std::memcpy(&friendName, &vtable[kFriendName], sizeof(friendName));
  QCHECK(std::string(friendName(obj, 0)) == "Bob");
}

}  // namespace

int main() {
  TestNoStaticRegistration();
  TestExplicitRegistrationDecodesARealProfile();
  TestAFriendRowShowsTheNameFromTheProfileReply();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_names_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_names_test: all checks pass\n");
  return 0;
}
