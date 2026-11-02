// Host test of q15t::RegisterScriptRedirect (src/script_redirect.cpp, #1737): the call the combat unit makes at init to ask the
// host to redirect its nine script libraries. A fake host API records what it is asked and answers a chosen result; the
// plugin's log context records what is logged. Real code, no mocks of q15t itself.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "plugin_log.h"
#include "script_redirect.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

namespace {

std::vector<std::string> g_lines;  // "<level>|<message>|k=v|k=v"
int g_calls = 0;
std::string g_id;
std::vector<std::uint64_t> g_hashes;
std::uint32_t g_count = 0;
std::int32_t g_answer = NEVR_SCRIPT_REDIRECT_OK;

void Record(const char*, std::uint32_t level, const char* message, const NevrLogField* fields, std::uint32_t n) {
  std::string line = std::to_string(level) + "|" + message;
  for (std::uint32_t i = 0; i < n; ++i) line += std::string("|") + fields[i].key + "=" + fields[i].value;
  g_lines.push_back(line);
}

std::int32_t FakeRegister(const char* id, const std::uint64_t* hashes, std::uint32_t count) {
  ++g_calls;
  g_id = id != nullptr ? id : "(null)";
  g_count = count;
  g_hashes.assign(hashes, hashes + count);
  return g_answer;
}

bool Logged(const char* fragment) {
  for (const std::string& l : g_lines) {
    if (l.find(fragment) != std::string::npos) return true;
  }
  return false;
}

void Reset() {
  g_lines.clear();
  g_calls = 0;
  g_id.clear();
  g_hashes.clear();
  g_count = 0;
  g_answer = NEVR_SCRIPT_REDIRECT_OK;
}

}  // namespace

int main() {
  const NevrQuestHostApi full{sizeof(NevrQuestHostApi), nullptr, &FakeRegister};
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.plugin_id = "q15t-host-id";
  ctx.log = &Record;
  ctx.quest = &full;
  q15t::SetLogContext(&ctx);

  // The registration: the plugin's nine hashes, the host-given id, the host's answer returned, an Info line saying ok.
  Reset();
  CHECK(q15t::RegisterScriptRedirect(&ctx) == NEVR_SCRIPT_REDIRECT_OK, "ok answer must be returned");
  CHECK(g_calls == 1, "the host must be asked once, got %d", g_calls);
  CHECK(g_id == "q15t-host-id", "the id is the host's plugin_id, got %s", g_id.c_str());
  CHECK(g_count == 9 && g_hashes.size() == 9, "nine hashes, got %u", g_count);
  bool same = g_hashes.size() == 9;
  for (std::size_t i = 0; same && i < 9; ++i) same = g_hashes[i] == q15t::kScriptRedirectHashes[i];
  CHECK(same, "the hashes the host gets are kScriptRedirectHashes, in order");
  CHECK(g_hashes.size() == 9 && g_hashes[0] == 0x37f88adb0d8066f7ULL && g_hashes[8] == 0xac4cd8e0a786d5c5ULL,
        "first and last pinned hash");
  CHECK(Logged("1|script redirect registered|result=ok"), "an Info line with result=ok");

  // A refusal: its code is returned and logged by name at Error, saying nothing is redirected.
  Reset();
  g_answer = NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY;
  CHECK(q15t::RegisterScriptRedirect(&ctx) == NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY, "refusal must be returned");
  CHECK(Logged("3|script redirect not registered: no combat library is redirected|result=missing_library"),
        "an Error line with result=missing_library");
  CHECK(!Logged("script redirect registered"), "a refusal must not log the success line");
  Reset();
  g_answer = NEVR_SCRIPT_REDIRECT_ALREADY_ARMED;
  q15t::RegisterScriptRedirect(&ctx);
  CHECK(Logged("result=already_armed"), "already_armed named");
  Reset();
  g_answer = NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR;
  q15t::RegisterScriptRedirect(&ctx);
  CHECK(Logged("result=no_library_dir"), "no_library_dir named");
  Reset();
  g_answer = NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT;
  q15t::RegisterScriptRedirect(&ctx);
  CHECK(Logged("result=bad_argument"), "bad_argument named");
  Reset();
  g_answer = 99;
  q15t::RegisterScriptRedirect(&ctx);
  CHECK(Logged("result=unknown"), "an unlisted code reads unknown");

  // The host gave no plugin_id: the plugin names itself.
  Reset();
  NvrGameContext noId = ctx;
  noId.plugin_id = nullptr;
  CHECK(q15t::RegisterScriptRedirect(&noId) == NEVR_SCRIPT_REDIRECT_OK, "ok through a context with no id");
  CHECK(g_id == "q15t", "the plugin's own name is the fallback id, got %s", g_id.c_str());

  // Nothing to register through: -1 and an Error line naming the cause, never a call.
  Reset();
  CHECK(q15t::RegisterScriptRedirect(nullptr) == -1, "null context");
  CHECK(g_calls == 0, "no call without a context");

  Reset();
  NvrGameContext noQuest = ctx;
  noQuest.quest = nullptr;
  CHECK(q15t::RegisterScriptRedirect(&noQuest) == -1, "no quest api");
  CHECK(Logged("3|script redirect not registered|result=no_host_api"), "an Error line with result=no_host_api");
  CHECK(g_calls == 0, "no call without an api");

  Reset();
  NvrGameContext shortContext = ctx;
  shortContext.ctx_size = offsetof(NvrGameContext, quest);  // an older host: the context ends before the quest member
  CHECK(q15t::RegisterScriptRedirect(&shortContext) == -1, "a context that does not cover quest");
  CHECK(Logged("result=no_host_api") && g_calls == 0, "no_host_api, no call");

  Reset();
  const NevrQuestHostApi old{offsetof(NevrQuestHostApi, register_script_redirect), nullptr, &FakeRegister};
  NvrGameContext oldCtx = ctx;
  oldCtx.quest = &old;
  CHECK(q15t::RegisterScriptRedirect(&oldCtx) == -1, "an api struct that ends before the member");
  CHECK(Logged("result=no_host_api") && g_calls == 0, "no_host_api, no call");

  Reset();
  const NevrQuestHostApi noFn{sizeof(NevrQuestHostApi), nullptr, nullptr};
  NvrGameContext noFnCtx = ctx;
  noFnCtx.quest = &noFn;
  CHECK(q15t::RegisterScriptRedirect(&noFnCtx) == -1, "a null function");
  CHECK(Logged("result=no_host_api"), "no_host_api");

  if (g_fail == 0) std::printf("q15t script_redirect_test: all checks pass\n");
  return g_fail == 0 ? 0 : 1;
}
