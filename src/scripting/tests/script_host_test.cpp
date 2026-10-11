// Unit tests for script manifests (script_manifest.h), manifest enforcement in
// the registry, and the script host's loading and dev reload (script_host.h).
// A fake VM stands in for a real binding: after the manifest block it reads
// lines of the form `override <key> <int>`, `hook <name>` and `fail`.
#include "scripting/script_host.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "quest/tests/mini_test.h"
#include "scripting/script_manifest.h"

namespace {

using namespace nevr_script;
namespace fs = std::filesystem;

NevrHookResult Noop(NevrHookCall*, void*) { return NEVR_HOOK_CONTINUE; }

class FakeVm final : public ScriptVm {
 public:
  explicit FakeVm(Registry& reg) : reg_(reg) {}
  const char* Name() const override { return "fake"; }
  bool Load(NevrOwner* owner, const std::string& chunk, const std::string& source, std::string* error) override {
    ++loads[owner->name];
    generation_at_load[owner] = owner->generation.load();
    std::istringstream in(source);
    std::string line;
    int n = 0;
    bool in_manifest = false, past_manifest = false;
    while (std::getline(in, line)) {
      ++n;
      if (!past_manifest) {
        if (line == "--[[nevr") in_manifest = true;
        else if (in_manifest && line == "]]") past_manifest = true;
        continue;
      }
      std::istringstream words(line);
      std::string op, name;
      words >> op >> name;
      if (op == "override") {
        NevrValue v{};
        v.type = NEVR_VALUE_INT;
        words >> v.as.i;
        last_status[owner->name] = reg_.Api()->override_set(owner, name.c_str(), &v);
      } else if (op == "hook") {
        last_status[owner->name] = reg_.Api()->hook_add(owner, name.c_str(), NEVR_HOOK_PRE, Noop, nullptr);
      } else if (op == "fail") {
        *error = chunk + ":" + std::to_string(n) + ": failed on purpose";
        return false;
      }
    }
    return true;
  }
  size_t MemoryBytes(const NevrOwner*) const override { return 0; }
  size_t TotalMemoryBytes() const override { return 0; }
  // Freeing a state whose callbacks were not quiesced first is the use-after-free
  // Registry::Quiesce exists to prevent: count it.
  void Unload(NevrOwner* owner) override {
    ++unloads[owner->name];
    if (owner->generation.load() == generation_at_load[owner]) ++unquiesced_unloads;
  }

  std::map<std::string, int> loads, unloads;
  std::map<const NevrOwner*, uint64_t> generation_at_load;
  int unquiesced_unloads = 0;
  std::map<std::string, NevrStatus> last_status;

 private:
  Registry& reg_;
};

struct Captured {
  std::string event, owner, target, detail;
};

struct Env {
  fs::path dir;
  std::vector<Captured> log;
  Registry reg{[this](const LogRecord& r) { log.push_back({r.event, r.owner, r.target, r.detail}); }};
  FakeVm vm{reg};
  HookPoint* add = reg.RegisterHookPoint("test.add", {{"a", NEVR_VALUE_INT, true, false}});
  bool keys = [this] {
    bool ok = true;
    for (const char* key : {"k", "j", "m", "physics.gravity", "physics.player", "physics.player.speed", "match.rounds"}) {
      ok = reg.RegisterOverridePoint(key, NEVR_VALUE_INT) && ok;
    }
    return ok;
  }();

  explicit Env(const char* test) {
    const char* base = std::getenv("NEVR_TEST_TMPDIR");
    dir = (base ? fs::path(base) : fs::temp_directory_path()) / (std::string("nevr_script_host_") + test);
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Env() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string Write(const char* name, const std::string& text) {
    const fs::path p = dir / name;
    std::ofstream(p, std::ios::binary) << text;
    return p.string();
  }
  const Captured* Find(const char* event, const std::string& target = "") const {
    for (const Captured& c : log) {
      if (c.event == event && (target.empty() || c.target == target)) return &c;
    }
    return nullptr;
  }
  int Count(const char* event) const {
    int n = 0;
    for (const Captured& c : log) n += c.event == event;
    return n;
  }
  bool Effective(const char* key, int64_t* value) {
    NevrOwner* probe = reg.FindOwner("probe");
    if (!probe) probe = reg.OpenOwner("probe");
    NevrValue v{};
    if (reg.Api()->override_get(probe, key, &v) != NEVR_OK) return false;
    *value = v.as.i;
    return true;
  }
};

std::string Manifest(const std::string& name, const std::string& extra = "") {
  return "--[[nevr\n{\"name\": \"" + name + "\", \"version\": \"1.0.0\", \"api\": 1" + extra + "}\n]]\n";
}

bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

// ---- manifest parsing -----------------------------------------------------------------------

TEST(manifest_parses_every_field) {
  ScriptManifest m;
  std::string error;
  const std::string src =
      "\n--[[nevr\n"
      "{\n"
      "  \"name\": \"low_gravity\", \"version\": \"1.2.3\", \"api\": 1,\n"
      "  \"description\": \"Halves gravity\",\n"
      "  \"overrides\": [\"physics.gravity\", \"physics.player.*\"],\n"
      "  \"hooks\": [\"test.add\"]\n"
      "}\n"
      "]]\n"
      "nevr.override('physics.gravity', -5)\n";
  CHECK(ParseScriptManifest("low_gravity.lua", src, &m, &error));
  CHECK_EQ(m.name, std::string("low_gravity"));
  CHECK_EQ(m.version, std::string("1.2.3"));
  CHECK_EQ(m.api, 1u);
  CHECK_EQ(m.declaration.overrides.size(), static_cast<size_t>(2));
  CHECK_EQ(m.declaration.hooks.size(), static_cast<size_t>(1));
  CHECK(Contains(ScriptManifestJson(m), "\"physics.player.*\""));
}

TEST(manifest_refusals_name_file_and_line) {
  struct Case {
    std::string src;
    const char* want;
  };
  const Case cases[] = {
      {"print('no manifest')\n", "mod.lua:1: manifest: the file must start"},
      {"--[[nevr\n{\"name\": \"a\"}\n", "mod.lua:1: manifest: the --[[nevr block has no closing"},
      {"--[[nevr\n{\"name\": \n]]\n", "mod.lua:2: manifest: the block is not valid JSON"},
      {"--[[nevr\n[1]\n]]\n", "must be one JSON object"},
      {Manifest("Bad-Name"), "\"name\" must match"},
      {"--[[nevr\n{\"name\": \"a\", \"version\": \"1.0\", \"api\": 1}\n]]\n", "\"version\" must be"},
      {"--[[nevr\n{\"name\": \"a\", \"version\": \"1.0.0\"}\n]]\n", "\"api\" is required"},
      {Manifest("a", ", \"hook\": [\"test.add\"]"), "unknown key \"hook\""},
      {Manifest("a", ", \"overrides\": [\"physics..g\"]"), "is not a dotted name"},
      {Manifest("a", ", \"hooks\": [\"test.*\"]"), "is not a dotted name"},
      {"--[[nevr\n{\"name\": \"a\", \"version\": \"1.0.0\", \"api\": 1,\n \"description\": \"x]]y\"}\n]]\n",
       "mod.lua:3: manifest: \"]]\" inside the block"},
  };
  for (const Case& c : cases) {
    std::string error;
    const bool ok = ParseScriptManifest("mod.lua", c.src, nullptr, &error);
    if (ok || !Contains(error, c.want)) {
      std::fprintf(stderr, "  wanted \"%s\", got ok=%d error \"%s\"\n", c.want, ok, error.c_str());
      ++mini_test::Failures();
    }
  }
}

// ---- declaration enforcement ----------------------------------------------------------------

TEST(declared_owner_cannot_touch_what_it_did_not_declare) {
  Env env("declared");
  CHECK(env.keys);
  NevrOwner* a = env.reg.OpenOwner("mod_a");
  Declaration d;
  d.overrides = {"physics.gravity", "physics.player.*"};
  d.hooks = {"test.add"};
  env.reg.Declare(a, d);
  const NevrHostApi* api = env.reg.Api();
  NevrValue v{};
  v.type = NEVR_VALUE_INT;
  CHECK_EQ(api->override_set(a, "physics.gravity", &v), NEVR_OK);
  CHECK_EQ(api->override_set(a, "physics.player.speed", &v), NEVR_OK);
  CHECK_EQ(api->override_set(a, "physics.player", &v), NEVR_ERR_UNDECLARED);  // the prefix itself is not under it
  CHECK_EQ(api->override_set(a, "match.rounds", &v), NEVR_ERR_UNDECLARED);
  CHECK(Contains(api->last_error(a), "match.rounds"));
  CHECK_EQ(api->hook_add(a, "test.add", NEVR_HOOK_PRE, Noop, nullptr), NEVR_OK);
  env.reg.RegisterHookPoint("test.other", {});
  CHECK_EQ(api->hook_add(a, "test.other", NEVR_HOOK_PRE, Noop, nullptr), NEVR_ERR_UNDECLARED);
  CHECK_EQ(env.Count("undeclared"), 3);
  // An owner without a declaration (a native plugin today) is not limited.
  NevrOwner* native = env.reg.OpenOwner("native");
  CHECK_EQ(api->override_set(native, "match.rounds", &v), NEVR_OK);
}

// ---- reading manifests before anything runs -------------------------------------------------

TEST(manifests_are_read_without_running_scripts) {
  Env env("read");
  const std::string good = env.Write("good.lua", Manifest("good", ", \"hooks\": [\"test.add\"]") + "fail\n");
  const std::string bad = env.Write("bad.lua", "fail\n");
  const std::vector<ManifestCheck> checks = ReadManifests({good, bad, (env.dir / "missing.lua").string()});
  CHECK_EQ(checks.size(), static_cast<size_t>(3));
  CHECK(checks[0].ok && checks[0].manifest.name == "good" && checks[0].manifest.declaration.hooks.size() == 1);
  CHECK(!checks[1].ok && Contains(checks[1].error, "bad.lua:1: manifest:"));
  CHECK(!checks[2].ok && Contains(checks[2].error, "cannot read"));
  CHECK(env.vm.loads.empty());
}

// ---- loading --------------------------------------------------------------------------------

TEST(load_all_in_order_refusing_the_bad_ones) {
  Env env("load");
  ScriptHost host(env.reg, env.vm, false);
  const std::vector<std::string> paths = {
      env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 1\n"),
      env.Write("nomanifest.lua", "override k 2\n"),
      env.Write("future.lua", "--[[nevr\n{\"name\": \"future\", \"version\": \"1.0.0\", \"api\": 99}\n]]\n"),
      env.Write("dup.lua", Manifest("mod_a")),
      env.Write("broken.lua", Manifest("broken", ", \"overrides\": [\"j\"]") + "override j 5\nfail\n"),
      env.Write("b.lua", Manifest("mod_b", ", \"overrides\": [\"k\"]") + "override k 3\n"),
  };
  CHECK_EQ(host.LoadAll(paths), 2);
  CHECK_EQ(env.Count("script_refused"), 4);
  CHECK(env.Find("script_refused", "nomanifest.lua") != nullptr);
  const Captured* future = env.Find("script_refused", "future.lua");
  CHECK(future && Contains(future->detail, "needs host API v99"));
  const Captured* dup = env.Find("script_refused", "dup.lua");
  CHECK(dup && Contains(dup->detail, "already used by a.lua"));
  const Captured* broken = env.Find("script_refused", "broken.lua");
  CHECK(broken && Contains(broken->detail, "broken.lua:5: failed on purpose"));
  CHECK_EQ(env.vm.unloads["broken"], 1);
  CHECK_EQ(env.vm.unquiesced_unloads, 0);
  int64_t v = 0;
  CHECK(!env.Effective("j", &v));  // a refused script leaves nothing behind
  CHECK(env.Effective("k", &v) && v == 1);  // mod_a loaded first and keeps k
  CHECK_EQ(env.vm.last_status["mod_b"], NEVR_ERR_CONFLICT);
  const std::string json = host.LoadedManifestsJson();
  CHECK(json.find("mod_a") < json.find("mod_b"));
  CHECK(!Contains(json, "broken"));
}

TEST(undeclared_use_in_a_script_is_refused) {
  Env env("undeclared");
  ScriptHost host(env.reg, env.vm, false);
  host.LoadAll({env.Write("a.lua", Manifest("mod_a") + "override k 1\nhook test.add\n")});
  CHECK_EQ(env.vm.last_status["mod_a"], NEVR_ERR_UNDECLARED);
  CHECK_EQ(env.Count("undeclared"), 2);
}

// ---- dev reload -----------------------------------------------------------------------------

TEST(reload_applies_a_changed_file_and_keeps_order) {
  Env env("reload");
  ScriptHost host(env.reg, env.vm, true);
  const std::string a = env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 1\n");
  host.LoadAll({a, env.Write("b.lua", Manifest("mod_b", ", \"overrides\": [\"k\", \"m\"]") + "override m 1\n")});
  CHECK_EQ(host.PollReload(), 0);  // nothing changed
  env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 42\n");
  CHECK_EQ(host.PollReload(), 1);
  int64_t v = 0;
  CHECK(env.Effective("k", &v) && v == 42);
  CHECK_EQ(env.vm.unloads["mod_a"], 1);
  CHECK_EQ(env.vm.loads["mod_a"], 2);
  CHECK_EQ(env.vm.unquiesced_unloads, 0);
  CHECK(env.Find("script_reloaded", "a.lua") != nullptr);
  CHECK_EQ(host.PollReload(), 0);  // once per change
}

TEST(reload_with_a_bad_manifest_keeps_the_running_version) {
  Env env("reload_bad");
  ScriptHost host(env.reg, env.vm, true);
  const std::string a = env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 1\n");
  host.LoadAll({a});
  env.Write("a.lua", "override k 2 -- the manifest is gone\n");
  CHECK_EQ(host.PollReload(), 0);
  int64_t v = 0;
  CHECK(env.Effective("k", &v) && v == 1);
  CHECK_EQ(env.vm.unloads["mod_a"], 0);
  env.Write("a.lua", Manifest("renamed", ", \"overrides\": [\"k\"]") + "override k 3\n");
  CHECK_EQ(host.PollReload(), 0);
  CHECK(env.Effective("k", &v) && v == 1);
  CHECK_EQ(env.Count("reload_failed"), 2);
}

TEST(reload_whose_top_level_fails_leaves_the_script_inert) {
  Env env("reload_fail");
  ScriptHost host(env.reg, env.vm, true);
  host.LoadAll({env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 1\n")});
  env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 2\nfail\n");
  CHECK_EQ(host.PollReload(), 0);
  int64_t v = 0;
  CHECK(!env.Effective("k", &v));
  const Captured* c = env.Find("reload_failed", "a.lua");
  CHECK(c && Contains(c->detail, "a.lua:5: failed on purpose") && Contains(c->detail, "inert"));
  CHECK_EQ(env.vm.unquiesced_unloads, 0);
}

TEST(no_reload_outside_dev_builds) {
  Env env("no_dev");
  ScriptHost host(env.reg, env.vm, false);
  host.LoadAll({env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 1\n")});
  env.Write("a.lua", Manifest("mod_a", ", \"overrides\": [\"k\"]") + "override k 22\n");
  CHECK_EQ(host.PollReload(), 0);
  int64_t v = 0;
  CHECK(env.Effective("k", &v) && v == 1);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
