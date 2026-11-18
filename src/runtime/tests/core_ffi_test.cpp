// core/update/core_ffi.cpp: the wrappers that turn the install core's C ABI (installer/include/nevr_install.h) into the update
// engine's Core. The C functions are replaced here by fakes that record each call, so a test sees which entry point a wrapper
// chose (NevrPcStage for the PC's empty profile, NevrStage for any other), what error text it reports, and that it frees what
// the core allocated and closes the session once.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/update/core_ffi.h"
#include "installer/include/nevr_install.h"

struct NevrSession {
  int unused = 0;
};

namespace {

// What the fakes were asked, in order, and what they answer.
struct Fake {
  std::vector<std::string> calls;
  int openStatus = NEVR_OK;
  bool openGivesSession = true;
  int fetchStatus = NEVR_OK;
  int stageStatus = NEVR_OK;
  const char* stageOut = nullptr;  // nullptr: the core allocated nothing
  int discardStatus = NEVR_OK;
  std::string lastError = "core said no";
  int freed = 0;
  NevrSession session;

  void Reset() { *this = Fake(); }
};
Fake g_fake;

}  // namespace

extern "C" {
int NevrInstallOpen(const char* config_json, NevrSession** out) {
  g_fake.calls.push_back(std::string("open:") + config_json);
  if (g_fake.openGivesSession) *out = &g_fake.session;
  return g_fake.openStatus;
}
int NevrManifestFetch(NevrSession*) {
  g_fake.calls.push_back("fetch");
  return g_fake.fetchStatus;
}
int NevrPcStage(NevrSession*, const char* root, const char* current, char** out) {
  g_fake.calls.push_back(std::string("pcstage:") + root + "|" + current);
  if (g_fake.stageOut != nullptr) *out = strdup(g_fake.stageOut);
  return g_fake.stageStatus;
}
int NevrStage(NevrSession*, const char* root, const char* profile, const char* current, char** out) {
  g_fake.calls.push_back(std::string("stage:") + root + "|" + profile + "|" + current);
  if (g_fake.stageOut != nullptr) *out = strdup(g_fake.stageOut);
  return g_fake.stageStatus;
}
int NevrPcDiscard(NevrSession*, const char* root, const char* version, const char* sha, const char* reason) {
  g_fake.calls.push_back(std::string("discard:") + root + "|" + version + "|" + sha + "|" + reason);
  return g_fake.discardStatus;
}
int NevrRunEnd(NevrSession*, const char* state, const char* reason) {
  g_fake.calls.push_back(std::string("runend:") + state + "|" + reason);
  return NEVR_OK;
}
const char* NevrLastError(const NevrSession*) { return g_fake.lastError.c_str(); }
void NevrStringFree(char* p) {
  ++g_fake.freed;
  std::free(p);
}
void NevrClose(NevrSession*) { g_fake.calls.push_back("close"); }

// The rest of the header's surface; the wrappers under test do not call them.
int NevrManifestVerify(NevrSession*, const uint8_t*, size_t, const uint8_t*, size_t) { return NEVR_FAILED; }
int NevrReleasePick(NevrSession*, const char*, char**) { return NEVR_FAILED; }
int NevrArtifactFetch(NevrSession*, const char*, const char*, const char*, const char*) { return NEVR_FAILED; }
int NevrArtifactVerifyZip(NevrSession*, const char*, const char*, const char*, const char*, int) { return NEVR_FAILED; }
int NevrPlanBuild(NevrSession*, const char*, const char*, char**) { return NEVR_FAILED; }
const char* NevrCoreVersion(void) { return "fake"; }
}

namespace {

constexpr char kProfile[] = "{\"profile\":\"quest-runtime\"}";

class CoreFfi : public ::testing::Test {
 protected:
  void SetUp() override { g_fake.Reset(); }
};

TEST_F(CoreFfi, OpenReportsAFailedOrSessionlessOpenWithItsOwnWords) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  std::string error;
  g_fake.openStatus = NEVR_FAILED;
  EXPECT_FALSE(core.open("{}", &error));
  EXPECT_EQ(error, "NevrInstallOpen failed");

  g_fake.openStatus = NEVR_OK;
  g_fake.openGivesSession = false;  // OK with no session is still a failure
  error.clear();
  EXPECT_FALSE(core.open("{}", &error));
  EXPECT_EQ(error, "NevrInstallOpen failed");
  EXPECT_FALSE(core.open("{}", nullptr));  // a caller that wants no text gets none and no crash
}

TEST_F(CoreFfi, OpenPassesTheConfigToTheCore) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  std::string error;
  EXPECT_TRUE(core.open("{\"a\":1}", &error));
  ASSERT_EQ(g_fake.calls.size(), 1u);
  EXPECT_EQ(g_fake.calls[0], "open:{\"a\":1}");
}

TEST_F(CoreFfi, ManifestFetchReportsTheCoresLastErrorOnFailure) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  ASSERT_TRUE(core.open("{}", nullptr));
  std::string error;
  EXPECT_TRUE(core.manifest_fetch(&error));
  EXPECT_TRUE(error.empty());
  g_fake.fetchStatus = NEVR_FAILED;
  EXPECT_FALSE(core.manifest_fetch(&error));
  EXPECT_EQ(error, "core said no");
  EXPECT_FALSE(core.manifest_fetch(nullptr));
  g_fake.fetchStatus = NEVR_NEEDS_USER;  // only NEVR_OK is success
  EXPECT_FALSE(core.manifest_fetch(&error));
}

TEST_F(CoreFfi, AnEmptyOrNullProfileStagesThroughThePcEntryPoint) {
  for (const char* profile : {static_cast<const char*>(nullptr), ""}) {
    g_fake.Reset();
    nevr_update::Core core = nevr_update::MakeFfiCore(profile);
    ASSERT_TRUE(core.open("{}", nullptr));
    g_fake.stageOut = "{\"files\":[]}";
    std::string result, error;
    EXPECT_TRUE(core.stage("C:\\nevr", "{\"v\":1}", &result, &error));
    EXPECT_EQ(g_fake.calls.back(), "pcstage:C:\\nevr|{\"v\":1}");
    EXPECT_EQ(result, "{\"files\":[]}");
    EXPECT_EQ(g_fake.freed, 1);  // the core's string is returned to the core
  }
}

TEST_F(CoreFfi, AProfileStagesThroughTheProfileEntryPointWithThatProfile) {
  nevr_update::Core core = nevr_update::MakeFfiCore(kProfile);
  ASSERT_TRUE(core.open("{}", nullptr));
  g_fake.stageOut = "{}";
  std::string result, error;
  EXPECT_TRUE(core.stage("/data/nevr", "{\"v\":2}", &result, &error));
  EXPECT_EQ(g_fake.calls.back(), std::string("stage:/data/nevr|") + kProfile + "|{\"v\":2}");
}

TEST_F(CoreFfi, AFailedStageReportsTheCoresErrorAndStillFreesWhatItAllocated) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  ASSERT_TRUE(core.open("{}", nullptr));
  g_fake.stageStatus = NEVR_NEEDS_USER;
  g_fake.stageOut = "partial";
  std::string result = "untouched", error;
  EXPECT_FALSE(core.stage("r", "c", &result, &error));
  EXPECT_EQ(error, "core said no");
  EXPECT_EQ(result, "untouched");  // a failed stage never reports a result
  EXPECT_EQ(g_fake.freed, 1);
}

TEST_F(CoreFfi, AStageThatAllocatedNothingLeavesTheResultAloneAndFreesNothing) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  ASSERT_TRUE(core.open("{}", nullptr));
  std::string result = "before", error;
  EXPECT_TRUE(core.stage("r", "c", &result, &error));
  EXPECT_EQ(result, "before");
  EXPECT_EQ(g_fake.freed, 0);
}

TEST_F(CoreFfi, RunEndAndDiscardDoNothingWithoutAnOpenSession) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  core.run_end("installed", "ok");
  std::string error = "keep";
  EXPECT_FALSE(core.discard("r", "1.2.3", "abc", "refused", &error));
  EXPECT_EQ(error, "keep");  // no session: nothing to ask the core for
  EXPECT_TRUE(g_fake.calls.empty());
}

TEST_F(CoreFfi, RunEndAndDiscardReachTheCoreOnceOpened) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  ASSERT_TRUE(core.open("{}", nullptr));
  core.run_end("unchanged", "same version");
  EXPECT_EQ(g_fake.calls.back(), "runend:unchanged|same version");
  std::string error;
  EXPECT_TRUE(core.discard("r", "1.2.3", "abc", "refused", &error));
  EXPECT_EQ(g_fake.calls.back(), "discard:r|1.2.3|abc|refused");
  g_fake.discardStatus = NEVR_FAILED;
  EXPECT_FALSE(core.discard("r", "1.2.3", "abc", "refused", &error));
  EXPECT_EQ(error, "core said no");
}

TEST_F(CoreFfi, CloseClosesTheSessionOnceAndForgetsIt) {
  nevr_update::Core core = nevr_update::MakeFfiCore("");
  core.close();  // never opened: nothing to close
  EXPECT_TRUE(g_fake.calls.empty());
  ASSERT_TRUE(core.open("{}", nullptr));
  core.close();
  core.close();
  int closes = 0;
  for (const std::string& call : g_fake.calls) closes += call == "close" ? 1 : 0;
  EXPECT_EQ(closes, 1);
  core.run_end("installed", "after close");  // the session is gone
  EXPECT_EQ(g_fake.calls.back(), "close");
}

}  // namespace
