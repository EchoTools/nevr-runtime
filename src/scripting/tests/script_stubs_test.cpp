// Unit tests for the Luau definition generator (script_stubs.h).
//   script_stubs_test            the tests
//   script_stubs_test --emit F   writes the definitions for the test registry to F
//                                (src/scripting/check/check_test.sh type-checks samples against it)
#include "scripting/script_stubs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "quest/tests/mini_test.h"

namespace {

using namespace nevr_script;

// The override points and hook point the samples and the conformance tests use.
void RegisterTestPoints(Registry& reg) {
  reg.RegisterOverridePoint("physics.gravity", NEVR_VALUE_FLOAT);
  reg.RegisterOverridePoint("team.colour", NEVR_VALUE_STRING);
  reg.RegisterOverridePoint("hud.visible", NEVR_VALUE_BOOL);
  reg.RegisterOverridePoint("match.rounds", NEVR_VALUE_INT);
  reg.RegisterHookPoint("test.add", {{"a", NEVR_VALUE_INT, true, false},
                                     {"b", NEVR_VALUE_INT, true, false},
                                     {"result", NEVR_VALUE_INT, true, true}});
}

bool Has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// The text between `from` and the next "}\n" after it.
std::string Block(const std::string& s, const char* from) {
  const size_t b = s.find(from);
  if (b == std::string::npos) return "";
  const size_t e = s.find("}\n", b);
  return s.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

}  // namespace

TEST(definitions_name_every_registered_key_and_hook) {
  Registry reg(nullptr);
  RegisterTestPoints(reg);
  const std::string d = GenerateLuauDefinitions(reg);
  CHECK(Has(d, "declare nevr: {"));
  CHECK(Has(d, "(key: \"physics.gravity\", value: number) -> (boolean?, string?)"));
  CHECK(Has(d, "(key: \"team.colour\", value: string)"));
  CHECK(Has(d, "(key: \"hud.visible\", value: boolean)"));
  CHECK(Has(d, "(name: \"test.add\", callbacks: { pre: ((h: NevrPre_test_add) -> ())?, post: ((h: NevrPost_test_add) -> ())? })"));
  CHECK(Has(d, "log: (level: \"debug\" | \"info\" | \"warn\" | \"error\", message: string) -> ()"));
}

TEST(each_phase_may_set_only_its_writable_fields) {
  Registry reg(nullptr);
  RegisterTestPoints(reg);
  const std::string d = GenerateLuauDefinitions(reg);
  const std::string pre = Block(d, "export type NevrPre_test_add");
  const std::string post = Block(d, "export type NevrPost_test_add");
  CHECK(Has(pre, "field: \"a\", value: number"));
  CHECK(Has(pre, "skip: (self: NevrPre_test_add) -> ()"));
  CHECK(Has(post, "field: \"result\", value: number"));
  CHECK(!Has(post, "field: \"a\", value:"));  // a is read-only after the call
  CHECK(!Has(post, "skip:"));
  CHECK(Has(post, "(self: NevrPost_test_add, field: \"a\") -> number"));  // still readable
}

TEST(an_empty_registry_still_declares_nevr) {
  Registry reg(nullptr);
  const std::string d = GenerateLuauDefinitions(reg);
  CHECK(Has(d, "override: never"));
  CHECK(Has(d, "hook: never"));
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--emit") == 0) {
    Registry reg(nullptr);
    RegisterTestPoints(reg);
    std::ofstream(argv[2], std::ios::binary) << GenerateLuauDefinitions(reg);
    return 0;
  }
  return mini_test::RunAll(argc, argv);
}
