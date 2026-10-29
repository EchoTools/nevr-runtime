#include "quest/invite/wizard_metric_hook.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/outside_game_call.h"
#include "quest/sentinel/pinned_targets.h"
#include "runtime/patch/invite_wizard.h"

namespace nevr_quest::wizard_metric {

namespace {

using Thunk = sentinel::pinned::LibR15MetricLogThunk;

constexpr const char* kTag = "NEVR.INVITE";

// The submit stays off until the invite service is linked (S3). While this is false, every plan is logged and not sent.
constexpr bool kSubmitEnabled = false;

sentinel::GotHook g_hook;
std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_copies{0};
std::atomic<unsigned long long> g_wizard{0};
std::atomic<unsigned long long> g_submits{0};

const char* OpName(nevr_invite::Op op) noexcept { return op == nevr_invite::Op::Create ? "create" : "resolve"; }

// The reason token for a plan that is not sent. These are the PC tokens (invite_wizard.h, NotSentLine).
const char* SkipReason(nevr_invite::Skip skip) noexcept {
  switch (skip) {
    case nevr_invite::Skip::None:
      return "none";
    case nevr_invite::Skip::Empty:
      return "empty";
    case nevr_invite::Skip::Malformed:
      return "malformed";
    case nevr_invite::Skip::TooLong:
      return "too_long";
    case nevr_invite::Skip::Incomplete:
      return "incomplete";
    case nevr_invite::Skip::NoLevel:
      return "no_level";
    case nevr_invite::Skip::NotConfigured:
      return "not_configured";
    case nevr_invite::Skip::NoToken:
      return "no_token";
    case nevr_invite::Skip::Busy:
      return "busy";
    case nevr_invite::Skip::Stopped:
      return "stopped";
  }
  return "none";
}

// The code itself is never passed here: a request line carries only its length.
void NotSent(const char* reason, const nevr_invite::Plan& plan) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "invite_request_not_sent",
                      {{"reason", reason}, {"op", OpName(plan.op)}, {"code_len", plan.code_len}, {"tag", kTag}});
}

// S3 replaces this body with the call to nevr_invite_service::Submit. Nothing calls it while kSubmitEnabled is false.
NEVR_OUTSIDE_GAME_CALL void SubmitPlan(const nevr_invite::Plan& /*plan*/) noexcept {
  g_submits.fetch_add(1, std::memory_order_relaxed);
}

// Everything the observer does before the game's function runs. It returns before the original is called, so it is
// outside the game call (outside_game_call.h) and may use the parser and the logger.
NEVR_OUTSIDE_GAME_CALL void Observe(const char* json, unsigned long long length) noexcept {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (json == nullptr || length == 0 || length > nevr_invite::kMetricMaxBytes) return;
  g_copies.fetch_add(1, std::memory_order_relaxed);

  const auto size = static_cast<std::size_t>(length);
  char copy[nevr_invite::kMetricMaxBytes];
  if (!CopyGameBytes(json, size, copy, sizeof(copy))) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_observer_skip",
                        {{"reason", "copy_failed"}, {"bytes", length}, {"tag", kTag}});
    return;
  }

  const std::string_view line(copy, size);
  if (!nevr_invite::MayBeWizardMetric(line)) return;
  const nevr_invite::Metric metric = nevr_invite::ParseMetric(line);
  if (metric.kind == nevr_invite::Kind::NotWizard) return;
  g_wizard.fetch_add(1, std::memory_order_relaxed);

  const bool create = metric.kind == nevr_invite::Kind::Create;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "invite_click",
                      {{"click", create ? "create_invite" : "join_via_code"}, {"panel", "deep_linking"}, {"tag", kTag}});

  const nevr_invite::Plan plan = nevr_invite::PlanRequest(metric, nevr_invite::Choices{}, nullptr);
  if (plan.skip != nevr_invite::Skip::None) {
    NotSent(SkipReason(plan.skip), plan);
    return;
  }
  if (!kSubmitEnabled) {
    NotSent("observe_only", plan);
    return;
  }
  SubmitPlan(plan);
}

// Runs before the game's function, on the thread that called it. Observe returns first, so the game is called once,
// with the pointer and length it was given.
void Handler(Thunk::Fn original, const char* json, unsigned long long length) noexcept {
  Observe(json, length);
  original(json, length);
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

// The one line a refused install writes. A prologue mismatch also carries the bytes expected and the bytes found.
void LogOff(const char* reason, const unsigned char* found) noexcept {
  char va[19];
  const char* const vaText = sentinel::HexString(va, sentinel::pinned::kMetricLogVaddr);
  if (found == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_observer_off",
                        {{"reason", reason}, {"name", "MetricLog"}, {"va", vaText}, {"tag", kTag}});
    return;
  }
  unsigned char expected[sizeof(sentinel::pinned::kMetricLogCode)];
  std::memcpy(expected, sentinel::pinned::kMetricLogCode, sizeof(expected));
  const std::string expectedHex = nevr_invite::HexBytes(expected, sizeof(expected));
  const std::string actualHex = nevr_invite::HexBytes(found, sizeof(expected));
  sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_observer_off",
                      {{"reason", reason},
                       {"name", "MetricLog"},
                       {"va", vaText},
                       {"expected", expectedHex.c_str()},
                       {"actual", actualHex.c_str()},
                       {"tag", kTag}});
}

}  // namespace

const char* SkipToken(nevr_invite::Skip skip) noexcept { return SkipReason(skip); }

bool CopyGameBytes(const char* source, std::size_t length, char* destination, std::size_t capacity) noexcept {
  if (source == nullptr || destination == nullptr || length == 0 || length > capacity) return false;
  int fds[2] = {-1, -1};
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return false;
  // The write is the access check: the kernel copies from the source, and an unmapped byte fails it with EFAULT.
  bool ok = write(fds[1], source, length) == static_cast<ssize_t>(length);
  std::size_t got = 0;
  while (ok && got < length) {
    const ssize_t n = read(fds[0], destination + got, length - got);
    if (n <= 0) {
      ok = false;
      break;
    }
    got += static_cast<std::size_t>(n);
  }
  close(fds[0]);
  close(fds[1]);
  return ok;
}

bool PrologueMatches(const unsigned char* code) noexcept {
  if (code == nullptr) return false;
  unsigned char pinned[sizeof(sentinel::pinned::kMetricLogCode)];
  std::memcpy(pinned, sentinel::pinned::kMetricLogCode, sizeof(pinned));
  return std::memcmp(code, pinned, sizeof(pinned)) == 0;
}

bool Install(sentinel::ImageLookup lookup) noexcept {
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    LogOff("not_patched", nullptr);
    return false;
  }
  const auto* prologue = reinterpret_cast<const unsigned char*>(image.base + sentinel::pinned::kMetricLogVaddr);
  if (!PrologueMatches(prologue)) {
    LogOff("prologue_mismatch", prologue);
    return false;
  }
  Thunk::Arm(kHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, sentinel::pinned::LibR15MetricLog(), lookup);
  if (status != sentinel::GotStatus::kOk) {
    LogOff(sentinel::GotStatusName(status), nullptr);
    return false;
  }
  char slot[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "invite_observer_installed",
                      {{"target", "MetricLog"},
                       {"slot", sentinel::HexString(slot, sentinel::pinned::kMetricLogSlotVaddr)},
                       {"tag", kTag}});
  return true;
}

void ArmForTest() noexcept { Thunk::Arm(kHook); }

void ResetForTest() noexcept {
  g_calls.store(0, std::memory_order_relaxed);
  g_copies.store(0, std::memory_order_relaxed);
  g_wizard.store(0, std::memory_order_relaxed);
  g_submits.store(0, std::memory_order_relaxed);
}

Totals CurrentTotals() noexcept {
  Totals totals;
  totals.calls = g_calls.load(std::memory_order_relaxed);
  totals.copies = g_copies.load(std::memory_order_relaxed);
  totals.wizard = g_wizard.load(std::memory_order_relaxed);
  totals.submits = g_submits.load(std::memory_order_relaxed);
  return totals;
}

}  // namespace nevr_quest::wizard_metric
