#include "login_prompt_hook.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/auth/prompt_board.h"
#include "quest/game_login_failures.h"

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
std::atomic<std::uint64_t> g_notOurs{0};

// The instance whose error block this hook follows: one whose login failed with a local text while
// it was logging in. The pointer is only compared with the `this` the game passes to Update; it is
// never dereferenced on its own.
std::atomic<CR15NetGameOpaque*> g_object{nullptr};
// The board version the block reflects; kNothingApplied when the board was busy at the failure, so
// the next Update applies whatever the board holds.
constexpr std::uint64_t kNothingApplied = ~static_cast<std::uint64_t>(0);
std::atomic<std::uint64_t> g_applied{0};
// One writer of the block and of the two copies below at a time (the hooks can run on different
// threads). A hook that finds it taken leaves things as they are; Update looks again next frame.
std::atomic_flag g_writing = ATOMIC_FLAG_INIT;
// The game's own block, saved at the failure, restored when the board is withdrawn.
unsigned char g_saved[layout::kErrorBlockBytes];
// What the block holds as far as this hook knows: the game's own block or the text it wrote. The
// game has other writers of the block (lobby, game-space and lobby-status errors, for example); a
// block that no longer matches this copy was rewritten by one of them and is left alone from then
// on, unless it again holds one of the game's own local login-failure texts (a new failure).
unsigned char g_written[layout::kErrorBlockBytes];
// What g_written holds: the game's own text, a prompt, or a notice that replaced a prompt. A notice
// only ever replaces a prompt; on a screen that shows the game's own text it is not applied.
enum class Kind : std::uint8_t { kGame, kPrompt, kNotice };
Kind g_kind = Kind::kGame;

bool Equal(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Overwrites `n` bytes the compiler may not drop as dead stores (a dump of the process must not
// find a code in a buffer this hook is done with).
void Wipe(void* p, std::size_t n) noexcept {
  volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) v[i] = 0;
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

// The block holds one of the game's local login-failure texts as its only line: what
// SetErrorMessage(char const*) leaves after a new local failure.
bool HoldsLocalFailure(const unsigned char* block) noexcept {
  for (const char* known : game_login_failures::kAll) {
    if (HoldsMessage(block, known)) return true;
  }
  return false;
}

bool SameBlock(const unsigned char* a, const unsigned char* b) noexcept {
  for (std::size_t i = 0; i < layout::kErrorBlockBytes; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

void CopyBlock(unsigned char* to, const unsigned char* from) noexcept {
  for (std::size_t i = 0; i < layout::kErrorBlockBytes; ++i) to[i] = from[i];
}

// Lays `text` ('\n'-separated, at most four lines of at most 63 characters, as the board holds it)
// out the way SetErrorMessage(4 args) does: the byte before the lines is 1, each line is cut at 63
// and NUL-filled to 64.
void LayOut(unsigned char* block, const char* text) noexcept {
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

// Stop following `self` (call with g_writing held). The copy of what was written may hold a code.
void Drop(CR15NetGameOpaque* self) noexcept {
  CR15NetGameOpaque* expected = self;
  if (g_object.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
    Wipe(g_written, sizeof(g_written));
    g_kind = Kind::kGame;
  }
}

// The game's login failed; its message is in the block. Runs on the game's call path.
void HookedSetDelimitedErrorMessage(ErrorThunk::Fn original, CR15NetGameOpaque* self, const char* message) noexcept {
  const std::int32_t state = State(self);  // read before the game's own write (it does not change it)
  original(self, message);
  if (state != layout::kStateLoggingIn || message == nullptr || !IsLocalLoginFailure(message)) {
    g_notLocal.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_writing.test_and_set(std::memory_order_acquire)) {
    g_busy.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  unsigned char* block = Block(self);
  if (!HoldsMessage(block, message)) {
    g_notOurs.fetch_add(1, std::memory_order_relaxed);
    g_writing.clear(std::memory_order_release);
    return;
  }
  CopyBlock(g_saved, block);
  char text[board::kCapacity + 1];
  board::Mode mode = board::Mode::kPrompt;
  std::uint64_t version = 0;
  const board::ReadResult read = board::Read(text, sizeof(text), &mode, &version);
  if (read == board::ReadResult::kCopied && mode == board::Mode::kPrompt) {
    LayOut(g_written, text);
    CopyBlock(block, g_written);
    g_kind = Kind::kPrompt;
    g_applied.store(version, std::memory_order_relaxed);
    g_shown.fetch_add(1, std::memory_order_relaxed);
  } else {
    // Nothing to show yet (or only a notice, or the board was busy): the game's text stays, and the
    // instance is followed so that a prompt published later still reaches this screen.
    CopyBlock(g_written, block);
    g_kind = Kind::kGame;
    if (read == board::ReadResult::kBusy) {
      g_applied.store(kNothingApplied, std::memory_order_relaxed);
      g_busy.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_applied.store(version, std::memory_order_relaxed);
      g_kept.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_object.store(self, std::memory_order_release);
  Wipe(text, sizeof(text));
  g_writing.clear(std::memory_order_release);
}

// Keeps the followed instance's block current. Runs on each CR15NetGame::Update call (up to four per
// game-loop iteration), before the game's own.
void Refresh(CR15NetGameOpaque* self) noexcept {
  const bool leftLoginFailed = State(self) != layout::kStateLoginFailed;
  if (g_writing.test_and_set(std::memory_order_acquire)) return;  // the other writer: next frame
  if (g_object.load(std::memory_order_acquire) != self) {  // dropped or replaced meanwhile
    g_writing.clear(std::memory_order_release);
    return;
  }
  unsigned char* block = Block(self);
  // Nothing to do while the board and the block are as last left (checked on every call: the game
  // can write the block again without the board changing, e.g. a new failure the error hook missed).
  if (!leftLoginFailed && board::Version() == g_applied.load(std::memory_order_relaxed) &&
      SameBlock(block, g_written)) {
    g_writing.clear(std::memory_order_release);
    return;
  }
  bool follow = true;
  if (leftLoginFailed) {
    follow = false;  // the game left its error screen's state
  } else if (!SameBlock(block, g_written)) {
    if (HoldsLocalFailure(block)) {
      // A local login-failure text the error hook did not take up: its writer flag was held, or the
      // game was not logging in at that moment (a second failure while already in "login failed",
      // which the error hook counted as not local). It is the game's own text: start over from it,
      // so it gets the prompt.
      CopyBlock(g_saved, block);
      CopyBlock(g_written, block);
      g_kind = Kind::kGame;
      g_applied.store(kNothingApplied, std::memory_order_relaxed);
    } else {
      g_notOurs.fetch_add(1, std::memory_order_relaxed);  // another writer changed the block
      follow = false;
    }
  }
  if (!follow) {
    Drop(self);
    g_writing.clear(std::memory_order_release);
    return;
  }
  char text[board::kCapacity + 1];
  board::Mode mode = board::Mode::kPrompt;
  std::uint64_t version = 0;
  const board::ReadResult read = board::Read(text, sizeof(text), &mode, &version);
  if (read == board::ReadResult::kCopied && mode == board::Mode::kPrompt) {
    LayOut(g_written, text);
    g_kind = Kind::kPrompt;
  } else if (read == board::ReadResult::kCopied && g_kind != Kind::kGame) {
    LayOut(g_written, text);  // a notice, in place of the prompt this screen shows
    g_kind = Kind::kNotice;
  } else if (read == board::ReadResult::kEmpty && g_kind != Kind::kGame) {
    CopyBlock(g_written, g_saved);  // withdrawn: the game's own message again
    g_kind = Kind::kGame;
  }  // a notice on a screen with the game's own text, or an empty board there: nothing to change
  if (read != board::ReadResult::kBusy) {  // busy: next frame
    if (!SameBlock(block, g_written)) {
      CopyBlock(block, g_written);
      g_refreshed.fetch_add(1, std::memory_order_relaxed);
    }
    g_applied.store(version, std::memory_order_relaxed);
  }
  Wipe(text, sizeof(text));
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
  for (const char* known : game_login_failures::kAll) {
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
      {"login_prompt_block_not_ours", &g_notOurs, sentinel::ReportKind::kFaults},
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

bool InstallIfCounted(bool countersRegistered) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "login_prompt_install",
                        {{"result", "skipped"}, {"why", "counters_refused"}});
    return false;
  }
  return Install();
}

void ArmForTest() noexcept {
  ErrorThunk::Arm(kErrorTextHook);
  UpdateThunk::Arm(kNetGameUpdateHook);
}

bool HoldBlockWriterForTest() noexcept { return !g_writing.test_and_set(std::memory_order_acquire); }
void ReleaseBlockWriterForTest() noexcept { g_writing.clear(std::memory_order_release); }

Counts CurrentCounts() noexcept {
  return {g_shown.load(std::memory_order_relaxed),    g_refreshed.load(std::memory_order_relaxed),
          g_kept.load(std::memory_order_relaxed),     g_notLocal.load(std::memory_order_relaxed),
          g_busy.load(std::memory_order_relaxed),     g_notOurs.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::login_prompt
