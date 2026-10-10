#include "login_prompt_hook.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/auth/prompt_board.h"
#include "quest/login/login_attempt_gate.h"
#include "quest/game_login_failures.h"

namespace nevr_quest::login_prompt {

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace layout = sentinel::pinned::game_layout;
namespace ui = sentinel::pinned::ui_layout;
namespace gate = nevr_quest_login::attempt_gate;
using sentinel::pinned::CR15NetGameOpaque;

sentinel::GotHook g_errorHook;
sentinel::GotHook g_updateHook;
sentinel::GotHook g_enableHook;

std::atomic<std::uint64_t> g_shown{0};
std::atomic<std::uint64_t> g_refreshed{0};
std::atomic<std::uint64_t> g_kept{0};
std::atomic<std::uint64_t> g_notLocal{0};
std::atomic<std::uint64_t> g_busy{0};
std::atomic<std::uint64_t> g_notOurs{0};
std::atomic<std::uint64_t> g_errorPageDropped{0};
std::atomic<std::uint64_t> g_loggingInPageDropped{0};
std::atomic<std::uint64_t> g_pagePassedArmed{0};
std::atomic<std::uint64_t> g_resent{0};
std::atomic<std::uint64_t> g_resendUnavailable{0};

// The error event: CR15NetGame::QuitOnError, resolved by Install() (null until then, and on a host).
std::atomic<QuitFn> g_quit{nullptr};
// A rewrite of the block while the game sits in "login failed" is followed by one error event, so the
// UI status script copies the new text again; pending until it is sent. Events are spaced by at least
// kResendSpacingNs, which is longer than a game-loop iteration (CR15NetGame::Update runs up to four
// times per iteration): at most one event per change, never two in one frame.
constexpr std::int64_t kResendSpacingNs = 50'000'000;
std::atomic<bool> g_resendPending{false};
std::atomic<std::int64_t> g_lastResendNs{0};
std::int64_t SteadyNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
using ClockFn = std::int64_t (*)() noexcept;
std::atomic<ClockFn> g_clock{&SteadyNs};

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
// kAwaitingNotice: the game's own text on a screen where a notice is due as soon as the login may proceed.
enum class Kind : std::uint8_t { kGame, kPrompt, kNotice, kAwaitingNotice };
Kind g_kind = Kind::kGame;

// ---- the prompt latch ---------------------------------------------------------------------------
//
// The latch says: "the error block still holds exactly the text this hook last wrote". While it is armed,
// the page-enable hook keeps the game from replacing the screen that shows the sign-in code with an error
// page or the logging-in page. The game thread owns it (every writer runs in the Update hook or the error
// text hook); the page-enable hook, which may run on a worker thread, reads only g_latchArmed.
//   * armed when this hook writes a prompt or a notice into the block (ArmLatch);
//   * cleared the moment the block holds anything else (RecheckLatch, after every game write this hook
//     sees and on every Update call): the game's own text or another writer's error is genuine;
//   * dead for good once the game reaches "loading global" (RecheckLatch): the login went through, and no
//     later error page is held back.
// It is independent of g_object/g_written, which are dropped when the game leaves "login failed" (the
// page-enable hook fires after the player selects RETRY, when the game is in "logging in").
std::atomic<bool> g_latchArmed{false};
std::atomic<bool> g_latchDead{false};
std::atomic<std::uint64_t> g_latchHash{0};
std::atomic<CR15NetGameOpaque*> g_latchSelf{nullptr};
// A sequence counter around this hook's own writes of the block (odd while one is in progress): the block then
// holds our text, whole or in part, and the page-enable hook must not take a half-written block for a genuine
// error. It also covers a hook that began reading before the write began.
std::atomic<std::uint32_t> g_writeSeq{0};
void BeginOwnWrite() noexcept { g_writeSeq.fetch_add(1, std::memory_order_acq_rel); }
void EndOwnWrite() noexcept { g_writeSeq.fetch_add(1, std::memory_order_acq_rel); }

// FNV-1a over the whole error block; never 0 (0 means "no latch").
std::uint64_t HashBlock(const unsigned char* block) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < layout::kErrorBlockBytes; ++i) {
    h ^= block[i];
    h *= 0x100000001b3ULL;
  }
  return h == 0 ? 1 : h;
}

void ClearLatch() noexcept {
  g_latchArmed.store(false, std::memory_order_release);
  g_latchHash.store(0, std::memory_order_relaxed);
}

// `block` is what this hook just left in `self`'s error block.
void ArmLatch(CR15NetGameOpaque* self, const unsigned char* block) noexcept {
  if (g_latchDead.load(std::memory_order_relaxed)) return;
  g_latchSelf.store(self, std::memory_order_relaxed);
  g_latchHash.store(HashBlock(block), std::memory_order_relaxed);
  g_latchArmed.store(true, std::memory_order_release);
}

const unsigned char* BlockOfConst(const CR15NetGameOpaque* self) noexcept;

// The same hash read from another thread while the game thread may write the block: relaxed atomic loads of
// each byte (no lock, no allocation). A read that overlaps a write is a mismatch at worst, which lets the
// game's call through: the safe side.
std::uint64_t HashLiveBlock(const unsigned char* block) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < layout::kErrorBlockBytes; ++i) {
    h ^= __atomic_load_n(block + i, __ATOMIC_RELAXED);
    h *= 0x100000001b3ULL;
  }
  return h == 0 ? 1 : h;
}

// Whether the error block, read now, still holds what the latch says. The latch flag itself is only
// recomputed on the game thread (Update, or right after the error text hook), so a genuine error written by
// the game's unhooked SetErrorMessage in this very frame would otherwise still look like our text.
bool LatchHoldsLive() noexcept {
  const std::uint32_t before = g_writeSeq.load(std::memory_order_acquire);
  if ((before & 1U) != 0) return true;  // our own write in progress
  const CR15NetGameOpaque* self = g_latchSelf.load(std::memory_order_relaxed);
  const std::uint64_t hash = g_latchHash.load(std::memory_order_relaxed);
  if (self == nullptr || hash == 0) return false;
  const bool same = HashLiveBlock(BlockOfConst(self)) == hash;
  if (same) return true;
  // The byte loads above are relaxed: on arm64 they may not be reordered after the sequence load below.
  std::atomic_thread_fence(std::memory_order_acquire);
  // A mismatch while one of our own writes began or finished in the meantime is our write, not a genuine one.
  return g_writeSeq.load(std::memory_order_acquire) != before;
}
std::int32_t State(const CR15NetGameOpaque* self) noexcept;

void RecheckLatch(CR15NetGameOpaque* self) noexcept {
  if (self == nullptr || self != g_latchSelf.load(std::memory_order_relaxed)) return;
  if (g_latchDead.load(std::memory_order_relaxed)) return;
  if (State(self) >= layout::kStateLoadingGlobal) {
    g_latchDead.store(true, std::memory_order_relaxed);
    ClearLatch();
    return;
  }
  const std::uint64_t hash = g_latchHash.load(std::memory_order_relaxed);
  if (hash == 0 || HashBlock(BlockOfConst(self)) != hash) ClearLatch();
}

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

const unsigned char* BlockOfConst(const CR15NetGameOpaque* self) noexcept {
  return reinterpret_cast<const unsigned char*>(self) + layout::kErrorBlockOffset;
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
    g_resendPending.store(false, std::memory_order_relaxed);
  }
}

// The game's login failed; its message is in the block. Runs on the game's call path.
void HookedSetDelimitedErrorMessage(ErrorThunk::Fn original, CR15NetGameOpaque* self, const char* message) noexcept {
  const std::int32_t state = State(self);  // read before the game's own write (it does not change it)
  original(self, message);
  RecheckLatch(self);  // the game just wrote the block: the latch holds only if it wrote what we left
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
    BeginOwnWrite();
    CopyBlock(block, g_written);
    g_kind = Kind::kPrompt;
    g_applied.store(version, std::memory_order_relaxed);
    g_shown.fetch_add(1, std::memory_order_relaxed);
    ArmLatch(self, block);
    EndOwnWrite();
  } else if (read == board::ReadResult::kCopied && mode == board::Mode::kNotice && gate::IsReady()) {
    // The player signed in while this attempt was in flight, and the attempt failed with the game's local
    // text: the screen says so, because RETRY on it now logs in with the new sign-in. (Only once the login
    // may proceed: a RETRY before that is held back by the page-enable hook and would strand the player.)
    LayOut(g_written, text);
    BeginOwnWrite();
    CopyBlock(block, g_written);
    g_kind = Kind::kNotice;
    g_applied.store(version, std::memory_order_relaxed);
    g_shown.fetch_add(1, std::memory_order_relaxed);
    ArmLatch(self, block);
    EndOwnWrite();
  } else {
    // Nothing to show yet (or the board was busy): the game's text stays, and the
    // instance is followed so that a prompt published later still reaches this screen. A notice that is
    // not due yet (the login may not proceed) is applied by Update once it is.
    CopyBlock(g_written, block);
    g_kind = (read == board::ReadResult::kCopied && mode == board::Mode::kNotice) ? Kind::kAwaitingNotice : Kind::kGame;
    if (read == board::ReadResult::kBusy) {
      g_applied.store(kNothingApplied, std::memory_order_relaxed);
      g_busy.fetch_add(1, std::memory_order_relaxed);
    } else {
      // A notice that is not due yet stays unapplied, so Update looks at it again every frame.
      g_applied.store(g_kind == Kind::kAwaitingNotice ? kNothingApplied : version, std::memory_order_relaxed);
      g_kept.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // The game sends its own error event for this failure right after this call, in this frame: it counts as the
  // last event, so a prompt published within the frame (rewritten by Update) does not send a second.
  g_resendPending.store(false, std::memory_order_relaxed);
  g_lastResendNs.store(g_clock.load(std::memory_order_relaxed)(), std::memory_order_relaxed);
  g_object.store(self, std::memory_order_release);
  Wipe(text, sizeof(text));
  g_writing.clear(std::memory_order_release);
}

// Keeps the followed instance's block current. Runs on each CR15NetGame::Update call (up to four per
// game-loop iteration), before the game's own.
// Returns true when it rewrote the block.
bool Refresh(CR15NetGameOpaque* self) noexcept {
  const bool leftLoginFailed = State(self) != layout::kStateLoginFailed;
  if (g_writing.test_and_set(std::memory_order_acquire)) return false;  // the other writer: next frame
  if (g_object.load(std::memory_order_acquire) != self) {  // dropped or replaced meanwhile
    g_writing.clear(std::memory_order_release);
    return false;
  }
  unsigned char* block = Block(self);
  // Nothing to do while the board and the block are as last left (checked on every call: the game
  // can write the block again without the board changing, e.g. a new failure the error hook missed).
  if (!leftLoginFailed && board::Version() == g_applied.load(std::memory_order_relaxed) &&
      SameBlock(block, g_written)) {
    g_writing.clear(std::memory_order_release);
    return false;
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
    return false;
  }
  char text[board::kCapacity + 1];
  board::Mode mode = board::Mode::kPrompt;
  std::uint64_t version = 0;
  const board::ReadResult read = board::Read(text, sizeof(text), &mode, &version);
  if (read == board::ReadResult::kCopied && mode == board::Mode::kNotice && !gate::IsReady()) {
    // Signed in, but the login may not proceed yet: a "select RETRY" now would send the player to a RETRY the
    // page-enable hook still holds back. The screen stays as it is, and the notice is applied on the frame
    // after the gate turns ready (the board version stays unapplied, so every frame looks again).
    Wipe(text, sizeof(text));
    g_writing.clear(std::memory_order_release);
    return false;
  }
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
  bool rewrote = false;
  if (read != board::ReadResult::kBusy) {  // busy: next frame
    BeginOwnWrite();
    if (!SameBlock(block, g_written)) {
      CopyBlock(block, g_written);
      g_refreshed.fetch_add(1, std::memory_order_relaxed);
      rewrote = true;
    }
    g_applied.store(version, std::memory_order_relaxed);
    // The block holds what was last written: a prompt or a notice keeps the latch, the game's own text
    // (a withdrawn board) does not.
    if (g_kind != Kind::kGame) {
      ArmLatch(self, block);
    } else {
      ClearLatch();
    }
    EndOwnWrite();
  }
  Wipe(text, sizeof(text));
  g_writing.clear(std::memory_order_release);
  return rewrote;
}

// Sends the pending error event for the followed instance when it is still in "login failed" and the last
// one was at least kResendSpacingNs ago. On the game's thread, inside the Update hook.
void ResendErrorEvent(CR15NetGameOpaque* self) noexcept {
  if (!g_resendPending.load(std::memory_order_relaxed)) return;
  if (State(self) != layout::kStateLoginFailed || g_object.load(std::memory_order_acquire) != self) {
    g_resendPending.store(false, std::memory_order_relaxed);  // the screen is gone: nothing to re-read
    return;
  }
  const std::int64_t now = g_clock.load(std::memory_order_relaxed)();
  const std::int64_t last = g_lastResendNs.load(std::memory_order_relaxed);
  if (last != 0 && now - last < kResendSpacingNs) return;  // next frame
  const QuitFn quit = g_quit.load(std::memory_order_acquire);
  g_resendPending.store(false, std::memory_order_relaxed);
  if (quit == nullptr) {
    g_resendUnavailable.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_lastResendNs.store(now, std::memory_order_relaxed);
  g_resent.fetch_add(1, std::memory_order_relaxed);
  quit(self);
}

void HookedNetGameUpdate(UpdateThunk::Fn original, CR15NetGameOpaque* self, std::uint64_t arg) noexcept {
  RecheckLatch(self);
  // A poisoned attempt ends when the game leaves "logging in" (its prerequisites failed): the next one is clean.
  if (self != nullptr && State(self) != layout::kStateLoggingIn) gate::ClearPoison();
  if (self != nullptr && self == g_object.load(std::memory_order_acquire)) {
    if (Refresh(self)) g_resendPending.store(true, std::memory_order_relaxed);
    ResendErrorEvent(self);
  }
  original(self, arg);
}

// CR15UIPage2EnablePageNode::Enter. While the latch is armed (the screen shows our sign-in text) the game
// must not replace it: the enable of the error page or the fatal error page is skipped, and so is the
// enable of the logging-in page while token auth still waits for the player (the screen would lose its
// header and buttons for a login that cannot succeed yet). Skipping is a plain return: the node writes
// nothing to its thread, and its caller ignores the return. Anything else, and every enable while the
// latch is not armed (a genuine error), goes to the game unchanged. Reads atomics only, no logging: it may
// run on a task-scheduler worker thread.
void HookedEnablePageNodeEnter(EnablePageThunk::Fn original, void* node, const void* data) noexcept {
  if (data != nullptr && g_latchArmed.load(std::memory_order_acquire)) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t actor = 0;
    __builtin_memcpy(&actor, bytes + ui::kEnablePageActorIdOffset, sizeof(actor));
    const bool errorPage = actor == ui::kErrorDisplayPage || actor == ui::kFatalErrorDisplayPage;
    // Not ready: the login this RETRY started cannot succeed yet. The same word the login prerequisites read.
    const bool loggingIn = actor == ui::kLoggingInPage && !gate::IsReady();
    if (errorPage || loggingIn) {
      // The record's own shape: the node is the record plus 0x20, and the block still holds our text, read
      // now. Otherwise this is not the record measured, or the game just wrote a genuine error: the game's
      // call goes ahead.
      if (node == const_cast<unsigned char*>(bytes) + ui::kEnablePageNodeOffset && LatchHoldsLive()) {
        // The attempt whose logging-in page is skipped must fail, even if the gate turns ready before it is
        // checked: it would otherwise log in on a screen that has no way to show it. Skipping and poisoning
        // are one compare-and-swap on the gate word: if the gate turned ready since it was read, the login
        // may proceed and needs the page, so the page goes through.
        if (errorPage || gate::PoisonIfNotReady()) {
          (errorPage ? g_errorPageDropped : g_loggingInPageDropped).fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
      g_pagePassedArmed.fetch_add(1, std::memory_order_relaxed);
    } else if (actor == ui::kLoggingInPage) {
      g_pagePassedArmed.fetch_add(1, std::memory_order_relaxed);  // armed, but token auth is not waiting
    }
  }
  original(node, data);
}

NEVR_HOOK_RECORD(kErrorTextHook, ErrorThunk, &HookedSetDelimitedErrorMessage);
NEVR_HOOK_RECORD(kNetGameUpdateHook, UpdateThunk, &HookedNetGameUpdate);
NEVR_HOOK_RECORD(kEnablePageHook, EnablePageThunk, &HookedEnablePageNodeEnter);

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
      {"login_prompt_enable_thunk_faults", &EnablePageThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"login_prompt_error_page_dropped", &g_errorPageDropped, sentinel::ReportKind::kCalls},
      {"login_prompt_logging_in_page_dropped", &g_loggingInPageDropped, sentinel::ReportKind::kCalls},
      {"login_prompt_page_passed_armed", &g_pagePassedArmed, sentinel::ReportKind::kCalls},
      {"login_prompt_error_resent", &g_resent, sentinel::ReportKind::kCalls},
      {"login_prompt_error_resend_unavailable", &g_resendUnavailable, sentinel::ReportKind::kFaults},
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

// Proves libr15's QuitOnError is the pinned build's (build ID, then its first four instructions) and
// publishes it. The error event is optional: without it the prompt still reaches the screen at the next
// failure, so a refusal is counted at the first rewrite and logged here once.
const char* ResolveQuitOnError() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return "module_not_loaded";
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return "no_build_id";
  if (!Equal(id, sentinel::pinned::kLibR15BuildId)) return "build_id_mismatch";
  const unsigned char* fn = reinterpret_cast<const unsigned char*>(image.base) + sentinel::pinned::kQuitOnErrorVaddr;
  if (std::memcmp(fn, sentinel::pinned::kQuitOnErrorCode, sizeof(sentinel::pinned::kQuitOnErrorCode)) != 0) {
    return "prologue_mismatch";
  }
  QuitFn quit = nullptr;
  static_assert(sizeof(quit) == sizeof(fn), "function pointer size");
  std::memcpy(&quit, &fn, sizeof(quit));
  g_quit.store(quit, std::memory_order_release);
  return "resolved";
}

bool Install() noexcept {
  ErrorThunk::Arm(kErrorTextHook);
  UpdateThunk::Arm(kNetGameUpdateHook);
  EnablePageThunk::Arm(kEnablePageHook);
  const sentinel::GotStatus error =
      sentinel::InstallThunk<ErrorThunk>(g_errorHook, sentinel::pinned::LibR15SetDelimitedErrorMessage());
  const sentinel::GotStatus update =
      sentinel::InstallThunk<UpdateThunk>(g_updateHook, sentinel::pinned::LibR15NetGameUpdate());
  const sentinel::GotStatus enable =
      sentinel::InstallThunk<EnablePageThunk>(g_enableHook, sentinel::pinned::LibR15EnablePageNodeEnter());
  const bool ok = error == sentinel::GotStatus::kOk && update == sentinel::GotStatus::kOk &&
                  enable == sentinel::GotStatus::kOk;
  const bool any = error == sentinel::GotStatus::kOk || update == sentinel::GotStatus::kOk ||
                   enable == sentinel::GotStatus::kOk;
  const char* const quit = ResolveQuitOnError();
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "login_prompt_install",
                      {{"result", ok ? "installed" : (any ? "partial" : "failed")},
                       {"error_text", sentinel::GotStatusName(error)},
                       {"update", sentinel::GotStatusName(update)},
                       {"enable_page", sentinel::GotStatusName(enable)},
                       {"quit_on_error", quit}});
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
  EnablePageThunk::Arm(kEnablePageHook);
}

bool LatchArmedForTest() noexcept { return g_latchArmed.load(std::memory_order_acquire); }

void ResetLatchForTest() noexcept {
  g_latchDead.store(false, std::memory_order_relaxed);
  g_latchSelf.store(nullptr, std::memory_order_relaxed);
  g_writeSeq.store(0, std::memory_order_relaxed);
  gate::ClearPoison();  // the gate's readiness is the caller's to set
  ClearLatch();
}

void SetQuitOnErrorForTest(QuitFn quit) noexcept {
  g_quit.store(quit, std::memory_order_release);
  g_resendPending.store(false, std::memory_order_relaxed);
  g_lastResendNs.store(0, std::memory_order_relaxed);
}
void SetClockForTest(std::int64_t (*clock)() noexcept) noexcept {
  g_clock.store(clock != nullptr ? clock : &SteadyNs, std::memory_order_relaxed);
}

bool HoldBlockWriterForTest() noexcept { return !g_writing.test_and_set(std::memory_order_acquire); }
void ReleaseBlockWriterForTest() noexcept { g_writing.clear(std::memory_order_release); }

Counts CurrentCounts() noexcept {
  return {g_shown.load(std::memory_order_relaxed),    g_refreshed.load(std::memory_order_relaxed),
          g_kept.load(std::memory_order_relaxed),     g_notLocal.load(std::memory_order_relaxed),
          g_busy.load(std::memory_order_relaxed),     g_notOurs.load(std::memory_order_relaxed),
          g_resent.load(std::memory_order_relaxed),   g_resendUnavailable.load(std::memory_order_relaxed),
          g_errorPageDropped.load(std::memory_order_relaxed), g_loggingInPageDropped.load(std::memory_order_relaxed),
          g_pagePassedArmed.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::login_prompt
