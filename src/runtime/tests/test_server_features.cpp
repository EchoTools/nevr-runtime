// What a server states about itself and what a client reads of it (#2333, runtime/compat/server_features.h): the combat
// module's feature list, the server's key set in a lobby settings document, the client's read of one, the gate it
// decides and the state the gates read.
#include "runtime/compat/server_features.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using nevr_server_features::Client;
using nevr_server_features::ClientState;
using nevr_server_features::CombatFeatures;
using nevr_server_features::Gate;
using nevr_server_features::GateOf;
using nevr_server_features::ReadDocument;
using nevr_server_features::Statement;
using nevr_server_features::StateInDocument;

using Names = std::vector<std::string>;

Statement Read(const std::string& text) { return ReadDocument(text.data(), text.size()); }

TEST(ServerFeaturesCombat, NothingWithoutVeilNet) {
  EXPECT_EQ(CombatFeatures(false, true, true), Names{});
  EXPECT_EQ(CombatFeatures(false, false, false), Names{});
}

TEST(ServerFeaturesCombat, EchoCombatWhenVeilNetStarted) { EXPECT_EQ(CombatFeatures(true, false, true), Names{"echocombat"}); }

TEST(ServerFeaturesCombat, MedigunNeedsTheKeyAndOvercharge) {
  EXPECT_EQ(CombatFeatures(true, true, true), (Names{"echocombat", "echocombat_medigun"}));
  EXPECT_EQ(CombatFeatures(true, true, false), Names{"echocombat"});
  EXPECT_EQ(CombatFeatures(true, false, true), Names{"echocombat"});
}

TEST(ServerFeaturesNames, OnlyTheServicesPattern) {
  EXPECT_TRUE(nevr_server_features::ValidName("echocombat_medigun"));
  EXPECT_TRUE(nevr_server_features::ValidName("a1"));
  EXPECT_FALSE(nevr_server_features::ValidName(""));
  EXPECT_FALSE(nevr_server_features::ValidName("EchoCombat"));
  EXPECT_FALSE(nevr_server_features::ValidName("echo combat"));
  EXPECT_FALSE(nevr_server_features::ValidName("echo-combat"));
  EXPECT_FALSE(nevr_server_features::ValidName(std::string(65, 'a')));
  EXPECT_TRUE(nevr_server_features::ValidName(std::string(64, 'a')));
}

TEST(ServerFeaturesDocument, AloneWhenTheServiceSentNone) {
  std::string out;
  ASSERT_TRUE(StateInDocument("", {"echocombat"}, &out));
  EXPECT_EQ(out, R"({"nevr_server":{"features":["echocombat"]}})");
}

TEST(ServerFeaturesDocument, KeepsEveryKeyOfTheServices) {
  const std::string service = R"({"parties_unlocked":false,"allowed_levels":["mpl_arena_a"],"x":{"y":[1,2]}})";
  std::string out;
  ASSERT_TRUE(StateInDocument(service, {"echocombat", "echocombat_medigun"}, &out));
  const nlohmann::json doc = nlohmann::json::parse(out);
  const nlohmann::json in = nlohmann::json::parse(service);
  for (auto it = in.begin(); it != in.end(); ++it) EXPECT_EQ(doc.at(it.key()), it.value()) << it.key();
  EXPECT_EQ(doc.size(), in.size() + 1);
  EXPECT_EQ(doc.at("nevr_server"), nlohmann::json::parse(R"({"features":["echocombat","echocombat_medigun"]})"));
}

TEST(ServerFeaturesDocument, ReplacesAnyNevrServerKeyTheServiceWrote) {
  std::string out;
  ASSERT_TRUE(StateInDocument(R"({"nevr_server":{"features":["echocombat"],"other":1}})", {}, &out));
  EXPECT_EQ(nlohmann::json::parse(out).at("nevr_server"), nlohmann::json::parse(R"({"features":[]})"));
}

TEST(ServerFeaturesDocument, RefusesWhatItCannotStateSafely) {
  std::string out = "untouched";
  EXPECT_FALSE(StateInDocument("[1,2]", {"echocombat"}, &out));
  EXPECT_FALSE(StateInDocument("{not json", {"echocombat"}, &out));
  EXPECT_FALSE(StateInDocument("", {"Echo Combat"}, &out));
  EXPECT_FALSE(StateInDocument("", Names(33, "a"), &out));
  EXPECT_EQ(out, "untouched");
}

TEST(ServerFeaturesRead, TheServersStatement) {
  const Statement s = Read(R"({"parties_unlocked":true,"nevr_server":{"features":["echocombat","echocombat_medigun"]}})");
  EXPECT_EQ(s.kind, Statement::Kind::kStated);
  EXPECT_EQ(s.features, (Names{"echocombat", "echocombat_medigun"}));
  EXPECT_EQ(s.dropped, 0U);
  EXPECT_TRUE(s.Has("echocombat"));
  EXPECT_FALSE(s.Has("echo"));
}

TEST(ServerFeaturesRead, AStockServersDocumentStatesNothing) {
  EXPECT_EQ(Read(R"({"parties_unlocked":true,"allowed_levels":[]})").kind, Statement::Kind::kNotStated);
  EXPECT_EQ(Read("{}").kind, Statement::Kind::kNotStated);
}

TEST(ServerFeaturesRead, WrongShapesAreMalformed) {
  EXPECT_EQ(Read("").kind, Statement::Kind::kMalformed);
  EXPECT_EQ(Read("[]").kind, Statement::Kind::kMalformed);
  EXPECT_EQ(Read("{").kind, Statement::Kind::kMalformed);
  EXPECT_EQ(Read(R"({"nevr_server":["echocombat"]})").kind, Statement::Kind::kMalformed);
  EXPECT_EQ(Read(R"({"nevr_server":{}})").kind, Statement::Kind::kMalformed);
  EXPECT_EQ(Read(R"({"nevr_server":{"features":"echocombat"}})").kind, Statement::Kind::kMalformed);
  EXPECT_EQ(ReadDocument(nullptr, 4).kind, Statement::Kind::kMalformed);
}

TEST(ServerFeaturesRead, BadNamesAreDroppedAndCounted) {
  const Statement s = Read(R"({"nevr_server":{"features":["echocombat",7,"Bad Name","echocombat_medigun",null]}})");
  EXPECT_EQ(s.kind, Statement::Kind::kStated);
  EXPECT_EQ(s.features, (Names{"echocombat", "echocombat_medigun"}));
  EXPECT_EQ(s.dropped, 3U);
}

TEST(ServerFeaturesRead, NoMoreThanTheServiceTakes) {
  nlohmann::json doc;
  doc["nevr_server"]["features"] = Names(40, "x");
  const Statement s = Read(doc.dump());
  EXPECT_EQ(s.features.size(), 32U);
  EXPECT_EQ(s.dropped, 8U);
}

// The game passes the text and its size with no terminator: only `size` bytes are read.
TEST(ServerFeaturesRead, ReadsOnlyTheSizeItIsGiven) {
  const std::string text = R"({"nevr_server":{"features":["echocombat"]}}GARBAGE)";
  const Statement s = ReadDocument(text.data(), text.size() - 7);
  EXPECT_EQ(s.kind, Statement::Kind::kStated);
  EXPECT_EQ(s.features, Names{"echocombat"});
}

TEST(ServerFeaturesGate, OpenOnlyAsStated) {
  EXPECT_FALSE(GateOf(Read("{}")).echoCombat);
  EXPECT_FALSE(GateOf(Read("[]")).echoCombat);
  const Gate ec = GateOf(Read(R"({"nevr_server":{"features":["echocombat"]}})"));
  EXPECT_TRUE(ec.echoCombat);
  EXPECT_FALSE(ec.medigun);
  const Gate both = GateOf(Read(R"({"nevr_server":{"features":["echocombat","echocombat_medigun"]}})"));
  EXPECT_TRUE(both.echoCombat);
  EXPECT_TRUE(both.medigun);
  // MEDIGUN is part of EchoCombat: stated alone, it opens nothing.
  const Gate medigunOnly = GateOf(Read(R"({"nevr_server":{"features":["echocombat_medigun"]}})"));
  EXPECT_FALSE(medigunOnly.echoCombat);
  EXPECT_FALSE(medigunOnly.medigun);
}

TEST(ServerFeaturesClient, ApplyClearAndCurrent) {
  ClientState state;
  EXPECT_FALSE(state.Current().echoCombat);
  EXPECT_TRUE(state.Apply(Gate{true, true}));
  EXPECT_FALSE(state.Apply(Gate{true, true}));  // the same gate again changes nothing
  EXPECT_TRUE(state.Current().echoCombat);
  EXPECT_TRUE(state.Current().medigun);
  EXPECT_TRUE(state.Apply(Gate{true, false}));
  EXPECT_FALSE(state.Current().medigun);
  EXPECT_TRUE(state.Clear());
  EXPECT_FALSE(state.Current().echoCombat);
  EXPECT_FALSE(state.Clear());
}

TEST(ServerFeaturesClient, OneStatePerProcess) { EXPECT_EQ(&Client(), &Client()); }

// A session in another lobby clears the statement; a repeat of the lobby already in does not.
TEST(ServerFeaturesClient, ANewLobbyClearsAndTheSameLobbyDoesNot) {
  ClientState state;
  const ClientState::Lobby a = {1}, b = {2};
  state.Apply(Gate{true, true});
  bool was = false;
  EXPECT_TRUE(state.ClearForLobby(a, &was));
  EXPECT_TRUE(was);
  EXPECT_FALSE(state.Current().echoCombat);
  state.Apply(Gate{true, false});
  EXPECT_FALSE(state.ClearForLobby(a, &was));
  EXPECT_TRUE(state.Current().echoCombat);
  EXPECT_TRUE(state.ClearForLobby(b, nullptr));
  EXPECT_FALSE(state.Current().echoCombat);
}

// The message as both binaries' callback reads it: the text at +0x18, its size at +0x20.
TEST(ServerFeaturesReceive, ReadsTheMessageWhereTheGameDoes) {
  ClientState state;
  const std::string text = R"({"nevr_server":{"features":["echocombat","echocombat_medigun"]}})";
  unsigned char msg[0x40] = {};
  const char* p = text.data();
  const std::uint64_t n = text.size();
  std::memcpy(msg + 0x18, &p, sizeof p);
  std::memcpy(msg + 0x20, &n, sizeof n);
  const nevr_server_features::Received r = nevr_server_features::Receive(msg, state);
  EXPECT_EQ(r.statement.kind, Statement::Kind::kStated);
  EXPECT_EQ(r.size, text.size());
  EXPECT_TRUE(r.changed);
  EXPECT_TRUE(state.Current().medigun);
  EXPECT_EQ(nevr_server_features::Receive(nullptr, state).statement.kind, Statement::Kind::kMalformed);
  EXPECT_FALSE(state.Current().echoCombat);
}

TEST(ServerFeaturesCleared, TheLineBothWritersGive) {
  EXPECT_EQ(nevr_server_features::FormatCleared("none", false),
            "[NEVR.WS] server features cleared lobby=none was_stated=no (the EchoCombat tac mods stay hidden until this server's "
            "lobby settings state them)");
}

}  // namespace
