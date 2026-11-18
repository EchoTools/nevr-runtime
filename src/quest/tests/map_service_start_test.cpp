// The Quest's map service started with a channel that runs (quest/cdn/map_service.h Start; docs/adr/0048 decisions 8 and 16): the order of
// what it installs (the boot mount first, then the pin's tick, the mount observer and the worker), what it logs, and that the worker's
// first pass waits for the game's manifest, mounts against it and tells the mount observer. Start runs once per process, so this is its
// own program (map_service_test holds the refusals). The libr15 half of the pin and the resource request hook are stand-ins
// (map_service_stubs.h); the cache directory has no index on the host, so the boot mounts nothing and the pass fetches nothing.

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "cdn/cdn_env.h"
#include "core/map_channel.h"
#include "core/map_groups.h"
#include "quest/cdn/map_service.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/map_service_stubs.h"
#include "quest/tests/map_set_fixture.h"
#include "quest/tests/test_check.h"

namespace {

namespace qs = nevr_quest::maps;
namespace stubs = map_service_stubs;

constexpr std::uint64_t kManifestRecords = 1;
constexpr std::chrono::milliseconds kStopWait{5000};  // generous: the idle worker joins at once
constexpr std::chrono::milliseconds kPollStep{10};
constexpr auto kWorkerDeadline = std::chrono::seconds(10);  // the worker's first pass, which only this program waits for
const char kOwner[] = "map:alpha";
const char kSessionGuid[] = "0A1B2C3D-0000-4000-8000-000000000001";  // a session success's group: not the nil GUID
const char kBaseUrl[] = "https://maps.example.test/";

// The sentinel log's lines, written by the starting thread and the worker's.
std::mutex g_mutex;
std::condition_variable g_changed;
std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.emplace_back(line);
  }
  g_changed.notify_all();
}

std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  if (line[begin] == '"') return line.substr(begin + 1, line.find('"', begin + 1) - begin - 1);
  return line.substr(begin, line.find_first_of(",}", begin) - begin);
}

// The first record of `event` whose `key` is `value`, or "" (the lock is held by the caller).
std::string FindLocked(const char* event, const char* key, const char* value) {
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (const std::string& line : g_lines) {
    if (line.find(needle) != std::string::npos && Field(line, key) == value) return line;
  }
  return std::string();
}

std::string Find(const char* event, const char* key, const char* value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return FindLocked(event, key, value);
}

// Waits (up to the deadline) until the stand-in has been called.
bool WaitForCalls(std::atomic<int>* calls) {
  std::unique_lock<std::mutex> lock(g_mutex);
  // The stand-in's counter wakes nobody, so the wait polls it in short steps.
  const auto give_up = std::chrono::steady_clock::now() + kWorkerDeadline;
  while (calls->load() == 0) {
    if (std::chrono::steady_clock::now() >= give_up) return false;
    g_changed.wait_for(lock, kPollStep);
  }
  return true;
}

// Waits (up to the deadline) for the record the worker writes.
std::string WaitFor(const char* event, const char* key, const char* value) {
  std::unique_lock<std::mutex> lock(g_mutex);
  std::string found;
  g_changed.wait_for(lock, kWorkerDeadline, [&] { return !(found = FindLocked(event, key, value)).empty(); });
  return found;
}

nevr_map_channel::Settings RunningSettings() {
  const nevr_map_channel::ScalarLookup scalars = [](const char* key) {
    return std::string(key) == "maps_base_url" ? std::string(kBaseUrl) : std::string();
  };
  const nevr_map_channel::ListLookup lists = [](const char*) { return std::vector<std::string>(); };
  return nevr_map_channel::ReadSettings(scalars, lists, {});
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  const nevr_map_channel::Settings settings = RunningSettings();
  QCHECK(nevr_map_channel::DecideStart(settings) == nevr_map_channel::Start::kRun);  // the premise: this channel runs

  cdn_test::FakeEnv env;
  stubs::State& state = stubs::Get();
  state.game_manifest_records = 0;  // the game's manifest is not built: the worker's first pass waits for it
  // What the core had written when the pin's tick was installed, read inside Start: the worker starts at its end and writes to the env
  // from then on, so the env is read from this thread only inside Start and after Stop has joined the worker.
  std::size_t records_at_install = 0;
  bool boot_said_no_index = false;
  state.on_install_tick = [&env, &records_at_install, &boot_said_no_index] {
    records_at_install = env.records.size();
    for (const cdn_test::Record& record : env.records) {
      if (record.name == "map_mount_group") boot_said_no_index = true;
    }
  };

  QCHECK(qs::Start(&env, settings, nullptr));

  // The boot mount ran before the pin's tick was installed: the core wrote its records (the cache has no index) first.
  QCHECK(state.install_tick_calls == 1);
  QCHECK(records_at_install > 0);
  QCHECK(state.alias_install_calls >= 1);  // the alias hook is asked for once the observer is set
  QCHECK(boot_said_no_index);
  const std::string started = Find("maps_start", "result", "started");
  QCHECK(!started.empty() && Field(started, "mounted") == "0" && Field(started, "deferred") == "0");

  // The score waiver is installed because the channel runs: a map's set whose script the table holds is taken (map_service_test holds the
  // same set refused when the channel does not run).
  QCHECK(nevr_quest::script_redirect::map_scripts::Admit(kOwner, map_set_fixture::kPart) == nullptr);
  QCHECK(map_set_fixture::RegisterSetFile(kOwner, "start_script", map_set_fixture::SetWithScript()));

  // A second start is refused and installs nothing more.
  QCHECK(!qs::Start(&env, settings, nullptr));
  QCHECK(!Find("maps_start", "reason", "already_started").empty());
  QCHECK(state.install_tick_calls == 1);

  // The worker waited for the game's manifest (it polled for it); once built, its pass mounts against it and the mount observer, installed
  // by Start, tells the service.
  QCHECK(WaitForCalls(&state.manifest_records_calls));
  QCHECK(Find("map_summary", "stage", "mount").empty());  // nothing is mounted while the manifest is unbuilt
  state.game_manifest_records = kManifestRecords;
  const std::string summary = WaitFor("map_summary", "stage", "mount");
  QCHECK(!summary.empty());
  QCHECK(Field(summary, "mounted") == "0" && Field(summary, "armed") == "0");
  QCHECK(Find("maps_mount", "result", "abandoned").empty());

  // The process end stops the channel: Stop returns within its wait with the worker joined (no join_timeout Warning); a second Stop is safe.
  qs::Stop(kStopWait);
  QCHECK(Find("map_stop", "reason", "join_timeout").empty());
  qs::Stop(kStopWait);
  QCHECK(Find("map_stop", "reason", "join_timeout").empty());

  // The request entry was stopped first: the group request a session success makes now is refused `stopped`, and the worker is joined, so the env is ours again.
  std::string group;
  QCHECK(nevr_maps::RequestSessionGroup(kSessionGuid, &group) == nevr_maps::SessionGroupRequest::kRequested);  // Start installed the seam
  QCHECK(group == "0a1b2c3d-0000-4000-8000-000000000001");
  bool refused_as_stopped = false;
  for (const cdn_test::Record& record : env.records) {
    if (record.name == "map_request" && record.Get("outcome") == "refused" && record.Get("reason") == "stopped") refused_as_stopped = true;
  }
  QCHECK(refused_as_stopped);

  std::fflush(stderr);
  // The worker is joined, but Start leaks its state by design: leave without running static teardown over it.
  std::_Exit(quest_test::Failures() == 0 ? 0 : 1);
}
