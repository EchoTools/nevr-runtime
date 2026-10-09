#include "login_prompt_hook.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/auth/prompt_board.h"

namespace nevr_quest::login_prompt {

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace layout = sentinel::pinned::game_layout;
using sentinel::pinned::CR15NetGameOpaque;

sentinel::GotHook g_errorHook;
sentinel::GotHook g_updateHook;

std::atomic<std::uint64_t> g_shown{0};
std::atomic<std::uint64_t> g_refreshed{0};
std::atomic<std::uint64_t> g_kept{0};
std::atomic<std::uint64_t> g_notLocal{0};
std::atomic<std::uint64_t> g_busy{0};
std::atomic<std::uint64_t> g_layoutMismatch{0};

// The object whose error block holds a prompt, and the board version written there. The pointer is
// only compared with the `this` the game passes to Update; it is never dereferenced on its own.
std::atomic<CR15NetGameOpaque*> g_object{nullptr};
std::atomic<std::uint64_t> g_applied{0};
// One writer of the block at a time (the two hooks could run on different threads); a hook that
// finds it taken does nothing now: Update tries again on the next frame.
std::atomic_flag g_writing = ATOMIC_FLAG_INIT;
// The game's own block, saved when the prompt replaced it, restored when the board is withdrawn.
unsigned char g_saved[layout::kErrorBlockBytes];

// The game's own local login-failure texts. Each is passed only to CNSUser::LogInFailed (through
// DeferredLogInFailed or the CNSOVRUser vtable slot): libpnsovr 0x5569ab (LogInInternal,
// GotUserProofCB), 0x556a5a and 0x556abf (GotUserProofCB), 0x556b40 (UpdateInternal), 0x584c42 and
// libr15 0x31737d7 (CNSUser::SendLogInRequest, ConnectFailedCB). A server-sent failure text is
// never one of these.
constexpr const char* kLocalFailures[] = {
    "Log in request failed: One or more prerequisites are missing",
    "Log in request failed: Failed to get user proof",
    "Log in request failed: Client error",
    "Log in request failed: Cryptography error",
    "Log in request failed: Service unavailable",
};

bool Equal(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

std::int32_t State(const CR15NetGameOpaque* self) noexcept {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(self) + layout::kStateOffset;
  std::int32_t state = 0;
  __builtin_memcpy(&state, p, sizeof(state));
  return state;
}

unsigned char* Block(CR15NetGameOpaque* self) noexcept {
  return reinterpret_cast<unsigned char*>(self) + layout::kErrorBlockOffset;
}

// The block holds the one-line `message` the way SetErrorMessage(char const*) leaves it
// (0x1241050): the byte before the lines is 0 (`strb wzr` at 0x1241080), the first line is the
// message cut at 63 characters, NUL-terminated.
bool HoldsMessage(const unsigned char* block, const char* message) noexcept {
  if (block[0] != 0) return false;
  const unsigned char* line = block + 1;
  std::size_t i = 0;
  for (; i + 1 < layout::kErrorLineBytes && message[i] != '\0'; ++i) {
    if (line[i] != static_cast<unsigned char>(message[i])) return false;
  }
  return line[i] == '\0';
}

// Writes `text` ('\n'-separated, at most four lines of at most 63 characters, as the board holds
// it) the way SetErrorMessage(4 args) does: the byte before the lines is 1, each line is cut at 63
// and NUL-filled to 64.
void WriteLines(unsigned char* block, const char* text) noexcept {
  block[0] = 1;
  const char* p = text;
  for (std::size_t n = 0; n < layout::kErrorLines; ++n) {
    unsigned char* line = block + 1 + n * layout::kErrorLineBytes;
    std::size_t i = 0;
    while (*p != '\0' && *p != '\n' && i + 1 < layout::kErrorLineBytes) line[i++] = static_cast<unsigned char>(*p++);
    while (*p != '\0' && *p != '\n') ++p;  // the rest of an over-long line
    if (*p == '\n') ++p;
    while (i < layout::kErrorLineBytes) line[i++] = 0;
  }
}

void CopyBlock(unsigned char* to, const unsigned char* from) noexcept {
  for (std::size_t i = 0; i < layout::kErrorBlockBytes; ++i) to[i] = from[i];
}

// The game's login failed: its message is in the block. Runs on the game's call path.
void HookedSetDelimitedErrorMessage(ErrorThunk::Fn original, CR15NetGameOpaque* self, const char* message) noexcept {
  const std::int32_t state = State(self);  // read before the game's own write (it does not change it)
  original(self, message);
  if (state != layout::kStateLoggingIn || message == nullptr || !IsLocalLoginFailure(message)) {
    g_notLocal.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  char text[board::kCapacity + 1];
  board::Mode mode = board::Mode::kPrompt;
  std::uint64_t version = 0;
  const board::ReadResult read = board::Read(text, sizeof(text), &mode, &version);
  if (read == board::ReadResult::kBusy) {
    g_busy.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (read == board::ReadResult::kEmpty || mode != board::Mode::kPrompt) {
    g_kept.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_writing.test_and_set(std::memory_order_acquire)) {
    g_busy.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  unsigned char* block = Block(self);
  if (!HoldsMessage(block, message)) {
    g_layoutMismatch.fetch_add(1, std::memory_order_relaxed);
  } else {
    CopyBlock(g_saved, block);
    WriteLines(block, text);
    g_applied.store(version, std::memory_order_relaxed);
    g_object.store(self, std::memory_order_release);
    g_shown.fetch_add(1, std::memory_order_relaxed);
  }
  g_writing.clear(std::memory_order_release);
}

// Keeps a screen that shows the prompt current. Runs once per game update, before the game's own.
void Refresh(CR15NetGameOpaque* self) noexcept {
  if (State(self) != layout::kStateLoginFailed) {
    // The game left the error screen's state: stop following this object.
    CR15NetGameOpaque* expected = self;
    g_object.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    return;
  }
  if (board::Version() == g_applied.load(std::memory_order_relaxed)) return;
  if (g_writing.test_and_set(std::memory_order_acquire)) return;  // the other writer: next frame
  char text[board::kCapacity + 1];
  board::Mode mode = board::Mode::kPrompt;
  std::uint64_t version = 0;
  const board::ReadResult read = board::Read(text, sizeof(text), &mode, &version);
  if (read == board::ReadResult::kCopied) {
    WriteLines(Block(self), text);
    g_applied.store(version, std::memory_order_relaxed);
    g_refreshed.fetch_add(1, std::memory_order_relaxed);
  } else if (read == board::ReadResult::kEmpty) {
    CopyBlock(Block(self), g_saved);  // withdrawn: the game's own message again
    g_applied.store(version, std::memory_order_relaxed);
    g_refreshed.fetch_add(1, std::memory_order_relaxed);
  }  // kBusy: next frame
  g_writing.clear(std::memory_order_release);
}

void HookedNetGameUpdate(UpdateThunk::Fn original, CR15NetGameOpaque* self, std::uint64_t arg) noexcept {
  if (self != nullptr && self == g_object.load(std::memory_order_acquire)) Refresh(self);
  original(self, arg);
}

NEVR_HOOK_RECORD(kErrorTextHook, ErrorThunk, &HookedSetDelimitedErrorMessage);
NEVR_HOOK_RECORD(kNetGameUpdateHook, UpdateThunk, &HookedNetGameUpdate);

}  // namespace

bool IsLocalLoginFailure(const char* message) noexcept {
  if (message == nullptr) return false;
  for (const char* known : kLocalFailures) {
    if (Equal(message, known)) return true;
  }
  return false;
}

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"login_prompt_text_shown", &g_shown, sentinel::ReportKind::kCalls},
      {"login_prompt_text_refreshed", &g_refreshed, sentinel::ReportKind::kCalls},
      {"login_prompt_text_kept", &g_kept, sentinel::ReportKind::kCalls},
      {"login_prompt_text_not_local", &g_notLocal, sentinel::ReportKind::kCalls},
      {"login_prompt_board_busy", &g_busy, sentinel::ReportKind::kFaults},
      {"login_prompt_layout_mismatch", &g_layoutMismatch, sentinel::ReportKind::kFaults},
      {"login_prompt_error_thunk_faults", &ErrorThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"login_prompt_update_thunk_faults", &UpdateThunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "login_prompt_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool Install() noexcept {
  ErrorThunk::Arm(kErrorTextHook);
  UpdateThunk::Arm(kNetGameUpdateHook);
  const sentinel::GotStatus error =
      sentinel::InstallThunk<ErrorThunk>(g_errorHook, sentinel::pinned::LibR15SetDelimitedErrorMessage());
  const sentinel::GotStatus update =
      sentinel::InstallThunk<UpdateThunk>(g_updateHook, sentinel::pinned::LibR15NetGameUpdate());
  const bool ok = error == sentinel::GotStatus::kOk && update == sentinel::GotStatus::kOk;
  const bool any = error == sentinel::GotStatus::kOk || update == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "login_prompt_install",
                      {{"result", ok ? "installed" : (any ? "partial" : "failed")},
                       {"error_text", sentinel::GotStatusName(error)},
                       {"update", sentinel::GotStatusName(update)}});
  return ok;
}

void ArmForTest() noexcept {
  ErrorThunk::Arm(kErrorTextHook);
  UpdateThunk::Arm(kNetGameUpdateHook);
}

Counts CurrentCounts() noexcept {
  return {g_shown.load(std::memory_order_relaxed),    g_refreshed.load(std::memory_order_relaxed),
          g_kept.load(std::memory_order_relaxed),     g_notLocal.load(std::memory_order_relaxed),
          g_busy.load(std::memory_order_relaxed),     g_layoutMismatch.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::login_prompt
