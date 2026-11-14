// The Quest runtime's entry: the one export of libnevr-runtime.so, NEVR_GetRuntimeApi (core/quest_runtime_abi.h), and the
// initialize it returns. initialize runs the constructor sequence, RunSentinelConstructor (integration/production_steps.cpp),
// over this library's own copy of the sentinel's sources (sentinel/CMakeLists.txt, target nevr_quest_runtime). That copy has
// no load-time entry: entry.cpp leaves its constructor, marker and JNI_OnLoad out when NEVR_QUEST_RUNTIME_LIBRARY is defined,
// and the build's check (justfile, _check-quest-runtime-library) fails on any other initializer.
//
// initialize sets the inputs the constructor sets from the variant stamp and this build (the variant, the crash report's build
// line, the login identity, the UI looper), then runs the sequence. Nothing in this tree calls initialize: no code maps this
// library, so the sentinel still runs its own copy in process.

#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "core/build_identity.h"
#include "core/crash_report.h"
#include "core/quest_runtime_abi.h"
#include "generated/nevr_build_info.h"
#include "quest/combat/combat_active.h"
#include "quest/integration/entry_hooks.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/sentinel/build_variant.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/toast_presenter.h"

namespace {

// The tag of the records this file writes: the runtime's own bootstrap (docs/standards/logging.md, NEVR.BOOT).
constexpr char kBootTag[] = "NEVR.BOOT";

std::int32_t Initialize(const NevrQuestHostContext* host) noexcept;

// The descriptor is constant data: the header is the v1 size and the major and minor of nevr_quest_runtime_abi.
constexpr NevrQuestRuntimeApi kRuntimeApi = {
    {nevr_quest_runtime_abi::kRuntimeApiV1Size, nevr_quest_runtime_abi::kMajor, nevr_quest_runtime_abi::kMinor, 0},
    &Initialize};

// Returns 0 when the sequence ran. A refusal returns the nevr_runtime_abi::Status that refused it, never 0, and writes one
// Error record of the refusal (`runtime_initialize_refused`), so a refused start is never silent.
std::int32_t Initialize(const NevrQuestHostContext* host) noexcept {
  const nevr_runtime_abi::Status refused = nevr_quest_runtime_abi::ValidateHostContext(host);
  if (refused != nevr_runtime_abi::Status::kOk) {
    sentinel::LogFields(sentinel::LogLevel::kError, "runtime_initialize_refused",
                        {{"tag", kBootTag},
                         {"reason", "host_context"},
                         {"status", nevr_runtime_abi::StatusName(refused)},
                         {"consequence", "the sequence did not run: this runtime hooks nothing"}});
    return static_cast<std::int32_t>(refused);
  }
  if (host->variant > nevr_quest::build_variant::kLastVariantWord) {
    sentinel::LogFields(sentinel::LogLevel::kError, "runtime_initialize_refused",
                        {{"tag", kBootTag},
                         {"reason", "variant_out_of_range"},
                         {"variant", host->variant},
                         {"last_variant", nevr_quest::build_variant::kLastVariantWord},
                         {"consequence", "the sequence did not run: this runtime hooks nothing"}});
    return static_cast<std::int32_t>(nevr_runtime_abi::Status::kInvalidField);
  }

  const auto variant = static_cast<nevr_quest::build_variant::Variant>(host->variant);
  const char* kind = nevr_build::kIsDev ? "dev" : "release";

  nevr_quest::combat::SetVariant(variant);
  char buildText[nevr_crash::kBuildBytes];
  std::snprintf(buildText, sizeof(buildText), "version=%s kind=%s variant=%s", nevr_build::kVersion, kind,
                nevr_quest::build_variant::VariantName(variant));
  nevr_crash::SetBuild(buildText);
  nevr_quest::redirect::SetLoginIdentity({"nevr_quest", kind, nevr_build::kVersion, nevr_build::kCommit});
  nevr_quest::toast::CaptureUiLooper();

  sentinel::LogFields(sentinel::LogLevel::kInfo, "runtime_initialize",
                      {{"tag", kBootTag},
                       {"identity", nevr_build_identity::IdentityLiteral()},
                       {"variant", nevr_quest::build_variant::VariantName(variant)},
                       {"kind", kind}});
  nevr_quest::integration::RunSentinelConstructor();
  return 0;
}

}  // namespace

extern "C" {

// The runtime's one export (nevr_runtime_abi::kGetRuntimeApiExport). Returns constant data; no code runs to build it.
__attribute__((visibility("default"))) const NevrQuestRuntimeApi* NEVR_GetRuntimeApi() { return &kRuntimeApi; }

}  // extern "C"

static_assert(std::is_same_v<decltype(&NEVR_GetRuntimeApi), NevrQuestGetRuntimeApiFn>,
              "NEVR_GetRuntimeApi must have the signature of NevrQuestGetRuntimeApiFn (core/quest_runtime_abi.h)");
