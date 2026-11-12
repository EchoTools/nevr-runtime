// Host test for the combat area's script redirect (#2189, combat/script_redirect.h): the module asks the host registry once, with owner
// `combat` and the nine hashes of script_redirect_list.h in their order, and writes one `combat_script_redirect_install` line with the
// result, the count and the registry's answer. A registry that refuses leaves the module not installed, with the registry's answer as its
// status; the shipped Install() reaches the real registry, which refuses here (no lib<hash>_nevr.so beside this program).
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/script_redirect.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/combat/script_redirect_list.h"
#include "quest/plugin/script_redirect.h"
#include "quest/tests/test_check.h"

namespace {

namespace sr = nevr_quest::combat::script_redirect;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (l.text.find(key) != std::string::npos) out.push_back(l);
  }
  return out;
}

// What the fake registry was asked.
struct Call {
  std::string owner;
  std::vector<std::uint64_t> hashes;
};
std::vector<Call> g_calls;
NevrScriptRedirectResult g_answer = NEVR_SCRIPT_REDIRECT_OK;

NevrScriptRedirectResult FakeRegister(const char* owner, const std::uint64_t* hashes, std::uint32_t count) noexcept {
  Call c;
  c.owner = owner != nullptr ? owner : "(null)";
  if (hashes != nullptr) c.hashes.assign(hashes, hashes + count);
  g_calls.push_back(c);
  return g_answer;
}

void Start(NevrScriptRedirectResult answer) {
  g_lines.clear();
  g_calls.clear();
  g_answer = answer;
  nevr_quest::script_redirect::ResetForTest();
  sentinel::SetLogSink(&Capture);
}
void Done() { sentinel::SetLogSink(nullptr); }

const std::uint64_t kNine[] = {0x37f88adb0d8066f7ULL, 0x3c11128c3bd783ddULL, 0x4b65d1b009973281ULL,
                               0x6a0f689e5b91c666ULL, 0x80461e04c6d82c21ULL, 0x812075466da9e147ULL,
                               0x954c89b6c61625f6ULL, 0xa5c64faa725f2334ULL, 0xac4cd8e0a786d5c5ULL};

// The registry is asked once, by `combat`, for the nine hashes in the packaging's order; an OK answer installs and logs Info.
void ARegisteredRedirectAsksOnceForTheNineAndSaysSo() {
  Start(NEVR_SCRIPT_REDIRECT_OK);
  const nevr_quest::combat::ModuleResult r = sr::Install(&FakeRegister);
  QCHECK(r.installed && !r.off && std::strcmp(r.status, "registered") == 0);
  QCHECK(g_calls.size() == 1U);
  if (g_calls.size() == 1U) {
    QCHECK(g_calls[0].owner == "combat");
    QCHECK(g_calls[0].hashes == std::vector<std::uint64_t>(std::begin(kNine), std::end(kNine)));
  }
  QCHECK(sr::kCount == 9U);
  const std::vector<Line> lines = Lines("combat_script_redirect_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"result\":\"registered\",\"count\":9,\"host_result\":\"ok\""));
  }
  Done();
}

// Each refusal the registry can answer: not installed, the registry's answer as the status and the field. A refusal that leaves nothing
// registered is an Error that says so; already_armed (another owner's registration is in force and keeps redirecting) is a Warning, result
// already_registered, and never says that nothing is redirected.
void ARefusalIsNotInstalledAndNamesTheRegistrysAnswer() {
  const struct {
    NevrScriptRedirectResult answer;
    const char* name;
    const char* result;
    sentinel::LogLevel level;
  } cases[] = {{NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT, "bad_argument", "refused", sentinel::LogLevel::kError},
               {NEVR_SCRIPT_REDIRECT_ALREADY_ARMED, "already_armed", "already_registered", sentinel::LogLevel::kWarn},
               {NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY, "missing_library", "refused", sentinel::LogLevel::kError},
               {NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR, "no_library_dir", "refused", sentinel::LogLevel::kError}};
  for (const auto& c : cases) {
    Start(c.answer);
    const nevr_quest::combat::ModuleResult r = sr::Install(&FakeRegister);
    QCHECK(!r.installed && !r.off && std::strcmp(r.status, c.name) == 0);
    QCHECK(g_calls.size() == 1U);
    const std::vector<Line> lines = Lines("combat_script_redirect_install");
    QCHECK(lines.size() == 1U);
    if (lines.size() == 1U) {
      QCHECK(lines[0].level == c.level);
      const std::string want =
          std::string("\"result\":\"") + c.result + "\",\"count\":9,\"host_result\":\"" + c.name + "\"";
      QCHECK(Has(lines[0].text, want.c_str()));
      const bool saysNothing = Has(lines[0].text, "no combat script library is redirected");
      QCHECK(saysNothing == (c.answer != NEVR_SCRIPT_REDIRECT_ALREADY_ARMED));
    }
    Done();
  }
}

// The shipped Install() reaches the host registry: here it refuses for the missing combat builds and registers nothing.
void TheShippedInstallAsksTheHostRegistry() {
  Start(NEVR_SCRIPT_REDIRECT_OK);
  const nevr_quest::combat::ModuleResult r = sr::Install();
  QCHECK(!r.installed && std::strcmp(r.status, "missing_library") == 0);
  QCHECK(g_calls.empty());  // not the fake
  QCHECK(nevr_quest::script_redirect::ArmedCount() == 0U);
  bool hostLine = false;  // the registry's own line names the owner
  for (const Line& l : g_lines) {
    if (Has(l.text, "\"op\":\"register\"") && Has(l.text, "\"plugin\":\"combat\"")) hostLine = true;
  }
  QCHECK(hostLine);
  Done();
}

}  // namespace

int main() {
  ARegisteredRedirectAsksOnceForTheNineAndSaysSo();
  ARefusalIsNotInstalledAndNamesTheRegistrysAnswer();
  TheShippedInstallAsksTheHostRegistry();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_script_redirect_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_script_redirect_test: all checks passed");
  return 0;
}
