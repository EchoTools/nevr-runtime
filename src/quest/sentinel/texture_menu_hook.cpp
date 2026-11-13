#include "texture_menu_hook.h"

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <string_view>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "core/nevr_config.h"
#include "hand_flip.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "outside_game_call.h"
#include "quest/combat/pinned_words.h"
#include "quest/headset_profile.h"
#include "texture_menu.h"
#include "texture_quality_input.h"
#include "texture_quality_rules.h"
#include "texture_quality_targets.h"

namespace nevr_quest::texture_menu {

namespace {

namespace tq = nevr_quest::texture_quality;
namespace pin = sentinel::pinned;
namespace rules = nevr_quest::texture_quality::rules;
namespace input = nevr_quest::texture_quality::input;
using tq::Level;

constexpr int kUnset = tq::kLevelUnset;

// ---- state --------------------------------------------------------------------------------------------------------------

sentinel::GotHook g_fileHook, g_textHook, g_validateHook, g_applyHook, g_checkHook;

std::atomic<pin::RefreshRTSizeFn> g_refreshRtSize{nullptr};
std::atomic<pin::VrFlagFn> g_vrInitialized{nullptr};
std::atomic<pin::VrFlagFn> g_vrGraphicsInitialized{nullptr};
std::atomic<pin::VrFlagFn> g_vrDisplayLost{nullptr};
std::atomic<pin::LocalAppDataDirectoryFn> g_localAppData{nullptr};
std::atomic<pin::EngineResizeFn> g_engineResize{nullptr};
std::atomic<PropertyReader> g_property{nullptr};

// The index the control starts on when the player has not chosen: the configuration's level (`stock` shows as Low).
std::atomic<int> g_defaultIndex{tq::kDefaultMenuIndex};
// What the start resolver found: 0 not asked, 2 the configuration (the file's graphics.texture_quality, which is also where a
// choice made in the menu is kept, or the default when the file names none), 3 the reference input's rule (its settings file named
// a level), 4 the platform default (the reference input's settings file named none). 1 is not used: the player's choice is the
// configuration's key.
enum StartSource : int { kStartNotAsked = 0, kStartConfig = 2, kStartReferenceRule = 3, kStartPlatformDefault = 4 };
std::atomic<int> g_startSource{kStartNotAsked};
// Whether the file gave the configuration's level (else it is the default).
std::atomic<bool> g_fromConfig{false};
std::atomic<int> g_startLevel{kUnset};
std::atomic<bool> g_playerSaved{false};
std::atomic<bool> g_firstDelivery{false};
// The index of the last delivery, and the index the player changed the control to that the poller has not yet written to
// config.yaml (kUnset: none). A delivery whose index differs from the one before it is a change in the menu: the first delivery
// is the load, and the game redelivers an unchanged setting when another setting changes.
std::atomic<int> g_lastIndex{kUnset};
std::atomic<int> g_pendingSave{kUnset};
// KNOWN-WRONG #2043 The Controller Hand Flip row's bit (hand_flip.h) of the last delivery (kUnset: no delivery yet).
std::atomic<int> g_lastHandFlip{kUnset};
// config.yaml's path, set before the hooks are armed (SetConfigFile) and read by the poller after g_configFileReady.
char g_configFile[704];
std::atomic<bool> g_configFileReady{false};
std::atomic<std::uint64_t> g_saveWrites{0};
std::atomic<std::uint64_t> g_saveFailures{0};

std::atomic<std::uint64_t> g_deliveries{0};
std::atomic<std::uint64_t> g_menuLevel{0};      // the Level of the last delivery
std::atomic<std::uint64_t> g_appliedLevel{0};   // the applied Level, mirrored here for the reporter
std::atomic<std::uint64_t> g_applies{0};
std::atomic<std::uint64_t> g_equalCommits{0};
std::atomic<std::uint64_t> g_refused{0};
std::atomic<std::uint64_t> g_failed{0};
std::atomic<std::uint64_t> g_jsonRewrites{0};
std::atomic<std::uint64_t> g_configOverrides{0};
std::atomic<bool> g_disabled{false};
std::atomic<const char*> g_disabledWhy{"none"};
std::atomic<int> g_lastRc{0};
std::atomic<int> g_lastFrom{kUnset};
std::atomic<int> g_lastTo{kUnset};

// ---- the Quest 2 step-down (#1737, texture_quality_rules.h) -------------------------------------------------------------------

// The game's upload count and lock (texture_quality_targets.h), published by Install after their pinned words hold; null: not pinned.
std::atomic<const std::uint64_t*> g_uploadCount{nullptr};
std::atomic<const std::uint32_t*> g_uploadLock{nullptr};
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};
// The look's gate, as the poller last stepped it: read by the render thread.
std::atomic<bool> g_lookOn{false};
std::atomic<int> g_gateReason{static_cast<int>(rules::RuleReason::kNotInCombat)};
std::atomic<std::int64_t> g_quietSinceMs{0};
// The step-down is off for the run, and why (a word; the poller logs it once).
std::atomic<bool> g_lookOff{false};
std::atomic<const char*> g_lookOffWhy{"none"};
// The applied level is the one the look asked for (a later change back is the look's too).
std::atomic<bool> g_lookApplied{false};
std::atomic<std::uint64_t> g_lookApplies{0};
// The hold in progress: whether one is, how many began, and what the last one waited on (the poller writes one line per hold).
std::atomic<bool> g_holding{false};
std::atomic<std::uint64_t> g_lookHolds{0};
std::atomic<int> g_holdReason{0};
std::atomic<int> g_holdFrom{kUnset};
std::atomic<int> g_holdTo{kUnset};
std::atomic<std::uint64_t> g_holdUploads{0};
std::atomic<std::uint32_t> g_holdLock{0};
std::atomic<std::int64_t> g_holdQuietMs{0};

std::int64_t NowMs() noexcept {
  if (const auto clock = g_clock.load(std::memory_order_acquire)) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The level the apply works towards: the player's (`requested`), or Very Low on a Quest 2 in a settled combat level (texture_quality_rules.h).
// With no look registered it is `requested` and nothing else is read.
rules::Effective Decide(Level requested) noexcept {
  rules::StepDownInputs in;
  in.requested = requested;
  in.registered = input::Registered() != nullptr;
  if (!in.registered) return {requested, rules::RuleReason::kOff};
  const nevr_quest::headset_profile::Profile* const profile = tq::ActiveHeadsetProfile();
  in.quest2 = profile != nullptr && profile->id == nevr_quest::headset_profile::Id::kQuest2;
  in.playerChose = tq::PlayerChose();
  in.lookOffForRun = g_lookOff.load(std::memory_order_acquire);
  in.gate.on = g_lookOn.load(std::memory_order_acquire);
  in.gate.reason = static_cast<rules::RuleReason>(g_gateReason.load(std::memory_order_acquire));
  return rules::EffectiveLevel(in);
}

void LookOff(const char* why) noexcept {
  if (g_lookOff.exchange(true, std::memory_order_acq_rel)) return;
  g_lookOffWhy.store(why, std::memory_order_release);
}

// Why a look-driven Resize may not be made now (texture_quality_rules.h ResizeHold): the look made now, the gate's quiet stretch, the upload words.
// Unpinned words read as an upload in flight (the look is off for the run then, so this is a second line of defence).
rules::Hold LookHold(std::uint64_t* uploads, std::uint32_t* lock) noexcept {
  const input::LookFn look = input::Registered();
  rules::ResizeInputs in;
  in.look = look != nullptr ? look() : rules::Look{};
  in.nowMs = NowMs();
  in.quietSinceMs = g_quietSinceMs.load(std::memory_order_acquire);
  const std::uint64_t* const count = g_uploadCount.load(std::memory_order_acquire);
  const std::uint32_t* const word = g_uploadLock.load(std::memory_order_acquire);
  in.uploads = count != nullptr ? __atomic_load_n(count, __ATOMIC_ACQUIRE) : 1U;
  in.lock = word != nullptr ? __atomic_load_n(word, __ATOMIC_ACQUIRE) : 1U;
  *uploads = in.uploads;
  *lock = in.lock;
  const rules::Hold hold = rules::ResizeHold(in);
  if (hold != rules::Hold::kNone) {
    // The quiet time the hold read: the poller's stretch, or the time since the input's own last load when that is shorter.
    std::int64_t quiet = in.quietSinceMs == 0 ? 0 : in.nowMs - in.quietSinceMs;
    if (in.look.lastLoadMs != 0 && in.nowMs - in.look.lastLoadMs < quiet) quiet = in.nowMs - in.look.lastLoadMs;
    g_holdQuietMs.store(quiet, std::memory_order_relaxed);
  }
  return hold;
}

void NoteHold(rules::Hold hold, Level from, Level to, std::uint64_t uploads, std::uint32_t lock) noexcept {
  g_holdReason.store(static_cast<int>(hold), std::memory_order_relaxed);
  g_holdFrom.store(static_cast<int>(from), std::memory_order_relaxed);
  g_holdTo.store(static_cast<int>(to), std::memory_order_relaxed);
  g_holdUploads.store(uploads, std::memory_order_relaxed);
  g_holdLock.store(lock, std::memory_order_relaxed);
  if (!g_holding.exchange(true, std::memory_order_acq_rel)) g_lookHolds.fetch_add(1, std::memory_order_release);
}

// Per document: what the last rewrite did (0 none, else texture_menu::Status + 1) and its counts.
enum Document : int { kNone = 0, kMenuSettings = 1, kGameSettings = 2 };
constexpr int kDocuments = 3;
std::atomic<int> g_jsonStatus[kDocuments];
std::atomic<int> g_jsonRows[kDocuments];
std::atomic<int> g_jsonPages[kDocuments];
// KNOWN-WRONG #2043 Controller Hand Flip rows each document's rewrite added or removed (hand_flip.h).
std::atomic<int> g_jsonHandFlip[kDocuments];
// #1823: the walk that found no row, and the top-level keys, of each document's last rewrite of another shape (texture_menu.h). The
// file-read path writes them and the poller copies them into the log line, under g_shapeMutex.
pthread_mutex_t g_shapeMutex = PTHREAD_MUTEX_INITIALIZER;
std::string g_shapeSite[kDocuments];
std::string g_topKeys[kDocuments];

// The document each thread is inside the file read of (kNone: none). Not a thread_local: the .so would get emulated TLS on the hook
// path (TestNoEmulatedTLSInHookPath). The slots are keyed by pthread_t under a pthread mutex, the sentinel's own lock convention.
struct ArmedSlot {
  pthread_t thread{};
  int document = kNone;
  bool used = false;
};
constexpr int kArmedSlots = 16;
pthread_mutex_t g_armedMutex = PTHREAD_MUTEX_INITIALIZER;
ArmedSlot g_armed[kArmedSlots];

// Under g_armedMutex.
ArmedSlot* FindArmed(pthread_t self) noexcept {
  for (ArmedSlot& slot : g_armed) {
    if (slot.used && pthread_equal(slot.thread, self)) return &slot;
  }
  return nullptr;
}

// Under g_armedMutex. A full table leaves the thread unarmed: the game parses what it read, unrewritten.
ArmedSlot* FirstFreeArmed() noexcept {
  for (ArmedSlot& slot : g_armed) {
    if (!slot.used) return &slot;
  }
  return nullptr;
}

// Arms the calling thread for `document`; kNone disarms it.
void SetArmed(int document) noexcept {
  const pthread_t self = pthread_self();
  pthread_mutex_lock(&g_armedMutex);
  ArmedSlot* slot = FindArmed(self);
  if (document == kNone) {
    if (slot != nullptr) slot->used = false;
  } else {
    if (slot == nullptr) slot = FirstFreeArmed();
    if (slot != nullptr) {
      slot->thread = self;
      slot->document = document;
      slot->used = true;
    }
  }
  pthread_mutex_unlock(&g_armedMutex);
}

// The document the calling thread is armed for, and disarms it (kNone: not armed).
int TakeArmed() noexcept {
  const pthread_t self = pthread_self();
  int document = kNone;
  pthread_mutex_lock(&g_armedMutex);
  if (ArmedSlot* slot = FindArmed(self)) {
    document = slot->document;
    slot->used = false;
  }
  pthread_mutex_unlock(&g_armedMutex);
  return document;
}

// ---- the two documents --------------------------------------------------------------------------------------------------

constexpr unsigned long long kMaxDocumentBytes = 1U << 20;

bool EndsWithPath(const char* path, const char* suffix) noexcept {
  const std::size_t n = std::strlen(path);
  const std::size_t m = std::strlen(suffix);
  if (n < m) return false;
  for (std::size_t i = 0; i < m; ++i) {
    char c = path[n - m + i];
    if (c == '|' || c == '\\') c = '/';  // the game's own separators, before and after it resolves them
    if (c != suffix[i]) return false;
  }
  return true;
}

int DocumentForPath(const char* path) noexcept {
  if (path == nullptr) return kNone;
  if (EndsWithPath(path, "/json/r14/config/uisettings/menu_settings.json")) return kMenuSettings;
  if (EndsWithPath(path, "/json/r14/config/gamesettings_config.json")) return kGameSettings;
  return kNone;
}

std::uint64_t Fnv1a(std::string_view text) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : text) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

// The rewritten text of each document, kept for the life of the process: the parser may keep pointers into what it was given, and
// a document is read once. A second read of the same text (same length and hash) is served from here, while the Controller Hand Flip
// row's index it was written with is still the current one (KNOWN-WRONG #2043: a re-read of the menu in one run then shows the
// current choice, not the launch-time one). Heap objects that are never freed, so no static destructor runs under a game thread.
struct Kept {
  std::uint64_t hash = 0;
  std::size_t inputSize = 0;
  int handFlipIndex = 0;
  const std::string* text = nullptr;
};
// std::mutex is out: its unlock carries a personality on the hook path. A pthread mutex with a constant initializer has no
// destructor, so it adds no _GLOBAL__sub_I initializer (tools/check_quest_static_init.sh).
pthread_mutex_t g_keptMutex = PTHREAD_MUTEX_INITIALIZER;
Kept g_kept[kDocuments];

struct Served {
  const char* data = nullptr;
  std::size_t size = 0;
};

// Rewrites the document the game has just read, or says nothing (data == nullptr) and the game parses what it read. Runs before
// any game code (the caller calls the original after it returns), so it may allocate and carry a personality
// (outside_game_call.h): it calls no game code, and nothing leaves it (it is noexcept; its work is texture_menu.cpp, which parses
// with no throwing call).
NEVR_OUTSIDE_GAME_CALL Served RewriteDocument(int document, const char* text, unsigned long long length) noexcept {
  if (document <= kNone || document >= kDocuments || text == nullptr || length == 0 || length > kMaxDocumentBytes) return {};
  const std::string_view input(text, static_cast<std::size_t>(length));
  const std::uint64_t hash = Fnv1a(input);
  pthread_mutex_lock(&g_keptMutex);
  const Kept kept = g_kept[document];
  pthread_mutex_unlock(&g_keptMutex);
  // The Controller Hand Flip row starts on the configured choice when the start was resolved before the menu was read (it is resolved
  // on the first eye-size query, earlier than the menu document), else on Off. Its index 0 is "On" (hand_flip::MenuIndexFor).
  // KNOWN-WRONG #2043 the toggle is the interim.
  // With the hand pose hooks not installed there is no row (kNoHandFlipRow), and the settings document keeps its stock entry.
  const bool offered = hand_flip::Offered();
  const int handFlipIndex = !offered ? texture_menu::kNoHandFlipRow
                            : document == kMenuSettings ? hand_flip::MenuIndexFor(hand_flip::Started() ? hand_flip::Current() : 0)
                                                        : 0;
  if (kept.text != nullptr && kept.hash == hash && kept.inputSize == input.size() && kept.handFlipIndex == handFlipIndex) {
    return {kept.text->data(), kept.text->size()};
  }
  const texture_menu::Result r = document == kMenuSettings
                                     ? texture_menu::RewriteMenuSettings(input, g_defaultIndex.load(std::memory_order_acquire), handFlipIndex)
                                     : texture_menu::RewriteGameSettings(input, offered);
  g_jsonRows[document].store(r.rows, std::memory_order_relaxed);
  g_jsonPages[document].store(r.pages, std::memory_order_relaxed);
  g_jsonHandFlip[document].store(r.handFlipRows, std::memory_order_relaxed);  // KNOWN-WRONG #2043 the interim toggle's rows
  g_jsonStatus[document].store(static_cast<int>(r.status) + 1, std::memory_order_release);
  pthread_mutex_lock(&g_shapeMutex);
  g_shapeSite[document] = r.shapeSite;
  g_topKeys[document] = r.topKeys;
  pthread_mutex_unlock(&g_shapeMutex);
  if (r.status != texture_menu::Status::kRewritten) return {};
  const std::string* const out = new std::string(r.text);
  pthread_mutex_lock(&g_keptMutex);
  g_kept[document] = Kept{hash, input.size(), handFlipIndex, out};
  pthread_mutex_unlock(&g_keptMutex);
  g_jsonRewrites.fetch_add(1, std::memory_order_relaxed);
  return {out->data(), out->size()};
}

// ---- the start level: what the player chose -----------------------------------------------------------------------------

// The start level is the configuration's: graphics.texture_quality in config.yaml, which is also where the poller writes a choice made
// in the menu (SaveChoice, through nevr_config), so the player's last choice is the key at the next launch and nothing else holds
// it. The game's own saved slot (`msaa` in settings_mp_v2_quest.json) is not read: a settings file that never held the setting
// reads 0, which is Very Low, and cannot tell "chose Very Low" from "never chose".
// The reference input's start rule (#1737 C4). The reference libr15 starts at the level its own settings file names: the `msaa`
// index V there (his cave's rule, read by tq::ReferenceIndexFromSettings), at level kLow + V. It applies only on the reference input,
// and only when the configuration is `stock` (the key absent or `stock`). A level this rule derives is not the player's choice, so it
// is never written to config.yaml: only a change in the menu is (#1988). A settings file that
// names no level (missing, unreadable, or no `msaa`) starts the platform default, the configuration's level (`stock`, shown as Low).
// `directory` is the settings directory (nullptr: none, which reads as a missing file). It runs inside the game's call: no logging.
char g_referenceText[tq::kReferenceReadCap + 1];
std::atomic<int> g_referenceIndex{kUnset};
std::atomic<const char*> g_referenceFile{"none"};

int ResolveReference(const char* directory) noexcept {
  if (tq::InputKind() != tq::EyeInput::kReference || tq::LatchedLevel() != Level::kStock) return kUnset;
  int index = tq::kReferenceDefaultIndex;
  const char* state = "missing";
  bool sawLevel = false;
  if (directory != nullptr) {
    char path[600];  // %.512s plus the suffix: the directory is at most 512 characters
    std::snprintf(path, sizeof(path), "%.512s/RAD/LoneEcho/settings_mp_v2_quest.json", directory);
    std::FILE* const file = std::fopen(path, "r");
    if (file != nullptr) {
      const std::size_t size = std::fread(g_referenceText, 1, tq::kReferenceReadCap, file);
      std::fclose(file);
      bool sawKey = false;
      index = tq::ReferenceIndexFromSettings(g_referenceText, size, &sawKey);
      state = sawKey ? "read" : "no_key";
      sawLevel = sawKey;
    }
  }
  g_referenceIndex.store(index, std::memory_order_release);
  g_referenceFile.store(state, std::memory_order_release);
  if (!sawLevel) {
    g_startSource.store(kStartPlatformDefault, std::memory_order_release);
    return kUnset;
  }
  const Level level = tq::LevelForReferenceIndex(index);
  g_playerSaved.store(true, std::memory_order_release);
  tq::NotePlayerChose();  // the reference rule named a level: the player's (#1737 step-down)
  g_startSource.store(kStartReferenceRule, std::memory_order_release);
  g_startLevel.store(static_cast<int>(level), std::memory_order_release);
  g_appliedLevel.store(static_cast<std::uint64_t>(level), std::memory_order_relaxed);
  return static_cast<int>(level);
}

// The game's local app data directory as a C string in `fixedString` (a CFixedStringT<512>: its characters come first), or nullptr
// when the lookup is not there or the directory is empty. Runs inside the game's call.
const char* LocalDirectory(char* fixedString, std::size_t size) noexcept {
  const pin::LocalAppDataDirectoryFn directory = g_localAppData.load(std::memory_order_acquire);
  if (directory == nullptr) return nullptr;
  std::memset(fixedString, 0, size);
  directory(fixedString);
  fixedString[512] = '\0';
  return fixedString[0] == '\0' ? nullptr : fixedString;
}

// The start: the texture level comes from config.yaml (the latched configuration) or the reference rule. The Controller Hand Flip
// start (KNOWN-WRONG #2043, hand_flip.h) is the configuration's graphics.hand_flip, handed to hand_flip.h before any hook is
// installed; it is resolved here too, on the first eye-size query, so it is in place before the menu document is read (the row
// starts on it), and the tracking handler resolves it when it needs it first. It does no file input or output.
int ResolveStart() noexcept {
  g_startSource.store(kStartConfig, std::memory_order_release);
  hand_flip::EnsureStarted();
  alignas(8) char fixedString[1024];
  return ResolveReference(LocalDirectory(fixedString, sizeof(fixedString)));
}

// ---- the five handlers --------------------------------------------------------------------------------------------------

using FileThunk = pin::LibR15JsonDecodeFromFileThunk;
using TextThunk = pin::LibR15JsonDecodeFromTextThunk;
using ValidateThunk = pin::LibR15ValidateGfxSettingsThunk;
using ApplyThunk = pin::LibR15ApplyRuntimeGfxSettingsThunk;
using CheckThunk = pin::LibR15CheckForResizeThunk;

template <typename T>
T LoadAt(const void* base, std::size_t offset) noexcept {
  T value;
  std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
  return value;
}
template <typename T>
void StoreAt(void* base, std::size_t offset, T value) noexcept {
  std::memcpy(static_cast<unsigned char*>(base) + offset, &value, sizeof(value));
}

// CJson::DecodeFrom(CFileSp const&, uint): arms the thread for the two documents for the length of the call.
int HandleDecodeFile(FileThunk::Fn original, pin::CJsonOpaque* self, const char* path, std::uint32_t strip) noexcept {
  SetArmed(DocumentForPath(path));
  const int result = original(self, path, strip);
  SetArmed(kNone);
  return result;
}

// CJson::DecodeFrom(char const*, unsigned long long): the parse. On a thread armed by the handler above, the first text it parses
// is the document, and the game parses the rewrite of it. The thread is disarmed at once: a second parse in the same file read
// (there is none) would be left alone.
int HandleDecodeText(TextThunk::Fn original, pin::CJsonOpaque* self, const char* text, unsigned long long length) noexcept {
  const int document = TakeArmed();
  if (document == kNone) return original(self, text, length);
  const Served served = RewriteDocument(document, text, length);
  if (served.data == nullptr) return original(self, text, length);
  return original(self, served.data, served.size);
}

// ValidateGfxSettings: the game clamps `graphics|msaa` to 0..2. The value the settings held, clamped to the menu's 0..5, goes back.
void HandleValidate(ValidateThunk::Fn original, unsigned char* settings) noexcept {
  if (settings == nullptr) {
    original(settings);
    return;
  }
  const std::int32_t held = LoadAt<std::int32_t>(settings, pin::gfx_layout::kMsaa);
  original(settings);
  const std::int32_t last = tq::kMenuLevelCount - 1;
  StoreAt<std::int32_t>(settings, pin::gfx_layout::kMsaa, held < 0 ? 0 : (held > last ? last : held));
}

// KNOWN-WRONG #2043 The Controller Hand Flip row rides on `graphics|fullscreen`, SR15GfxSettings flags bit 0 (hand_flip.h), whose
// value in the game is 1: Off is the bit set (the game's own value), On is the bit cleared (hand_flip::BitFor). The first delivery
// is the load: the bit is set to the stored choice's bit (with the default, Off, it is already 1 and nothing is written), so the
// menu shows the choice and the next save keeps it; the write, if any, is recorded for the poller to log once (hand_flip_carrier).
// A later delivery with another bit is the player's choice in the menu: the flag the tracking handler reads on its next call
// takes it now (no restart), and the choice is queued to be written. The game redelivers an unchanged bit when another setting
// changes; that is not a change. The game can also put the stock settings back without the menu (UpdateSettings re-reads the
// settings JSON, InitCI copies the UI rows back): the stock bit is 1, which is Off, so a restore after On reads as the player
// turning it Off, is saved once (the next delivery carries the same bit, which is no change), and never turns the roll on.
void HandleHandFlipDelivery(unsigned char* settings) noexcept {
  // No row, no write to the game's bit: the hooks that roll, save and log it are not installed.
  if (!hand_flip::Offered()) return;
  // The start is resolved to completion before its flag is trusted: another thread may be inside it (the tracking handler's
  // first call), and reading the flag now would write the bit from a value the start has not stored yet. A wait that times out
  // leaves the load to the next delivery: the bit is untouched and the last-delivery word stays unset.
  if (!hand_flip::AwaitStart()) return;
  std::uint32_t flags = LoadAt<std::uint32_t>(settings, hand_flip::kCarrierOffset);
  int carried = (flags & hand_flip::kCarrierBit) != 0 ? 1 : 0;
  if (g_lastHandFlip.load(std::memory_order_acquire) == kUnset) {
    const int before = carried;
    const int wanted = hand_flip::BitFor(hand_flip::Current());
    if (carried != wanted) {
      flags = wanted != 0 ? (flags | hand_flip::kCarrierBit) : (flags & ~hand_flip::kCarrierBit);
      StoreAt<std::uint32_t>(settings, hand_flip::kCarrierOffset, flags);
      carried = wanted;
    }
    hand_flip::RecordCarrier(before, carried);
    g_lastHandFlip.store(carried, std::memory_order_release);
    return;
  }
  const int previous = g_lastHandFlip.exchange(carried, std::memory_order_acq_rel);
  if (previous != carried) hand_flip::SetFromMenu(hand_flip::FlipFor(carried));
}

// ApplyRuntimeGfxSettings: the game's settings, delivered at boot and on every change. `graphics|msaa` is the control.
void HandleApply(ApplyThunk::Fn original, const unsigned char* settings, void* game, std::uint32_t refreshUi) noexcept {
  if (settings != nullptr) {
    g_deliveries.fetch_add(1, std::memory_order_relaxed);
    unsigned char* const field = const_cast<unsigned char*>(settings) + pin::gfx_layout::kMsaa;
    std::int32_t index = LoadAt<std::int32_t>(field, 0);
    // The first delivery is the load. What the game holds in the slot is not the player's choice (a settings file that never held
    // the setting reads 0, Very Low): the configuration's level is (the file's key, else the default, Low for `stock`), or the
    // reference input's rule's. The settings are changed to it, so the control shows it and the next save keeps it.
    if (!g_firstDelivery.exchange(true, std::memory_order_acq_rel)) {
      const bool marked = g_playerSaved.load(std::memory_order_acquire);  // the reference rule named a level
      const int start = marked ? tq::MenuIndexForLevel(static_cast<Level>(g_startLevel.load(std::memory_order_acquire)))
                               : g_defaultIndex.load(std::memory_order_acquire);
      if (index != start) {
        StoreAt<std::int32_t>(field, 0, start);
        index = start;
        if (!marked) g_configOverrides.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // Every later delivery with another index is the player's choice in the menu: it is written to config.yaml for the next launch.
    const int previous = g_lastIndex.exchange(index, std::memory_order_acq_rel);
    if (previous != kUnset && previous != index) {
      g_pendingSave.store(index, std::memory_order_release);
      tq::NotePlayerChose();  // a change in the menu: the player's level stands in combat too (#1737 step-down)
    }
    const Level level = tq::LevelForMenuIndex(index);
    g_menuLevel.store(static_cast<std::uint64_t>(level), std::memory_order_relaxed);
    tq::RequestLevel(level, tq::RequestSource::kMenu);
    HandleHandFlipDelivery(const_cast<unsigned char*>(settings));  // KNOWN-WRONG #2043 the interim toggle rides on this delivery
  }
  original(settings, game, refreshUi);
}

void RefuseForGood(const char* why) noexcept {
  g_disabledWhy.store(why, std::memory_order_release);
  g_disabled.store(true, std::memory_order_release);
  g_refused.fetch_add(1, std::memory_order_relaxed);
}

// The apply, on the render thread, after the engine's own per-frame check. Everything it calls is a function the engine calls
// itself at this point of the frame.
void ApplyPending(void* engine) noexcept {
  const int wanted = tq::RequestedLevelValue();
  if (wanted == kUnset || !tq::IsLatched() || g_disabled.load(std::memory_order_acquire)) return;
  const Level have = tq::LatchedLevel();
  // The player's level, or the Quest 2 step-down's (Decide); with no look registered, the player's.
  const rules::Effective effective = Decide(static_cast<Level>(wanted));
  const Level want = effective.level;
  if (want == have) {
    if (effective.reason == rules::RuleReason::kPlayerChoice) g_lookApplied.store(false, std::memory_order_release);
    g_holding.store(false, std::memory_order_release);
    return;
  }
  // A change the look causes: stepping down, or back from its level while the player has not chosen. It waits for a quiet window.
  const bool lookDriven = effective.reason == rules::RuleReason::kQuest2Combat ||
                          (g_lookApplied.load(std::memory_order_acquire) && effective.reason != rules::RuleReason::kPlayerChoice);

  // Two levels that give the same eye size need no rebuild (`stock` and Low on one headset).
  const tq::Counts counts = tq::CurrentCounts();
  if (counts.nativeWidth != 0 && counts.nativeHeight != 0) {
    const tq::EyeSize native{static_cast<int>(counts.nativeWidth), static_cast<int>(counts.nativeHeight)};
    const tq::EyeSize from = tq::EffectiveSize(have, native, tq::ActiveHeadsetProfile());
    const tq::EyeSize to = tq::EffectiveSize(want, native, tq::ActiveHeadsetProfile());
    if (from.width == to.width && from.height == to.height) {
      g_lookApplied.store(effective.reason == rules::RuleReason::kQuest2Combat, std::memory_order_release);
      tq::CommitApplied(want);
      g_appliedLevel.store(static_cast<std::uint64_t>(want), std::memory_order_relaxed);
      g_equalCommits.fetch_add(1, std::memory_order_relaxed);
      g_lastFrom.store(static_cast<int>(have), std::memory_order_relaxed);
      g_lastTo.store(static_cast<int>(want), std::memory_order_relaxed);
      return;
    }
  }

  // The eye size is asked through the eye-size route when it is in place (the reference input's RefreshRTSize hook), else through
  // the game's own function. The route is looked up here, not at install, so either install order gives the same answer.
  const pin::RefreshRTSizeFn routed = tq::HookedRefresh();
  const pin::RefreshRTSizeFn refresh = routed != nullptr ? routed : g_refreshRtSize.load(std::memory_order_acquire);
  const pin::VrFlagFn initialized = g_vrInitialized.load(std::memory_order_acquire);
  const pin::VrFlagFn graphicsInitialized = g_vrGraphicsInitialized.load(std::memory_order_acquire);
  const pin::VrFlagFn displayLost = g_vrDisplayLost.load(std::memory_order_acquire);
  const pin::EngineResizeFn expected = g_engineResize.load(std::memory_order_acquire);
  if (refresh == nullptr || initialized == nullptr || graphicsInitialized == nullptr || displayLost == nullptr ||
      expected == nullptr || engine == nullptr) {
    RefuseForGood("game_functions_missing");
    return;
  }
  // Not yet: the request stays pending and is looked at again next frame.
  if ((LoadAt<std::uint8_t>(engine, pin::engine_layout::kReadyFlags) & pin::engine_layout::kReadyBit) == 0) return;
  if (initialized() == 0 || graphicsInitialized() == 0 || displayLost() != 0) return;
  // The engine's resize slot must be CEngine::Resize itself; anything else is a different engine than the one read.
  void* const* const table = LoadAt<void* const*>(engine, 0);
  void* const slot = table != nullptr ? table[pin::engine_layout::kResizeVtableOffset / sizeof(void*)] : nullptr;
  pin::EngineResizeFn resize = nullptr;
  static_assert(sizeof(resize) == sizeof(slot), "function pointer size");
  std::memcpy(&resize, &slot, sizeof(resize));
  if (resize == nullptr || resize != expected) {
    RefuseForGood("resize_slot_is_not_engine_resize");
    return;
  }
  const std::uint32_t displayWidth = LoadAt<std::uint32_t>(engine, pin::engine_layout::kDisplayWidth);
  const std::uint32_t displayHeight = LoadAt<std::uint32_t>(engine, pin::engine_layout::kDisplayHeight);
  const std::int32_t displayMode = LoadAt<std::int32_t>(engine, pin::engine_layout::kDisplayMode);

  // The step-down's guard: no look-driven Resize while a gamespace loads, within 10 s of one, or while the upload count or lock is taken
  // (docs/quest/combat/texture_quest2.md). Held: looked at again next frame.
  if (lookDriven) {
    std::uint64_t uploads = 0;
    std::uint32_t lock = 0;
    const rules::Hold hold = LookHold(&uploads, &lock);
    if (hold != rules::Hold::kNone) {
      NoteHold(hold, have, want, uploads, lock);
      return;
    }
  }
  g_holding.store(false, std::memory_order_release);

  // CCanvas::Resize resizes the render-target manager before CVR::Resize refreshes the eye size, so the manager would be built
  // for the previous level. The eye size is refreshed first (it only writes CVR's size, and the eye-size hook answers it with the
  // level committed here).
  const unsigned long long queriesBefore = tq::SizeQueries();
  tq::CommitApplied(want);
  refresh();
  if (tq::SizeQueries() == queriesBefore) {
    // The stored size override is set: the runtime is never asked, so no level can apply. Put everything back.
    tq::CommitApplied(have);
    refresh();
    RefuseForGood("eye_size_override_is_set");
    return;
  }
  g_appliedLevel.store(static_cast<std::uint64_t>(want), std::memory_order_relaxed);
  g_lastFrom.store(static_cast<int>(have), std::memory_order_relaxed);
  g_lastTo.store(static_cast<int>(want), std::memory_order_relaxed);
  const int rc = resize(engine, displayWidth, displayHeight, displayMode);
  g_lastRc.store(rc, std::memory_order_relaxed);
  if (rc == 0) {
    g_applies.fetch_add(1, std::memory_order_relaxed);
    g_lookApplied.store(effective.reason == rules::RuleReason::kQuest2Combat, std::memory_order_release);
    if (lookDriven) g_lookApplies.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The engine could not rebuild at the new level: back to the level its buffers were built for, and the failed one is not
  // asked for again.
  g_failed.fetch_add(1, std::memory_order_relaxed);
  if (lookDriven) LookOff("resize_failed");  // the step-down is not tried again in this run
  tq::CommitApplied(have);
  g_appliedLevel.store(static_cast<std::uint64_t>(have), std::memory_order_relaxed);
  tq::RequestLevel(have, tq::RequestSource::kMenu);
  refresh();
  const int back = resize(engine, displayWidth, displayHeight, displayMode);
  if (back != 0) g_failed.fetch_add(1, std::memory_order_relaxed);
}

// CEngine::CheckForResize: the engine's own per-frame check runs unchanged and its result is returned as it came.
std::uint64_t HandleCheck(CheckThunk::Fn original, void* engine) noexcept {
  const std::uint64_t result = original(engine);
  ApplyPending(engine);
  return result;
}

NEVR_HOOK_RECORD(kFileHook, FileThunk, &HandleDecodeFile);
NEVR_HOOK_RECORD(kTextHook, TextThunk, &HandleDecodeText);
NEVR_HOOK_RECORD(kValidateHook, ValidateThunk, &HandleValidate);
NEVR_HOOK_RECORD(kApplyHook, ApplyThunk, &HandleApply);
NEVR_HOOK_RECORD(kCheckHook, CheckThunk, &HandleCheck);

// ---- the poller: the only place this feature logs -----------------------------------------------------------------------

struct Logged {
  bool start = false;
  int request = kUnset;
  int applied = kUnset;
  bool json[kDocuments] = {false, false, false};
  unsigned long long refused = 0;
  unsigned long long failed = 0;
  // The last eye-size decision written (texture_quality_eye): its source, width and height.
  int eyeSource = -1;
  int eyeW = -1;
  int eyeH = -1;
  // The step-down's lines: the last rule written (level and reason), how many, the holds written and the look-off line.
  int ruleLevel = -1;
  int ruleReason = -1;
  int ruleLines = 0;
  unsigned long long ruleUnlogged = 0;
  unsigned long long ruleCounted = 0;       // the value of ruleUnlogged the last count line named
  std::int64_t ruleCountAtMs = 0;           // when it was written (0: none yet)
  unsigned long long holds = 0;
  bool lookOff = false;
};
Logged g_logged;
// The look's gate (texture_quality_rules.h), stepped by the poller only (under g_pollMutex).
rules::LookGate g_gate;
constexpr int kRuleLines = 32;
// After the cap, the count of changes not written is logged at most this often, and only while it grew.
constexpr std::int64_t kRuleCountEveryMs = 60000;

// Steps the look's gate and publishes it, then writes the step-down's lines: one per change of the level sent or its reason (at most kRuleLines,
// then one notice and a count), one per hold, one when the step-down is turned off for the run. Writes nothing with no look registered.
int PollLook() noexcept {
  const input::LookFn look = input::Registered();
  if (look == nullptr) return 0;
  int lines = 0;
  if (g_uploadCount.load(std::memory_order_acquire) == nullptr || g_uploadLock.load(std::memory_order_acquire) == nullptr) {
    LookOff("upload_words_unavailable");
  }
  const std::int64_t now = NowMs();
  const rules::Look seen = look();
  g_gate = rules::LookStep(g_gate, seen, input::Switches(), now);
  g_quietSinceMs.store(g_gate.quietSinceMs, std::memory_order_release);
  g_gateReason.store(static_cast<int>(g_gate.reason), std::memory_order_release);
  g_lookOn.store(g_gate.on, std::memory_order_release);

  const int requested = tq::RequestedLevelValue();
  const Level asked = requested == kUnset ? tq::LatchedLevel() : static_cast<Level>(requested);
  const rules::Effective effective = Decide(asked);
  if (static_cast<int>(effective.level) != g_logged.ruleLevel || static_cast<int>(effective.reason) != g_logged.ruleReason) {
    g_logged.ruleLevel = static_cast<int>(effective.level);
    g_logged.ruleReason = static_cast<int>(effective.reason);
    if (g_logged.ruleLines < kRuleLines) {
      ++g_logged.ruleLines;
      const nevr_quest::headset_profile::Profile* const profile = tq::ActiveHeadsetProfile();
      const bool quest2 = profile != nullptr && profile->id == nevr_quest::headset_profile::Id::kQuest2;
      const bool stepped = effective.reason == rules::RuleReason::kQuest2Combat;
      const sentinel::LogField fields[] = {
          {"level_asked", tq::LevelName(asked)},
          {"level_sent", tq::LevelName(effective.level)},
          {"reason", rules::RuleReasonName(effective.reason)},
          {"where", rules::WhereName(seen.where)},
          {"quest2", quest2 ? 1 : 0},
          {"player_chose", tq::PlayerChose() ? 1 : 0},
          {"unlogged", static_cast<long long>(g_logged.ruleUnlogged)},
          {"msg", stepped ? "a Quest 2 in a settled combat level is sent Very Low; the Resize waits for a window with no level loading"
                          : "the player's level is sent"}};
      sentinel::LogFieldList(sentinel::LogLevel::kInfo, "texture_quality_level_rule", fields, 8U);
      ++lines;
      if (g_logged.ruleLines == kRuleLines) {
        sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_level_rule",
                            {{"capped", kRuleLines}, {"msg", "later changes of the level sent are counted, not logged; texture_quality_level_rule_count names how many, at most once a minute while it grows"}});
        ++lines;
      }
    } else {
      ++g_logged.ruleUnlogged;
    }
  }
  if (g_logged.ruleLines >= kRuleLines && g_logged.ruleUnlogged != g_logged.ruleCounted &&
      (g_logged.ruleCountAtMs == 0 || now - g_logged.ruleCountAtMs >= kRuleCountEveryMs)) {
    g_logged.ruleCounted = g_logged.ruleUnlogged;
    g_logged.ruleCountAtMs = now;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_level_rule_count",
                        {{"unlogged", static_cast<long long>(g_logged.ruleUnlogged)},
                         {"level_sent", tq::LevelName(static_cast<Level>(g_logged.ruleLevel))},
                         {"reason", rules::RuleReasonName(static_cast<rules::RuleReason>(g_logged.ruleReason))},
                         {"msg", "changes of the level sent past the 32 written lines; the level and reason here are the latest"}});
    ++lines;
  }
  const unsigned long long holds = g_lookHolds.load(std::memory_order_acquire);
  if (holds != g_logged.holds) {
    g_logged.holds = holds;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_level_held",
                        {{"level_from", tq::LevelName(static_cast<Level>(g_holdFrom.load(std::memory_order_relaxed)))},
                         {"level_to", tq::LevelName(static_cast<Level>(g_holdTo.load(std::memory_order_relaxed)))},
                         {"hold", rules::HoldName(static_cast<rules::Hold>(g_holdReason.load(std::memory_order_relaxed)))},
                         {"uploads", static_cast<long long>(g_holdUploads.load(std::memory_order_relaxed))},
                         {"lock", static_cast<long long>(g_holdLock.load(std::memory_order_relaxed))},
                         {"quiet_ms", static_cast<long long>(g_holdQuietMs.load(std::memory_order_relaxed))},
                         {"holds", static_cast<long long>(holds)},
                         {"msg", "the step-down's Resize waits: it is made only with no level loading for 10 s and no upload in flight"}});
    ++lines;
  }
  if (!g_logged.lookOff && g_lookOff.load(std::memory_order_acquire)) {
    g_logged.lookOff = true;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "texture_quality_level_rule_off",
                        {{"why", g_lookOffWhy.load(std::memory_order_acquire)},
                         {"msg", "the Quest 2 step-down is off for this run: the player's level is sent in combat too"}});
    ++lines;
  }
  return lines;
}
std::atomic<int> g_propertyLast{kUnset};

const char* DocumentName(int document) noexcept {
  return document == kMenuSettings ? "menu_settings" : (document == kGameSettings ? "gamesettings_config" : "unknown");
}

bool ParsePropertyLevel(const char* value, Level* out) noexcept {
  if (tq::ParseLevel(value, out)) return true;
  if (value[0] >= '0' && value[0] <= '9' && value[1] == '\0') {
    *out = tq::LevelForMenuIndex(value[0] - '0');
    return value[0] - '0' < tq::kMenuLevelCount;
  }
  return false;
}

#if defined(__ANDROID__)
int AndroidProperty(const char* name, char* out) noexcept { return __system_property_get(name, out); }
#endif

// Writes the player's last choice in the menu to config.yaml as graphics.texture_quality, through nevr_config (NevrConfig::Set: the
// file is edited in place, comments and every other key kept, and replaced atomically; a file it cannot edit safely is refused and
// left as it was). Runs on the poller thread, never inside a game call. A refusal or failure is logged with its reason and errno; the
// choice then lasts until the game exits. Returns the number of lines logged.
int SaveChoice() noexcept {
  const int index = g_pendingSave.exchange(kUnset, std::memory_order_acq_rel);
  if (index == kUnset) return 0;
  const char* const consequence = "this choice is not remembered at the next launch";
  if (!g_configFileReady.load(std::memory_order_acquire)) {
    g_saveFailures.fetch_add(1, std::memory_order_relaxed);
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_saved",
                        {{"result", "no_config_file"}, {"menu_index", index}, {"consequence", consequence}});
    return 1;
  }
  const Level level = tq::LevelForMenuIndex(index);
  nevr::NevrConfig config;
  config.BindFile(g_configFile);
  // The hand pose poller edits the same file: NevrConfig::Set holds one lock per file across its read, splice and rename.
  const nevr::ConfigSetResult set = config.Set("graphics.texture_quality", std::string(tq::LevelName(level)));
  if (!set.ok) {
    g_saveFailures.fetch_add(1, std::memory_order_relaxed);
    sentinel::LogFields(sentinel::LogLevel::kWarn, "texture_quality_saved",
                        {{"result", "refused"},
                         {"reason", set.reason},
                         {"errno", set.err},
                         {"menu_index", index},
                         {"config_file", g_configFile},
                         {"consequence", consequence}});
    return 1;
  }
  g_saveWrites.fetch_add(1, std::memory_order_relaxed);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_saved",
                      {{"result", set.wrote ? "written" : "unchanged"},
                       {"quality", tq::LevelName(level)},
                       {"menu_index", index},
                       {"config_file", g_configFile},
                       {"consequence", "the next launch starts at this level"}});
  return 1;
}

int PollLocked() noexcept {
  int lines = 0;
  if (const PropertyReader property = g_property.load(std::memory_order_acquire)) {
    char value[92] = {};
    if (property(kLevelProperty, value) > 0) {
      Level level = Level::kStock;
      if (ParsePropertyLevel(value, &level) && static_cast<int>(level) != g_propertyLast.load(std::memory_order_relaxed)) {
        g_propertyLast.store(static_cast<int>(level), std::memory_order_relaxed);
        tq::NotePlayerChose();  // a level set by hand stands in combat too (#1737 step-down)
        tq::RequestLevel(level, tq::RequestSource::kProperty);
      }
    }
  }
  const int source = g_startSource.load(std::memory_order_acquire);
  if (!g_logged.start && source != kStartNotAsked) {
    g_logged.start = true;
    const int start = g_startLevel.load(std::memory_order_acquire);
    const Level startLevel = start == kUnset ? tq::LatchedLevel() : static_cast<Level>(start);
    const char* const sourceName = source == kStartReferenceRule     ? "reference_rule"
                                   : source == kStartPlatformDefault ? "platform_default"
                                   : g_fromConfig.load(std::memory_order_acquire) ? "config"
                                                                                  : "default";
    const int referenceIndex = g_referenceIndex.load(std::memory_order_acquire);
    const bool reference = source == kStartReferenceRule || source == kStartPlatformDefault;
    const char* const consequence = "this start is not saved; only a change in the menu is written to config.yaml for the next launch";
    if (reference) {
      const sentinel::LogField fields[] = {{"source", sourceName},
                                           {"input", tq::InputName(tq::InputKind())},
                                           {"quality", tq::LevelName(startLevel)},
                                           {"menu_index", tq::MenuIndexForLevel(startLevel)},
                                           {"reference_index", referenceIndex < 0 ? 0 : referenceIndex},
                                           {"reference_file", g_referenceFile.load(std::memory_order_acquire)},
                                           {"consequence", consequence}};
      sentinel::LogFieldList(sentinel::LogLevel::kInfo, "texture_quality_start", fields, 7U);
    } else {
      const sentinel::LogField fields[] = {{"source", sourceName},
                                           {"input", tq::InputName(tq::InputKind())},
                                           {"quality", tq::LevelName(startLevel)},
                                           {"menu_index", tq::MenuIndexForLevel(startLevel)},
                                           {"consequence", consequence}};
      sentinel::LogFieldList(sentinel::LogLevel::kInfo, "texture_quality_start", fields, 5U);
    }
    ++lines;
  }
  for (int document = kMenuSettings; document < kDocuments; ++document) {
    const int status = g_jsonStatus[document].load(std::memory_order_acquire);
    if (status == 0 || g_logged.json[document]) continue;
    g_logged.json[document] = true;
    const bool ok = status - 1 == static_cast<int>(texture_menu::Status::kRewritten);
    pthread_mutex_lock(&g_shapeMutex);
    const std::string shapeSite = g_shapeSite[document];
    const std::string topKeys = g_topKeys[document];
    pthread_mutex_unlock(&g_shapeMutex);
    std::vector<sentinel::LogField> fields = {
        {"document", DocumentName(document)},
        {"result", texture_menu::StatusName(static_cast<texture_menu::Status>(status - 1))},
        {"rows", g_jsonRows[document].load(std::memory_order_relaxed)},
        {"pages", g_jsonPages[document].load(std::memory_order_relaxed)},
        // KNOWN-WRONG #2043 the Controller Hand Flip rows the rewrite wrote or removed (hand_flip.h).
        {"hand_flip_rows", g_jsonHandFlip[document].load(std::memory_order_relaxed)}};
    // #1823: a document of another shape also says which walk found no row and its top-level keys (texture_menu.h); a rewritten
    // document's line is unchanged.
    if (!shapeSite.empty()) {
      fields.emplace_back("shape_site", shapeSite.c_str());
      fields.emplace_back("top_keys", topKeys.c_str());
    }
    sentinel::LogFieldList(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "texture_menu_json", fields.data(),
                           fields.size());
    ++lines;
  }
  lines += SaveChoice();
  lines += PollLook();
  const int requested = tq::RequestedLevelValue();
  if (requested != kUnset && requested != g_logged.request) {
    const int from = g_logged.request == kUnset ? static_cast<int>(tq::LatchedLevel()) : g_logged.request;
    g_logged.request = requested;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_request",
                        {{"source", tq::LastRequestSource() == tq::RequestSource::kProperty ? "property" : "menu"},
                         {"from", tq::LevelName(static_cast<Level>(from))},
                         {"to", tq::LevelName(static_cast<Level>(requested))}});
    ++lines;
  }
  // The eye size the last RefreshRTSize stored, when it differs from the one written before (the actual size, per the route).
  const tq::Decision decision = tq::LastDecision();
  if (decision.source != tq::DecisionSource::kNone &&
      (static_cast<int>(decision.source) != g_logged.eyeSource || decision.w != g_logged.eyeW || decision.h != g_logged.eyeH)) {
    g_logged.eyeSource = static_cast<int>(decision.source);
    g_logged.eyeW = decision.w;
    g_logged.eyeH = decision.h;
    const tq::Counts counts = tq::CurrentCounts();
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_eye",
                        {{"input", tq::InputName(tq::InputKind())},
                         {"quality", tq::LevelName(tq::LatchedLevel())},
                         {"native_w", counts.nativeWidth},
                         {"native_h", counts.nativeHeight},
                         {"w", decision.w},
                         {"h", decision.h},
                         {"source", tq::DecisionSourceName(decision.source)}});
    ++lines;
  }
  const int applied = tq::IsLatched() ? static_cast<int>(tq::LatchedLevel()) : kUnset;
  if (applied != kUnset && applied != g_logged.applied) {
    if (g_logged.applied != kUnset) {
      const tq::Counts counts = tq::CurrentCounts();
      const tq::EyeSize size = tq::EffectiveSize(static_cast<Level>(applied),
                                                 tq::EyeSize{static_cast<int>(counts.nativeWidth), static_cast<int>(counts.nativeHeight)},
                                                 tq::ActiveHeadsetProfile());
      sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_applied",
                          {{"from", tq::LevelName(static_cast<Level>(g_logged.applied))},
                           {"to", tq::LevelName(static_cast<Level>(applied))},
                           {"w", size.width},
                           {"h", size.height},
                           {"rc", g_lastRc.load(std::memory_order_relaxed)},
                           {"applies", static_cast<long long>(g_applies.load(std::memory_order_relaxed))},
                           {"equal_size_commits", static_cast<long long>(g_equalCommits.load(std::memory_order_relaxed))}});
      ++lines;
    }
    g_logged.applied = applied;
  }
  const unsigned long long refused = g_refused.load(std::memory_order_relaxed);
  if (refused != g_logged.refused) {
    g_logged.refused = refused;
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_apply_refused",
                        {{"why", g_disabledWhy.load(std::memory_order_acquire)},
                         {"consequence", "a level change now applies at the next launch"}});
    ++lines;
  }
  const unsigned long long failed = g_failed.load(std::memory_order_relaxed);
  if (failed != g_logged.failed) {
    g_logged.failed = failed;
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_apply_failed",
                        {{"rc", g_lastRc.load(std::memory_order_relaxed)},
                         {"from", tq::LevelName(static_cast<Level>(g_lastFrom.load(std::memory_order_relaxed) == kUnset
                                                                       ? 0
                                                                       : g_lastFrom.load(std::memory_order_relaxed)))},
                         {"to", tq::LevelName(static_cast<Level>(g_lastTo.load(std::memory_order_relaxed) == kUnset
                                                                     ? 0
                                                                     : g_lastTo.load(std::memory_order_relaxed)))},
                         {"consequence", "the engine could not rebuild; the previous level was put back"}});
    ++lines;
  }
  lines += hand_flip::PollRow();  // KNOWN-WRONG #2043 says once that the toggle's row is not offered
  return lines;
}

pthread_mutex_t g_pollMutex = PTHREAD_MUTEX_INITIALIZER;  // one pass at a time (the thread and a test)
pthread_mutex_t g_wait = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond;
bool g_condReady = false;
bool g_running = false;
pthread_t g_thread;
constexpr unsigned kPeriodMs = 500;

int PollOnce() noexcept {
  pthread_mutex_lock(&g_pollMutex);
  const int lines = PollLocked();
  pthread_mutex_unlock(&g_pollMutex);
  return lines;
}

// The poller has no stop: it ends with the process, like the reporter thread (hook_report.h, "Teardown"); the sentinel is never
// unloaded.
void* PollerMain(void*) {
  pthread_mutex_lock(&g_wait);
  for (;;) {
    timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += static_cast<long>(kPeriodMs) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_nsec -= 1000000000L;
      ++deadline.tv_sec;
    }
    int rc = 0;
    while (rc == 0) rc = pthread_cond_timedwait(&g_cond, &g_wait, &deadline);  // nothing signals it: it ends at the deadline
    pthread_mutex_unlock(&g_wait);
    PollOnce();
    pthread_mutex_lock(&g_wait);
  }
  return nullptr;
}

bool StartPoller() noexcept {
  if (g_running) return true;
  if (!g_condReady) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    g_condReady = true;
  }
  const int rc = pthread_create(&g_thread, nullptr, &PollerMain, nullptr);
  if (rc == 0) {
    pthread_setname_np(g_thread, "nevr-texture");
    g_running = true;
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_menu_install", {{"result", "poller_not_started"}, {"errno", rc}});
  }
  return rc == 0;
}

// ---- install ------------------------------------------------------------------------------------------------------------

bool Equal(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

// Proves one function of the live libr15 (its first words are the pinned build's) and publishes its address.
const char* ResolveFunction(const sentinel::ElfImage& image, const pin::PinnedFunction& f, void** out) noexcept {
  const unsigned char* const fn = reinterpret_cast<const unsigned char*>(image.base) + f.vaddr;
  if (std::memcmp(fn, f.code, f.words * sizeof(std::uint32_t)) != 0) return "prologue_mismatch";
  static_assert(sizeof(*out) == sizeof(fn), "function pointer size");
  std::memcpy(out, &fn, sizeof(fn));
  return nullptr;
}

template <typename Thunk>
sentinel::GotStatus Hook(sentinel::GotHook& hook, const sentinel::GotTarget& target) noexcept {
  return sentinel::InstallThunk<Thunk>(hook, target);
}

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"gfx_check_calls", &CheckThunk::CallCounter(), sentinel::ReportKind::kCalls},
      {"gfx_check_faults", &CheckThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_apply_faults", &ApplyThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_validate_faults", &ValidateThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_decode_file_faults", &FileThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_decode_text_faults", &TextThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_menu_deliveries", &g_deliveries, sentinel::ReportKind::kCalls},
      {"gfx_menu_level", &g_menuLevel, sentinel::ReportKind::kCalls},
      {"gfx_applied_level", &g_appliedLevel, sentinel::ReportKind::kCalls},
      {"gfx_live_applies", &g_applies, sentinel::ReportKind::kCalls},
      {"gfx_live_refused", &g_refused, sentinel::ReportKind::kCalls},
      {"gfx_live_failed", &g_failed, sentinel::ReportKind::kFaults},
      {"gfx_menu_json_rewrites", &g_jsonRewrites, sentinel::ReportKind::kCalls},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_menu_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

void SetConfigFile(const char* path) noexcept {
  const std::size_t length = path == nullptr ? 0 : std::strlen(path);
  if (length == 0 || length >= sizeof(g_configFile)) {
    g_configFileReady.store(false, std::memory_order_release);
    return;
  }
  std::memcpy(g_configFile, path, length + 1);
  g_configFileReady.store(true, std::memory_order_release);
}

bool Install(tq::Level level, bool fromConfig) noexcept {
  const char* const source = fromConfig ? "config" : "default";
  auto skipped = [&](const char* why, const char* detail) noexcept {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_menu_install",
                        {{"result", "skipped"}, {"why", why}, {"detail", detail}, {"quality", tq::LevelName(level)},
                         {"source", source},
                         {"consequence", "no Texture Resolution control; the level is the configuration's, fixed at launch"}});
    return false;
  };
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(pin::kLibR15, &image)) return skipped("module_not_loaded", "libr15.so");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return skipped("no_build_id", "libr15.so");
  if (!Equal(id, pin::LibR15PatchedBuildId())) return skipped("build_id_mismatch", id);

  void* refresh = nullptr;
  void* resize = nullptr;
  void* initialized = nullptr;
  void* graphicsInitialized = nullptr;
  void* displayLost = nullptr;
  void* localAppData = nullptr;
  struct Item {
    const pin::PinnedFunction* f;
    void** out;
  };
  const Item items[] = {{&pin::kRefreshRTSize, &refresh},           {&pin::kEngineResize, &resize},
                        {&pin::kVrInitialized, &initialized},       {&pin::kVrGraphicsInitialized, &graphicsInitialized},
                        {&pin::kVrDisplayLost, &displayLost},       {&pin::kLocalAppDataDirectory, &localAppData}};
  for (const Item& item : items) {
    if (const char* why = ResolveFunction(image, *item.f, item.out)) return skipped(why, item.f->name);
  }
  Game game;
  std::memcpy(&game.refreshRtSize, &refresh, sizeof(refresh));
  std::memcpy(&game.engineResize, &resize, sizeof(resize));
  std::memcpy(&game.vrInitialized, &initialized, sizeof(initialized));
  std::memcpy(&game.vrGraphicsInitialized, &graphicsInitialized, sizeof(graphicsInitialized));
  std::memcpy(&game.vrDisplayLost, &displayLost, sizeof(displayLost));
  std::memcpy(&game.localAppDataDirectory, &localAppData, sizeof(localAppData));
#if defined(__ANDROID__)
  const PropertyReader property = &AndroidProperty;
#else
  const PropertyReader property = nullptr;
#endif
  // The upload count and lock a look-driven Resize reads first: their addresses are the pinned build's when the words that form them hold.
  {
    namespace upl = nevr_quest::texture_quality::targets;
    static constexpr nevr_quest::combat::pinned::Word kUploadWords[] = {upl::kLockPage, upl::kLockAdd, upl::kCountPage, upl::kCountLoad};
    const nevr_quest::combat::pinned::Pins pins{kUploadWords, 4, nullptr, 0};
    nevr_quest::combat::pinned::Failure failure;
    if (!nevr_quest::combat::pinned::FirstFailure(image.base, &pins, 1, &failure)) {
      game.uploadCount = reinterpret_cast<const std::uint64_t*>(image.base + upl::kUploadCountVaddr);
      game.uploadLock = reinterpret_cast<const std::uint32_t*>(image.base + upl::kUploadLockVaddr);
    } else {
      char va[19];
      char expected[19];
      char actual[19];
      sentinel::LogFields(sentinel::LogLevel::kWarn, "texture_menu_upload_words",
                          {{"result", "mismatch"}, {"what", failure.what}, {"va", sentinel::HexString(va, failure.vaddr)},
                           {"expected", sentinel::HexString(expected, failure.want)},
                           {"actual", sentinel::HexString(actual, failure.have)},
                           {"consequence", "the upload words are not pinned in this libr15, so a Quest 2 step-down, if this build registers one, is off for this run; the player's texture level is unaffected"}});
    }
  }
  ArmForTest(game, level, fromConfig, property);

  // The three hooks that make the control work first; the documents that offer it only when they hold.
  FileThunk::Arm(kFileHook);
  TextThunk::Arm(kTextHook);
  ValidateThunk::Arm(kValidateHook);
  ApplyThunk::Arm(kApplyHook);
  CheckThunk::Arm(kCheckHook);
  const sentinel::GotStatus validate = Hook<ValidateThunk>(g_validateHook, pin::LibR15ValidateGfxSettings());
  const sentinel::GotStatus apply = Hook<ApplyThunk>(g_applyHook, pin::LibR15ApplyRuntimeGfxSettings());
  const sentinel::GotStatus check = Hook<CheckThunk>(g_checkHook, pin::LibR15CheckForResize());
  const bool engineHooks = validate == sentinel::GotStatus::kOk && apply == sentinel::GotStatus::kOk && check == sentinel::GotStatus::kOk;
  sentinel::GotStatus file = sentinel::GotStatus::kNotInstalled;
  sentinel::GotStatus text = sentinel::GotStatus::kNotInstalled;
  if (engineHooks) {
    text = Hook<TextThunk>(g_textHook, pin::LibR15JsonDecodeFromText());
    file = Hook<FileThunk>(g_fileHook, pin::LibR15JsonDecodeFromFile());
  }
  const bool all = engineHooks && file == sentinel::GotStatus::kOk && text == sentinel::GotStatus::kOk;
  sentinel::LogFields(all ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "texture_menu_install",
                      {{"result", all ? "installed" : "partial"},
                       {"quality", tq::LevelName(level)},
                       {"source", source},
                       {"validate_gfx_settings", sentinel::GotStatusName(validate)},
                       {"apply_runtime_gfx_settings", sentinel::GotStatusName(apply)},
                       {"check_for_resize", sentinel::GotStatusName(check)},
                       {"decode_from_text", sentinel::GotStatusName(text)},
                       {"decode_from_file", sentinel::GotStatusName(file)},
                       {"consequence", all ? "the Texture Resolution control is offered and a change applies at the next frame"
                                           : "a level change applies at the next launch, or the control is not offered"}});
  if (engineHooks) {
    tq::SetStartResolver(&ResolveStart);
    StartPoller();
  }
  return all;
}

void ArmForTest(const Game& game, tq::Level configLevel, bool fromConfig, PropertyReader property) noexcept {
  g_fromConfig.store(fromConfig, std::memory_order_release);
  g_refreshRtSize.store(game.refreshRtSize, std::memory_order_release);
  g_vrInitialized.store(game.vrInitialized, std::memory_order_release);
  g_vrGraphicsInitialized.store(game.vrGraphicsInitialized, std::memory_order_release);
  g_vrDisplayLost.store(game.vrDisplayLost, std::memory_order_release);
  g_localAppData.store(game.localAppDataDirectory, std::memory_order_release);
  g_engineResize.store(game.engineResize, std::memory_order_release);
  g_uploadCount.store(game.uploadCount, std::memory_order_release);
  g_uploadLock.store(game.uploadLock, std::memory_order_release);
  g_property.store(property, std::memory_order_release);
  g_defaultIndex.store(tq::MenuIndexForLevel(configLevel), std::memory_order_release);
  tq::SetStartResolver(&ResolveStart);
  FileThunk::Arm(kFileHook);
  TextThunk::Arm(kTextHook);
  ValidateThunk::Arm(kValidateHook);
  ApplyThunk::Arm(kApplyHook);
  CheckThunk::Arm(kCheckHook);
}

void ResetForTest() noexcept {
  g_referenceIndex.store(kUnset);
  g_referenceFile.store("none");
  g_refreshRtSize.store(nullptr);
  g_vrInitialized.store(nullptr);
  g_vrGraphicsInitialized.store(nullptr);
  g_vrDisplayLost.store(nullptr);
  g_localAppData.store(nullptr);
  g_engineResize.store(nullptr);
  g_property.store(nullptr);
  g_lastIndex.store(kUnset);
  g_pendingSave.store(kUnset);
  g_lastHandFlip.store(kUnset);
  hand_flip::ResetForTest();
  g_configFileReady.store(false);
  g_configFile[0] = '\0';
  g_saveWrites.store(0);
  g_saveFailures.store(0);
  g_defaultIndex.store(tq::kDefaultMenuIndex);
  g_startSource.store(kStartNotAsked);
  g_fromConfig.store(false);
  g_startLevel.store(kUnset);
  g_playerSaved.store(false);
  g_firstDelivery.store(false);
  g_deliveries.store(0);
  g_menuLevel.store(0);
  g_appliedLevel.store(0);
  g_applies.store(0);
  g_equalCommits.store(0);
  g_refused.store(0);
  g_failed.store(0);
  g_jsonRewrites.store(0);
  g_configOverrides.store(0);
  g_disabled.store(false);
  g_disabledWhy.store("none");
  g_lastRc.store(0);
  g_lastFrom.store(kUnset);
  g_lastTo.store(kUnset);
  for (int i = 0; i < kDocuments; ++i) {
    g_jsonStatus[i].store(0);
    g_jsonRows[i].store(0);
    g_jsonPages[i].store(0);
    g_jsonHandFlip[i].store(0);  // KNOWN-WRONG #2043
    pthread_mutex_lock(&g_shapeMutex);
    g_shapeSite[i].clear();
    g_topKeys[i].clear();
    pthread_mutex_unlock(&g_shapeMutex);
  }
  g_logged = Logged();
  g_gate = rules::LookGate();
  g_uploadCount.store(nullptr);
  g_uploadLock.store(nullptr);
  g_clock.store(nullptr);
  g_lookOn.store(false);
  g_gateReason.store(static_cast<int>(rules::RuleReason::kNotInCombat));
  g_quietSinceMs.store(0);
  g_lookOff.store(false);
  g_lookOffWhy.store("none");
  g_lookApplied.store(false);
  g_lookApplies.store(0);
  g_holding.store(false);
  g_lookHolds.store(0);
  g_holdReason.store(0);
  g_holdFrom.store(kUnset);
  g_holdTo.store(kUnset);
  g_holdUploads.store(0);
  g_holdLock.store(0);
  g_holdQuietMs.store(0);
  g_propertyLast.store(kUnset);
  SetArmed(kNone);
  pthread_mutex_lock(&g_keptMutex);
  for (Kept& k : g_kept) k = Kept();  // the strings are leaked on purpose (the parser may hold them)
  pthread_mutex_unlock(&g_keptMutex);
}

int PollOnceForTest() noexcept { return PollOnce(); }

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }

Totals CurrentTotals() noexcept {
  return {g_deliveries.load(), g_jsonRewrites.load(), g_applies.load(),       g_equalCommits.load(),
          g_refused.load(),    g_failed.load(),       g_configOverrides.load(), g_saveWrites.load(),
          g_saveFailures.load(), g_disabled.load(), g_disabledWhy.load(), g_lookApplies.load(), g_lookHolds.load(), g_lookOff.load()};
}

}  // namespace nevr_quest::texture_menu
