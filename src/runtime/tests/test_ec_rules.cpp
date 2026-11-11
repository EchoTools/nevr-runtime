// The EC network client's rules (patch/ec_rules.h, #2358). The hooks that feed them are runtime/combat/veilnet.cpp (PC: Hooked_recvfrom,
// Hooked_sendto, Handle's client branch, Tick's use watcher, SawLog, TacModsGuarded) and quest/combat/ec.cpp (Quest); the sockets, the
// game's lists and the page need the game and are proven by a live run. The Quest host test (quest/tests/ec_test.cpp) compiles this same
// header. The tac-mod list is laid out here as the game lays it out ({ allocator, entries, count, capacity }, entries of 0x88 bytes).
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/patch/ec_rules.h"

namespace {

namespace ecr = nevr_ec;
using State = ecr::BasicState<std::uintptr_t>;
using ecr::ClientAct;

ecr::Packet Pkt(char type, std::uint32_t id) { return ecr::Packet{ecr::kMagic, static_cast<std::uint8_t>(type), {0, 0, 0}, id, ecr::kMagic2}; }

struct alignas(8) Page {
  std::uint8_t b[ecr::kPageSize] = {};
  std::uint32_t U32(std::uint32_t at) const { return ecr::Load32(b + at); }
  std::uint64_t U64(std::uint32_t at) const { return ecr::Load64(b + at); }
};

// ---- the packet

TEST(EcRules, ThePacketIsSixteenBytesWithBothMagics) {
  const ecr::Packet p = Pkt('V', 5);
  ecr::Packet out{};
  EXPECT_TRUE(ecr::Ours(&p, 16, &out));
  EXPECT_EQ(out.id, 5u);
  EXPECT_FALSE(ecr::Ours(&p, 15, &out));
  EXPECT_FALSE(ecr::Ours(&p, 17, &out));
  ecr::Packet bad = p;
  bad.magic ^= 1;
  EXPECT_FALSE(ecr::Ours(&bad, 16, &out));
  bad = p;
  bad.magic2 ^= 1;
  EXPECT_FALSE(ecr::Ours(&bad, 16, &out));
  EXPECT_EQ(ecr::kMagic, 0x4c564345u);   // "ECVL"
  EXPECT_EQ(ecr::kMagic2, 0x4c494556u);  // "VEIL"
}

// D1, D2: a type v2.0.0's wire does not carry is not ours (it goes to the game); every type it carries is, match control's included.
TEST(EcRules, OnlyTheTwelveTypesTheWireCarriesAreOurs) {
  ecr::Packet out{};
  for (char t : {'H', 'A', 'V', 'P', 'T', 'O', 'K', 'M', 'C', 'L', 'R', 'G'}) {
    const ecr::Packet p = Pkt(t, 1);
    EXPECT_TRUE(ecr::Ours(&p, 16, &out)) << "type " << t;
  }
  for (char t : {'X', 'Z', 'h', 'a', '\0'}) {
    const ecr::Packet p = Pkt(t, 1);
    EXPECT_FALSE(ecr::Ours(&p, 16, &out)) << "type " << static_cast<int>(t);
  }
}

// The receive hook's test: a peek, no sender length, a short buffer and the per-call cap each hand the datagram to the game.
TEST(EcRules, TheReceiveHookTakesOutOnlyAWholeUnpeekedPacket) {
  const ecr::Packet p = Pkt('V', 5);
  ecr::Packet out{};
  EXPECT_TRUE(ecr::TakeOut(&p, 16, 16, false, true, 0, &out));
  EXPECT_FALSE(ecr::TakeOut(&p, 16, 16, true, true, 0, &out));
  EXPECT_FALSE(ecr::TakeOut(&p, 16, 16, false, false, 0, &out));
  EXPECT_FALSE(ecr::TakeOut(&p, 16, 15, false, true, 0, &out));
  EXPECT_FALSE(ecr::TakeOut(&p, -1, 16, false, true, 0, &out));
}

// D3: one call takes out at most 256 in a row; the 257th is handed to the game.
TEST(EcRules, TheTwoHundredFiftySeventhInOneCallGoesToTheGame) {
  const ecr::Packet p = Pkt('V', 5);
  ecr::Packet out{};
  int taken = 0;
  for (int consumed = 0; consumed < 1000; ++consumed) {
    if (!ecr::TakeOut(&p, 16, 1500, false, true, consumed, &out)) break;
    ++taken;
  }
  EXPECT_EQ(taken, 256);
  EXPECT_EQ(ecr::kMaxConsumePerCall, 256);
}

// D3, counted (#2358): the datagram TakeOut refuses only for the cap is ours and is counted (Capped); below the cap it is taken out.
// A datagram the rest of TakeOut refuses is not ours to count.
TEST(EcRules, TheDatagramPastTheCapIsCountedNotTakenOut) {
  const ecr::Packet p = Pkt('V', 5);
  ecr::Packet out{};
  for (int consumed = 0; consumed <= 300; ++consumed) {
    const bool taken = ecr::TakeOut(&p, 16, 1500, false, true, consumed, &out);
    EXPECT_EQ(taken, consumed < 256) << consumed;
    EXPECT_EQ(ecr::Capped(&p, 16, 1500, false, true, consumed), consumed >= 256) << consumed;
  }
  EXPECT_FALSE(ecr::Capped(&p, 16, 1500, true, true, 256));    // a peek is passed through, not ours to count
  EXPECT_FALSE(ecr::Capped(&p, 16, 1500, false, false, 256));  // no sender length: passed through untouched
  EXPECT_FALSE(ecr::Capped(&p, 16, 15, false, true, 256));     // a buffer too short for a packet
  EXPECT_FALSE(ecr::Capped(&p, 15, 1500, false, true, 256));   // not a whole packet
  const ecr::Packet bad = Pkt('X', 5);
  EXPECT_FALSE(ecr::Capped(&bad, 16, 1500, false, true, 256));  // not ours
}

// ---- AddToken

TEST(EcRules, ATokenBlockCountsUpWithinItsLifeAndStartsAgainAfter) {
  Page pg;
  std::uint8_t* tok = pg.b + ecr::kVeilTok;
  ecr::AddToken(tok, 100);
  ecr::AddToken(tok, 200);
  EXPECT_EQ(pg.U32(ecr::kVeilTok), 2u);
  EXPECT_EQ(pg.U64(ecr::kVeilTok + 8), 200 + ecr::kTokenLife);
  EXPECT_EQ(ecr::kTokenLife, 15000000u);  // 1.5 s in 100 ns
  ecr::AddToken(tok, 200 + ecr::kTokenLife);  // at its expiry: over
  EXPECT_EQ(pg.U32(ecr::kVeilTok), 1u);
  ecr::AddToken(tok, 400 + 3 * ecr::kTokenLife);  // past it
  EXPECT_EQ(pg.U32(ecr::kVeilTok), 1u);
  EXPECT_EQ(pg.U64(ecr::kVeilTok + 8), 400 + 4 * ecr::kTokenLife);
}

TEST(EcRules, ThePageOffsetsAreEchovrsRvas) {
  EXPECT_EQ(ecr::kPageRva + ecr::kVeilTok, 0x2104cc0u);
  EXPECT_EQ(ecr::kPageRva + ecr::kVeilUse, 0x2104f00u);
  EXPECT_EQ(ecr::kPageRva + ecr::kPointTok, 0x2104ce0u);
  EXPECT_EQ(ecr::kPageRva + ecr::kPointUse, 0x2104f80u);
  EXPECT_EQ(ecr::kPageRva + ecr::kLocTok, 0x2104d20u);
  EXPECT_EQ(ecr::kPageRva + ecr::kLocUse, 0x2104d40u);
  EXPECT_EQ(ecr::kPageRva + ecr::kOcTok, 0x2104ca8u);
  EXPECT_EQ(ecr::kPageRva + ecr::kOcUse, 0x2104ca4u);
  EXPECT_EQ(ecr::kPageRva + ecr::kEquipSel, 0x2104e80u);
  EXPECT_EQ(ecr::kPageRva + ecr::kEquipReruns, 0x2104ea0u);
}

// ---- FirstTime

TEST(EcRules, FirstTimeRemembersTheLastThirtyTwoIds) {
  State st;
  EXPECT_FALSE(st.FirstTime(0));  // zero-filled: id 0 is never first
  EXPECT_TRUE(st.FirstTime(7));
  EXPECT_FALSE(st.FirstTime(7));
  State sr;
  for (std::uint32_t i = 1; i <= 33; ++i) EXPECT_TRUE(sr.FirstTime(i));
  EXPECT_TRUE(sr.FirstTime(1));    // the 33rd pushed out the first
  EXPECT_FALSE(sr.FirstTime(33));
}

// ---- the client branch

TEST(EcRules, AKillGoesToTheDroneModuleOnce) {
  Page pg;
  State st(ecr::kAllTypes);
  const ecr::ClientResult r = st.Client(pg.b, Pkt('K', 0x00030009), 1);
  EXPECT_EQ(r.act, ClientAct::kKill);
  EXPECT_EQ(r.drone, 9u);
  EXPECT_EQ(st.Client(pg.b, Pkt('K', 0x00030009), 2).act, ClientAct::kNone);
  EXPECT_EQ(st.Handle(pg.b, Pkt('K', 0x00040011), 3), 0x11u + 1);
  EXPECT_EQ(st.kills, 2u);
}

TEST(EcRules, EachCarriedTypeGivesOneTokenOnItsOwnBlock) {
  const struct {
    char type;
    std::uint32_t block;
  } kCases[] = {{'V', ecr::kVeilTok}, {'P', ecr::kPointTok}, {'T', ecr::kLocTok}, {'O', ecr::kOcTok}};
  std::uint32_t id = 100;
  for (const auto& c : kCases) {
    Page pg;
    State st(ecr::kAllTypes);
    EXPECT_EQ(st.Client(pg.b, Pkt(c.type, id), 10).act, ClientAct::kToken) << c.type;
    EXPECT_EQ(st.Client(pg.b, Pkt(c.type, id), 11).act, ClientAct::kNone) << c.type;  // the resend
    for (std::uint32_t b : {ecr::kVeilTok, ecr::kPointTok, ecr::kLocTok, ecr::kOcTok})
      EXPECT_EQ(pg.U32(b), b == c.block ? 1u : 0u) << c.type << " block " << b;
    ++id;
  }
}

// D4: a carried type outside the mask is dropped: no token.
TEST(EcRules, ATypeOutsideTheMaskIsDroppedWithoutAToken) {
  Page pg;
  State st(1u << 2);  // 'T' only
  EXPECT_EQ(st.Client(pg.b, Pkt('V', 500), 1).act, ClientAct::kDropped);
  EXPECT_EQ(pg.U32(ecr::kVeilTok), 0u);
  EXPECT_EQ(st.dropped, 1u);
  EXPECT_EQ(st.Client(pg.b, Pkt('T', 501), 1).act, ClientAct::kToken);
  EXPECT_TRUE(st.Carries('T'));
  EXPECT_FALSE(st.Carries('V'));
  EXPECT_FALSE(st.Carries('Z'));
}

// D5 (v2.0.0): an 'M' acts once per id, and takes one of the ring's slots like any other id.
TEST(EcRules, AMedigunActsOncePerIdAndTakesARingSlot) {
  Page pg;
  State st(ecr::kAllTypes);
  EXPECT_EQ(st.Client(pg.b, Pkt('M', 1), 1).act, ClientAct::kMedigun);
  EXPECT_EQ(st.Client(pg.b, Pkt('M', 1), 2).act, ClientAct::kNone);
  EXPECT_EQ(st.medi, 1u);
  // a carried id, then 31 'M's: the carried id is pushed out, so its late resend gives a second token
  State sr(ecr::kAllTypes);
  EXPECT_EQ(sr.Client(pg.b, Pkt('V', 1000), 3).act, ClientAct::kToken);
  for (std::uint32_t i = 0; i < 31; ++i) EXPECT_EQ(sr.Client(pg.b, Pkt('M', 2000 + i), 3).act, ClientAct::kMedigun);
  EXPECT_EQ(sr.Client(pg.b, Pkt('V', 1000), 4).act, ClientAct::kNone);  // 32 ids: still remembered
  EXPECT_EQ(sr.Client(pg.b, Pkt('M', 3000), 4).act, ClientAct::kMedigun);
  EXPECT_EQ(sr.Client(pg.b, Pkt('V', 1000), 5).act, ClientAct::kToken);  // the 33rd pushed it out
}

// #2333: an 'A' opens nothing; the hello and match control are not the client branch's.
TEST(EcRules, AnAnswerIsCountedAndTheOtherTypesAreNotTheBranchs) {
  Page pg;
  State st(ecr::kAllTypes);
  EXPECT_EQ(st.Client(pg.b, Pkt('A', 1), 1).act, ClientAct::kAnswer);
  EXPECT_EQ(st.answers, 1u);
  for (char t : {'H', 'C', 'L', 'R', 'G'}) EXPECT_EQ(st.Client(pg.b, Pkt(t, 50), 1).act, ClientAct::kNone) << t;
  for (std::size_t i = 0; i < sizeof pg.b; ++i) ASSERT_EQ(pg.b[i], 0u) << "the page was written at " << i;
}

// ---- the local uses

TEST(EcRules, ALocalUseIsAnIdATokenExceptTeleportAndOnePacket) {
  Page pg;
  State st(ecr::kAllTypes);
  std::uint32_t seen[4] = {};
  ecr::Use uses[4];
  EXPECT_EQ(ecr::WatchUses(st, pg.b, seen, 10, 1, 1000, uses), 0);
  ecr::Store32(pg.b + ecr::kVeilUse, 3);
  ecr::Store32(pg.b + ecr::kLocUse, 1);
  ASSERT_EQ(ecr::WatchUses(st, pg.b, seen, 10, 7, 1000, uses), 2);
  EXPECT_EQ(uses[0].type, 'V');
  EXPECT_EQ(uses[0].id, 10u * 2654435761u ^ 7u << 16 ^ 3u ^ 0u);
  EXPECT_EQ(uses[1].type, 'T');
  EXPECT_EQ(uses[1].id, 10u * 2654435761u ^ 7u << 16 ^ 1u ^ 2u);
  EXPECT_EQ(pg.U32(ecr::kVeilTok), 1u);
  EXPECT_EQ(pg.U32(ecr::kLocTok), 0u);  // the teleporting machine shows its own
  EXPECT_FALSE(st.FirstTime(uses[0].id));  // remembered: its echo from the server gives no second token
  EXPECT_FALSE(st.FirstTime(uses[1].id));
  EXPECT_EQ(ecr::WatchUses(st, pg.b, seen, 11, 7, 1001, uses), 0);  // seen once
  ecr::Store32(pg.b + ecr::kPointUse, 1);
  ecr::Store32(pg.b + ecr::kOcUse, 1);
  ASSERT_EQ(ecr::WatchUses(st, pg.b, seen, 12, 7, 1002, uses), 2);
  EXPECT_EQ(uses[0].type, 'P');
  EXPECT_EQ(uses[1].type, 'O');
  EXPECT_EQ(pg.U32(ecr::kPointTok), 1u);
  EXPECT_EQ(pg.U32(ecr::kOcTok), 1u);
}

// D4: a local use of a type outside the mask is seen but neither tokened nor sent.
TEST(EcRules, ALocalUseOutsideTheMaskIsNotSent) {
  Page pg;
  State st(1u << 2);  // 'T' only
  std::uint32_t seen[4] = {};
  ecr::Use uses[4];
  ecr::Store32(pg.b + ecr::kVeilUse, 1);
  EXPECT_EQ(ecr::WatchUses(st, pg.b, seen, 10, 1, 1000, uses), 0);
  EXPECT_EQ(pg.U32(ecr::kVeilTok), 0u);
  EXPECT_EQ(seen[0], 1u);
}

// WatchUses counts each token it gives: the veil's use is tokened, the teleport's is shown, not tokened, and not counted.
TEST(EcRules, ALocalUseCountsTheTokensItGives) {
  Page pg;
  State st(ecr::kAllTypes);
  std::uint32_t seen[4] = {};
  ecr::Use uses[4];
  ecr::Store32(pg.b + ecr::kVeilUse, 1);
  ecr::Store32(pg.b + ecr::kLocUse, 1);
  ASSERT_EQ(ecr::WatchUses(st, pg.b, seen, 10, 1, 1000, uses), 2);
  EXPECT_EQ(st.tokens, 1u);
}

// ---- the destinations and the server pick

TEST(EcRules, ANewDestinationTakesAFreeSlotThenTheLeastUsed) {
  State st;
  st.SawSend(7, 0x0a000001, 6792);
  EXPECT_EQ(st.best, 0);  // the first destination is the server until the pick
  st.SawSend(7, 0x0a000001, 6792);
  st.SawSend(8, 0x0a000002, 53);
  EXPECT_EQ(st.dests[1].count, 1);
  st.SawSend(7, 0x0a000001, 6793);  // another port: another destination
  EXPECT_EQ(st.dests[2].port, 6793);
  State sd;
  for (std::uint32_t i = 0; i < 16; ++i) sd.SawSend(1, 100 + i, 1);
  sd.SawSend(1, 100, 1);  // slot 0 now has 2
  sd.SawSend(1, 999, 1);  // no free slot: the least used, slot 1, is replaced
  EXPECT_EQ(sd.dests[1].addr, 999u);
  EXPECT_EQ(sd.dests[1].count, 1);
  EXPECT_EQ(sd.best, 0);
  State sb;
  for (std::uint32_t i = 0; i < 16; ++i) sb.SawSend(1, 100 + i, 1);
  sb.SawSend(1, 999, 1);  // the server's own slot is the least used: the server is forgotten, the new one is it
  EXPECT_EQ(sb.dests[0].addr, 999u);
  EXPECT_EQ(sb.best, 0);
}

TEST(EcRules, TheBusiestDestinationOfASecondIsTheServer) {
  State st;
  st.SawSend(7, 0x0a000001, 6792);
  st.SawSend(8, 0x0a000002, 53);
  st.SawSend(8, 0x0a000002, 53);
  st.PickServer();
  EXPECT_EQ(st.best, 1);
  EXPECT_EQ(st.dests[1].count, 1);  // the server keeps 1
  EXPECT_EQ(st.dests[0].count, 0);
  st.PickServer();  // no sends: the server stays
  EXPECT_EQ(st.best, 1);
  ecr::BasicDest<std::uintptr_t> d{};
  ASSERT_TRUE(st.Server(&d));
  EXPECT_EQ(d.sock, 8u);
  EXPECT_EQ(d.addr, 0x0a000002u);
  EXPECT_EQ(d.port, 53);
  State none;
  EXPECT_FALSE(none.Server(&d));
}

// v2.0.0 keys a destination by its socket as well as by its address and port: the same address on another socket is another one.
TEST(EcRules, ADestinationIsKeyedBySocketAsWellAsAddressAndPort) {
  State st;
  st.SawSend(7, 0x0a000001, 6792);
  st.SawSend(8, 0x0a000001, 6792);
  st.SawSend(8, 0x0a000001, 6792);
  EXPECT_EQ(st.dests[0].sock, 7u);
  EXPECT_EQ(st.dests[0].count, 1);
  EXPECT_EQ(st.dests[1].sock, 8u);
  EXPECT_EQ(st.dests[1].count, 2);
  st.PickServer();
  ecr::BasicDest<std::uintptr_t> d{};
  ASSERT_TRUE(st.Server(&d));
  EXPECT_EQ(d.sock, 8u);
}

// ---- the parse

TEST(EcRules, EachTacModNameGivesItsSymbol) {
  const struct {
    const char* name;
    std::uint64_t sym;
  } kNames[] = {{"smoke", 0xe32dc7c9d4804fb5ull}, {"point", 0xe32dc7cad6864aa4ull}, {"buff", 0xc8c33e48386618b6ull},
                {"heal", 0xc8c33e4832761fbcull},  {"shield", 0x41d2d43513260b1aull}, {"sensor", 0x41d2d4381430080cull},
                {"wraith", 0x41d2d02f1b2a1316ull}};
  for (const auto& n : kNames) {
    char json[64];
    std::snprintf(json, sizeof json, "{\"ability\":\"%s\",\"x\":1}", n.name);
    char name[24];
    EXPECT_EQ(ecr::SelectedFromJson(json, name, sizeof name), n.sym) << n.name;
    EXPECT_STREQ(name, n.name);
  }
  EXPECT_EQ(ecr::SelectedFromJson("{\"ability\":\"nonsense\"}"), 0u);
  EXPECT_EQ(ecr::SelectedFromJson("{\"abilit\":\"smoke\"}"), 0u);
  EXPECT_EQ(ecr::SelectedFromJson("{\"ability\":\"smoke"), 0u);
  EXPECT_EQ(ecr::SelectedFromJson("{\"ability\":\"smokee\"}"), 0u);
  EXPECT_EQ(ecr::SelectedFromJson("{\"ability\":\"smok\"}"), 0u);
  EXPECT_EQ(ecr::SelectedFromJson(nullptr), 0u);
}

TEST(EcRules, OnlyTheProfileUpdateFormatIsRead) {
  EXPECT_TRUE(ecr::IsProfileFmt("[NSUSER] sent profile update: %s"));
  EXPECT_FALSE(ecr::IsProfileFmt("[NSUSER] sent profile update"));
  EXPECT_FALSE(ecr::IsProfileFmt("[NETLOBBY] Starting session (gametype %s"));
  EXPECT_FALSE(ecr::IsProfileFmt(nullptr));
}

TEST(EcRules, AStoredSelectionClearsTheReRunsFirst) {
  Page pg;
  ecr::Store32(pg.b + ecr::kEquipReruns, 9);
  ecr::StoreSelected(pg.b, 0xe32dc7c9d4804fb5ull);
  EXPECT_EQ(pg.U64(ecr::kEquipSel), 0xe32dc7c9d4804fb5ull);
  EXPECT_EQ(pg.U32(ecr::kEquipReruns), 0u);
}

TEST(EcRules, OursAreSmokePointAndBuff) {
  EXPECT_TRUE(ecr::OurSelected(0xe32dc7c9d4804fb5ull));
  EXPECT_TRUE(ecr::OurSelected(0xe32dc7cad6864aa4ull));
  EXPECT_TRUE(ecr::OurSelected(0xc8c33e48386618b6ull));
  EXPECT_FALSE(ecr::OurSelected(0xc8c33e4832761fbcull));
  EXPECT_FALSE(ecr::OurSelected(0));
}

// ---- the tac-mod list

struct alignas(8) GameList {
  std::uint64_t allocator = 0;
  std::uintptr_t entries = 0;
  std::uint64_t count = 0, capacity = 0;
};

// A list of n entries; ours (smoke, buff, point) last when ours. The memory the Env reads; `after` moves the count once the count has been
// read (the game writing between the read and the write).
struct Fixture {
  std::vector<std::array<char, ecr::kTacEntry>> entries;
  GameList list;
  std::uint64_t after = 0;
  bool moved = false;
  explicit Fixture(int n, bool ours = true) : entries(16) {
    for (auto& e : entries) e.fill(0);
    for (int i = 0; i < n; ++i) std::snprintf(entries[static_cast<std::size_t>(i)].data(), 64, "stock%d", i);
    if (ours && n >= 3) {
      std::snprintf(entries[static_cast<std::size_t>(n - 3)].data(), 64, "smoke");
      std::snprintf(entries[static_cast<std::size_t>(n - 2)].data(), 64, "buff");
      std::snprintf(entries[static_cast<std::size_t>(n - 1)].data(), 64, "point");
    }
    list.entries = reinterpret_cast<std::uintptr_t>(entries.data());
    list.count = static_cast<std::uint64_t>(n);
    list.capacity = 16;
  }
  std::uintptr_t L() const { return reinterpret_cast<std::uintptr_t>(&list); }
  bool Read(std::uintptr_t a, void* out, std::size_t n) {
    std::memcpy(out, reinterpret_cast<const void*>(a), n);
    if (a == L() + 0x10 && after != 0 && !moved) {
      moved = true;
      list.count = after;
    }
    return true;
  }
};

int Look(ecr::TacList& st, Fixture& f, bool serverHasIt, std::uint64_t* n, bool keepWhole = false, bool settled = true,
         std::uint32_t gen = 0) {
  return ecr::TacModsFor(st, f, f.L(), serverHasIt, true, n, gen, keepWhole, settled);
}

TEST(EcRules, WithoutEchoCombatTheThreeAreHiddenAndWithItShown) {
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n), 2);
  EXPECT_EQ(f.list.count, 4u);
  EXPECT_EQ(n, 4u);
  EXPECT_EQ(Look(st, f, false, &n), 0);  // already right
  EXPECT_EQ(Look(st, f, true, &n), 1);
  EXPECT_EQ(f.list.count, 7u);
  EXPECT_EQ(Look(st, f, true, &n), 0);
}

TEST(EcRules, TheDecisionWithoutAWriteOnlyTells) {
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(ecr::TacModsFor(st, f, f.L(), false, false, &n, 0, false, true), 2);
  EXPECT_EQ(f.list.count, 7u);
  EXPECT_EQ(n, 4u);
}

// The plan is the decision before any cap of the caller's: it is ok, and wants the count, also when the count is already right.
TEST(EcRules, ThePlanIsMadeAlsoWhenTheCountIsRight) {
  Fixture f(7);
  ecr::TacList st;
  const ecr::TacPlan plan = ecr::PlanTacMods(st, f, f.L(), true, 0, false, true);
  EXPECT_TRUE(plan.ok);
  EXPECT_EQ(plan.want, 7u);
  EXPECT_EQ(plan.count, 7u);
  EXPECT_EQ(plan.entries, f.list.entries);
  EXPECT_EQ(plan.result, 1);
  std::uint64_t n = 0;
  EXPECT_EQ(ecr::WriteTacMods(f.L(), plan, 4, true, &n), 1);  // a caller's own cap is written
  EXPECT_EQ(f.list.count, 4u);
}

TEST(EcRules, ANewEntriesPointerStartsTheFullCountAgain) {
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  Look(st, f, false, &n);
  EXPECT_EQ(st.full, 7u);
  Fixture g(5);
  f.list.entries = g.list.entries;
  f.list.count = 5;
  EXPECT_EQ(Look(st, f, false, &n), 2);
  EXPECT_EQ(f.list.count, 2u);
  EXPECT_EQ(st.full, 5u);
  f.list.count = 3;  // the same list rebuilt shorter: only a new gen tells
  Look(st, f, false, &n, false, true, 0);
  EXPECT_EQ(st.full, 5u);
  f.list.count = 3;
  Look(st, f, false, &n, false, true, 1);
  EXPECT_EQ(st.full, 3u);
}

TEST(EcRules, NothingIsTouchedUnlessTheListIsOurs) {
  std::uint64_t n = 0;
  {
    Fixture f(2, false);
    ecr::TacList st;
    EXPECT_EQ(Look(st, f, false, &n), 0);  // fewer than 3
  }
  {
    // two entries whose preceding bytes read as smoke: not a list of ours (full - 3 would wrap)
    Fixture f(3);
    f.list.entries += ecr::kTacEntry;
    f.list.count = 2;
    ecr::TacList st;
    EXPECT_EQ(Look(st, f, false, &n), 0);
    EXPECT_EQ(f.list.count, 2u);
  }
  {
    Fixture f(7);
    f.list.capacity = 6;  // a count above capacity
    ecr::TacList st;
    EXPECT_EQ(Look(st, f, false, &n), 0);
    EXPECT_EQ(f.list.count, 7u);
  }
  {
    Fixture f(7);
    std::snprintf(f.entries[5].data(), 64, "buffx");  // not ours at the end
    ecr::TacList st;
    EXPECT_EQ(Look(st, f, false, &n), 0);
    EXPECT_EQ(f.list.count, 7u);
  }
  {
    Fixture f(7);
    std::snprintf(f.entries[4].data(), 64, "buff");  // ours, in another order
    std::snprintf(f.entries[5].data(), 64, "smoke");
    ecr::TacList st;
    EXPECT_EQ(Look(st, f, false, &n), 0);
  }
  {
    Fixture f(7);
    f.list.entries = 0;
    ecr::TacList st;
    EXPECT_EQ(Look(st, f, false, &n), 0);
    EXPECT_EQ(ecr::TacModsFor(st, f, 0, false, true, &n, 0, false, true), 0);
  }
}

// D7: a capacity over 4096 is not a list.
TEST(EcRules, AnImplausibleCapacityRefuses) {
  Fixture f(7);
  f.list.capacity = 4097;
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n), 0);
  EXPECT_EQ(f.list.count, 7u);
  Fixture g(7);
  g.list.capacity = 4096;
  ecr::TacList sg;
  EXPECT_EQ(Look(sg, g, false, &n), 2);
}

// D8: a count the game moves between the read and the write is left as the game wrote it.
TEST(EcRules, ACountThatMovedSinceTheReadIsLeftAlone) {
  Fixture f(7);
  f.after = 6;
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n), 0);
  EXPECT_EQ(f.list.count, 6u);
  EXPECT_EQ(Look(st, f, false, &n), 2);  // the next look decides again
  EXPECT_EQ(f.list.count, 4u);
}

// D9: without EchoCombat, a cut waits until the selection has been still for 2000 ms; putting the list back does not wait.
TEST(EcRules, ACutWaitsForTheSelectionToSettle) {
  ecr::SelectionSettle settle;
  EXPECT_TRUE(settle.Look(0, 5000));  // nothing selected since the start
  EXPECT_FALSE(settle.Look(0xc8c33e4832761fbcull, 10000));
  EXPECT_FALSE(settle.Look(0xc8c33e4832761fbcull, 11999));
  EXPECT_TRUE(settle.Look(0xc8c33e4832761fbcull, 12000));
  EXPECT_FALSE(settle.Look(0x41d2d4381430080cull, 12001));  // another change starts the wait again
  EXPECT_EQ(ecr::kHideSettleMs, 2000u);
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n, false, /*settled=*/false), 0);
  EXPECT_EQ(f.list.count, 7u);
  EXPECT_EQ(Look(st, f, false, &n, false, /*settled=*/true), 2);
  EXPECT_EQ(f.list.count, 4u);
  EXPECT_EQ(Look(st, f, true, &n, false, /*settled=*/false), 1);  // putting back does not wait
  EXPECT_EQ(f.list.count, 7u);
}

// D10: a list kept whole because one of ours is selected is result 3, not "hidden".
TEST(EcRules, AListKeptWholeForTheSelectionIsThree) {
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n, /*keepWhole=*/true), 0);  // whole already
  EXPECT_EQ(Look(st, f, false, &n), 2);
  EXPECT_EQ(Look(st, f, false, &n, /*keepWhole=*/true), 3);
  EXPECT_EQ(f.list.count, 7u);
  EXPECT_EQ(Look(st, f, true, &n, /*keepWhole=*/true), 0);
}

// The result when the server has EchoCombat is shown (1), also when the list is kept whole for a selection (3 is for a server without it).
TEST(EcRules, AShownListIsOneEvenWhenItIsKeptWholeForTheSelection) {
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n), 2);  // hidden: the full count (7) is remembered, the list holds 4
  EXPECT_EQ(f.list.count, 4u);
  EXPECT_EQ(Look(st, f, true, &n, /*keepWhole=*/true), 1);
  EXPECT_EQ(f.list.count, 7u);
}

// A repeated generation (the game rebuilt nothing) keeps the full count the list has had; a new generation forgets it.
TEST(EcRules, ARepeatedGenerationKeepsTheFullCount) {
  Fixture f(7);
  ecr::TacList st;
  std::uint64_t n = 0;
  EXPECT_EQ(Look(st, f, false, &n, false, true, /*gen=*/5), 2);  // a new generation: hidden
  EXPECT_EQ(f.list.count, 4u);
  EXPECT_EQ(st.gen, 5u);
  EXPECT_EQ(Look(st, f, true, &n, false, true, /*gen=*/5), 1);  // the same generation: the full count is remembered, shown
  EXPECT_EQ(f.list.count, 7u);
  EXPECT_EQ(st.full, 7u);
}

}  // namespace
