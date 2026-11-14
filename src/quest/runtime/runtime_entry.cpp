// The Quest runtime's entry: the one export of libnevr-runtime.so, NEVR_GetRuntimeApi (core/quest_runtime_abi.h), and the
// initialize it returns. initialize runs the constructor sequence, RunSentinelConstructor (integration/production_steps.cpp),
// over this library's own copy of the sentinel's sources (sentinel/CMakeLists.txt, target nevr_quest_runtime). That copy has
// no load-time entry: entry.cpp leaves its constructor, marker and JNI_OnLoad out when NEVR_QUEST_RUNTIME_LIBRARY is defined,
// and the build's check (justfile, _check-quest-runtime-library) fails on any other initializer.
//
// The shim (libovrplatformloader.so) loads this library from its constructor (sentinel/entry.cpp, StartRuntime, through
// shim/runtime_load.cpp) and calls initialize on that thread. The shim owns what a process has once: the crash handlers
// (sentinel::Arm does nothing in this copy), the crash report's build text, the crash-first hooks, the read of the variant stamp
// and the log file. This copy opens no log file: initialize registers the shim's log (NevrQuestHostContext::log) as the sink of
// this copy's records (sentinel::SetHostRecordSink), and a shim with no log leaves them on logcat alone. What the sequence reads
// from the shim reaches it here: the variant, the login identity and the UI looper.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "core/build_identity.h"
#include "core/quest_runtime_abi.h"
#include "generated/nevr_build_info.h"
#include "quest/combat/combat_active.h"
#include "quest/integration/entry_hooks.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/sentinel/build_variant.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/toast_presenter.h"
#include "quest/shim/runtime_load.h"

namespace {

// The tag of the records this file writes: the runtime's own bootstrap (docs/standards/logging.md, NEVR.BOOT).
constexpr char kBootTag[] = "NEVR.BOOT";

// The most fields one forwarded record carries, which sizes the stack arrays of ForwardToHost; a longer list is cut and the record
// says so (a `truncated` field).
constexpr std::size_t kMaxForwardedFields = 48;
// Digits of a long long, a sign and the terminator, with room to spare: an integer field's text.
constexpr std::size_t kIntegerTextBytes = 24;
// The keys the shim's RuntimeLog (sentinel/entry.cpp) turns back into the record's tag and message, and the field that marks a cut.
constexpr char kTagKey[] = "tag";
constexpr char kMessageKey[] = "message";
constexpr char kTruncatedKey[] = "truncated";
constexpr char kTruncatedValue[] = "true";

// The shim's log (NevrQuestHostContext::log), set once by Initialize before the first record and read from any thread. Null: the
// shim has none, or Initialize has not run.
std::atomic<NevrQuestLogFn> g_hostLog{nullptr};

// The level number of NevrQuestLogFn (core/quest_runtime_abi.h) for a sentinel level.
std::uint32_t HostLevel(sentinel::LogLevel level) noexcept {
  namespace rl = nevr_quest::runtime_load;
  switch (level) {
    case sentinel::LogLevel::kDebug: return rl::kLogLevelDebug;
    case sentinel::LogLevel::kWarn: return rl::kLogLevelWarning;
    case sentinel::LogLevel::kError: return rl::kLogLevelError;
    case sentinel::LogLevel::kInfo: break;
  }
  return rl::kLogLevelInfo;
}

// The sink of this copy's records (sentinel::SetHostRecordSink): one record to the shim's log. The first field keyed "tag" is the
// call's tag and the first keyed "message" its message, which is how the shim's RuntimeLog puts them back; every other field follows
// in order. The ABI carries text only, so an integer field arrives as its decimal text. Returns true only when the shim's log was
// called; false leaves the record to the copy's own writer, which here is logcat.
bool ForwardToHost(sentinel::LogLevel level, const char* event, const sentinel::LogField* fields, std::size_t count) {
  const NevrQuestLogFn hostLog = g_hostLog.load(std::memory_order_acquire);
  if (hostLog == nullptr) return false;
  NevrQuestLogField forwarded[kMaxForwardedFields + 1];  // one more than the cap: the cut marker
  char integers[kMaxForwardedFields][kIntegerTextBytes];
  const char* tag = nullptr;
  const char* message = nullptr;
  std::uint32_t kept = 0;
  bool truncated = false;
  for (std::size_t i = 0; i < count; ++i) {
    if (i >= kMaxForwardedFields) {
      truncated = true;
      break;
    }
    const sentinel::LogField& field = fields[i];
    if (field.key == nullptr) continue;
    const char* value = field.text;
    if (value == nullptr) {
      std::snprintf(integers[i], sizeof(integers[i]), "%lld", field.number);
      value = integers[i];
    }
    if (tag == nullptr && std::strcmp(field.key, kTagKey) == 0) {
      tag = value;
    } else if (message == nullptr && std::strcmp(field.key, kMessageKey) == 0) {
      message = value;
    } else {
      forwarded[kept++] = NevrQuestLogField{field.key, value};
    }
  }
  if (truncated) forwarded[kept++] = NevrQuestLogField{kTruncatedKey, kTruncatedValue};
  hostLog(HostLevel(level), tag, event, message, forwarded, kept);
  return true;
}

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

  // The shim's log first: every record below, and every one the sequence writes, reaches the process's one log file, and none of
  // them opens a file here.
  g_hostLog.store(host->log, std::memory_order_release);
  sentinel::SetHostRecordSink(&ForwardToHost);

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

  // The shim has installed the crash-first hooks (entry_hooks.h); the sequence's InstallCrashFirst step reads this.
  nevr_quest::integration::SetShimOwnsCrashFirst(true);

  nevr_quest::combat::SetVariant(variant);
  nevr_quest::redirect::SetLoginIdentity({"nevr_quest", kind, nevr_build::kVersion, nevr_build::kCommit});
  nevr_quest::toast::CaptureUiLooper();

  sentinel::LogFields(sentinel::LogLevel::kInfo, "runtime_initialize",
                      {{"tag", kBootTag},
                       {"identity", nevr_build_identity::IdentityLiteral()},
                       {"variant", nevr_quest::build_variant::VariantName(variant)},
                       {"kind", kind},
                       {"host_log", host->log != nullptr ? "shim" : "logcat_only"}});
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
