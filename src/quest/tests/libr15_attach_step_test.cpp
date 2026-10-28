// Host test for the constructor's attach step (integration/libr15_attach_step.h; docs/adr/0021 sections 6, 7, 10 and 12): the
// step runs the real Attach on a libr15 laid out in host memory, and on a verified file registers the plugin identity and the
// build-ID alias exactly once; on any other file it registers nothing and names the reason.
//
// The image is libr15_live_image.h's: the shipped patcher's output for a synthetic stock libr15 (or the stock file itself, or a
// patched file whose build-ID note was changed), driven through Attach's own seams. The registrations are observed through the
// setters' return values, which hand back what was registered before.
//
// Run: libr15_attach_step_test <fixtures-dir> [name-filter]

#include <cstring>
#include <string>

#include "got_hook.h"
#include "libr15_attach.h"
#include "libr15_identity.h"
#include "libr15_live_image.h"
#include "mini_test.h"
#include "quest/integration/libr15_attach_step.h"
#include "quest/patch/libr15_patch_identity.h"
#include "quest/plugin/plugin_platform.h"

NEVR_LIVE_IMAGE_MPROTECT_OVERRIDE()

namespace {

using namespace live_fixture;
using namespace nevr_quest;
using integration::AttachDetail;
using integration::AttachStepOutcome;
using libr15_patch::AttachStatus;

// What the process has registered right now, read and put back: the setters return the previous value.
struct Registered {
  plugin::Libr15PatchSource source;
  sentinel::BuildIdAliasFn alias;
};
Registered Read() {
  Registered r{plugin::SetLibr15PatchSource(nullptr), sentinel::SetBuildIdAlias(nullptr)};
  plugin::SetLibr15PatchSource(r.source);
  sentinel::SetBuildIdAlias(r.alias);
  return r;
}
void Clear() {
  plugin::SetLibr15PatchSource(nullptr);
  sentinel::SetBuildIdAlias(nullptr);
  integration::ResetAttachStepForTest();
}

// Each test starts with nothing registered and ends the same way.
struct Fresh {
  Env env;
  Fresh() { Clear(); }
  ~Fresh() { Clear(); }
};

std::string PatchedId() { return patch::PatchedBuildId(); }

// ---- a verified file ------------------------------------------------------------------------------------------------------

TEST(AVerifiedFileRegistersThePluginIdentityAndTheAliasAndNamesTheStep) {
  Fresh fresh;
  LiveImage live(PatchedBytes());
  g_live = &live;

  const AttachStepOutcome outcome = integration::RunAttachStep(Opts());

  CHECK(outcome.verified);
  CHECK_EQ(std::string(outcome.detail), std::string("verified"));
  const Registered now = Read();
  CHECK(now.source == &integration::PatchStateForPlugins);
  CHECK(now.alias == &integration::BuildIdAliasForPlugins);
  CHECK(libr15_patch::Verified());
}

TEST(TheIdentityIsRegisteredOnceHoweverManyTimesTheStepRuns) {
  Fresh fresh;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK(integration::RunAttachStep(Opts()).verified);
  // A second registration would put these back; clear them so a repeat is visible.
  plugin::SetLibr15PatchSource(nullptr);
  sentinel::SetBuildIdAlias(nullptr);

  const AttachStepOutcome again = integration::RunAttachStep(Opts());

  CHECK(again.verified);  // a second Attach in one process stays verified
  CHECK_EQ(std::string(again.detail), std::string("already_verified"));
  const Registered now = Read();
  CHECK(now.source == nullptr);
  CHECK(now.alias == nullptr);
}

TEST(TheStockLibr15IsRefusedAsNotPatchedAndRegistersNothing) {
  Fresh fresh;
  LiveImage live(Stock());
  g_live = &live;

  const AttachStepOutcome outcome = integration::RunAttachStep(Opts());

  CHECK(!outcome.verified);
  CHECK_EQ(std::string(outcome.detail), std::string(libr15_patch::kReasonNotPatched));
  const Registered now = Read();
  CHECK(now.source == nullptr);
  CHECK(now.alias == nullptr);
  CHECK(!libr15_patch::Verified());
  ExpectNoImageProtection(live);
}

TEST(AFileWithAnyOtherBuildIdIsRefusedAsAMismatchAndRegistersNothing) {
  Fresh fresh;
  std::vector<std::uint8_t> file = PatchedBytes();
  file[FileOffsetOf(file, kNoteDescVaddr)] ^= 0xFF;  // the first byte of the note's 20-byte ID
  LiveImage live(file);
  g_live = &live;

  const AttachStepOutcome outcome = integration::RunAttachStep(Opts());

  CHECK(!outcome.verified);
  CHECK_EQ(std::string(outcome.detail), std::string(libr15_patch::kReasonBuildIdMismatch));
  const Registered now = Read();
  CHECK(now.source == nullptr);
  CHECK(now.alias == nullptr);
}

TEST(NoLoadedLibr15IsAStepFailureWithTheStatusNameAndRegistersNothing) {
  Fresh fresh;

  const AttachStepOutcome outcome = integration::RunAttachStep(Opts());

  CHECK(!outcome.verified);
  CHECK_EQ(std::string(outcome.detail), std::string(libr15_patch::AttachStatusName(AttachStatus::kModuleNotLoaded)));
  const Registered now = Read();
  CHECK(now.source == nullptr);
  CHECK(now.alias == nullptr);
}

TEST(TheExtraRowsOfTheBuildReachAttachInTheOptions) {
  Fresh fresh;
  const libr15_patch::AttachOptions options = integration::BuildAttachOptions();
  CHECK(options.extraRows.size() == sentinel::pinned::LibR15ExtraRows().size());
  CHECK_EQ(options.extraRowsSha256, std::string(sentinel::pinned::LibR15ExtraRowsSha256()));
  CHECK(options.extraRows.empty());  // the public build carries no row and no digest
  CHECK(options.extraRowsSha256.empty());
  // The default seams: Attach runs the real lookup and the real protection and map calls on the device.
  CHECK(options.lookup == sentinel::FindLoadedImage);
  CHECK(options.protect == nullptr && options.map == nullptr && options.end == nullptr);
  CHECK(options.module == nullptr);
}

// ---- the adapters ---------------------------------------------------------------------------------------------------------

TEST(TheAdaptersReadAttachsRecordAndNothingElse) {
  Fresh fresh;
  const plugin::Libr15PatchState before = integration::PatchStateForPlugins();
  CHECK(!before.verified);
  CHECK(before.verifiedBuildId.empty());
  CHECK(integration::BuildIdAliasForPlugins(PatchedId().c_str()) == nullptr);  // not verified: no mapping

  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK(integration::RunAttachStep(Opts()).verified);

  const plugin::Libr15PatchState after = integration::PatchStateForPlugins();
  CHECK(after.verified);
  CHECK_EQ(after.verifiedBuildId, PatchedId());
  // The patched note maps to the stock ID; the stock note, another ID and no note map to nothing.
  const char* const mapped = integration::BuildIdAliasForPlugins(PatchedId().c_str());
  CHECK(mapped != nullptr);
  if (mapped != nullptr) CHECK_EQ(std::string(mapped), std::string(patch::kLibR15StockBuildId));
  CHECK(integration::BuildIdAliasForPlugins(patch::kLibR15StockBuildId) == nullptr);
  CHECK(integration::BuildIdAliasForPlugins("0123456789abcdef0123456789abcdef01234567") == nullptr);
  CHECK(integration::BuildIdAliasForPlugins("") == nullptr);
  CHECK(integration::BuildIdAliasForPlugins(nullptr) == nullptr);
}

TEST(EveryAttachResultHasAFixedDetailToken) {
  libr15_patch::AttachResult r;
  r.status = AttachStatus::kVerified;
  CHECK_EQ(std::string(AttachDetail(r)), std::string("verified"));
  r.status = AttachStatus::kAlreadyVerified;
  CHECK_EQ(std::string(AttachDetail(r)), std::string("already_verified"));
  r.status = AttachStatus::kNotPatched;
  CHECK_EQ(std::string(AttachDetail(r)), std::string("libr15_not_patched"));
  r.status = AttachStatus::kBuildIdMismatch;
  CHECK_EQ(std::string(AttachDetail(r)), std::string("build_id_mismatch"));
  r.status = AttachStatus::kNoBuildId;
  CHECK_EQ(std::string(AttachDetail(r)), std::string(libr15_patch::AttachStatusName(AttachStatus::kNoBuildId)));
  for (const char* reason : {libr15_patch::kReasonRowStub, libr15_patch::kReasonRowSlot, libr15_patch::kReasonRowSegment,
                             libr15_patch::kReasonRowWords, libr15_patch::kReasonTrampolineFailed}) {
    r.status = AttachStatus::kRowFailed;
    r.reason = reason;
    CHECK_EQ(std::string(AttachDetail(r)), std::string(reason));
  }
  r.status = AttachStatus::kTrampolineFailed;
  r.reason = "an unknown token";
  CHECK_EQ(std::string(AttachDetail(r)), std::string(libr15_patch::AttachStatusName(AttachStatus::kTrampolineFailed)));
}

}  // namespace

extern "C" {
// liblog is not linked on the host; what the sentinel's log writes to logcat goes nowhere here.
int __android_log_write(int, const char*, const char*) { return 1; }
int __android_log_print(int, const char*, const char*, ...) { return 1; }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: libr15_attach_step_test <fixtures-dir> [name-filter]\n");
    return 2;
  }
  g_fixtures = argv[1];
  return mini_test::RunAll(argc - 1, argv + 1);
}
