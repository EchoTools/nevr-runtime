// #1094: sdk/include/nevr/level_gate.h, the plugin side of the per-level pattern.

#include <gtest/gtest.h>

#include <cstddef>

#include "nevr/level_gate.h"

namespace {

NevrGameContext V8Context(uint64_t level) {
  NevrGameContext ctx = {};
  ctx.ctx_size = sizeof(NevrGameContext);
  ctx.level_symbol = level;
  return ctx;
}

}  // namespace

TEST(LevelGate, NoNamesMeansEveryLevelOnAnyHost) {
  LevelGate gate;
  EXPECT_FALSE(gate.Configured());
  EXPECT_TRUE(gate.Allows(nullptr));
  NevrGameContext old = {};
  old.ctx_size = static_cast<uint32_t>(offsetof(NevrGameContext, plugin_id));  // a v5 host
  EXPECT_TRUE(gate.Allows(&old));
  const NevrGameContext none = V8Context(0);
  EXPECT_TRUE(gate.Allows(&none));
}

TEST(LevelGate, ANamedLevelIsAllowedAndAnotherIsNot) {
  LevelGate gate;
  gate.Set({"mpl_arena_dodgeball", "mpl_arena_a"});
  EXPECT_TRUE(gate.Configured());
  const NevrGameContext dodgeball = V8Context(EchoVR::CSymbol64Hash("mpl_arena_dodgeball"));
  const NevrGameContext arenaA = V8Context(EchoVR::CSymbol64Hash("mpl_arena_a"));
  const NevrGameContext combat = V8Context(EchoVR::CSymbol64Hash("mpl_combat_combustion"));
  EXPECT_TRUE(gate.Allows(&dodgeball));
  EXPECT_TRUE(gate.Allows(&arenaA));
  EXPECT_FALSE(gate.Allows(&combat));
}

TEST(LevelGate, TheHashIgnoresCaseLikeTheGame) {
  LevelGate gate;
  gate.Set({"MPL_Arena_A"});
  const NevrGameContext arenaA = V8Context(EchoVR::CSymbol64Hash("mpl_arena_a"));
  EXPECT_TRUE(gate.Allows(&arenaA));
}

TEST(LevelGate, NoLevelAnOlderHostAndNoContextAllCloseANamedGate) {
  LevelGate gate;
  gate.Set({"mpl_arena_a"});
  const NevrGameContext none = V8Context(0);
  EXPECT_FALSE(gate.Allows(&none));
  NevrGameContext old = V8Context(EchoVR::CSymbol64Hash("mpl_arena_a"));
  old.ctx_size = static_cast<uint32_t>(offsetof(NevrGameContext, level_symbol));  // a v7 host's size
  EXPECT_FALSE(LevelGate::Covers(&old));
  EXPECT_FALSE(gate.Allows(&old)) << "a v7 host's context holds no level; the field's bytes are not the host's";
  EXPECT_FALSE(gate.Allows(nullptr));
}

TEST(LevelGate, EmptyNamesAreIgnoredAndSetReplacesTheList) {
  LevelGate gate;
  gate.Set({"", ""});
  EXPECT_FALSE(gate.Configured());
  gate.Set({"mpl_arena_a"});
  gate.Set({"mpl_combat_combustion"});
  const NevrGameContext arenaA = V8Context(EchoVR::CSymbol64Hash("mpl_arena_a"));
  const NevrGameContext combat = V8Context(EchoVR::CSymbol64Hash("mpl_combat_combustion"));
  EXPECT_FALSE(gate.Allows(&arenaA));
  EXPECT_TRUE(gate.Allows(&combat));
}
