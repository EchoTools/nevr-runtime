// The Quest's map service (quest/cdn/map_service.h; docs/adr/0048 decisions 5, 8 and 16): what its public functions decide around the
// core. The channel's config for the Quest, the UTC clock the core compares expiries with, the wait for the game's manifest before the
// first mount (what it passes to the registry, how long it polls, what it logs and does when it gives up), the later mount on the
// calling thread, and Start's refusals. Start's run path is map_service_start_test (the service starts once per process). The libr15
// half of the pin and the resource request hook are stand-ins (map_service_stubs.h); the registry, the script table and the
// alias table are the real ones. The core's own rules are tested in core/tests; these checks hold the edge's arguments to them.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <string>
#include <vector>

#include "cdn/cdn_env.h"
#include "core/map_channel.h"
#include "quest/cdn/map_service.h"
#include "quest/sentinel/hook_log.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/map_sets.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/map_service_stubs.h"
#include "quest/tests/map_set_fixture.h"
#include "quest/tests/test_check.h"

namespace {

namespace qs = nevr_quest::maps;
namespace ms = nevr_quest::map_sets;
namespace rs = nevr_resource_sets;
namespace stubs = map_service_stubs;
using namespace map_set_fixture;

constexpr std::uint32_t kNoWaitMs = 0;              // a wait that has no time to give
constexpr std::uint32_t kShortWaitMs = 1;           // a wait shorter than any poll step
constexpr std::uint64_t kManifestRecords = 1;       // the game's manifest, built
constexpr std::uint32_t kLongWaitMs = 60 * 1000;    // a wait the test never lets run out
constexpr int kPollsBeforeReady = 2;                // the manifest is built on the third look
constexpr std::chrono::milliseconds kStopWaitNeverRan{5000};  // a Stop that waited on a worker would take this long
constexpr std::int64_t kClockToleranceS = 5;        // the clock's reading may differ from the test's by this much
constexpr int kPublishedAfterBeta = 2, kPublishedAfterGamma = 3;  // shadows published once the second and the third map are mounted
constexpr std::size_t kMountedAfterBeta = 2, kMountedAfterGamma = 3;  // the level names mounted at those points
constexpr std::size_t kUtcTextChars = 20;           // "YYYY-MM-DDTHH:MM:SSZ"
constexpr std::size_t kTPosition = 10, kZPosition = 19;
const char kAlphaOwner[] = "map:alpha";
const char kBetaOwner[] = "map:beta";
constexpr std::uint64_t kBetaLevel = 0x7691d78d414c5702ULL, kBetaComponent = 0x2222333344445555ULL;
constexpr std::uint64_t kGammaLevel = 0x7691d78d414c5703ULL, kGammaComponent = 0x3333444455556666ULL;

std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

// The value of `key` in a JSON record line, as text ("<absent>" when the line has no such key).
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  if (line[begin] == '"') return line.substr(begin + 1, line.find('"', begin + 1) - begin - 1);
  return line.substr(begin, line.find_first_of(",}", begin) - begin);
}

// The last record of `event` ("" when none).
std::string Last(const char* event) {
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it) {
    if (it->find(needle) != std::string::npos) return *it;
  }
  return std::string();
}

bool Register(const char* owner, const char* name, const std::string& set) { return RegisterSetFile(owner, name, set); }

int g_published = 0;
void Publish(const nevr_shadow::Shadow* shadow) {
  if (shadow != nullptr) ++g_published;
}

int g_observed = 0;
constexpr std::size_t kUnobserved = 99;  // the observer was not called
std::size_t g_observed_sets = kUnobserved;
std::uint64_t g_observed_stock_records = 0;  // the game's record count the mount was given
void Observer(const rs::MountReport& report) {
  ++g_observed;
  g_observed_sets = report.sets;
  g_observed_stock_records = report.stock_records;
}

// The game's manifest as the wait polls it: 0 for the first kPollsBeforeReady looks, then built.
int g_looks = 0;
std::uint64_t BuiltAfterTwoLooks() { return ++g_looks > kPollsBeforeReady ? kManifestRecords : 0; }
std::uint64_t BuiltAtOnce() {
  ++g_looks;
  return kManifestRecords;
}
std::uint64_t NeverBuilt() {
  ++g_looks;
  return 0;
}

int g_holds_calls = 0;
bool g_holds_answer = false;
bool Holds(std::uint64_t, std::uint64_t) {
  ++g_holds_calls;
  return g_holds_answer;
}

constexpr auto kKeepGoing = [] { return true; };

void Reset() {
  ms::ResetForTest();
  ms::SetMountObserver(&Observer);
  g_lines.clear();
  g_published = 0;
  g_observed = 0;
  g_observed_sets = kUnobserved;
  g_observed_stock_records = 0;
  g_looks = 0;
  g_holds_calls = 0;
  g_holds_answer = false;
}

// ---- The channel's config and the clock -------------------------------------------------------------------------------------

void TheChannelConfigIsTheQuestsAndNamesTheCacheDirectory() {
  const nevr_maps::ChannelConfig config = qs::MakeChannelConfig();
  QCHECK(config.dir == qs::kDefaultCacheDir);
  QCHECK(config.platform == nevr_maps::kPlatformQuest);
  QCHECK(config.mount_on_demand);  // a client: it mounts the maps a session asks for (ADR 0048 decision 16)
  QCHECK(qs::MakeChannelConfig("/cache/maps").dir == "/cache/maps");
}

// "YYYY-MM-DDTHH:MM:SSZ", in UTC whatever the device's zone is: the core compares it with the expiry of a manifest.
void NowUtcIsTheUtcTimeInTheCoresFormat() {
  const char* const saved = std::getenv("TZ");
  const std::string saved_zone = saved != nullptr ? saved : "";
  ::setenv("TZ", "UTC-5", 1);  // a zone five hours ahead: a local-time reading would show it
  ::tzset();
  const std::string text = qs::NowUtc();
  const std::time_t before = std::time(nullptr);
  QCHECK(text.size() == kUtcTextChars && text[kTPosition] == 'T' && text[kZPosition] == 'Z');
  struct tm parsed {};
  QCHECK(::strptime(text.c_str(), "%Y-%m-%dT%H:%M:%SZ", &parsed) != nullptr);
  const std::time_t read = ::timegm(&parsed);
  QCHECK(read - before <= kClockToleranceS && before - read <= kClockToleranceS);
  if (saved != nullptr) {
    ::setenv("TZ", saved_zone.c_str(), 1);
  } else {
    ::unsetenv("TZ");
  }
  ::tzset();
  QCHECK((std::getenv("TZ") != nullptr) == (saved != nullptr));  // the zone is the process's own again
  QCHECK(saved == nullptr || saved_zone == std::getenv("TZ"));
}

// The set file a registration writes is unlinked once the registry holds its descriptor: the test directory is left as it was found.
void ARegisteredSetFileIsNotLeftInTheTestDirectory() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha_cleanup", ValidSet()));
  QCHECK(::access(SetFilePath("alpha_cleanup").c_str(), F_OK) != 0);
  ms::ResetForTest();
}

// ---- MountWhenManifestReady ---------------------------------------------------------------------------------------------

// The set is mounted against the game's manifest through `has`, and the shadow is published once; the call answers true.
void ABuiltManifestMountsTheRegisteredSetsAndPublishesTheirShadow() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha", ValidSet()));
  QCHECK(qs::MountWhenManifestReady(&BuiltAtOnce, &Holds, &Publish, kLongWaitMs, kKeepGoing));
  QCHECK(g_published == 1);
  QCHECK(g_observed == 1 && g_observed_sets == 1);
  QCHECK(g_observed_stock_records == kManifestRecords);  // the registry was given the game's live record count
  QCHECK(g_looks >= 1 && Last("maps_mount").empty());  // it did not wait or give up
  ms::ResetForTest();
}

// The registry asks the game's manifest, not its own: a record the game already holds refuses the set, and the call answers false
// with nothing published.
void ASetThatTheGameAlreadyHoldsIsNotMounted() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha_stock", ValidSet()));
  g_holds_answer = true;
  QCHECK(!qs::MountWhenManifestReady(&BuiltAtOnce, &Holds, &Publish, kLongWaitMs, kKeepGoing));
  QCHECK(g_holds_calls > 0 && g_published == 0);
  ms::ResetForTest();
}

// With nothing registered the mount mounts no set: false, and no shadow.
void NothingRegisteredMountsNothing() {
  Reset();
  QCHECK(!qs::MountWhenManifestReady(&BuiltAtOnce, &Holds, &Publish, kLongWaitMs, kKeepGoing));
  QCHECK(g_published == 0 && g_observed == 1);
  ms::ResetForTest();
}

// The wait polls for the manifest: it looks again after a poll step, mounts when the manifest is built, and does not give up.
void TheWaitPollsUntilTheManifestIsBuilt() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha_wait", ValidSet()));
  QCHECK(qs::MountWhenManifestReady(&BuiltAfterTwoLooks, &Holds, &Publish, kLongWaitMs, kKeepGoing));
  QCHECK(g_looks > kPollsBeforeReady);
  QCHECK(g_published == 1 && Last("maps_mount").empty());
  ms::ResetForTest();
}

// KNOWN-WRONG #2764: a manifest that is not built within the wait abandons the run's registration for good. The record says so, with the
// time waited, and later registrations are refused too_late; the observer hears of the end with an empty report.
void AManifestThatIsNeverBuiltAbandonsTheRegistrationAndSaysSo() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha_never", ValidSet()));
  QCHECK(!qs::MountWhenManifestReady(&NeverBuilt, &Holds, &Publish, kShortWaitMs, kKeepGoing));
  const std::string record = Last("maps_mount");
  QCHECK(Field(record, "result") == "abandoned");
  QCHECK(std::stoul(Field(record, "waited_ms")) >= kShortWaitMs);  // it waited at least the time it was given
  QCHECK(Field(record, "consequence") != "<absent>");
  QCHECK(g_published == 0 && g_observed == 1 && g_observed_sets == 0);
  QCHECK(ms::Held() == 0);
  QCHECK(!Register(kBetaOwner, "beta_late", ValidSet(kBetaLevel, kBetaComponent)));  // kTooLate
  ms::ResetForTest();
}

// A wait with no time to give looks once and gives up at once: waited_ms is 0, not a poll step.
void AWaitOfNoTimeGivesUpWithoutWaiting() {
  Reset();
  QCHECK(!qs::MountWhenManifestReady(&NeverBuilt, &Holds, &Publish, kNoWaitMs, kKeepGoing));
  QCHECK(Field(Last("maps_mount"), "result") == "abandoned" && Field(Last("maps_mount"), "waited_ms") == "0");
  QCHECK(g_looks == 1);
  ms::ResetForTest();
}

// A shutdown (keep_going false) ends the wait at once: abandoned with no time waited, and not a single poll step slept.
void AStoppedWorkerGivesTheWaitUpAtOnce() {
  Reset();
  QCHECK(!qs::MountWhenManifestReady(&NeverBuilt, &Holds, &Publish, kLongWaitMs, [] { return false; }));
  const std::string record = Last("maps_mount");
  QCHECK(Field(record, "result") == "abandoned" && Field(record, "waited_ms") == "0");
  ms::ResetForTest();
}

// ---- MountLaterNow ------------------------------------------------------------------------------------------------------

// Nothing waits: with the manifest not built it answers false and mounts nothing; with it built but no first mount yet the registry has
// nothing to mount later either.
void ALaterMountOfAnUnbuiltManifestOrBeforeTheFirstMountMountsNothing() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha_later0", ValidSet()));
  QCHECK(!qs::MountLaterNow(&NeverBuilt, &Holds, &Publish));
  QCHECK(g_published == 0 && g_observed == 0 && ms::Held() == 1);
  QCHECK(g_looks == 1);  // it looked once and did not wait
  QCHECK(!qs::MountLaterNow(&BuiltAtOnce, &Holds, &Publish));  // no first mount: the registry holds the set for it
  QCHECK(g_published == 0);
  ms::ResetForTest();
}

// A map registered after the first mount mounts on the calling thread, and the one new shadow, holding every mounted set, is published.
void AMapRegisteredAfterTheFirstMountMountsLaterAndIsPublished() {
  Reset();
  QCHECK(Register(kAlphaOwner, "alpha_later1", ValidSet()));
  QCHECK(qs::MountWhenManifestReady(&BuiltAtOnce, &Holds, &Publish, kLongWaitMs, kKeepGoing));
  QCHECK(g_published == 1);
  QCHECK(Register(kBetaOwner, "beta_later1", ValidSet(kBetaLevel, kBetaComponent)));
  QCHECK(qs::MountLaterNow(&BuiltAtOnce, &Holds, &Publish));
  QCHECK(g_published == kPublishedAfterBeta);
  QCHECK(g_observed_stock_records == kManifestRecords);  // and a later mount is given it too
  QCHECK(ms::MountedLevelNames().size() == kMountedAfterBeta);
  QCHECK(!qs::MountLaterNow(&BuiltAtOnce, &Holds, &Publish));  // nothing new: false, nothing published
  QCHECK(g_published == kPublishedAfterBeta);
  // The game's manifest reads as unbuilt (the count is 0): a map registered now is left for a later call, not mounted against nothing.
  QCHECK(Register("map:gamma", "gamma_later1", ValidSet(kGammaLevel, kGammaComponent)));
  QCHECK(!qs::MountLaterNow(&NeverBuilt, &Holds, &Publish));
  QCHECK(g_published == kPublishedAfterBeta && ms::MountedLevelNames().size() == kMountedAfterBeta);
  QCHECK(qs::MountLaterNow(&BuiltAtOnce, &Holds, &Publish));
  QCHECK(g_published == kPublishedAfterGamma && ms::MountedLevelNames().size() == kMountedAfterGamma);
  ms::ResetForTest();
}

// ---- Start's refusals ---------------------------------------------------------------------------------------------------

// No environment: refused with that reason, and the service is not marked as started (the next start with an env is judged on its own).
// A channel that does not run (no maps.base_url) starts nothing: the core says why, and no pin tick, mount observer or thread is made.
// A second start is refused already_started, whatever it brings.
void StartRefusesWithoutAnEnvironmentAndAfterTheFirstStart() {
  Reset();
  stubs::Get().install_tick_calls = 0;
  ms::SetMountObserver(nullptr);
  QCHECK(!qs::Start(nullptr, nevr_map_channel::Settings{}, nullptr));
  QCHECK(Field(Last("maps_start"), "result") == "refused" && Field(Last("maps_start"), "reason") == "no_env");

  cdn_test::FakeEnv env;
  nevr_map_channel::Settings unconfigured;
  unconfigured.reason = "not_configured";  // what ReadSettings answers when maps.base_url is empty
  g_lines.clear();
  QCHECK(!qs::Start(&env, unconfigured, nullptr));
  bool core_said_why = false;
  for (const cdn_test::Record& record : env.records) {
    if (record.name == "map_start" && record.Get("reason") == "not_configured") core_said_why = true;
  }
  QCHECK(core_said_why);
  QCHECK(stubs::Get().install_tick_calls == 0);  // the pin's tick is installed only when the channel runs
  QCHECK(Last("maps_start").empty());            // and the service says nothing more

  g_lines.clear();
  QCHECK(!qs::Start(&env, unconfigured, nullptr));
  QCHECK(Field(Last("maps_start"), "result") == "refused" && Field(Last("maps_start"), "reason") == "already_started");
  g_lines.clear();
  QCHECK(!qs::Start(nullptr, unconfigured, nullptr));
  QCHECK(Field(Last("maps_start"), "reason") == "no_env");  // no env is named before a second start
  QCHECK(stubs::Get().install_tick_calls == 0);
  // A run that does not run has no map script to answer for: the registry's script waiver is not installed, so a map's set that carries
  // a script record is refused even for a script the table holds.
  QCHECK(nevr_quest::script_redirect::map_scripts::Admit(kAlphaOwner, kPart) == nullptr);
  QCHECK(!Register(kAlphaOwner, "alpha_script", SetWithScript()));
  nevr_quest::script_redirect::map_scripts::ResetForTest();
}

// The process end's Stop of a channel that never ran has nothing to stop: it returns at once (a client whose settings turn the channel off
// still reaches it at exit) and writes no record, in particular no join_timeout Warning.
void StopOfAChannelThatNeverRanReturnsAtOnceAndSaysNothing() {
  g_lines.clear();
  const auto begin = std::chrono::steady_clock::now();
  qs::Stop(kStopWaitNeverRan);
  const auto took = std::chrono::steady_clock::now() - begin;
  QCHECK(took < kStopWaitNeverRan);
  QCHECK(g_lines.empty());
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  TheChannelConfigIsTheQuestsAndNamesTheCacheDirectory();
  NowUtcIsTheUtcTimeInTheCoresFormat();
  ARegisteredSetFileIsNotLeftInTheTestDirectory();
  ABuiltManifestMountsTheRegisteredSetsAndPublishesTheirShadow();
  ASetThatTheGameAlreadyHoldsIsNotMounted();
  NothingRegisteredMountsNothing();
  TheWaitPollsUntilTheManifestIsBuilt();
  AManifestThatIsNeverBuiltAbandonsTheRegistrationAndSaysSo();
  AWaitOfNoTimeGivesUpWithoutWaiting();
  AStoppedWorkerGivesTheWaitUpAtOnce();
  ALaterMountOfAnUnbuiltManifestOrBeforeTheFirstMountMountsNothing();
  AMapRegisteredAfterTheFirstMountMountsLaterAndIsPublished();
  StartRefusesWithoutAnEnvironmentAndAfterTheFirstStart();
  StopOfAChannelThatNeverRanReturnsAtOnceAndSaysNothing();
  ms::ResetForTest();
  return quest_test::Failures() == 0 ? 0 : 1;
}
