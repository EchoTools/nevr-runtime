// Host test of combat_gates.h: which of the three combat gates (permissions G1, bot body G6, levelname G7) a run installs, from
// what is built and which kill files exist in a real temporary data directory.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "combat_gates.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

using q15t::CombatGatesBuilt;
using q15t::CombatGatesChoice;
using q15t::CombatGatesOffFiles;
using q15t::DecideCombatGates;

static bool Eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

static void Touch(const std::string& dir, const char* name) {
  const std::string p = dir + "/" + name;
  FILE* f = std::fopen(p.c_str(), "w");
  if (f == nullptr) std::abort();
  std::fclose(f);
}

int main() {
  const CombatGatesBuilt all{true, true, true};
  // the shipped build, no kill file: all three installed
  {
    const CombatGatesChoice c = DecideCombatGates(all, {false, false, false});
    CHECK(c.perm && c.bots && c.levelname, "all built, no off file: want all three on, got %d %d %d", c.perm, c.bots, c.levelname);
    CHECK(Eq(c.perm_reason, "") && Eq(c.bots_reason, "") && Eq(c.levelname_reason, ""), "an installed gate carries no reason");
  }
  // each kill file turns its own gate off
  {
    const CombatGatesChoice c = DecideCombatGates(all, {true, false, false});
    CHECK(!c.perm && Eq(c.perm_reason, "off_file") && c.bots && c.levelname, "gate_perm.off: only perm off (got %d %d %d %s)", c.perm, c.bots,
          c.levelname, c.perm_reason);
  }
  {
    const CombatGatesChoice c = DecideCombatGates(all, {false, true, false});
    CHECK(c.perm && !c.bots && Eq(c.bots_reason, "off_file") && c.levelname, "gate_bots.off: only bots off (got %d %d %d %s)", c.perm, c.bots,
          c.levelname, c.bots_reason);
  }
  // levelname off takes the bot-body gate with it: the combat bot body exists only in the full global_mp
  {
    const CombatGatesChoice c = DecideCombatGates(all, {false, false, true});
    CHECK(!c.levelname && Eq(c.levelname_reason, "off_file"), "levelname.off: levelname must be off (%s)", c.levelname_reason);
    CHECK(!c.bots && Eq(c.bots_reason, "needs_levelname"), "levelname.off: bots must follow it off with needs_levelname (got %d %s)", c.bots,
          c.bots_reason);
    CHECK(c.perm, "levelname.off: the permissions gate does not depend on it");
  }
  {
    const CombatGatesChoice c = DecideCombatGates({true, true, false}, {false, false, false});
    CHECK(!c.bots && Eq(c.bots_reason, "needs_levelname") && Eq(c.levelname_reason, "not_built"), "levelname not built: bots off (got %d %s %s)",
          c.bots, c.bots_reason, c.levelname_reason);
  }
  {
    const CombatGatesChoice c = DecideCombatGates({false, false, false}, {false, false, false});
    CHECK(!c.perm && !c.bots && !c.levelname && Eq(c.perm_reason, "not_built") && Eq(c.bots_reason, "not_built"), "nothing built: all off, not_built");
  }
  // the kill files, read from a real directory
  char dir[] = "/tmp/q15t_combat_gates_XXXXXX";
  if (mkdtemp(dir) == nullptr) std::abort();
  q15t::SetModuleSwitchDirForTest(dir);
  {
    const CombatGatesOffFiles o = q15t::ReadCombatGatesOffFiles();
    CHECK(!o.perm && !o.bots && !o.levelname, "empty directory: no kill file must be seen");
  }
  Touch(dir, "gate_perm.off");
  {
    const CombatGatesOffFiles o = q15t::ReadCombatGatesOffFiles();
    CHECK(o.perm && !o.bots && !o.levelname, "gate_perm.off: only perm must be seen (%d %d %d)", o.perm, o.bots, o.levelname);
  }
  Touch(dir, "gate_bots.off");
  Touch(dir, "levelname.off");
  {
    const CombatGatesOffFiles o = q15t::ReadCombatGatesOffFiles();
    CHECK(o.perm && o.bots && o.levelname, "all three kill files must be seen (%d %d %d)", o.perm, o.bots, o.levelname);
  }
  for (const char* n : {"gate_perm.off", "gate_bots.off", "levelname.off"}) unlink((std::string(dir) + "/" + n).c_str());
  rmdir(dir);
  q15t::SetModuleSwitchDirForTest("");
  if (g_fail == 0) std::printf("combat_gates_test: all checks pass\n");
  return g_fail == 0 ? 0 : 1;
}
